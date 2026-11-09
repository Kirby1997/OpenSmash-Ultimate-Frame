/* datapicker.h - the in-game game-data picker of the Linux port (WP-U5b): the model and the view, without SDL.
 *
 * The port ships nothing of the game: the player points it at his own dump. When boot.c finds no usable data (or the player
 * asked with --pick-data / SSBU_PICK_DATA=1) it calls dp_picker_hook before the game window exists; datapicker_sdl.c opens
 * its own SDL window for that and closes it again (SDL_Quit) before display.c creates the game's window.
 *
 * This file is the part that needs no display: the screens (status / error, folder browser, places, waiting for the system
 * dialog), what each input does, the checks (datapaths.c dp_validate_picked, dp_fonts_check) and the saved choice
 * (dp_save_choice, dp_config_set "fonts"), and the drawing into a plain 32-bit canvas with a built-in bitmap font (no font
 * file is read: the system fonts may be exactly what is missing). The frontend turns SDL events into dpk_* calls and shows
 * the canvas; the tests (tests/port/picker) drive the same calls with synthetic input and read the canvas back.
 *
 * Controls follow the in-game settings menu (Dear ImGui navigation, upstream menu.cpp) and the game's own pad layout
 * (nnpad_sdl.c "positional": the button in the Nintendo A position, on the right, confirms; the bottom one, B, goes back):
 *   confirm  A (right face button) / Enter / Space / left click        back   B (bottom) / Esc / Backspace / right click
 *   move     d-pad / left stick / arrows / mouse                       page   L / R / Page Up / Page Down / wheel
 *   use this folder  X (top) / Ctrl+Enter     places  Y (left) / Tab   hidden files  - (Minus, View) / Ctrl+H
 *   quit     the window's close button / Ctrl+Q / the Quit entry       typing a letter jumps to the first name with it
 * Every function is reachable from a list row too (mouse only, or a pad without X / Y).
 *
 * Linux only, one thread (the caller's). */
#ifndef SSBU_DATAPICKER_H
#define SSBU_DATAPICKER_H
#include <stdint.h>
#include "datapaths.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DPK_NONE, DPK_UP, DPK_DOWN, DPK_LEFT, DPK_RIGHT, DPK_PAGE_UP, DPK_PAGE_DOWN, DPK_FIRST, DPK_LAST,
    DPK_CONFIRM, DPK_BACK, DPK_USE_HERE, DPK_PLACES, DPK_HIDDEN, DPK_QUIT, DPK_ACTIONS
} dpk_action;
typedef enum { DPK_SCR_STATUS, DPK_SCR_BROWSE, DPK_SCR_PLACES, DPK_SCR_DIALOG } dpk_screen;
typedef enum { DPK_FOR_DATA, DPK_FOR_FONTS } dpk_target;
typedef enum { DPK_RUNNING = 0, DPK_PICKED = 1, DPK_KEEP = 2, DPK_QUITTED = -1 } dpk_result;
typedef enum { DPK_DEV_KEYS, DPK_DEV_PAD } dpk_device;              /* which hints are shown: the last input wins */
/* status-screen entries */
enum { DPK_M_START, DPK_M_DATA, DPK_M_DATA_HERE, DPK_M_FONTS, DPK_M_RETRY, DPK_M_QUIT };
/* browser rows: the first ones are commands */
enum { DPK_E_USE, DPK_E_PLACES, DPK_E_UP, DPK_E_DIR, DPK_E_FILE };
enum { DPK_TAG_NONE, DPK_TAG_DUMP, DPK_TAG_ROMFS, DPK_TAG_FONTS, DPK_TAG_PACKAGE };

typedef struct { char *name; unsigned char kind, tag; } dpk_entry;
typedef struct { char label[96]; char path[DP_PATH]; } dpk_place;
typedef struct { int x, y, w, h, row_h, first, count, screen; } dpk_list_area;   /* where the last dpk_render put the list (mouse), on which screen */

#define DPK_MAX_ENTRIES 4096
#define DPK_MAX_PLACES 24
typedef struct dpk_state {
    dp_result was;                  /* what boot.c reported (the hook's argument) */
    dp_result now;                  /* what the status screen shows */
    char expect[24];                /* the build this exe needs ("13.0.4" / "13.0.5"), "" = either */
    int pick_mode;                  /* opened by --pick-data with usable data: "Start the game" is offered */
    int dialog_ok;                  /* the system folder dialog may be used (Desktop Mode, not under gamescope) */
    dpk_screen screen, back_to;
    dpk_target target;
    int result;                     /* dpk_result */
    int menu[8], menu_n, menu_sel, msg_scroll, msg_lines;
    char data_dir[DP_PATH];         /* the data folder checked last ("" = was.dir) */
    /* browser */
    char cwd[DP_PATH];
    dpk_entry *ent;
    int n, cap, sel, top, show_hidden, truncated, cwd_tag;
    /* places */
    dpk_place place[DPK_MAX_PLACES];
    int nplaces, place_sel, place_top;
    /* system dialog: requested by the model, run by the frontend */
    int dialog_req, dialog_id;
    /* one line under the list / menu: what just happened */
    char note[640];
    uint32_t note_rgb;
    /* input */
    dpk_device device;
    dpk_action held;
    uint64_t next_repeat;
    char typed[32];
    uint64_t typed_at;
    dpk_list_area area;             /* filled by dpk_render */
    int list_rows;                  /* rows the list showed last (paging); 10 before the first render */
} dpk_state;

/* model */
void dpk_init(dpk_state *s, const dp_result *was, int pick_mode, int dialog_ok);
void dpk_free(dpk_state *s);
void dpk_do(dpk_state *s, dpk_action a);                         /* one action (keyboard auto-repeat: call again) */
void dpk_press(dpk_state *s, dpk_action a, uint64_t now_ms);     /* a pad button / stick went down: the action, then repeats while held */
void dpk_release(dpk_state *s, dpk_action a);
void dpk_tick(dpk_state *s, uint64_t now_ms);                    /* the repeats of a held d-pad / stick direction */
void dpk_pointer(dpk_state *s, int x, int y, int click);         /* canvas coordinates; click 0 = moved, 1 = left, 2 = right */
void dpk_wheel(dpk_state *s, int steps);                         /* + = away from the player (up) */
void dpk_typed(dpk_state *s, const char *utf8, uint64_t now_ms); /* text typed: jump to the first name starting with it */
void dpk_open_browser(dpk_state *s, dpk_target t);               /* the in-game browser (start folder from the state) */
int dpk_browse(dpk_state *s, const char *dir);                   /* list `dir` (1), or keep the old folder with a note (0) */
void dpk_choose(dpk_state *s, const char *dir);                  /* the player chose `dir` for s->target: check, save, maybe done */
/* the system dialog: dpk_dialog_take returns 1 once per request (*target, *start = the folder to open in); the answer is
 * given with the id it had: path = the folder (chosen), NULL with failed = 0 (cancelled) or 1 (no dialog: in-game browser) */
int dpk_dialog_take(dpk_state *s, dpk_target *target, const char **start, int *id);
void dpk_dialog_done(dpk_state *s, int id, const char *path, int failed);

/* view: draw the current screen into a w x h canvas of 0xAARRGGBB pixels (pitch in pixels) */
void dpk_render(dpk_state *s, uint32_t *px, int w, int h, int pitch);
/* the bitmap font, also for the frontend's own messages: 6 x 11 canvas pixels per character at scale 1 */
#define DPK_CELL_W 6
#define DPK_CELL_H 11
int dpk_text(uint32_t *px, int w, int h, int pitch, int x, int y, int scale, uint32_t rgb, const char *utf8, int max_cols);
const char *dpk_headline(const dp_result *r, int pick_mode);     /* one line for the status screen */

/* ---- datapicker_sdl.c: the window. Registers itself as dp_picker_hook (a constructor), so linking it is enough.
 * Returns what the hook returns: 1 chosen and saved, 0 no display / start with the usable data, -1 the player quit.
 * `t` (tests only, NULL in the game): called once per frame after the canvas was drawn into the window, before it is shown;
 * the test pushes SDL events and reads the frame back (window and renderer are SDL_Window * / SDL_Renderer *). */
typedef struct {
    void (*frame)(void *ctx, void *window, void *renderer, dpk_state *s, const uint32_t *px, int w, int h, int frame_no);
    void *ctx;
} dpk_sdl_test;
int datapicker_sdl_run(const dp_result *r, int pick_mode, const dpk_sdl_test *t);

#ifdef __cplusplus
}
#endif
#endif

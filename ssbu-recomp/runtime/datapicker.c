/* datapicker.c - the in-game game-data picker of the Linux port (WP-U5b): model and view, no SDL. See datapicker.h. */
#ifdef __linux__
#include "datapicker.h"
#include "hostpath.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"     /* labels and notes are cut at their buffer on purpose */
#endif

/* ================================================================================================ the bitmap font
 * Drawn for this port (no font file, no third-party glyphs): 5 x 9 pixels in a 6 x 11 cell, rows 0-6 above the baseline,
 * 7-8 the descenders. Codes 1-5 are the picker's icons (folder, file, check, up arrow, pointer), 127 the box for every
 * character that is not ASCII. One string per glyph: the rows top-down, separated by spaces, missing rows empty. */
static const struct { unsigned char c; const char *rows; } g_font_src[] = {
    {1, "..... ##... #.### #...# #...# #...# #####"},       {2, "####. #..## #...# #...# #...# #...# #####"},
    {3, "..... ....# ...## #.##. ###.. .#... ....."},       {4, "..#.. .###. #.#.# ..#.. ..#.. ..#.. ....."},
    {5, "#.... ##... ###.. ####. ###.. ##... #...."},       {127, "##### #...# #...# #...# #...# #...# #####"},
    {' ', ""},                                              {'!', "..#.. ..#.. ..#.. ..#.. ..#.. ..... ..#.."},
    {'"', ".#.#. .#.#."},                                   {'#', ".#.#. .#.#. ##### .#.#. ##### .#.#. .#.#."},
    {'$', "..#.. .#### #.#.. .###. ..#.# ####. ..#.."},       {'%', "##... ##..# ...#. ..#.. .#... #..## ...##"},
    {'&', ".##.. #..#. #.#.. .#... #.#.# #..#. .##.#"},       {'\'', "..#.. ..#.."},
    {'(', "...#. ..#.. .#... .#... .#... ..#.. ...#."},       {')', ".#... ..#.. ...#. ...#. ...#. ..#.. .#..."},
    {'*', "..... ..#.. #.#.# .###. #.#.# ..#.. ....."},       {'+', "..... ..#.. ..#.. ##### ..#.. ..#.. ....."},
    {',', "..... ..... ..... ..... ..... .##.. ..#.. .#..."}, {'-', "..... ..... ..... ##### ..... ..... ....."},
    {'.', "..... ..... ..... ..... ..... .##.. .##.."},       {'/', "..... ....# ...#. ..#.. .#... #.... ....."},
    {'0', ".###. #...# #..## #.#.# ##..# #...# .###."},       {'1', "..#.. .##.. ..#.. ..#.. ..#.. ..#.. .###."},
    {'2', ".###. #...# ....# ...#. ..#.. .#... #####"},       {'3', "##### ...#. ..#.. ...#. ....# #...# .###."},
    {'4', "...#. ..##. .#.#. #..#. ##### ...#. ...#."},       {'5', "##### #.... ####. ....# ....# #...# .###."},
    {'6', "..##. .#... #.... ####. #...# #...# .###."},       {'7', "##### ....# ...#. ..#.. .#... .#... .#..."},
    {'8', ".###. #...# #...# .###. #...# #...# .###."},       {'9', ".###. #...# #...# .#### ....# ...#. .##.."},
    {':', "..... .##.. .##.. ..... .##.. .##.. ....."},       {';', "..... .##.. .##.. ..... .##.. ..#.. .#..."},
    {'<', "...#. ..#.. .#... #.... .#... ..#.. ...#."},       {'=', "..... ..... ##### ..... ##### ..... ....."},
    {'>', ".#... ..#.. ...#. ....# ...#. ..#.. .#..."},       {'?', ".###. #...# ....# ...#. ..#.. ..... ..#.."},
    {'@', ".###. #...# ....# .##.# #.#.# #.#.# .###."},       {'A', ".###. #...# #...# ##### #...# #...# #...#"},
    {'B', "####. #...# #...# ####. #...# #...# ####."},       {'C', ".###. #...# #.... #.... #.... #...# .###."},
    {'D', "###.. #..#. #...# #...# #...# #..#. ###.."},       {'E', "##### #.... #.... ####. #.... #.... #####"},
    {'F', "##### #.... #.... ####. #.... #.... #...."},       {'G', ".###. #...# #.... #.### #...# #...# .####"},
    {'H', "#...# #...# #...# ##### #...# #...# #...#"},       {'I', ".###. ..#.. ..#.. ..#.. ..#.. ..#.. .###."},
    {'J', "..### ...#. ...#. ...#. ...#. #..#. .##.."},       {'K', "#...# #..#. #.#.. ##... #.#.. #..#. #...#"},
    {'L', "#.... #.... #.... #.... #.... #.... #####"},       {'M', "#...# ##.## #.#.# #.#.# #...# #...# #...#"},
    {'N', "#...# #...# ##..# #.#.# #..## #...# #...#"},       {'O', ".###. #...# #...# #...# #...# #...# .###."},
    {'P', "####. #...# #...# ####. #.... #.... #...."},       {'Q', ".###. #...# #...# #...# #.#.# #..#. .##.#"},
    {'R', "####. #...# #...# ####. #.#.. #..#. #...#"},       {'S', ".#### #.... #.... .###. ....# ....# ####."},
    {'T', "##### ..#.. ..#.. ..#.. ..#.. ..#.. ..#.."},       {'U', "#...# #...# #...# #...# #...# #...# .###."},
    {'V', "#...# #...# #...# #...# #...# .#.#. ..#.."},       {'W', "#...# #...# #...# #.#.# #.#.# #.#.# .#.#."},
    {'X', "#...# #...# .#.#. ..#.. .#.#. #...# #...#"},       {'Y', "#...# #...# .#.#. ..#.. ..#.. ..#.. ..#.."},
    {'Z', "##### ....# ...#. ..#.. .#... #.... #####"},       {'[', ".###. .#... .#... .#... .#... .#... .###."},
    {'\\', "..... #.... .#... ..#.. ...#. ....# ....."},      {']', ".###. ...#. ...#. ...#. ...#. ...#. .###."},
    {'^', "..#.. .#.#. #...#"},                             {'_', "..... ..... ..... ..... ..... ..... ..... #####"},
    {'`', ".#... ..#.."},                                   {'a', "..... ..... .###. ....# .#### #...# .####"},
    {'b', "#.... #.... #.##. ##..# #...# #...# ####."},       {'c', "..... ..... .###. #.... #.... #...# .###."},
    {'d', "....# ....# .##.# #..## #...# #...# .####"},       {'e', "..... ..... .###. #...# ##### #.... .###."},
    {'f', "..##. .#..# .#... ###.. .#... .#... .#..."},       {'g', "..... ..... .#### #...# #...# #...# .#### ....# .###."},
    {'h', "#.... #.... #.##. ##..# #...# #...# #...#"},       {'i', "..#.. ..... .##.. ..#.. ..#.. ..#.. .###."},
    {'j', "...#. ..... ..##. ...#. ...#. ...#. ...#. #..#. .##.."}, {'k', "#.... #.... #..#. #.#.. ##... #.#.. #..#."},
    {'l', ".##.. ..#.. ..#.. ..#.. ..#.. ..#.. .###."},       {'m', "..... ..... ##.#. #.#.# #.#.# #.#.# #.#.#"},
    {'n', "..... ..... #.##. ##..# #...# #...# #...#"},       {'o', "..... ..... .###. #...# #...# #...# .###."},
    {'p', "..... ..... ####. #...# #...# #...# ####. #.... #...."}, {'q', "..... ..... .#### #...# #...# #...# .#### ....# ....#"},
    {'r', "..... ..... #.##. ##..# #.... #.... #...."},       {'s', "..... ..... .#### #.... .###. ....# ####."},
    {'t', ".#... .#... ###.. .#... .#... .#..# ..##."},       {'u', "..... ..... #...# #...# #...# #..## .##.#"},
    {'v', "..... ..... #...# #...# #...# .#.#. ..#.."},       {'w', "..... ..... #...# #...# #.#.# #.#.# .#.#."},
    {'x', "..... ..... #...# .#.#. ..#.. .#.#. #...#"},       {'y', "..... ..... #...# #...# #...# #...# .#### ....# .###."},
    {'z', "..... ..... ##### ...#. ..#.. .#... #####"},       {'{', "...#. ..#.. ..#.. .#... ..#.. ..#.. ...#."},
    {'|', "..#.. ..#.. ..#.. ..#.. ..#.. ..#.. ..#.."},       {'}', ".#... ..#.. ..#.. ...#. ..#.. ..#.. .#..."},
    {'~', "..... ..... .#... #.#.# ...#."},
};
static unsigned char g_glyph[128][9];   /* bit 4 = the leftmost column */
static int g_font_ready;
static void font_init(void) {
    if (g_font_ready) return;
    for (size_t i = 0; i < sizeof g_font_src / sizeof g_font_src[0]; i++) {
        const char *p = g_font_src[i].rows;
        for (int r = 0; r < 9 && *p; r++) {
            unsigned char bits = 0;
            for (int c = 0; c < 5 && *p && *p != ' '; c++, p++) if (*p == '#') bits |= (unsigned char)(0x10 >> c);
            while (*p && *p != ' ') p++;
            while (*p == ' ') p++;
            g_glyph[g_font_src[i].c][r] = bits;
        }
    }
    g_font_ready = 1;
}
/* the next character of a UTF-8 string as a glyph code (1-127); 0 at the end. A few typographic marks become their ASCII
 * look-alikes, everything else the box. Malformed bytes count as one box each. */
static int next_glyph(const char **pp) {
    const unsigned char *p = (const unsigned char *)*pp;
    unsigned cp;
    int len;
    if (!*p) return 0;
    if (*p < 0x80) { *pp += 1; return *p; }
    if ((*p & 0xe0) == 0xc0) { cp = *p & 0x1f; len = 2; }
    else if ((*p & 0xf0) == 0xe0) { cp = *p & 0x0f; len = 3; }
    else if ((*p & 0xf8) == 0xf0) { cp = *p & 0x07; len = 4; }
    else { *pp += 1; return 127; }
    for (int i = 1; i < len; i++) {
        if ((p[i] & 0xc0) != 0x80) { *pp += 1; return 127; }
        cp = (cp << 6) | (p[i] & 0x3f);
    }
    *pp += len;
    switch (cp) {
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x2018: case 0x2019: return '\'';
    case 0x201c: case 0x201d: return '"';
    case 0x2026: return '.';
    case 0xa0: return ' ';
    }
    return 127;
}
static int glyph_count(const char *s, const char *end) {
    int n = 0;
    while (s < end && *s) { next_glyph(&s); n++; }
    return n;
}

/* ================================================================================================ drawing */
typedef struct { uint32_t *px; int w, h, pitch; } canvas;
static void fill(const canvas *c, int x, int y, int w, int h, uint32_t rgb) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint32_t *row = c->px + (size_t)(y + j) * (size_t)c->pitch + x;
        for (int i = 0; i < w; i++) row[i] = 0xff000000u | rgb;
    }
}
static void frame_rect(const canvas *c, int x, int y, int w, int h, uint32_t rgb) {
    fill(c, x, y, w, 1, rgb); fill(c, x, y + h - 1, w, 1, rgb);
    fill(c, x, y, 1, h, rgb); fill(c, x + w - 1, y, 1, h, rgb);
}
static void draw_glyph(const canvas *c, int x, int y, int scale, uint32_t rgb, int g) {
    const unsigned char *rows = g_glyph[g & 127];
    for (int r = 0; r < 9; r++)
        for (int b = 0; b < 5; b++)
            if (rows[r] & (0x10 >> b)) fill(c, x + b * scale, y + r * scale, scale, scale, rgb);
}
/* draws up to max_cols characters (<= 0: all) of one line; returns the columns drawn */
static int text(const canvas *c, int x, int y, int scale, uint32_t rgb, const char *s, int max_cols) {
    int n = 0, g;
    font_init();
    while ((max_cols <= 0 || n < max_cols) && (g = next_glyph(&s)) != 0) {
        if (g == '\n') break;
        if (g >= 32 || (g >= 1 && g <= 5)) draw_glyph(c, x + n * DPK_CELL_W * scale, y, scale, rgb, g);
        n++;
    }
    return n;
}
int dpk_text(uint32_t *px, int w, int h, int pitch, int x, int y, int scale, uint32_t rgb, const char *utf8, int max_cols) {
    canvas c = {px, w, h, pitch};
    return text(&c, x, y, scale, rgb, utf8, max_cols);
}
/* a path that may not fit: its end is kept ("...rest") */
static void text_tail(const canvas *c, int x, int y, uint32_t rgb, const char *s, int cols) {
    int n = glyph_count(s, s + strlen(s));
    if (cols <= 3) return;
    if (n <= cols) { text(c, x, y, 1, rgb, s, cols); return; }
    for (int skip = n - (cols - 3); skip > 0; skip--) next_glyph(&s);
    text(c, x, y, 1, rgb, "...", 3);
    text(c, x + 3 * DPK_CELL_W, y, 1, rgb, s, cols - 3);
}
/* the face buttons as a diamond, the one meant filled: 0 right (A), 1 bottom (B), 2 top (X), 3 left (Y) - Nintendo letters */
static void face_icon(const canvas *c, int x, int y, int which, uint32_t on, uint32_t off) {
    static const int pos[4][2] = {{8, 4}, {4, 8}, {4, 0}, {0, 4}};
    for (int i = 0; i < 4; i++) {
        if (i == which) fill(c, x + pos[i][0], y + pos[i][1], 3, 3, on);
        else frame_rect(c, x + pos[i][0], y + pos[i][1], 3, 3, off);
    }
}
/* a key or button name in a box; returns its width */
static int keycap(const canvas *c, int x, int y, const char *label, uint32_t fg, uint32_t box) {
    int n = glyph_count(label, label + strlen(label)), w = n * DPK_CELL_W + 5;
    frame_rect(c, x, y - 2, w, DPK_CELL_H + 3, box);
    text(c, x + 3, y, 1, fg, label, 0);
    return w;
}

/* colours: Dear ImGui's dark style, as the settings menu (menu.cpp: StyleColorsDark, title in orange) */
#define C_BG       0x0f0f0fu    /* WindowBg 0.06 */
#define C_TITLEBG  0x294a7au    /* TitleBgActive */
#define C_FRAME    0x1d2b3du    /* Button 0.26,0.59,0.98 at 0.40 over the background, darkened */
#define C_SEL      0x2c5d9cu    /* HeaderActive over the background */
#define C_NAV      0x4296fau    /* NavHighlight */
#define C_TEXT     0xffffffu
#define C_DIM      0x808080u    /* TextDisabled */
#define C_SEP      0x6e6e80u    /* Separator */
#define C_ORANGE   0xff8c33u    /* the menu's "SSBU native" */
#define C_GREEN    0x73d973u    /* the menu's [live] */
#define C_AMBER    0xffbf33u    /* the menu's [restart: changed] */
#define C_RED      0xff6666u

/* ================================================================================================ text layout */
/* splits `t` into lines of at most `cols` characters, at spaces where possible; '\n' ends a line. Returns the count. */
static int wrap(const char *t, int cols, const char **ls, int *ll, int max) {
    int n = 0;
    const char *p = t;
    if (cols < 1) cols = 1;
    while (*p && n < max) {
        const char *q = p, *brk = NULL;
        int c = 0;
        while (*q && *q != '\n' && c < cols) {
            if (*q == ' ') brk = q;
            next_glyph(&q);
            c++;
        }
        ls[n] = p;
        if (!*q || *q == '\n') { ll[n++] = (int)(q - p); p = *q ? q + 1 : q; continue; }
        if (*q == ' ') { ll[n++] = (int)(q - p); p = q + 1; }
        else if (brk && brk > p) { ll[n++] = (int)(brk - p); p = brk + 1; }
        else { ll[n++] = (int)(q - p); p = q; }
    }
    return n;
}
static void text_n(const canvas *c, int x, int y, uint32_t rgb, const char *s, int bytes) {
    char line[1024];
    if (bytes >= (int)sizeof line) bytes = (int)sizeof line - 1;
    memcpy(line, s, (size_t)bytes);
    line[bytes] = 0;
    text(c, x, y, 1, rgb, line, 0);
}

/* ================================================================================================ model helpers */
static void note(dpk_state *s, uint32_t rgb, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void note(dpk_state *s, uint32_t rgb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->note, sizeof s->note, fmt, ap);
    va_end(ap);
    s->note_rgb = rgb;
}
static int join2(char *out, size_t n, const char *a, const char *b) {
    int k = !strcmp(a, "/") ? snprintf(out, n, "/%s", b) : snprintf(out, n, "%s/%s", a, b);
    return k >= 0 && (size_t)k < n;
}
static void parent(const char *p, char *out, size_t n) {
    snprintf(out, n, "%s", p);
    char *sl = strrchr(out, '/');
    if (!sl) { snprintf(out, n, "/"); return; }
    if (sl == out) out[1] = 0; else *sl = 0;
}
static int exists_in(const char *dir, const char *rel, int want_dir) {     /* case-insensitively, as datapaths.c finds */
    char p[DP_PATH], r[DP_PATH];
    struct stat st;
    if (!join2(p, sizeof p, dir, rel) || !host_path_resolve(p, r, sizeof r) || stat(r, &st)) return 0;
    return want_dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode);
}
static int is_package(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".xci") || !strcasecmp(dot, ".nsp") || !strcasecmp(dot, ".nsz") || !strcasecmp(dot, ".xcz"));
}
static int folder_tag(const dpk_state *s, const char *dir) {
    if (s->target == DPK_FOR_FONTS) {
        char f[DP_PATH];
        if (dp_fonts_check(dir, NULL, 0)) return DPK_TAG_FONTS;
        return join2(f, sizeof f, dir, "sysdata/fonts") && dp_fonts_check(f, NULL, 0) ? DPK_TAG_FONTS : DPK_TAG_NONE;
    }
    if (exists_in(dir, "romfs/data.arc", 0)) return DPK_TAG_DUMP;
    if (exists_in(dir, "data.arc", 0)) return DPK_TAG_ROMFS;
    return DPK_TAG_NONE;
}
static int wanted_tag(const dpk_state *s, int tag) {
    return s->target == DPK_FOR_FONTS ? tag == DPK_TAG_FONTS : tag == DPK_TAG_DUMP || tag == DPK_TAG_ROMFS;
}
static const char *expect_or_null(const dpk_state *s) { return s->expect[0] ? s->expect : NULL; }
static void clamp_sel(int *sel, int *top, int n, int rows) {
    if (rows < 1) rows = 1;
    if (*sel >= n) *sel = n - 1;
    if (*sel < 0) *sel = 0;
    if (*top > *sel) *top = *sel;
    if (*sel >= *top + rows) *top = *sel - rows + 1;
    if (*top > n - rows) *top = n - rows;
    if (*top < 0) *top = 0;
}

const char *dpk_headline(const dp_result *r, int pick_mode) {
    switch (r->status) {
    case DP_OK:              return pick_mode ? "Game data found" : "Usable game data, but not saved";
    case DP_NOT_CONFIGURED:  return "Welcome! Show the game where your dump is";
    case DP_NOT_FOUND:       return "The game data folder is not there";
    case DP_PACKAGES_ONLY:   return "Game packages: extract them first";
    case DP_MISSING_ROMFS:   return "No game data (romfs/data.arc) in this folder";
    case DP_MISSING_NRR:     return "The RomFS is incomplete: .nrr is missing";
    case DP_UNKNOWN_VERSION: return "A game version this port does not handle";
    case DP_WRONG_VERSION:   return "Wrong game version for this build";
    case DP_MISSING_FONTS:   return "The system fonts are missing";
    }
    return "Game data";
}
static void build_menu(dpk_state *s) {
    int ok = s->now.status == DP_OK, n = 0;
    if (s->pick_mode && ok) s->menu[n++] = DPK_M_START;
    if (s->now.status == DP_MISSING_FONTS) s->menu[n++] = DPK_M_FONTS;
    s->menu[n++] = DPK_M_DATA;
    if (s->dialog_ok) s->menu[n++] = DPK_M_DATA_HERE;
    if (s->pick_mode && ok) s->menu[n++] = DPK_M_FONTS;
    if (!ok) s->menu[n++] = DPK_M_RETRY;
    if (s->pick_mode && !ok) s->menu[n++] = DPK_M_START;     /* --pick-data: a folder tried here failed; the data found at the start still work */
    s->menu[n++] = DPK_M_QUIT;
    s->menu_n = n;
    s->menu_sel = 0;
    s->msg_scroll = 0;
}
static const char *menu_label(const dpk_state *s, int m) {
    switch (m) {
    case DPK_M_START:     return s->now.status == DP_OK ? "Start the game with this data" : "Start the game with the data found at the start";
    case DPK_M_DATA:      return s->dialog_ok ? "Choose the game data folder (system file dialog)" : "Choose the game data folder";
    case DPK_M_DATA_HERE: return "Choose the game data folder in this window";
    case DPK_M_FONTS:     return s->dialog_ok ? "Choose the folder with the system fonts (system file dialog)" : "Choose the folder with the system fonts";
    case DPK_M_RETRY:     return "Check again (after connecting the SD card or drive)";
    case DPK_M_QUIT:      return "Quit";
    }
    return "?";
}

/* ================================================================================================ model */
void dpk_init(dpk_state *s, const dp_result *was, int pick_mode, int dialog_ok) {
    memset(s, 0, sizeof *s);
    font_init();
    s->was = *was;
    s->now = *was;
    if ((!strcmp(was->expected, "13.0.4") || !strcmp(was->expected, "13.0.5"))) snprintf(s->expect, sizeof s->expect, "%s", was->expected);
    s->pick_mode = pick_mode && was->status == DP_OK;
    s->dialog_ok = dialog_ok;
    s->screen = DPK_SCR_STATUS;
    s->list_rows = 10;
    build_menu(s);
}
static void free_entries(dpk_state *s) {
    for (int i = 0; i < s->n; i++) free(s->ent[i].name);
    s->n = 0;
}
void dpk_free(dpk_state *s) {
    free_entries(s);
    free(s->ent);
    s->ent = NULL;
    s->cap = 0;
}

/* data: check `dir` as boot.c will after dp_picked; usable (or only the fonts missing): saved. Usable: done. */
static void check_data(dpk_state *s, const char *dir) {
    dp_result r;
    dp_validate_picked(dir, expect_or_null(s), &s->was, &r);
    if (r.status == DP_MISSING_FONTS && r.fonts_source == DP_SRC_CONFIG && r.romfs[0]) {
        /* fonts saved earlier that are gone, while this dump has its own: forget the old fonts choice */
        char root[DP_PATH], f[DP_PATH];
        parent(r.romfs, root, sizeof root);
        if (join2(f, sizeof f, root, "sysdata/fonts") && dp_fonts_check(f, NULL, 0) && dp_config_set("fonts", NULL) == 0)
            dp_validate_picked(dir, expect_or_null(s), &s->was, &r);
    }
    snprintf(s->data_dir, sizeof s->data_dir, "%s", dir ? dir : "");
    s->now = r;
    s->screen = DPK_SCR_STATUS;
    build_menu(s);
    s->note[0] = 0;
    if (r.status == DP_OK || r.status == DP_MISSING_FONTS) {
        int rc = dp_save_choice(dir);
        if (rc) {
            char cfg[DP_PATH];
            if (!dp_config_path(cfg, sizeof cfg)) snprintf(cfg, sizeof cfg, "the config file");
            if (s->pick_mode) {                 /* "Start the game with this data" must mean the data boot.c will start with */
                s->now = s->was;
                s->data_dir[0] = 0;
                build_menu(s);
            }
            note(s, C_RED, "The choice could not be saved in %s: %s.", cfg, strerror(-rc));
            return;
        }
        if (r.status == DP_OK) { s->result = DPK_PICKED; return; }
        note(s, C_AMBER, "The game data folder is saved. Now choose the folder with the 7 system fonts.");
    }
}
static void choose_fonts(dpk_state *s, const char *dir) {
    static const char *const sub[] = {"", "sysdata/fonts", "fonts"};
    char f[DP_PATH], abs[PATH_MAX], miss[128] = "", m[128];
    int found = 0;
    for (int i = 0; i < 3 && !found; i++) {
        if (!sub[i][0]) snprintf(f, sizeof f, "%s", dir); else if (!join2(f, sizeof f, dir, sub[i])) continue;
        found = dp_fonts_check(f, m, sizeof m);
        if (!i) snprintf(miss, sizeof miss, "%s", m);
    }
    if (!found) {
        note(s, C_RED, "No system fonts in %s (%s is missing). Choose the folder with all 7 nintendo_*_003.ttf files.", dir, miss);
        if (s->screen == DPK_SCR_DIALOG) s->screen = DPK_SCR_STATUS;
        return;
    }
    if (realpath(f, abs)) snprintf(f, sizeof f, "%s", abs);
    int rc = dp_config_set("fonts", f);
    if (rc) { note(s, C_RED, "The fonts folder could not be saved: %s.", strerror(-rc)); return; }
    check_data(s, s->data_dir[0] ? s->data_dir : s->was.dir);
    if (!s->result && s->now.status != DP_MISSING_FONTS && !s->note[0])
        note(s, C_AMBER, "The fonts folder %s is saved; the game data still need attention.", f);
}
void dpk_choose(dpk_state *s, const char *dir) {
    if (s->target == DPK_FOR_FONTS) choose_fonts(s, dir);
    else check_data(s, dir);
}

/* ---- the folder list */
static int cmp_entry(const void *a, const void *b) {
    const dpk_entry *x = a, *y = b;
    if (x->kind != y->kind) return x->kind - y->kind;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}
static int add_entry(dpk_state *s, const char *name, int kind, int tag) {
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 64;
        dpk_entry *e = realloc(s->ent, (size_t)cap * sizeof *e);
        if (!e) return 0;
        s->ent = e; s->cap = cap;
    }
    char *copy = strdup(name);
    if (!copy) return 0;
    s->ent[s->n].name = copy;
    s->ent[s->n].kind = (unsigned char)kind;
    s->ent[s->n].tag = (unsigned char)tag;
    s->n++;
    return 1;
}
static int first_sel(const dpk_state *s) {
    if (wanted_tag(s, s->cwd_tag)) return 0;                     /* this folder is it: "Use this folder" */
    for (int i = 0; i < s->n; i++) if (s->ent[i].kind == DPK_E_DIR) return i;
    for (int i = 0; i < s->n; i++) if (s->ent[i].kind == DPK_E_UP) return i;
    return 0;
}
int dpk_browse(dpk_state *s, const char *dir) {
    char d[DP_PATH], p[DP_PATH];
    struct stat st;
    host_path_resolve(dir && dir[0] ? dir : "/", d, sizeof d);
    if (realpath(d, p)) snprintf(d, sizeof d, "%s", p);
    DIR *h = opendir(d);
    if (!h) { note(s, C_RED, "%s cannot be opened: %s.", d, strerror(errno)); return 0; }
    free_entries(s);
    snprintf(s->cwd, sizeof s->cwd, "%s", d);
    s->truncated = 0;
    s->cwd_tag = folder_tag(s, d);
    add_entry(s, "Use this folder", DPK_E_USE, s->cwd_tag);
    add_entry(s, "Places: home, SD card, USB drives ...", DPK_E_PLACES, 0);
    if (strcmp(d, "/")) add_entry(s, "..", DPK_E_UP, 0);
    int fixed = s->n, tagged = 0, dirs = 0, files = 0;
    for (struct dirent *e; (e = readdir(h));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (e->d_name[0] == '.' && !s->show_hidden) continue;
        /* the cap counts folders and files apart: a folder full of files must not hide the dump folder beside them
         * (readdir's order is not the sorted one) */
        if (files >= DPK_MAX_ENTRIES && (e->d_type == DT_REG || dirs >= DPK_MAX_ENTRIES)) { s->truncated = 1; if (dirs >= DPK_MAX_ENTRIES) break; continue; }
        if (!join2(p, sizeof p, d, e->d_name) || stat(p, &st)) continue;    /* dangling links are left out */
        int isdir = S_ISDIR(st.st_mode);
        if (isdir ? dirs >= DPK_MAX_ENTRIES : files >= DPK_MAX_ENTRIES) { s->truncated = 1; continue; }
        if (isdir) dirs++; else files++;
        int tag = isdir ? (tagged++ < 1024 ? folder_tag(s, p) : 0) : is_package(e->d_name) ? DPK_TAG_PACKAGE : 0;
        add_entry(s, e->d_name, isdir ? DPK_E_DIR : DPK_E_FILE, tag);
    }
    closedir(h);
    qsort(s->ent + fixed, (size_t)(s->n - fixed), sizeof *s->ent, cmp_entry);
    s->top = 0;
    s->sel = first_sel(s);
    s->typed[0] = 0;
    s->note[0] = 0;
    return 1;
}
static void select_name(dpk_state *s, const char *name) {
    for (int i = 0; i < s->n; i++)
        if (s->ent[i].kind >= DPK_E_DIR && !strcmp(s->ent[i].name, name)) { s->sel = i; clamp_sel(&s->sel, &s->top, s->n, s->list_rows); return; }
}
static void go_up(dpk_state *s) {
    char up[DP_PATH], child[DP_PATH];
    const char *sl = strrchr(s->cwd, '/');
    if (!strcmp(s->cwd, "/")) { s->screen = DPK_SCR_STATUS; return; }
    snprintf(child, sizeof child, "%s", sl ? sl + 1 : "");
    parent(s->cwd, up, sizeof up);
    if (dpk_browse(s, up)) select_name(s, child);
}
static void home_dir(char *out, size_t n) {
    const char *h = getenv("HOME");
    struct passwd *pw;
    if ((!h || !h[0]) && (pw = getpwuid(getuid()))) h = pw->pw_dir;
    snprintf(out, n, "%s", h && h[0] ? h : "/");
}
/* where the browser (and the system dialog) start: beside the folder checked last, else home */
static void start_folder(dpk_state *s, char *out, size_t n, char *select, size_t sn) {
    const char *cand[4];
    int k = 0;
    select[0] = 0;
    if (s->target == DPK_FOR_FONTS && s->now.status == DP_MISSING_FONTS && s->now.missing[0]) cand[k++] = s->now.missing;
    cand[k++] = s->data_dir;
    cand[k++] = s->now.dir;
    cand[k++] = s->was.dir;
    for (int i = 0; i < k; i++) {
        char p[DP_PATH], up[DP_PATH];
        if (!cand[i][0]) continue;
        host_path_resolve(cand[i], p, sizeof p);
        for (int depth = 0; depth < 4 && p[0] == '/'; depth++) {
            if (dp_is_dir(p)) {
                if (wanted_tag(s, folder_tag(s, p)) && strcmp(p, "/")) {           /* it is the folder: show it selected in its parent */
                    const char *sl = strrchr(p, '/');
                    snprintf(select, sn, "%s", sl + 1);
                    parent(p, out, n);
                } else snprintf(out, n, "%s", p);
                return;
            }
            parent(p, up, sizeof up);
            if (!strcmp(up, p)) break;
            snprintf(p, sizeof p, "%s", up);
        }
    }
    home_dir(out, n);
}
void dpk_open_browser(dpk_state *s, dpk_target t) {
    char dir[DP_PATH], sel[DP_PATH];
    s->target = t;
    start_folder(s, dir, sizeof dir, sel, sizeof sel);
    if (!dpk_browse(s, dir)) {
        char home[DP_PATH];
        home_dir(home, sizeof home);
        if (!dpk_browse(s, home) && !dpk_browse(s, "/")) return;
    } else if (sel[0]) select_name(s, sel);
    s->screen = DPK_SCR_BROWSE;
}

/* ---- places */
static void add_place(dpk_state *s, const char *label, const char *path) {
    if (s->nplaces >= DPK_MAX_PLACES) return;
    for (int i = 0; i < s->nplaces; i++) if (path[0] && !strcmp(s->place[i].path, path)) return;
    snprintf(s->place[s->nplaces].label, sizeof s->place[0].label, "%s", label);
    snprintf(s->place[s->nplaces].path, sizeof s->place[0].path, "%s", path);
    s->nplaces++;
}
static void add_mounts(dpk_state *s, const char *base, const char *user, int depth) {
    DIR *d = opendir(base);
    char p[DP_PATH], label[96];
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.' || !join2(p, sizeof p, base, e->d_name) || !dp_is_dir(p)) continue;
        if (depth == 0 && user && !strcmp(e->d_name, user)) { add_mounts(s, p, NULL, 1); continue; }   /* /run/media/<user>/<label> */
        snprintf(label, sizeof label, "Drive: %s", e->d_name);
        add_place(s, label, p);
    }
    closedir(d);
}
static void build_places(dpk_state *s) {
    char p[DP_PATH], q[DP_PATH];
    const char *media = getenv("SSBU_PICKER_MEDIA");             /* tests (and odd systems): the folders whose sub-folders are drives */
    struct passwd *pw = getpwuid(getuid());
    const char *user = getenv("USER");
    if ((!user || !user[0]) && pw) user = pw->pw_name;
    s->nplaces = 0;
    home_dir(p, sizeof p);
    add_place(s, "Home", p);
    if (media) {
        char list[DP_PATH], *save = NULL;
        snprintf(list, sizeof list, "%s", media);
        for (char *t = strtok_r(list, ":", &save); t; t = strtok_r(NULL, ":", &save)) add_mounts(s, t, user, 0);
    } else {
        add_mounts(s, "/run/media", user, 0);                    /* SteamOS / KDE: SD card and USB drives */
        add_mounts(s, "/media", user, 0);
        add_mounts(s, "/mnt", NULL, 1);
    }
    if (dp_config_get("data", q, sizeof q) && host_path_resolve(q, p, sizeof p) && dp_is_dir(p)) add_place(s, "Saved game data folder", p);
    if (s->was.dir[0] && host_path_resolve(s->was.dir, p, sizeof p) && dp_is_dir(p)) add_place(s, "Folder checked at the start", p);
    if (dp_exe_path("..", q, sizeof q) && realpath(q, p)) add_place(s, "Folder of this game build", p);
    add_place(s, "Whole system (/)", "/");
    add_place(s, "Back to the start screen", "");
    s->place_sel = 0;
    s->place_top = 0;
}

/* ---- actions */
static void activate_menu(dpk_state *s) {
    switch (s->menu[s->menu_sel]) {
    case DPK_M_START: s->result = DPK_KEEP; break;
    case DPK_M_DATA: case DPK_M_FONTS:
        s->target = s->menu[s->menu_sel] == DPK_M_FONTS ? DPK_FOR_FONTS : DPK_FOR_DATA;
        if (s->dialog_ok) { s->dialog_req = 1; s->screen = DPK_SCR_DIALOG; s->note[0] = 0; }
        else dpk_open_browser(s, s->target);
        break;
    case DPK_M_DATA_HERE: dpk_open_browser(s, DPK_FOR_DATA); break;
    case DPK_M_RETRY: {
        char d[DP_PATH];
        snprintf(d, sizeof d, "%s", s->data_dir[0] ? s->data_dir : s->was.dir);
        if (!d[0]) { note(s, C_AMBER, "No folder chosen yet: choose the game data folder."); break; }
        check_data(s, d);
        if (!s->result && !s->note[0]) note(s, C_AMBER, "Checked again: %s.", dp_status_name(s->now.status));
        break;
    }
    case DPK_M_QUIT: s->result = DPK_QUITTED; break;
    }
}
static void activate_entry(dpk_state *s) {
    char p[DP_PATH];
    if (s->sel < 0 || s->sel >= s->n) return;
    const dpk_entry *e = &s->ent[s->sel];
    switch (e->kind) {
    case DPK_E_USE: dpk_choose(s, s->cwd); break;
    case DPK_E_PLACES: build_places(s); s->screen = DPK_SCR_PLACES; break;
    case DPK_E_UP: go_up(s); break;
    case DPK_E_DIR: if (join2(p, sizeof p, s->cwd, e->name)) dpk_browse(s, p); break;
    case DPK_E_FILE:
        if (e->tag == DPK_TAG_PACKAGE)
            note(s, C_AMBER, "%s is a game package. Extract it first on a PC or here: python3 tools/extract/dump_folder.py "
                 "<base> <update> <dump folder>, then choose the dump folder.", e->name);
        else note(s, C_AMBER, "Choose a folder, not a file.");
        break;
    }
}
static void move(int *sel, int *top, int n, int rows, int by) {
    *sel += by;
    clamp_sel(sel, top, n, rows);
}
void dpk_do(dpk_state *s, dpk_action a) {
    int page = s->list_rows > 1 ? s->list_rows - 1 : 1;
    if (s->result) return;
    if (a == DPK_QUIT) { s->result = DPK_QUITTED; return; }
    switch (s->screen) {
    case DPK_SCR_STATUS: {
        int t = 0;
        switch (a) {
        case DPK_UP: if (s->menu_sel > 0) s->menu_sel--; break;
        case DPK_DOWN: if (s->menu_sel < s->menu_n - 1) s->menu_sel++; break;
        case DPK_FIRST: s->menu_sel = 0; break;
        case DPK_LAST: s->menu_sel = s->menu_n - 1; break;
        case DPK_PAGE_UP: s->msg_scroll -= 3; if (s->msg_scroll < 0) s->msg_scroll = 0; break;
        case DPK_PAGE_DOWN: s->msg_scroll += 3; if (s->msg_scroll > s->msg_lines - 1) s->msg_scroll = s->msg_lines > 0 ? s->msg_lines - 1 : 0; break;
        case DPK_CONFIRM: activate_menu(s); break;
        default: (void)t; break;
        }
        break;
    }
    case DPK_SCR_BROWSE:
        switch (a) {
        case DPK_UP: move(&s->sel, &s->top, s->n, s->list_rows, -1); break;
        case DPK_DOWN: move(&s->sel, &s->top, s->n, s->list_rows, 1); break;
        case DPK_PAGE_UP: move(&s->sel, &s->top, s->n, s->list_rows, -page); break;
        case DPK_PAGE_DOWN: move(&s->sel, &s->top, s->n, s->list_rows, page); break;
        case DPK_FIRST: move(&s->sel, &s->top, s->n, s->list_rows, -s->n); break;
        case DPK_LAST: move(&s->sel, &s->top, s->n, s->list_rows, s->n); break;
        case DPK_CONFIRM: activate_entry(s); break;
        case DPK_RIGHT: if (s->sel >= 0 && s->sel < s->n && (s->ent[s->sel].kind == DPK_E_DIR || s->ent[s->sel].kind == DPK_E_UP)) activate_entry(s); break;
        case DPK_BACK: case DPK_LEFT: go_up(s); break;
        case DPK_USE_HERE: dpk_choose(s, s->cwd); break;
        case DPK_PLACES: build_places(s); s->screen = DPK_SCR_PLACES; break;
        case DPK_HIDDEN: {
            char d[DP_PATH], name[DP_PATH] = "";
            if (s->sel >= 0 && s->sel < s->n && s->ent[s->sel].kind >= DPK_E_DIR) snprintf(name, sizeof name, "%s", s->ent[s->sel].name);
            snprintf(d, sizeof d, "%s", s->cwd);
            s->show_hidden = !s->show_hidden;
            if (dpk_browse(s, d) && name[0]) select_name(s, name);
            note(s, C_DIM, "Hidden files and folders (names starting with a dot) are %s.", s->show_hidden ? "shown" : "hidden");
            break;
        }
        default: break;
        }
        break;
    case DPK_SCR_PLACES:
        switch (a) {
        case DPK_UP: move(&s->place_sel, &s->place_top, s->nplaces, s->list_rows, -1); break;
        case DPK_DOWN: move(&s->place_sel, &s->place_top, s->nplaces, s->list_rows, 1); break;
        case DPK_PAGE_UP: case DPK_FIRST: move(&s->place_sel, &s->place_top, s->nplaces, s->list_rows, -s->nplaces); break;
        case DPK_PAGE_DOWN: case DPK_LAST: move(&s->place_sel, &s->place_top, s->nplaces, s->list_rows, s->nplaces); break;
        case DPK_CONFIRM: case DPK_RIGHT: {
            const dpk_place *p = &s->place[s->place_sel];
            if (!p->path[0]) { s->screen = DPK_SCR_STATUS; break; }
            if (dpk_browse(s, p->path)) s->screen = DPK_SCR_BROWSE;
            break;
        }
        case DPK_BACK: case DPK_LEFT: case DPK_PLACES: s->screen = DPK_SCR_BROWSE; break;
        default: break;
        }
        break;
    case DPK_SCR_DIALOG:
        if (a == DPK_BACK) { s->dialog_id++; s->dialog_req = 0; dpk_open_browser(s, s->target); }   /* a late answer is ignored */
        break;
    }
}
static int repeats(dpk_action a) { return a == DPK_UP || a == DPK_DOWN || a == DPK_LEFT || a == DPK_RIGHT || a == DPK_PAGE_UP || a == DPK_PAGE_DOWN; }
void dpk_press(dpk_state *s, dpk_action a, uint64_t now_ms) {
    s->device = DPK_DEV_PAD;
    dpk_do(s, a);
    if (repeats(a)) { s->held = a; s->next_repeat = now_ms + 400; }
}
void dpk_release(dpk_state *s, dpk_action a) { if (s->held == a) s->held = DPK_NONE; }
void dpk_tick(dpk_state *s, uint64_t now_ms) {
    if (s->held != DPK_NONE && now_ms >= s->next_repeat) { dpk_do(s, s->held); s->next_repeat = now_ms + 70; }
}
void dpk_pointer(dpk_state *s, int x, int y, int click) {
    const dpk_list_area *a = &s->area;
    if (click) s->device = DPK_DEV_KEYS;
    if (click == 2) { dpk_do(s, DPK_BACK); return; }
    if (s->screen == DPK_SCR_DIALOG || a->row_h <= 0 || x < a->x || x >= a->x + a->w || y < a->y || y >= a->y + a->h) return;
    /* the area is the last frame's: events of one batch may come after a key or click changed the screen or the list */
    int n = s->screen == DPK_SCR_STATUS ? s->menu_n : s->screen == DPK_SCR_BROWSE ? s->n : s->nplaces;
    int row = a->first + (y - a->y) / a->row_h;
    if (a->screen != (int)s->screen || row < a->first || row >= a->count || row >= n) return;
    if (s->screen == DPK_SCR_STATUS) s->menu_sel = row;
    else if (s->screen == DPK_SCR_BROWSE) s->sel = row;
    else s->place_sel = row;
    if (click == 1) dpk_do(s, DPK_CONFIRM);
}
void dpk_wheel(dpk_state *s, int steps) {
    if (s->screen == DPK_SCR_STATUS) { dpk_do(s, steps > 0 ? DPK_PAGE_UP : DPK_PAGE_DOWN); return; }
    int *sel = s->screen == DPK_SCR_PLACES ? &s->place_sel : &s->sel, *top = s->screen == DPK_SCR_PLACES ? &s->place_top : &s->top;
    int n = s->screen == DPK_SCR_PLACES ? s->nplaces : s->n;
    if (s->screen == DPK_SCR_DIALOG) return;
    move(sel, top, n, s->list_rows, -3 * steps);
}
void dpk_typed(dpk_state *s, const char *utf8, uint64_t now_ms) {
    size_t l;
    if (s->screen != DPK_SCR_BROWSE || !utf8 || !utf8[0]) return;
    s->device = DPK_DEV_KEYS;
    if (now_ms - s->typed_at > 1000) s->typed[0] = 0;
    s->typed_at = now_ms;
    l = strlen(s->typed);
    snprintf(s->typed + l, sizeof s->typed - l, "%s", utf8);
    l = strlen(s->typed);
    for (int i = 0; i < s->n; i++)
        if (s->ent[i].kind >= DPK_E_DIR && !strncasecmp(s->ent[i].name, s->typed, l)) { s->sel = i; clamp_sel(&s->sel, &s->top, s->n, s->list_rows); return; }
}
int dpk_dialog_take(dpk_state *s, dpk_target *target, const char **start, int *id) {
    static char dir[DP_PATH];
    char sel[DP_PATH];
    if (!s->dialog_req) return 0;
    s->dialog_req = 0;
    start_folder(s, dir, sizeof dir, sel, sizeof sel);
    if (sel[0]) { size_t l = strlen(dir); snprintf(dir + l, sizeof dir - l, "%s%s", strcmp(dir, "/") ? "/" : "", sel); }
    *target = s->target;
    *start = dir;
    *id = ++s->dialog_id;
    return 1;
}
void dpk_dialog_done(dpk_state *s, int id, const char *path, int failed) {
    if (id != s->dialog_id || s->screen != DPK_SCR_DIALOG || s->result) return;
    if (path && path[0]) {
        dpk_choose(s, path);
        if (s->screen == DPK_SCR_DIALOG) s->screen = DPK_SCR_STATUS;
    } else if (failed) {
        s->dialog_ok = 0;
        build_menu(s);
        dpk_open_browser(s, s->target);
        note(s, C_AMBER, "The system file dialog could not be opened: choose the folder here.");
    } else {
        s->screen = DPK_SCR_STATUS;
        s->note[0] = 0;
    }
}

/* ================================================================================================ view */
typedef struct { int icon; const char *key, *what; } hint;      /* icon: 0-3 a face button, -1 the key name in a box */
static void hints(dpk_state *s, const canvas *c, int y, const hint *h, int n) {
    int x = 12;
    for (int i = 0; i < n && x < c->w; i++) {
        if (s->device == DPK_DEV_PAD && h[i].icon >= 0) { face_icon(c, x, y - 1, h[i].icon, C_TEXT, C_DIM); x += 15; }
        else x += keycap(c, x, y, h[i].key, C_TEXT, C_SEP) + 4;
        x += text(c, x, y, 1, C_DIM, h[i].what, 0) * DPK_CELL_W + 14;
    }
}
static void scrollbar(const canvas *c, int x, int y, int h, int first, int rows, int n) {
    if (n <= rows || h < 8) return;
    int th = h * rows / n, ty = y + (h - th) * first / (n - rows);
    if (th < 6) th = 6;
    fill(c, x, y, 3, h, C_FRAME);
    fill(c, x, ty, 3, th, C_SEP);
}
static void row_bg(const canvas *c, int x, int y, int w, int h, int selected) {
    if (selected) { fill(c, x, y, w, h, C_SEL); frame_rect(c, x, y, w, h, C_NAV); }
}
static const char *tag_text(int tag, uint32_t *rgb) {
    *rgb = C_GREEN;
    switch (tag) {
    case DPK_TAG_DUMP: return "game data";
    case DPK_TAG_ROMFS: return "RomFS";
    case DPK_TAG_FONTS: return "system fonts";
    case DPK_TAG_PACKAGE: *rgb = C_AMBER; return "package: extract first";
    }
    return NULL;
}
static int note_line(dpk_state *s, const canvas *c, int y_bottom, int cols) {     /* the note above the hints; returns its top */
    const char *ls[3]; int ll[3];
    if (!s->note[0]) return y_bottom;
    int n = wrap(s->note, cols, ls, ll, 3), y = y_bottom - n * 12;
    for (int i = 0; i < n; i++) text_n(c, 12, y + i * 12, s->note_rgb ? s->note_rgb : C_AMBER, ls[i], ll[i]);
    return y - 4;
}
static void header(dpk_state *s, const canvas *c, const char *what) {
    char right[64];
    fill(c, 0, 0, c->w, 30, C_TITLEBG);
    int x = 10 + text(c, 10, 5, 2, C_ORANGE, "SSBU native", 0) * DPK_CELL_W * 2;
    text(c, x + 10, 11, 1, C_TEXT, what, (c->w - x - 20) / DPK_CELL_W);
    snprintf(right, sizeof right, "this build: %s", s->expect[0] ? s->expect : "13.0.4 / 13.0.5");
    int rw = (int)strlen(right) * DPK_CELL_W;
    if (c->w - rw - 10 > x + 10 + (int)strlen(what) * DPK_CELL_W + 20) text(c, c->w - rw - 10, 11, 1, C_DIM, right, 0);
}
static void render_status(dpk_state *s, const canvas *c) {
    int cols = (c->w - 24) / DPK_CELL_W, y = 40, foot = c->h - 20;
    uint32_t hc = s->now.status == DP_OK ? C_GREEN : s->now.status == DP_MISSING_FONTS || s->now.status == DP_NOT_CONFIGURED ? C_AMBER : C_RED;
    const char *head = dpk_headline(&s->now, s->pick_mode);
    int big = glyph_count(head, head + strlen(head)) * DPK_CELL_W * 2 <= c->w - 24 ? 2 : 1;
    text(c, 12, y, big, hc, head, cols);
    y += DPK_CELL_H * big + 8;
    /* the bottom part first: menu, folder lines, note */
    int mh = s->menu_n * 17, info = 3 * 12;
    int ntop = note_line(s, c, foot - 4, cols);
    int menu_y = ntop - mh - 4, info_y = menu_y - info - 8;
    /* the message: what is wrong and what to do (datapaths.c) */
    static const char *ls[256]; static int ll[256];
    const char *msg = s->now.status != DP_OK ? s->now.message
        : s->pick_mode ? "The game will start with the data below. Choose another folder to change it; the choice is saved for the next starts (start with --pick-data or SSBU_PICK_DATA=1 to come back here)."
        : "This folder is usable, but the choice could not be saved (below), and the game starts only with a saved choice. Make the "
          "config folder writable and choose the folder again, or start with --data <folder> / SSBU_DATA=<folder>.";
    int nl = wrap(msg, cols, ls, ll, 256), rows = (info_y - 6 - y) / 12;
    s->msg_lines = nl;
    if (rows < 1) rows = 1;
    if (s->msg_scroll > nl - rows) s->msg_scroll = nl - rows > 0 ? nl - rows : 0;
    for (int i = 0; i < rows && s->msg_scroll + i < nl; i++) text_n(c, 12, y + i * 12, C_TEXT, ls[s->msg_scroll + i], ll[s->msg_scroll + i]);
    if (nl > rows) {
        scrollbar(c, c->w - 8, y, rows * 12, s->msg_scroll, rows, nl);
    }
    /* the folders */
    char cfg[DP_PATH];
    fill(c, 12, info_y - 4, c->w - 24, 1, C_SEP);
    text(c, 12, info_y, 1, C_DIM, "Data", 0);
    text_tail(c, 12 + 8 * DPK_CELL_W, info_y, C_TEXT, s->now.dir[0] ? s->now.dir : "-", cols - 8);
    text(c, 12, info_y + 12, 1, C_DIM, "Fonts", 0);
    text_tail(c, 12 + 8 * DPK_CELL_W, info_y + 12, C_TEXT, s->now.fonts[0] ? s->now.fonts : s->now.status == DP_MISSING_FONTS && s->now.missing[0] ? s->now.missing : "-", cols - 8);
    text(c, 12, info_y + 24, 1, C_DIM, "Saved in", 0);
    text_tail(c, 12 + 9 * DPK_CELL_W, info_y + 24, C_DIM, dp_config_path(cfg, sizeof cfg) ? cfg : "-", cols - 9);
    /* the menu */
    s->area = (dpk_list_area){12, menu_y, c->w - 24, s->menu_n * 17, 17, 0, s->menu_n, DPK_SCR_STATUS};
    for (int i = 0; i < s->menu_n; i++) {
        int ry = menu_y + i * 17, sel = i == s->menu_sel;
        if (sel) row_bg(c, 12, ry, c->w - 24, 15, 1); else fill(c, 12, ry, c->w - 24, 15, C_FRAME);
        text(c, 20, ry + 3, 1, s->menu[i] == DPK_M_QUIT && !sel ? C_DIM : C_TEXT, menu_label(s, s->menu[i]), cols - 2);
    }
    hint h[] = {{0, "Enter", "Select"}, {-1, "PgUp/PgDn", "Scroll the message"}};
    if (s->device == DPK_DEV_PAD) h[1].key = "L/R";
    fill(c, 0, foot, c->w, 1, C_SEP);
    hints(s, c, foot + 6, h, nl > rows ? 2 : 1);
}
static void render_browse(dpk_state *s, const canvas *c) {
    int cols = (c->w - 24) / DPK_CELL_W, y = 38, foot = c->h - 20;
    const char *what = s->target == DPK_FOR_FONTS ? "Choose the folder with the system fonts" : "Choose the game data folder";
    text(c, 12, y, 1, C_TEXT, what, cols);
    y += 14;
    text(c, 12, y, 1, C_DIM, "In", 0);
    text_tail(c, 12 + 3 * DPK_CELL_W, y, C_TEXT, s->cwd, cols - 3);
    y += 13;
    text(c, 12, y, 1, C_DIM, s->target == DPK_FOR_FONTS
         ? "The folder with the 7 nintendo_*_003.ttf files (a dump's sysdata/fonts): marked \"system fonts\"."
         : "The folder dump_folder.py made (exefs, romfs, sysdata), or its romfs: marked \"game data\".", cols);
    y += 16;
    int ntop = note_line(s, c, foot - 4, cols);
    int row_h = 13, rows = (ntop - 2 - y) / row_h;
    if (rows < 1) rows = 1;
    s->list_rows = rows;
    clamp_sel(&s->sel, &s->top, s->n, rows);
    s->area = (dpk_list_area){12, y, c->w - 30, rows * row_h, row_h, s->top, s->n, DPK_SCR_BROWSE};
    fill(c, 12, y - 2, c->w - 24, 1, C_SEP);
    for (int i = 0; i < rows && s->top + i < s->n; i++) {
        const dpk_entry *e = &s->ent[s->top + i];
        int ry = y + i * row_h, sel = s->top + i == s->sel;
        uint32_t trgb, fg = C_TEXT;
        const char *tag = tag_text(e->tag, &trgb);
        char icon[2] = {0, 0};
        row_bg(c, 12, ry, c->w - 30, row_h - 1, sel);
        switch (e->kind) {
        case DPK_E_USE: icon[0] = 3; fg = wanted_tag(s, e->tag) ? C_GREEN : C_TEXT; tag = wanted_tag(s, e->tag) ? (s->target == DPK_FOR_FONTS ? "the fonts are here" : "the game data are here") : NULL; break;
        case DPK_E_PLACES: icon[0] = 5; break;
        case DPK_E_UP: icon[0] = 4; break;
        case DPK_E_DIR: icon[0] = 1; break;
        default: icon[0] = 2; fg = e->tag == DPK_TAG_PACKAGE ? C_TEXT : C_DIM; break;
        }
        text(c, 16, ry + 2, 1, e->kind == DPK_E_DIR ? C_ORANGE : C_DIM, icon, 1);
        int tagw = tag ? (int)strlen(tag) + 2 : 0, room = cols - 4 - tagw;
        char name[300];
        snprintf(name, sizeof name, "%s%s", e->kind == DPK_E_UP ? ".. (one folder up)" : e->name, e->kind == DPK_E_DIR ? "/" : "");
        text(c, 16 + 2 * DPK_CELL_W, ry + 2, 1, fg, name, room);
        if (tag) text(c, c->w - 24 - tagw * DPK_CELL_W + 2 * DPK_CELL_W, ry + 2, 1, e->kind == DPK_E_USE ? C_GREEN : trgb, tag, 0);
    }
    scrollbar(c, c->w - 14, y, rows * row_h, s->top, rows, s->n);
    if (s->truncated) text(c, 12, ntop - 12, 1, C_AMBER, "(a very full folder: at most 4096 folders and 4096 files are listed)", cols);
    hint h[] = {{0, "Enter", "Open"}, {1, "Esc", "Up"}, {2, "Ctrl+Enter", "Use this folder"}, {3, "Tab", "Places"}, {-1, "PgUp/PgDn", "Page"}, {-1, "Ctrl+H", "Hidden"}};
    if (s->device == DPK_DEV_PAD) { h[4].key = "L/R"; h[5].key = "-"; }
    fill(c, 0, foot, c->w, 1, C_SEP);
    hints(s, c, foot + 6, h, 6);
}
static void render_places(dpk_state *s, const canvas *c) {
    int cols = (c->w - 24) / DPK_CELL_W, y = 38, foot = c->h - 20, row_h = 13;
    text(c, 12, y, 1, C_TEXT, "Places", cols);
    y += 18;
    int ntop = note_line(s, c, foot - 4, cols), rows = (ntop - 2 - y) / row_h;
    if (rows < 1) rows = 1;
    s->list_rows = rows;
    clamp_sel(&s->place_sel, &s->place_top, s->nplaces, rows);
    s->area = (dpk_list_area){12, y, c->w - 30, rows * row_h, row_h, s->place_top, s->nplaces, DPK_SCR_PLACES};
    fill(c, 12, y - 2, c->w - 24, 1, C_SEP);
    for (int i = 0; i < rows && s->place_top + i < s->nplaces; i++) {
        const dpk_place *p = &s->place[s->place_top + i];
        int ry = y + i * row_h, sel = s->place_top + i == s->place_sel, lw = 34;
        row_bg(c, 12, ry, c->w - 30, row_h - 1, sel);
        text(c, 16, ry + 2, 1, C_TEXT, p->label, lw - 1);
        if (p->path[0]) text_tail(c, 16 + lw * DPK_CELL_W, ry + 2, C_DIM, p->path, cols - lw - 2);
    }
    scrollbar(c, c->w - 14, y, rows * row_h, s->place_top, rows, s->nplaces);
    hint h[] = {{0, "Enter", "Open"}, {1, "Esc", "Back"}};
    fill(c, 0, foot, c->w, 1, C_SEP);
    hints(s, c, foot + 6, h, 2);
}
static void render_dialog(dpk_state *s, const canvas *c) {
    int cols = (c->w - 24) / DPK_CELL_W, y = c->h / 2 - 20, foot = c->h - 20;
    text(c, 12, y, 1, C_TEXT, "The system file dialog is open: choose the folder there.", cols);
    text(c, 12, y + 14, 1, C_DIM, s->target == DPK_FOR_FONTS ? "(the folder with the 7 system fonts)" : "(the folder dump_folder.py made: exefs, romfs, sysdata)", cols);
    s->area.row_h = 0;
    hint h[] = {{1, "Esc", "Choose in this window instead"}};
    fill(c, 0, foot, c->w, 1, C_SEP);
    hints(s, c, foot + 6, h, 1);
}
void dpk_render(dpk_state *s, uint32_t *px, int w, int h, int pitch) {
    canvas c = {px, w, h, pitch};
    font_init();
    fill(&c, 0, 0, w, h, C_BG);
    header(s, &c, s->screen == DPK_SCR_STATUS ? "Game data" : s->screen == DPK_SCR_PLACES ? "Game data: places" : "Game data: choose a folder");
    switch (s->screen) {
    case DPK_SCR_STATUS: render_status(s, &c); break;
    case DPK_SCR_BROWSE: render_browse(s, &c); break;
    case DPK_SCR_PLACES: render_places(s, &c); break;
    case DPK_SCR_DIALOG: render_dialog(s, &c); break;
    }
}
#endif /* __linux__ */

/* datapicker_sdl.c - the window of the game-data picker (WP-U5b), SDL3. See datapicker.h for the screens and controls.
 *
 * boot.c calls dp_picker_hook (set here by a constructor) on the main thread, before display_init: this file initialises SDL
 * video, events and gamepads itself, shows the picker in its own window (fullscreen under gamescope: the Steam Frame's Game
 * Mode, where no desktop dialog can appear; windowed on a desktop) and ends with SDL_Quit, so display.c's window thread starts
 * SDL from scratch afterwards (SDL_Quit resets the subsystem counts, the hints and the video/events thread).
 *
 * The canvas is drawn by datapicker.c at about 640 x 360 (the window's pixels divided by a whole factor) and shown scaled up
 * by that factor (SDL_Renderer, nearest pixel). Desktop Mode (not gamescope): "Choose the game data folder" opens the system
 * folder dialog (SDL_ShowOpenFolderDialog: xdg-desktop-portal, else zenity); when it cannot open, the in-game browser takes
 * over. SSBU_PICKER_DIALOG=0 never uses it. No display at all (a console start), or SSBU_SHOW=0 (a run without a window, as
 * display.c): returns 0, boot.c prints why and exits 9 (or starts, with usable data). */
#ifdef __linux__
#include "datapicker.h"
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int under_gamescope(void) {                        /* display.c's rule */
    const char *wl = getenv("WAYLAND_DISPLAY"), *de = getenv("XDG_CURRENT_DESKTOP");
    return getenv("GAMESCOPE_WAYLAND_DISPLAY") || (wl && strstr(wl, "gamescope")) || (de && !strcasecmp(de, "gamescope"));
}

/* ---- the system dialog's answer: its callback may run on another thread, also after the picker has ended */
static pthread_mutex_t g_dlg_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { int ready, id, failed; char path[DP_PATH], err[160]; } g_dlg;
static void SDLCALL dialog_cb(void *ud, const char *const *list, int filter) {
    (void)filter;
    pthread_mutex_lock(&g_dlg_mu);
    g_dlg.id = (int)(intptr_t)ud;
    g_dlg.failed = !list;
    snprintf(g_dlg.path, sizeof g_dlg.path, "%s", list && list[0] ? list[0] : "");
    snprintf(g_dlg.err, sizeof g_dlg.err, "%s", list ? "" : SDL_GetError());
    g_dlg.ready = 1;
    pthread_mutex_unlock(&g_dlg_mu);
}

/* ---- pads: the game's default layout (nnpad_sdl.c "auto"): the Nintendo positions (right = A, bottom = B, top = X,
 * left = Y), GameCube controllers by their printed letters */
#define MAX_PADS 8
static SDL_Gamepad *g_pad[MAX_PADS];
static signed char g_stick[MAX_PADS][2];                  /* left stick, -1 / 0 / +1 per axis, with hysteresis */
static int pad_slot(SDL_JoystickID id) {
    for (int i = 0; i < MAX_PADS; i++) if (g_pad[i] && SDL_GetGamepadID(g_pad[i]) == id) return i;
    return -1;
}
static dpk_action pad_action(SDL_Gamepad *g, int b) {
    int letter = -1;                                      /* 0 A, 1 B, 2 X, 3 Y */
    if (b <= SDL_GAMEPAD_BUTTON_NORTH) {
        static const int pos[4] = {1, 0, 3, 2};           /* south B, east A, west Y, north X */
        letter = pos[b];
        if (g && SDL_GetGamepadType(g) == SDL_GAMEPAD_TYPE_GAMECUBE) {
            SDL_GamepadButtonLabel l = SDL_GetGamepadButtonLabel(g, (SDL_GamepadButton)b);
            letter = l == SDL_GAMEPAD_BUTTON_LABEL_A ? 0 : l == SDL_GAMEPAD_BUTTON_LABEL_B ? 1 : l == SDL_GAMEPAD_BUTTON_LABEL_X ? 2
                   : l == SDL_GAMEPAD_BUTTON_LABEL_Y ? 3 : letter;
        }
        static const dpk_action act[4] = {DPK_CONFIRM, DPK_BACK, DPK_USE_HERE, DPK_PLACES};
        return act[letter];
    }
    switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return DPK_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return DPK_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return DPK_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return DPK_RIGHT;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return DPK_PAGE_UP;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return DPK_PAGE_DOWN;
    case SDL_GAMEPAD_BUTTON_START: return DPK_CONFIRM;    /* + */
    case SDL_GAMEPAD_BUTTON_BACK: return DPK_HIDDEN;      /* - */
    }
    return DPK_NONE;
}
static void stick(dpk_state *s, int slot, int axis, int value, uint64_t now) {
    float v = value / 32767.0f;
    signed char *st = &g_stick[slot][axis];
    int want = *st;
    if (v > 0.5f) want = 1; else if (v < -0.5f) want = -1; else if (v > -0.3f && v < 0.3f) want = 0;
    if (want == *st) return;
    dpk_action neg = axis ? DPK_UP : DPK_LEFT, pos = axis ? DPK_DOWN : DPK_RIGHT;
    if (*st) dpk_release(s, *st > 0 ? pos : neg);
    *st = (signed char)want;
    if (want) dpk_press(s, want > 0 ? pos : neg, now);
}

static void event(dpk_state *s, SDL_Renderer *ren, SDL_Event *ev) {
    uint64_t now = SDL_GetTicks();
    switch (ev->type) {
    case SDL_EVENT_QUIT: case SDL_EVENT_WINDOW_CLOSE_REQUESTED: dpk_do(s, DPK_QUIT); break;
    case SDL_EVENT_KEY_DOWN: {
        SDL_Keycode k = ev->key.key;
        int ctrl = (ev->key.mod & SDL_KMOD_CTRL) != 0;
        dpk_action a = DPK_NONE;
        switch (k) {
        case SDLK_UP: a = DPK_UP; break;
        case SDLK_DOWN: a = DPK_DOWN; break;
        case SDLK_LEFT: a = DPK_LEFT; break;
        case SDLK_RIGHT: a = DPK_RIGHT; break;
        case SDLK_PAGEUP: a = DPK_PAGE_UP; break;
        case SDLK_PAGEDOWN: a = DPK_PAGE_DOWN; break;
        case SDLK_HOME: a = DPK_FIRST; break;
        case SDLK_END: a = DPK_LAST; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: a = ctrl ? DPK_USE_HERE : DPK_CONFIRM; break;
        case SDLK_SPACE: a = DPK_CONFIRM; break;
        case SDLK_ESCAPE: case SDLK_BACKSPACE: a = DPK_BACK; break;
        case SDLK_TAB: a = DPK_PLACES; break;
        case SDLK_H: if (ctrl) a = DPK_HIDDEN; break;
        case SDLK_Q: if (ctrl) a = DPK_QUIT; break;
        }
        /* a held key repeats the moves only: a held Enter must not open folder after folder */
        int moves = a == DPK_UP || a == DPK_DOWN || a == DPK_LEFT || a == DPK_RIGHT || a == DPK_PAGE_UP || a == DPK_PAGE_DOWN || a == DPK_BACK;
        if (a != DPK_NONE && (!ev->key.repeat || moves)) { s->device = DPK_DEV_KEYS; dpk_do(s, a); }
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (ev->text.text && strcmp(ev->text.text, " ") && !(SDL_GetModState() & SDL_KMOD_CTRL)) dpk_typed(s, ev->text.text, now);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        for (int i = 0; i < MAX_PADS; i++)
            if (!g_pad[i]) {
                if ((g_pad[i] = SDL_OpenGamepad(ev->gdevice.which))) { g_stick[i][0] = g_stick[i][1] = 0; printf("data picker: controller %s\n", SDL_GetGamepadName(g_pad[i]) ? SDL_GetGamepadName(g_pad[i]) : "?"); }
                break;
            }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED: {
        int i = pad_slot(ev->gdevice.which);
        if (i >= 0) { SDL_CloseGamepad(g_pad[i]); g_pad[i] = NULL; s->held = DPK_NONE; }
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        int i = pad_slot(ev->gbutton.which);
        dpk_action a = pad_action(i >= 0 ? g_pad[i] : NULL, ev->gbutton.button);
        if (a == DPK_NONE) break;
        if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) dpk_press(s, a, now); else dpk_release(s, a);
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        int i = pad_slot(ev->gaxis.which);
        if (i >= 0 && (ev->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || ev->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY))
            stick(s, i, ev->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY, ev->gaxis.value, now);
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN:
        SDL_ConvertEventToRenderCoordinates(ren, ev);
        if (ev->type == SDL_EVENT_MOUSE_MOTION) dpk_pointer(s, (int)ev->motion.x, (int)ev->motion.y, 0);
        else if ((ev->button.button == SDL_BUTTON_LEFT || ev->button.button == SDL_BUTTON_RIGHT) && ev->button.clicks < 2)
            /* one click opens: the second click of a double-click would open the row under it in the folder just opened */
            dpk_pointer(s, (int)ev->button.x, (int)ev->button.y, ev->button.button == SDL_BUTTON_LEFT ? 1 : 2);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (ev->wheel.integer_y) dpk_wheel(s, ev->wheel.integer_y > 0 ? 1 : -1);
        break;
    }
}

int datapicker_sdl_run(const dp_result *r, int pick_mode, const dpk_sdl_test *t) {
    static dpk_state s;                                   /* big (paths): not on the stack */
    int gs = under_gamescope(), rc = 0, frame_no = 0, cw = 0, ch = 0;
    const char *wm = getenv("SSBU_WINDOW"), *dlg = getenv("SSBU_PICKER_DIALOG"), *drv;
    uint32_t *px = NULL;
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;

    printf("data picker: %s (%s)\n", pick_mode ? "--pick-data" : dpk_headline(r, 0), dp_status_name(r->status));
    const char *show = getenv("SSBU_SHOW");
    if (show && show[0] && atoi(show) == 0) {             /* display.c's rule: SSBU_SHOW=0 is a run without a window (tests) */
        printf("data picker: not shown (SSBU_SHOW=0)\n");
        return 0;
    }
    if (gs) {
        SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR, "0");
        /* gamescope shows this window alone, but does not always give it the keyboard focus SDL waits for before it passes
         * on controller input (a start from a console, the VR backend): take the pads regardless, as nnpad_sdl.c does */
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        printf("data picker: no display to show it on (SDL: %s)\n", SDL_GetError());
        SDL_Quit();
        return 0;
    }
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) printf("data picker: no controllers (SDL: %s); keyboard and mouse work\n", SDL_GetError());
    drv = SDL_GetCurrentVideoDriver();
    SDL_WindowFlags fl = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE;
    if (gs || (wm && !strcasecmp(wm, "fullscreen"))) fl |= SDL_WINDOW_FULLSCREEN;
    win = SDL_CreateWindow("SSBU native - game data", 1280, 720, fl);
    if (win) ren = SDL_CreateRenderer(win, NULL);
    if (win && !ren) ren = SDL_CreateRenderer(win, SDL_SOFTWARE_RENDERER);
    if (!win || !ren) {
        printf("data picker: no window (SDL: %s)\n", SDL_GetError());
        goto out;
    }
    SDL_SetRenderVSync(ren, 1);
    int dialog_ok = !gs && !(dlg && dlg[0] == '0') && drv && strcmp(drv, "offscreen") && strcmp(drv, "dummy");
    printf("data picker: window on %s (%s)%s, renderer %s\n", drv ? drv : "?", fl & SDL_WINDOW_FULLSCREEN ? "fullscreen" : "windowed",
           dialog_ok ? ", system folder dialog offered" : "", SDL_GetRendererName(ren) ? SDL_GetRendererName(ren) : "?");
    if (!gs) SDL_StartTextInput(win);                     /* type-ahead in the browser (not under gamescope: no keyboard pop-up) */
    dpk_init(&s, r, pick_mode, dialog_ok);
    pthread_mutex_lock(&g_dlg_mu);
    g_dlg.ready = 0;
    pthread_mutex_unlock(&g_dlg_mu);

    while (!s.result) {
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, s.held != DPK_NONE || t ? 16 : 100)) do event(&s, ren, &ev); while (!s.result && SDL_PollEvent(&ev));
        if (s.result) break;
        dpk_tick(&s, SDL_GetTicks());
        dpk_target target;
        const char *start;
        int id;
        if (dpk_dialog_take(&s, &target, &start, &id)) {
            printf("data picker: system folder dialog (%s)\n", target == DPK_FOR_FONTS ? "fonts" : "game data");
            SDL_ShowOpenFolderDialog(dialog_cb, (void *)(intptr_t)id, win, start, false);
        }
        pthread_mutex_lock(&g_dlg_mu);
        if (g_dlg.ready) {
            g_dlg.ready = 0;
            if (g_dlg.failed) printf("data picker: the system dialog failed (%s): in-game browser\n", g_dlg.err);
            dpk_dialog_done(&s, g_dlg.id, g_dlg.path, g_dlg.failed);
        }
        pthread_mutex_unlock(&g_dlg_mu);
        if (s.result) break;
        /* the canvas: the output size divided by the largest whole factor that keeps it at least 640 x 360 */
        int pw = 0, ph = 0;
        if (!SDL_GetRenderOutputSize(ren, &pw, &ph) || pw <= 0 || ph <= 0) { pw = 1280; ph = 720; }
        int k = pw / 640 < ph / 360 ? pw / 640 : ph / 360;
        if (k < 1) k = 1;
        if (!tex || pw / k != cw || ph / k != ch) {
            cw = pw / k; ch = ph / k;
            if (tex) SDL_DestroyTexture(tex);
            free(px);
            px = malloc((size_t)cw * (size_t)ch * 4);
            tex = px ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, cw, ch) : NULL;
            if (!tex) { printf("data picker: no texture (SDL: %s)\n", SDL_GetError()); goto out; }
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
            SDL_SetRenderLogicalPresentation(ren, cw, ch, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
        }
        dpk_render(&s, px, cw, ch, cw);
        SDL_UpdateTexture(tex, NULL, px, cw * 4);
        SDL_SetRenderDrawColor(ren, 15, 15, 15, 255);
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        if (t && t->frame) t->frame(t->ctx, win, ren, &s, px, cw, ch, frame_no);
        frame_no++;
        SDL_RenderPresent(ren);
    }
    rc = s.result == DPK_PICKED ? 1 : s.result == DPK_QUITTED ? -1 : 0;
    if (s.result == DPK_PICKED) printf("data picker: chose %s\n", s.data_dir[0] ? s.data_dir : s.was.dir);
    else printf("data picker: %s\n", s.result == DPK_KEEP ? "start with the data found" : "the player quit");
out:
    dpk_free(&s);
    free(px);
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) { SDL_StopTextInput(win); SDL_DestroyWindow(win); }
    for (int i = 0; i < MAX_PADS; i++) if (g_pad[i]) { SDL_CloseGamepad(g_pad[i]); g_pad[i] = NULL; }
    SDL_Quit();                                           /* display.c / nnpad_sdl.c / nnaudio.c start SDL again from scratch */
    return rc;
}

static int picker_hook(const dp_result *r) { return datapicker_sdl_run(r, r->status == DP_OK, NULL); }
__attribute__((constructor)) static void datapicker_register(void) { if (!dp_picker_hook) dp_picker_hook = picker_hook; }
#endif /* __linux__ */

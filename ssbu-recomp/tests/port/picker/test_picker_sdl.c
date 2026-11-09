/* tests/port/picker/test_picker_sdl.c - the picker's SDL3 window (runtime/datapicker_sdl.c) as boot.c runs it: SDL started and
 * ended by the picker, driven by real SDL input (an SDL virtual gamepad, keyboard and mouse events pushed into the queue),
 * the frames read back from the renderer, then SDL started again on another thread as display.c's window thread does.
 *   test_picker_sdl <scratch folder> <shots folder> [hold ms]
 * SDL_VIDEODRIVER=offscreen runs it headless; on the Steam Frame run_frame.sh also runs it on the gamescope session's display
 * (hold > 0: each pictured screen stays up that long, and $PICKER_SHOT_CMD <file> is run to take the compositor's picture). */
#include "datapicker.h"
#include "hostpath.h"
#include <windows.h>
#include "tp.h"
#include <SDL3/SDL.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char T[PATH_MAX + 32], SHOTS[PATH_MAX + 32];
static int g_hold;
static char *P(const char *fmt, ...) {
    static char buf[16][2048];
    static int at;
    char *o = buf[at++ & 15];
    va_list ap;
    int k = snprintf(o, 2048, "%s/", T);
    va_start(ap, fmt);
    vsnprintf(o + k, (size_t)(2048 - k), fmt, ap);
    va_end(ap);
    return o;
}
static void put(const char *path, const void *data, size_t n) {
    host_make_parents(path);
    FILE *f = fopen(path, "wb");
    CHECKF(f != NULL, "%s", path);
    if (f) { fwrite(data, 1, n, f); fclose(f); }
}
static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *ftw) { (void)st; (void)flag; (void)ftw; return remove(p); }
static void rmrf(const char *p) { nftw(p, rm_one, 16, FTW_DEPTH | FTW_PHYS); }
static const char *const FONTS[] = {
    "nintendo_udsg-r_std_003.ttf", "nintendo_udsg-r_org_zh-cn_003.ttf", "nintendo_udsg-r_ext_zh-cn_003.ttf",
    "nintendo_udjxh-db_zh-tw_003.ttf", "nintendo_udsg-r_ko_003.ttf", "nintendo_ext_003.ttf", "nintendo_ext2_003.ttf",
};
static void make_dump(const char *d, uint32_t text, int fonts) {
    char p[2100];
    uint8_t h[0x100] = {0};
    memcpy(h, "NSO0", 4); memcpy(h + 0x18, &text, 4);
    snprintf(p, sizeof p, "%s/exefs/main", d); put(p, h, sizeof h);
    snprintf(p, sizeof p, "%s/romfs/data.arc", d); put(p, "A", 1);
    snprintf(p, sizeof p, "%s/romfs/.nrr/a.nrr", d); put(p, "N", 1);
    for (int i = 0; fonts && i < 7; i++) { snprintf(p, sizeof p, "%s/sysdata/fonts/%s", d, FONTS[i]); put(p, "F", 1); }
}
static char *real(const char *p) { static char v[PATH_MAX]; if (!realpath(p, v)) snprintf(v, sizeof v, "%s", p); return v; }

/* ---- the frames */
static void save_canvas(const char *name, const uint32_t *px, int w, int h) {
    char path[PATH_MAX + 128];
    snprintf(path, sizeof path, "%s/%s_canvas.bmp", SHOTS, name);
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ARGB8888, (void *)px, w * 4);
    CHECK(s && SDL_SaveBMP(s, path));
    SDL_DestroySurface(s);
}
/* the renderer's frame (what the window shows next): saved, and checked for the title bar colour */
static void save_frame(const char *name, SDL_Renderer *ren) {
    char path[PATH_MAX + 128];
    SDL_Surface *f = SDL_RenderReadPixels(ren, NULL), *c = f ? SDL_ConvertSurface(f, SDL_PIXELFORMAT_ARGB8888) : NULL;
    CHECKF(c != NULL, "read back: %s", SDL_GetError());
    if (c) {
        int title = 0;
        for (int y = 0; y < c->h; y++)
            for (int x = 0; x < c->w; x += 7) title += (((const uint32_t *)((const uint8_t *)c->pixels + y * c->pitch))[x] & 0xffffff) == 0x294a7a;
        CHECKF(title > 100, "%s: the title bar is not in the frame read back (%d)", name, title);
        snprintf(path, sizeof path, "%s/%s_frame.bmp", SHOTS, name);
        CHECK(SDL_SaveBMP(c, path));
        printf("    frame %s: %dx%d (%s)\n", name, c->w, c->h, path);
    }
    SDL_DestroySurface(c);
    SDL_DestroySurface(f);
}
static void picture(const char *name, SDL_Renderer *ren, const uint32_t *px, int w, int h) {
    save_canvas(name, px, w, h);
    save_frame(name, ren);
    if (g_hold > 0) {                                     /* on screen: let the compositor show it, then take its picture */
        SDL_RenderPresent(ren);
        SDL_Delay((Uint32)g_hold);
        const char *cmd = getenv("PICKER_SHOT_CMD");
        if (cmd && cmd[0]) {
            char line[PATH_MAX * 2];
            snprintf(line, sizeof line, "%s %s/%s_screen.png", cmd, SHOTS, name);
            int rc = system(line);
            printf("    compositor picture %s: %s (rc %d)\n", name, line, rc);
        }
    }
}

/* ---- input as a player gives it */
static SDL_Joystick *g_vj;
static void pad_attach(void) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.vendor_id = 0x1209; d.product_id = 0x5b5b;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT; d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.name = "Picker test pad";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    CHECKF(id != 0, "virtual pad: %s", SDL_GetError());
    g_vj = id ? SDL_OpenJoystick(id) : NULL;
    CHECK(g_vj != NULL);
}
static void pad_button(int b, int down) { if (g_vj) SDL_SetJoystickVirtualButton(g_vj, b, down); }
static void pad_axis(int a, int v) { if (g_vj) SDL_SetJoystickVirtualAxis(g_vj, a, (Sint16)v); }
static void key(SDL_Window *w, SDL_Keycode k, SDL_Keymod mod) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = SDL_GetWindowID(w);
    e.key.key = k; e.key.mod = mod; e.key.down = true;
    SDL_PushEvent(&e);
}
/* a left click on canvas point (x, y): moved there, then pressed */
static void click(SDL_Window *w, SDL_Renderer *ren, int x, int y, int button) {
    float wx = 0, wy = 0;
    SDL_Event e;
    CHECK(SDL_RenderCoordinatesToWindow(ren, (float)x + 0.5f, (float)y + 0.5f, &wx, &wy));
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = SDL_GetWindowID(w); e.motion.x = wx; e.motion.y = wy;
    SDL_PushEvent(&e);
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.windowID = SDL_GetWindowID(w); e.button.button = (Uint8)button; e.button.down = true; e.button.clicks = 1;
    e.button.x = wx; e.button.y = wy;
    SDL_PushEvent(&e);
}
static int entry(const dpk_state *s, const char *name) {
    for (int i = 0; i < s->n; i++) if (s->ent[i].kind >= DPK_E_DIR && !strcmp(s->ent[i].name, name)) return i;
    return -1;
}

/* ---- run 1: first start; pad, keyboard and mouse to the good dump */
static int g_step, g_wait, g_sel0;
static uint64_t g_t0;
static void run1(void *ctx, void *window, void *renderer, dpk_state *s, const uint32_t *px, int w, int h, int frame) {
    SDL_Window *win = window;
    SDL_Renderer *ren = renderer;
    (void)ctx;
    if (frame > 3000) { CHECKF(0, "run 1 stuck at step %d", g_step); SDL_Event q; SDL_zero(q); q.type = SDL_EVENT_QUIT; SDL_PushEvent(&q); return; }
    if (g_wait > 0) { g_wait--; return; }
    switch (g_step) {
    case 0: pad_attach(); g_wait = 5; g_step++; break;
    case 1:
        CHECK(s->screen == DPK_SCR_STATUS && s->now.status == DP_NOT_CONFIGURED);
        picture("sdl_01_first_start", ren, px, w, h);
        pad_button(SDL_GAMEPAD_BUTTON_SOUTH, 1); g_wait = 2; g_step++;       /* bottom = B: back, nothing to go back to */
        break;
    case 2: pad_button(SDL_GAMEPAD_BUTTON_SOUTH, 0); CHECK(s->screen == DPK_SCR_STATUS && s->device == DPK_DEV_PAD); g_wait = 2; g_step++; break;
    case 3: pad_button(SDL_GAMEPAD_BUTTON_EAST, 1); g_wait = 2; g_step++; break;        /* right = A: "Choose the game data folder" */
    case 4:
        pad_button(SDL_GAMEPAD_BUTTON_EAST, 0);
        CHECK(s->screen == DPK_SCR_BROWSE && !strcmp(s->cwd, real(P("home"))));
        picture("sdl_02_browse_home_pad", ren, px, w, h);
        g_sel0 = s->sel;
        pad_axis(SDL_GAMEPAD_AXIS_LEFTY, 32000);            /* the stick held down: one step, then repeats */
        g_t0 = SDL_GetTicks(); g_step++;
        break;
    case 5:
        if (SDL_GetTicks() - g_t0 < 700) break;
        pad_axis(SDL_GAMEPAD_AXIS_LEFTY, 0);
        CHECKF(s->sel >= g_sel0 + 3 || s->sel == s->n - 1, "stick repeat: %d -> %d of %d", g_sel0, s->sel, s->n);
        g_wait = 2; g_step++;
        break;
    case 6: {                                               /* keyboard: Home, then Down to "Games", Enter */
        int want = entry(s, "Games");
        CHECK(want > 0);
        key(win, SDLK_HOME, SDL_KMOD_NONE);
        for (int i = 0; i < want; i++) key(win, SDLK_DOWN, SDL_KMOD_NONE);
        key(win, SDLK_RETURN, SDL_KMOD_NONE);
        g_wait = 3; g_step++;
        break;
    }
    case 7: {
        CHECK(!strcmp(s->cwd, real(P("home/Games"))) && s->device == DPK_DEV_KEYS);
        picture("sdl_03_games_keyboard", ren, px, w, h);
        int i = entry(s, "dump_good");                      /* the mouse: a click on its row opens it */
        CHECK(i >= 0 && s->area.row_h > 0);
        click(win, ren, s->area.x + 30, s->area.y + (i - s->area.first) * s->area.row_h + s->area.row_h / 2, SDL_BUTTON_LEFT);
        g_wait = 3; g_step++;
        break;
    }
    case 8:
        CHECK(!strcmp(s->cwd, real(P("home/Games/dump_good"))) && s->sel == 0);
        picture("sdl_04_inside_dump_mouse", ren, px, w, h);
        pad_button(SDL_GAMEPAD_BUTTON_NORTH, 1);            /* top = X: use this folder */
        g_wait = 2; g_step++;
        break;
    default: break;
    }
}
/* ---- run 2: --pick-data with usable data: B does nothing, A starts with them */
static void run2(void *ctx, void *window, void *renderer, dpk_state *s, const uint32_t *px, int w, int h, int frame) {
    (void)ctx; (void)window;
    if (frame > 2000) { SDL_Event q; SDL_zero(q); q.type = SDL_EVENT_QUIT; SDL_PushEvent(&q); CHECK(0); return; }
    if (g_wait > 0) { g_wait--; return; }
    switch (g_step) {
    case 0: pad_attach(); g_wait = 5; g_step++; break;
    case 1:
        CHECK(s->pick_mode && s->menu[0] == DPK_M_START);
        picture("sdl_05_pick_data", renderer, px, w, h);
        pad_button(SDL_GAMEPAD_BUTTON_SOUTH, 1); g_wait = 2; g_step++;
        break;
    case 2: pad_button(SDL_GAMEPAD_BUTTON_SOUTH, 0); CHECK(!s->result && s->screen == DPK_SCR_STATUS); g_wait = 2; g_step++; break;
    case 3: pad_button(SDL_GAMEPAD_BUTTON_START, 1); g_step++; break;      /* + confirms too */
    }
}
/* ---- run 3: fonts missing: pictured, then the window's close button */
static void run3(void *ctx, void *window, void *renderer, dpk_state *s, const uint32_t *px, int w, int h, int frame) {
    (void)ctx;
    if (frame == 3) {
        CHECK(s->now.status == DP_MISSING_FONTS && s->menu[0] == DPK_M_FONTS);
        picture("sdl_06_fonts_missing", renderer, px, w, h);
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        e.window.windowID = SDL_GetWindowID(window);
        SDL_PushEvent(&e);
    }
    if (frame > 500) { SDL_Event q; SDL_zero(q); q.type = SDL_EVENT_QUIT; SDL_PushEvent(&q); }
}
/* ---- run 4: SIGTERM / SDL_EVENT_QUIT while it is open */
static void run4(void *ctx, void *window, void *renderer, dpk_state *s, const uint32_t *px, int w, int h, int frame) {
    (void)ctx; (void)window; (void)renderer; (void)s; (void)px; (void)w; (void)h;
    if (frame == 2) { SDL_Event q; SDL_zero(q); q.type = SDL_EVENT_QUIT; SDL_PushEvent(&q); }
}

/* ---- run 5 (review): a double-click on a folder opens that folder only, not the row under the pointer in it too */
static void press_at(SDL_Window *w, SDL_Renderer *ren, int x, int y, int clicks) {
    float wx = 0, wy = 0;
    SDL_Event e;
    CHECK(SDL_RenderCoordinatesToWindow(ren, (float)x + 0.5f, (float)y + 0.5f, &wx, &wy));
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.windowID = SDL_GetWindowID(w); e.button.button = SDL_BUTTON_LEFT; e.button.down = true; e.button.clicks = (Uint8)clicks;
    e.button.x = wx; e.button.y = wy;
    SDL_PushEvent(&e);
}
static int g_x5, g_y5;
static void run5(void *ctx, void *window, void *renderer, dpk_state *s, const uint32_t *px, int w, int h, int frame) {
    (void)ctx; (void)px; (void)w; (void)h;
    if (frame > 500) { SDL_Event q; SDL_zero(q); q.type = SDL_EVENT_QUIT; SDL_PushEvent(&q); CHECKF(0, "run 5 stuck at step %d", g_step); return; }
    if (g_wait > 0) { g_wait--; return; }
    switch (g_step) {
    case 0: CHECK(dpk_browse(s, P("home/dbl"))); s->screen = DPK_SCR_BROWSE; g_wait = 2; g_step++; break;
    case 1: {
        int i = entry(s, "a");
        CHECK(i >= 0 && s->area.row_h > 0);
        g_x5 = s->area.x + 30; g_y5 = s->area.y + (i - s->area.first) * s->area.row_h + s->area.row_h / 2;
        press_at(window, renderer, g_x5, g_y5, 1);
        g_wait = 3; g_step++;
        break;
    }
    case 2:
        CHECK(!strcmp(s->cwd, real(P("home/dbl/a"))));
        CHECK(entry(s, "b") == 3);                          /* "b" sits on the row just clicked */
        press_at(window, renderer, g_x5, g_y5, 2);          /* the second click of the double-click */
        g_wait = 3; g_step++;
        break;
    case 3: {
        CHECKF(!strcmp(s->cwd, real(P("home/dbl/a"))), "double-click went on to %s", s->cwd);
        SDL_Event q; SDL_zero(q); q.type = SDL_EVENT_QUIT; SDL_PushEvent(&q);
        g_step++;
        break;
    }
    }
}
static int g_shown;
static void run6(void *ctx, void *window, void *renderer, dpk_state *s, const uint32_t *px, int w, int h, int frame) {
    (void)ctx; (void)window; (void)renderer; (void)s; (void)px; (void)w; (void)h; (void)frame;
    g_shown++;
    SDL_Event q; SDL_zero(q); q.type = SDL_EVENT_QUIT; SDL_PushEvent(&q);
}

/* display.c's window thread after the picker: SDL_Init(VIDEO) on another thread, a window, a few event pumps */
static void *display_like(void *arg) {
    int *ok = arg;
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Window *w = SDL_CreateWindow("after the picker", 640, 360, SDL_WINDOW_VULKAN);
        if (!w) w = SDL_CreateWindow("after the picker", 640, 360, 0);
        if (w) {
            SDL_Event e;
            for (int i = 0; i < 5; i++) { while (SDL_PollEvent(&e)) {} SDL_Delay(10); }
            *ok = SDL_IsMainThread() && !strcmp(SDL_GetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE), "0");
            SDL_DestroyWindow(w);
        } else printf("    display-like window: %s\n", SDL_GetError());
    } else printf("    display-like SDL_Init: %s\n", SDL_GetError());
    SDL_Quit();
    return NULL;
}

static void env(void) {
    setenv("HOME", P("home"), 1);
    setenv("XDG_CONFIG_HOME", P("xdg/config"), 1);
    setenv("XDG_DATA_HOME", P("xdg/data"), 1);
    setenv("SSBU_PICKER_MEDIA", P("media"), 1);
    unsetenv("SSBU_DATA"); unsetenv("SSBU_ROMFS"); unsetenv("SSBU_FONTS"); unsetenv("SSBU_PICK_DATA");
}

int main(int argc, char **argv) {
    char abs[PATH_MAX], v[DP_PATH];
    dp_result r;
    if (argc < 3) { fprintf(stderr, "usage: test_picker_sdl <scratch folder> <shots folder> [hold ms]\n"); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(v, sizeof v, "%s/x", argv[1]); host_make_parents(v);
    snprintf(v, sizeof v, "%s/x", argv[2]); host_make_parents(v);
    if (!realpath(argv[1], abs)) { perror(argv[1]); return 2; }
    snprintf(T, sizeof T, "%s/picker_sdl_scratch", abs);
    if (!realpath(argv[2], SHOTS)) { perror(argv[2]); return 2; }
    g_hold = argc > 3 ? atoi(argv[3]) : 0;
    rmrf(T);
    make_dump(P("home/Games/dump_good"), 0x39c7e90, 1);
    make_dump(P("home/Games/dump_nofonts"), 0x39c7e90, 0);
    for (int i = 0; i < 12; i++) { char d[64]; snprintf(d, sizeof d, "home/Folder %02d/x", i); host_make_parents(P("%s", d)); }
    env();
    CHECK(dp_picker_hook != NULL);                          /* linking datapicker_sdl.c registers the hook */

    SECTION("SDL run 1: first start; virtual pad (B, A, stick repeat, X), keyboard (Home, Down, Enter), mouse click -> chosen");
    dp_check("13.0.4", &r);
    dpk_sdl_test t1 = {run1, NULL};
    int rc = datapicker_sdl_run(&r, 0, &t1);
    CHECKF(rc == 1, "run 1 returned %d at step %d", rc, g_step);
    CHECK(dp_config_get("data", v, sizeof v) && !strcmp(v, real(P("home/Games/dump_good"))));
    CHECK(SDL_WasInit(0) == 0);                             /* SDL is down again: nothing initialised */
    SECTION("after the picker: SDL starts again on another thread (display.c's window thread), hints reset");
    int ok = 0;
    pthread_t th;
    pthread_create(&th, NULL, display_like, &ok);
    pthread_join(th, NULL);
    CHECK(ok);
    CHECK(SDL_WasInit(0) == 0);

    SECTION("SDL run 2: --pick-data with usable data: B nothing, + = start with them -> 0");
    g_step = g_wait = 0;
    g_vj = NULL;                                            /* closed with SDL by the last run */
    dp_check("13.0.4", &r);
    CHECK(r.status == DP_OK);
    dpk_sdl_test t2 = {run2, NULL};
    rc = datapicker_sdl_run(&r, 1, &t2);
    CHECK(rc == 0);
    SECTION("SDL run 3 and 4: fonts missing pictured; the window closed / SDL_EVENT_QUIT -> -1 (the player quit)");
    setenv("SSBU_DATA", P("home/Games/dump_nofonts"), 1);
    dp_check("13.0.4", &r);
    unsetenv("SSBU_DATA");
    dpk_sdl_test t3 = {run3, NULL}, t4 = {run4, NULL};
    CHECK(datapicker_sdl_run(&r, 0, &t3) == -1);
    CHECK(datapicker_sdl_run(&r, 0, &t4) == -1);
    CHECK(SDL_WasInit(0) == 0);
    SECTION("the hook as boot.c calls it, without a display: 0 at once (boot.c then prints the reason and exits 9)");
    const char *drv = getenv("SDL_VIDEODRIVER");
    char saved[64] = "";
    if (drv) snprintf(saved, sizeof saved, "%s", drv);
    setenv("SDL_VIDEODRIVER", "no-such-driver", 1);
    CHECK(dp_picker_hook(&r) == 0);
    if (saved[0]) setenv("SDL_VIDEODRIVER", saved, 1); else unsetenv("SDL_VIDEODRIVER");
    CHECK(SDL_WasInit(0) == 0);
    SECTION("review: a double-click on a folder opens it once (the second click is not taken as a click in the new folder)");
    host_make_parents(P("home/dbl/a/b/c/x"));
    setenv("SSBU_PICKER_DIALOG", "0", 1);
    g_step = g_wait = 0;
    dpk_sdl_test t5 = {run5, NULL};
    CHECK(datapicker_sdl_run(&r, 0, &t5) == -1);
    CHECK(g_step == 4);
    unsetenv("SSBU_PICKER_DIALOG");
    SECTION("review: SSBU_SHOW=0 (display.c's run without a window): 0 at once, no window, SDL untouched");
    setenv("SSBU_SHOW", "0", 1);
    dpk_sdl_test t6 = {run6, NULL};
    CHECK(datapicker_sdl_run(&r, 0, &t6) == 0);
    CHECK(g_shown == 0);
    unsetenv("SSBU_SHOW");
    CHECK(SDL_WasInit(0) == 0);
    if (!tp_fail) rmrf(T);
    printf("shots: %s\n", SHOTS);
    return tp_done("picker_sdl");
}

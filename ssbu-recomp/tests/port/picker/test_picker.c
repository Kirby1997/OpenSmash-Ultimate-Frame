/* tests/port/picker - the in-game game-data picker (WP-U5b), the part without a display: runtime/datapicker.c (screens,
 * folder browser, places, system-dialog hand-over, checks and the saved choice, the bitmap font and the drawing) driven by
 * synthetic input, plus datapaths.c's picker helpers (dp_validate_picked, dp_fonts_check, --pick-data). datapicker.c is
 * included here so that its static helpers (wrap, glyphs) can be checked too. Every input is a synthetic fixture: no game data.
 *   test_picker <scratch folder> [shots folder]     the scratch folder is removed when every check passes; the shots folder
 *                                                   (default <scratch>/../shots) keeps a PPM of every screen drawn */
#include "../../../runtime/datapicker.c"
#include <windows.h>
#include "tp.h"
#include <fcntl.h>
#include <ftw.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- fixtures (as tests/port/boot) */
static char T[PATH_MAX + 32], SHOTS[PATH_MAX + 32];
static char *P(const char *fmt, ...) {          /* T + "/" + the formatted rest, in one of 16 rotating buffers */
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
static void mkdirs(const char *dir) { char x[2100]; snprintf(x, sizeof x, "%s/x", dir); CHECK(host_make_parents(x) == 0); }
static void put(const char *path, const void *data, size_t n) {
    host_make_parents(path);
    FILE *f = fopen(path, "wb");
    CHECKF(f != NULL, "%s", path);
    if (f) { fwrite(data, 1, n, f); fclose(f); }
}
static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *ftw) { (void)st; (void)flag; (void)ftw; chmod(p, 0700); return remove(p); }
static void rmrf(const char *p) { nftw(p, rm_one, 16, FTW_DEPTH | FTW_PHYS); }
static void put_nso(const char *path, uint32_t text) {
    uint8_t h[0x100] = {0};
    memcpy(h, "NSO0", 4);
    memcpy(h + 0x18, &text, 4);
    put(path, h, sizeof h);
}
static const char *const FONTS[] = {
    "nintendo_udsg-r_std_003.ttf", "nintendo_udsg-r_org_zh-cn_003.ttf", "nintendo_udsg-r_ext_zh-cn_003.ttf",
    "nintendo_udjxh-db_zh-tw_003.ttf", "nintendo_udsg-r_ko_003.ttf", "nintendo_ext_003.ttf", "nintendo_ext2_003.ttf",
};
static void put_fonts(const char *dir) { char p[2100]; for (int i = 0; i < 7; i++) { snprintf(p, sizeof p, "%s/%s", dir, FONTS[i]); put(p, "F", 1); } }
static void make_dump(const char *d, uint32_t text, int fonts) {
    char p[2100];
    if (text) { snprintf(p, sizeof p, "%s/exefs/main", d); put_nso(p, text); }
    snprintf(p, sizeof p, "%s/romfs/data.arc", d); put(p, "A", 1);
    snprintf(p, sizeof p, "%s/romfs/.nrr/a.nrr", d); put(p, "N", 1);
    snprintf(p, sizeof p, "%s/sysdata/fonts", d);
    if (fonts) put_fonts(p); else mkdirs(p);
}
static void clean_env(void) {
    extern char **environ;
    char names[256][128];
    int n = 0;
    for (char **e = environ; *e && n < 256; e++) if ((!strncmp(*e, "SSBU_", 5) && strncmp(*e, "SSBU_VMEM_", 10)) || !strncmp(*e, "XDG_", 4)) {
        size_t l = strcspn(*e, "=");
        if (l < sizeof names[0]) { memcpy(names[n], *e, l); names[n++][l] = 0; }
    }
    for (int i = 0; i < n; i++) unsetenv(names[i]);
    setenv("HOME", P("home"), 1);
    setenv("USER", "tester", 1);
    setenv("XDG_CONFIG_HOME", P("xdg/config"), 1);
    setenv("XDG_DATA_HOME", P("xdg/data"), 1);
    setenv("XDG_STATE_HOME", P("xdg/state"), 1);
    setenv("SSBU_PICKER_MEDIA", P("media"), 1);
    for (int k = 0; k < DP_KINDS; k++) dp_set_cli((dp_kind)k, NULL);
}
static void fresh(void) {                       /* no saved choice */
    rmrf(P("xdg"));
    clean_env();
}
static char *cfg(const char *key) {
    static char v[2][DP_PATH];
    static int at;
    char *o = v[at++ & 1];
    if (!dp_config_get(key, o, DP_PATH)) snprintf(o, DP_PATH, "-");
    return o;
}
static char *real(const char *p) {
    static char v[4][PATH_MAX];
    static int at;
    char *o = v[at++ & 3];
    if (!realpath(p, o)) snprintf(o, PATH_MAX, "%s", p);
    return o;
}

/* ---- the canvas */
#define CW 640
#define CH 360
static uint32_t g_px[1280 * 800];
static void shot(dpk_state *s, const char *name, int w, int h) {
    dpk_render(s, g_px, w, h, w);
    char path[PATH_MAX + 128];
    snprintf(path, sizeof path, "%s/%s.ppm", SHOTS, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { unsigned char rgb[3] = {(unsigned char)(g_px[i] >> 16), (unsigned char)(g_px[i] >> 8), (unsigned char)g_px[i]}; fwrite(rgb, 1, 3, f); }
    fclose(f);
}
static int count_rgb(int w, int h, uint32_t rgb, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1 && y < h; y++) for (int x = x0; x < x1 && x < w; x++) n += (g_px[y * w + x] & 0xffffff) == rgb;
    return n;
}
static int find_entry(const dpk_state *s, const char *name) {
    for (int i = 0; i < s->n; i++) if (s->ent[i].kind >= DPK_E_DIR && !strcmp(s->ent[i].name, name)) return i;
    return -1;
}
static void go_to(dpk_state *s, const char *name) {           /* moves the selection with DOWN / UP presses, as a player */
    int want = find_entry(s, name), guard = 0;
    CHECKF(want >= 0, "no entry %s in %s", name, s->cwd);
    while (want >= 0 && s->sel != want && guard++ < 10000) dpk_do(s, s->sel < want ? DPK_DOWN : DPK_UP);
}
static int has_menu(const dpk_state *s, int m) { for (int i = 0; i < s->menu_n; i++) if (s->menu[i] == m) return i; return -1; }
static dp_result g_was;
static void was_check(void) { dp_check("13.0.4", &g_was); }

/* ================================================================================================ font, text, drawing */
static void t_font(void) {
    SECTION("font: every printable ASCII character has a glyph, the space none; UTF-8 -> boxes / look-alikes");
    font_init();
    for (int c = 33; c < 127; c++) { int any = 0; for (int r = 0; r < 9; r++) any |= g_glyph[c][r]; CHECKF(any, "glyph %c", c); }
    for (int r = 0; r < 9; r++) CHECK(g_glyph[' '][r] == 0);
    for (int c = 1; c <= 5; c++) { int any = 0; for (int r = 0; r < 9; r++) any |= g_glyph[c][r]; CHECK(any); }
    CHECK(g_glyph['g'][8] && g_glyph['p'][8] && !g_glyph['a'][8]);       /* descenders below the baseline */
    const char *u = "\xc3\xa9\xe2\x80\x94\xe2\x80\x9cx\xff";              /* e-acute, em dash, left quote, x, a bad byte */
    CHECK(next_glyph(&u) == 127); CHECK(next_glyph(&u) == '-'); CHECK(next_glyph(&u) == '"'); CHECK(next_glyph(&u) == 'x');
    CHECK(next_glyph(&u) == 127); CHECK(next_glyph(&u) == 0);
    const char *trunc = "\xe2\x80";                                       /* cut short: one box per byte, never past the end */
    CHECK(next_glyph(&trunc) == 127); CHECK(next_glyph(&trunc) == 127); CHECK(next_glyph(&trunc) == 0);
    memset(g_px, 0, sizeof g_px);
    CHECK(dpk_text(g_px, 100, 20, 100, 0, 0, 1, 0xffffff, "AB\xc3\xa9", 0) == 3);
    CHECK(count_rgb(100, 20, 0xffffff, 0, 0, 100, 20) > 20);
    CHECK(dpk_text(g_px, 100, 20, 100, -50, -50, 3, 0xffffff, "clipped away", 0) == 12);  /* off the canvas: drawn nowhere, no crash */
    CHECK(dpk_text(g_px, 100, 20, 100, 0, 0, 1, 0xffffff, "abcdef", 2) == 2);
    SECTION("wrap: at spaces, at newlines, long words cut, never more lines than asked");
    const char *ls[16]; int ll[16];
    int n = wrap("one two three four", 9, ls, ll, 16);
    CHECK(n == 3 && ll[0] == 7 && !strncmp(ls[0], "one two", 7) && ll[1] == 5 && !strncmp(ls[1], "three", 5) && !strcmp(ls[2], "four"));
    n = wrap("abcdefghijklmnop", 5, ls, ll, 16);
    CHECK(n == 4 && ll[0] == 5 && ll[3] == 1);
    n = wrap("a\n\nb", 10, ls, ll, 16);
    CHECK(n == 3 && ll[1] == 0);
    CHECK(wrap("x x x x x x x x", 1, ls, ll, 3) == 3);
    CHECK(wrap("", 10, ls, ll, 16) == 0);
}

/* ================================================================================================ screens and choices */
static void t_status(void) {
    dpk_state s;
    SECTION("status screen: first start (nothing configured): choose / check again / quit; drawn at several sizes");
    fresh();
    was_check();
    CHECK(g_was.status == DP_NOT_CONFIGURED);
    dpk_init(&s, &g_was, 0, 0);
    CHECK(!strcmp(s.expect, "13.0.4"));
    CHECK(s.menu_n == 3 && s.menu[0] == DPK_M_DATA && s.menu[1] == DPK_M_RETRY && s.menu[2] == DPK_M_QUIT);
    shot(&s, "01_first_start", CW, CH);
    CHECK(count_rgb(CW, CH, C_TITLEBG, 0, 0, CW, 30) > CW * 10);          /* the title bar */
    CHECK(count_rgb(CW, CH, C_ORANGE, 0, 0, 200, 30) > 50);              /* "SSBU native" */
    CHECK(count_rgb(CW, CH, C_AMBER, 0, 36, CW, 70) > 50);               /* the headline */
    CHECK(count_rgb(CW, CH, C_TEXT, 0, 70, CW, 200) > 300);              /* the message */
    CHECK(s.area.count == 3 && s.area.row_h == 17 && s.area.y > 200);
    CHECK(count_rgb(CW, CH, C_NAV, 0, s.area.y, CW, s.area.y + 17) > 100);   /* the selected entry's frame */
    shot(&s, "01_first_start_1280x800", 1280, 800);
    shot(&s, "01_first_start_tiny", 200, 120);                           /* too small to be useful, but no crash */
    dpk_do(&s, DPK_DOWN); dpk_do(&s, DPK_DOWN); dpk_do(&s, DPK_DOWN);
    CHECK(s.menu_sel == 2);
    dpk_do(&s, DPK_UP); dpk_do(&s, DPK_FIRST);
    CHECK(s.menu_sel == 0);
    dpk_do(&s, DPK_BACK);                                                /* nothing to go back to */
    CHECK(s.screen == DPK_SCR_STATUS && !s.result);
    dpk_do(&s, DPK_DOWN);
    dpk_do(&s, DPK_CONFIRM);                                             /* check again, nothing chosen yet */
    CHECK(strstr(s.note, "No folder chosen yet") != NULL);
    dpk_do(&s, DPK_LAST); dpk_do(&s, DPK_CONFIRM);
    CHECK(s.result == DPK_QUITTED);
    dpk_free(&s);
    SECTION("status screen: a wrong version / missing fonts / --pick-data each get their own headline and entries");
    make_dump(P("home/Games/dump_good"), 0x39c7e90, 1);
    make_dump(P("home/Games/dump_new"), 0x39c8440, 1);
    setenv("SSBU_DATA", P("home/Games/dump_new"), 1);
    was_check();
    CHECK(g_was.status == DP_WRONG_VERSION);
    dpk_init(&s, &g_was, 0, 0);
    CHECK(!strcmp(dpk_headline(&s.now, 0), "Wrong game version for this build"));
    CHECK(s.menu[0] == DPK_M_DATA && has_menu(&s, DPK_M_FONTS) < 0 && has_menu(&s, DPK_M_START) < 0);
    shot(&s, "02_wrong_version", CW, CH);
    CHECK(count_rgb(CW, CH, C_RED, 0, 36, CW, 70) > 50);
    dpk_free(&s);
    unsetenv("SSBU_DATA");
    setenv("SSBU_DATA", P("home/Games/dump_good"), 1);
    was_check();
    CHECK(g_was.status == DP_OK);
    dpk_init(&s, &g_was, 1, 0);
    CHECK(s.pick_mode && s.menu[0] == DPK_M_START && has_menu(&s, DPK_M_FONTS) > 0 && has_menu(&s, DPK_M_RETRY) < 0);
    shot(&s, "03_pick_data", CW, CH);
    dpk_do(&s, DPK_CONFIRM);
    CHECK(s.result == DPK_KEEP);
    dpk_free(&s);
    dpk_init(&s, &g_was, 0, 0);                                          /* not asked for: no "Start the game" */
    CHECK(!s.pick_mode && has_menu(&s, DPK_M_START) < 0);
    dpk_do(&s, DPK_QUIT);
    CHECK(s.result == DPK_QUITTED);
    dpk_do(&s, DPK_CONFIRM);                                             /* nothing happens after the end */
    CHECK(s.result == DPK_QUITTED);
    dpk_free(&s);
    unsetenv("SSBU_DATA");
    SECTION("status screen: a long message scrolls (PgDn / L R) and keeps the menu visible");
    fresh();
    was_check();
    dpk_init(&s, &g_was, 0, 0);
    static char longmsg[3072];
    for (int i = 0; i < 60; i++) { size_t l = strlen(longmsg); snprintf(longmsg + l, sizeof longmsg - l, "line %d of a long message\n", i); }
    snprintf(s.now.message, sizeof s.now.message, "%s", longmsg);
    shot(&s, "04_long_message", CW, CH);
    CHECK(s.msg_lines == 60);
    dpk_do(&s, DPK_PAGE_DOWN); dpk_do(&s, DPK_PAGE_DOWN);
    CHECK(s.msg_scroll == 6);
    for (int i = 0; i < 40; i++) dpk_do(&s, DPK_PAGE_DOWN);
    shot(&s, "04_long_message_end", CW, CH);
    CHECK(s.msg_scroll > 6 && s.msg_scroll < 60);
    dpk_wheel(&s, 1);
    CHECK(s.msg_scroll >= 0);
    CHECK(s.area.count == 3 && s.area.y + 3 * 17 < CH);
    dpk_free(&s);
}

static void t_browse(void) {
    dpk_state s;
    SECTION("browser: first start -> home; typed letters, DOWN/UP, open, up; hidden names left out; tags");
    fresh();
    rmrf(P("home"));
    make_dump(P("home/Games/dump_good"), 0x39c7e90, 1);
    make_dump(P("home/Games/dump_new"), 0x39c8440, 1);
    make_dump(P("home/Games/RomFS_Only_PARENT"), 0, 1);
    put(P("home/Games/pkgs/SSBU.xci"), "X", 1);
    put(P("home/Games/pkgs/update.NSP"), "X", 1);
    put(P("home/Games/pkgs/readme.txt"), "X", 1);
    mkdirs(P("home/Documents"));
    mkdirs(P("home/.cache"));
    put(P("home/notes.txt"), "n", 1);
    symlink("/nonexistent/target", P("home/dangling"));
    was_check();
    dpk_init(&s, &g_was, 0, 0);
    dpk_do(&s, DPK_CONFIRM);                                             /* "Choose the game data folder" */
    CHECK(s.screen == DPK_SCR_BROWSE && s.target == DPK_FOR_DATA);
    CHECK(!strcmp(s.cwd, real(P("home"))));
    CHECK(s.ent[0].kind == DPK_E_USE && s.ent[1].kind == DPK_E_PLACES && s.ent[2].kind == DPK_E_UP);
    CHECK(s.n == 6 && find_entry(&s, ".cache") < 0 && find_entry(&s, "dangling") < 0);
    CHECK(!strcmp(s.ent[3].name, "Documents") && !strcmp(s.ent[4].name, "Games") && s.ent[5].kind == DPK_E_FILE);
    CHECK(s.sel == 3);                                                   /* the first folder */
    shot(&s, "05_browse_home", CW, CH);
    dpk_typed(&s, "g", 1000);
    CHECK(s.sel == 4);
    dpk_do(&s, DPK_CONFIRM);
    CHECK(!strcmp(s.cwd, real(P("home/Games"))));
    int i_good = find_entry(&s, "dump_good"), i_new = find_entry(&s, "dump_new"), i_pk = find_entry(&s, "pkgs");
    CHECK(i_good > 0 && s.ent[i_good].tag == DPK_TAG_DUMP && s.ent[i_new].tag == DPK_TAG_DUMP && s.ent[i_pk].tag == DPK_TAG_NONE);
    shot(&s, "06_browse_games", CW, CH);
    CHECK(count_rgb(CW, CH, C_GREEN, CW / 2, 80, CW, 300) > 50);         /* the "game data" tags */
    go_to(&s, "pkgs");
    dpk_do(&s, DPK_RIGHT);                                               /* right = open, as A */
    CHECK(!strcmp(s.cwd, real(P("home/Games/pkgs"))));
    int i_x = find_entry(&s, "SSBU.xci"), i_n = find_entry(&s, "update.NSP"), i_t = find_entry(&s, "readme.txt");
    CHECK(s.ent[i_x].tag == DPK_TAG_PACKAGE && s.ent[i_n].tag == DPK_TAG_PACKAGE && s.ent[i_t].tag == DPK_TAG_NONE);
    CHECK(s.sel == 2);                                                   /* no folder here: ".." */
    go_to(&s, "SSBU.xci");
    dpk_do(&s, DPK_CONFIRM);                                             /* a file: what to do with it */
    CHECK(s.screen == DPK_SCR_BROWSE && strstr(s.note, "dump_folder.py"));
    shot(&s, "07_browse_packages", CW, CH);
    SECTION("browser: a folder of packages, the other build, a RomFS folder: refused with the reason, nothing saved");
    dpk_do(&s, DPK_USE_HERE);
    CHECK(s.screen == DPK_SCR_STATUS && s.now.status == DP_PACKAGES_ONLY && !s.result);
    CHECK(!strcmp(cfg("data"), "-"));
    CHECK(s.menu[0] == DPK_M_DATA && has_menu(&s, DPK_M_RETRY) > 0);
    shot(&s, "08_packages_only", CW, CH);
    dpk_do(&s, DPK_CONFIRM);                                             /* again: back where it was */
    CHECK(!strcmp(s.cwd, real(P("home/Games/pkgs"))));
    dpk_do(&s, DPK_BACK);                                                /* up, the folder we came from selected */
    CHECK(!strcmp(s.cwd, real(P("home/Games"))) && s.sel == find_entry(&s, "pkgs"));
    go_to(&s, "dump_new");
    dpk_do(&s, DPK_CONFIRM);
    CHECK(s.sel == 0 && wanted_tag(&s, s.cwd_tag));                      /* inside a dump: "Use this folder" first */
    shot(&s, "09_inside_dump", CW, CH);
    dpk_do(&s, DPK_CONFIRM);
    CHECK(s.now.status == DP_WRONG_VERSION && strstr(s.now.message, "13.0.5") && !strcmp(cfg("data"), "-"));
    dpk_do(&s, DPK_CONFIRM);                                             /* starts beside the dump that failed, it selected */
    CHECK(!strcmp(s.cwd, real(P("home/Games"))) && s.sel == find_entry(&s, "dump_new"));
    go_to(&s, "RomFS_Only_PARENT");
    dpk_do(&s, DPK_CONFIRM);
    go_to(&s, "romfs");
    CHECK(s.ent[s.sel].tag == DPK_TAG_ROMFS);
    SECTION("browser: the good dump -> checked, saved, done (the RomFS folder itself is accepted too)");
    dpk_do(&s, DPK_CONFIRM);
    dpk_do(&s, DPK_USE_HERE);
    CHECK(s.result == DPK_PICKED);
    CHECK(!strcmp(cfg("data"), real(P("home/Games/RomFS_Only_PARENT/romfs"))));
    dpk_free(&s);
    fresh();
    was_check();
    dpk_init(&s, &g_was, 0, 0);
    dpk_do(&s, DPK_CONFIRM);
    dpk_typed(&s, "G", 5000);
    dpk_do(&s, DPK_CONFIRM);
    dpk_typed(&s, "d", 9000); dpk_typed(&s, "u", 9100); dpk_typed(&s, "mp_g", 9200);
    CHECK(s.sel == find_entry(&s, "dump_good"));
    dpk_typed(&s, "zz", 9300);                                           /* nothing starts with it: the selection stays */
    CHECK(s.sel == find_entry(&s, "dump_good"));
    dpk_do(&s, DPK_CONFIRM);
    dpk_do(&s, DPK_CONFIRM);
    CHECK(s.result == DPK_PICKED && !strcmp(s.data_dir, real(P("home/Games/dump_good"))));
    CHECK(!strcmp(cfg("data"), real(P("home/Games/dump_good"))));
    dp_result r;
    CHECK(dp_check("13.0.4", &r) && r.source == DP_SRC_CONFIG);         /* the next start finds it */
    dpk_free(&s);
    SECTION("browser: hidden names on request, unreadable folders and \"/\" handled, paging and wrap-free ends");
    fresh();
    was_check();
    dpk_init(&s, &g_was, 0, 0);
    dpk_do(&s, DPK_CONFIRM);
    dpk_do(&s, DPK_HIDDEN);
    CHECK(s.show_hidden && find_entry(&s, ".cache") > 0 && strstr(s.note, "shown"));
    dpk_do(&s, DPK_HIDDEN);
    CHECK(!s.show_hidden && find_entry(&s, ".cache") < 0);
    if (geteuid() != 0) {
        mkdirs(P("home/locked/inner"));
        chmod(P("home/locked"), 0);
        dpk_browse(&s, P("home"));
        go_to(&s, "locked");
        dpk_do(&s, DPK_CONFIRM);
        CHECK(!strcmp(s.cwd, real(P("home"))) && strstr(s.note, "cannot be opened"));
        chmod(P("home/locked"), 0700);
    }
    CHECK(dpk_browse(&s, "/"));
    CHECK(s.ent[2].kind != DPK_E_UP);                                    /* no ".." at the top */
    dpk_do(&s, DPK_BACK);                                                /* B at "/" -> the start screen */
    CHECK(s.screen == DPK_SCR_STATUS);
    for (int i = 0; i < 300; i++) { char p[64]; snprintf(p, sizeof p, "home/many/d%03d", i); mkdirs(P("%s", p)); }
    CHECK(dpk_browse(&s, P("home/many")));
    s.screen = DPK_SCR_BROWSE;
    shot(&s, "10_many", CW, CH);
    int rows = s.list_rows;
    CHECK(rows > 10 && rows < 40);
    dpk_do(&s, DPK_PAGE_DOWN);
    CHECK(s.sel == 3 + rows - 1 && s.top >= 1);
    dpk_do(&s, DPK_LAST);
    CHECK(s.sel == s.n - 1 && s.top == s.n - rows);
    dpk_do(&s, DPK_DOWN);
    CHECK(s.sel == s.n - 1);
    dpk_do(&s, DPK_FIRST);
    CHECK(s.sel == 0 && s.top == 0);
    shot(&s, "10_many_top", CW, CH);
    dpk_free(&s);
}

static void t_fonts(void) {
    dpk_state s;
    SECTION("fonts missing: the data are saved, \"choose the fonts folder\" comes first; a folder without fonts is refused");
    fresh();
    make_dump(P("home/Games/dump_nofonts"), 0x39c7e90, 0);
    put_fonts(P("home/Fonts/firmware_fonts"));
    put(P("home/Fonts/partial/nintendo_ext_003.ttf"), "F", 1);
    put_fonts(P("home/Fonts/zero"));
    put(P("home/Fonts/zero/nintendo_ext2_003.ttf"), "", 0);              /* empty: not a font */
    setenv("SSBU_DATA", P("home/Games/dump_nofonts"), 1);
    was_check();
    unsetenv("SSBU_DATA");
    CHECK(g_was.status == DP_MISSING_FONTS);
    dpk_init(&s, &g_was, 0, 0);
    CHECK(s.menu[0] == DPK_M_FONTS && s.menu[1] == DPK_M_DATA);
    shot(&s, "11_fonts_missing", CW, CH);
    dpk_do(&s, DPK_CONFIRM);
    CHECK(s.screen == DPK_SCR_BROWSE && s.target == DPK_FOR_FONTS);
    CHECK(!strcmp(s.cwd, real(P("home/Games/dump_nofonts/sysdata/fonts"))));   /* where they should have been */
    shot(&s, "12_fonts_browse", CW, CH);
    CHECK(dpk_browse(&s, P("home/Fonts")));
    CHECK(s.ent[find_entry(&s, "firmware_fonts")].tag == DPK_TAG_FONTS && s.ent[find_entry(&s, "partial")].tag == DPK_TAG_NONE);
    CHECK(s.ent[find_entry(&s, "zero")].tag == DPK_TAG_NONE);
    go_to(&s, "partial");
    dpk_do(&s, DPK_CONFIRM);
    dpk_do(&s, DPK_USE_HERE);
    CHECK(s.screen == DPK_SCR_BROWSE && strstr(s.note, "No system fonts") && strstr(s.note, "nintendo_udsg-r_std_003.ttf"));
    CHECK(!strcmp(cfg("fonts"), "-"));
    SECTION("fonts: the player's fonts folder -> saved as \"fonts\", the data checked again -> done; the data were saved too");
    dpk_do(&s, DPK_BACK);
    go_to(&s, "firmware_fonts");
    dpk_do(&s, DPK_CONFIRM);
    dpk_do(&s, DPK_CONFIRM);                                             /* "Use this folder", selected first */
    CHECK(s.result == DPK_PICKED);
    CHECK(!strcmp(cfg("fonts"), real(P("home/Fonts/firmware_fonts"))));
    CHECK(!strcmp(cfg("data"), real(P("home/Games/dump_nofonts"))));
    dp_result r;
    dp_picked(&g_was);
    CHECK(dp_check("13.0.4", &r) && r.fonts_source == DP_SRC_CONFIG);
    dpk_free(&s);
    SECTION("fonts: a whole dump (its sysdata/fonts) is taken as the fonts folder too");
    fresh();
    make_dump(P("home/Games/dump_good"), 0x39c7e90, 1);
    setenv("SSBU_DATA", P("home/Games/dump_nofonts"), 1);
    was_check();
    unsetenv("SSBU_DATA");
    dpk_init(&s, &g_was, 0, 0);
    s.target = DPK_FOR_FONTS;
    dpk_choose(&s, P("home/Games/dump_good"));
    CHECK(s.result == DPK_PICKED && !strcmp(cfg("fonts"), real(P("home/Games/dump_good/sysdata/fonts"))));
    dpk_free(&s);
    SECTION("fonts: choosing a data folder without fonts saves it and asks for the fonts next");
    fresh();
    was_check();
    dpk_init(&s, &g_was, 0, 0);
    dpk_choose(&s, P("home/Games/dump_nofonts"));
    CHECK(!s.result && s.now.status == DP_MISSING_FONTS && s.menu[0] == DPK_M_FONTS && strstr(s.note, "is saved"));
    CHECK(!strcmp(cfg("data"), real(P("home/Games/dump_nofonts"))));
    s.target = DPK_FOR_FONTS;
    dpk_choose(&s, P("home/Fonts/firmware_fonts"));
    CHECK(s.result == DPK_PICKED);
    dpk_free(&s);
    SECTION("fonts: a saved fonts folder that is gone is forgotten when the chosen dump has its own");
    fresh();
    CHECK(dp_config_set("fonts", P("home/Fonts/gone")) == 0);
    was_check();
    dpk_init(&s, &g_was, 0, 0);
    dpk_choose(&s, P("home/Games/dump_good"));
    CHECK(s.result == DPK_PICKED && !strcmp(cfg("fonts"), "-"));
    dpk_free(&s);
    fresh();
    CHECK(dp_config_set("fonts", P("home/Fonts/gone")) == 0);            /* ... but kept when the dump has none either */
    was_check();
    dpk_init(&s, &g_was, 0, 0);
    dpk_choose(&s, P("home/Games/dump_nofonts"));
    CHECK(s.now.status == DP_MISSING_FONTS && !strcmp(cfg("fonts"), P("home/Fonts/gone")));
    dpk_free(&s);
}

static void t_datapaths(void) {
    dp_result was, r;
    SECTION("dp_validate_picked: checks as boot.c will after dp_picked(was); the caller's --fonts / SSBU_FONTS are restored");
    fresh();
    make_dump(P("home/Games/dump_good"), 0x39c7e90, 1);
    make_dump(P("home/Games/dump_nofonts"), 0x39c7e90, 0);
    setenv("SSBU_FONTS", P("home/Games/dump_nofonts/sysdata/fonts"), 1);   /* exported for the folder that failed */
    setenv("SSBU_DATA", P("home/Games/dump_nofonts"), 1);
    dp_check("13.0.4", &was);
    CHECK(was.status == DP_MISSING_FONTS);
    CHECK(!dp_validate(P("home/Games/dump_good"), "13.0.4", &r) && r.status == DP_MISSING_FONTS);   /* the plain check sees the bad fonts */
    CHECK(dp_validate_picked(P("home/Games/dump_good"), "13.0.4", &was, &r) && r.fonts_source == DP_SRC_DUMP);
    CHECK(getenv("SSBU_FONTS") && !strcmp(getenv("SSBU_FONTS"), P("home/Games/dump_nofonts/sysdata/fonts")));
    dp_set_cli(DP_FONTS, P("home/Games/dump_nofonts/sysdata/fonts"));
    unsetenv("SSBU_FONTS");
    dp_check("13.0.4", &was);
    CHECK(was.status == DP_MISSING_FONTS && was.fonts_source == DP_SRC_CLI);
    CHECK(dp_validate_picked(P("home/Games/dump_good"), "13.0.4", &was, &r));
    CHECK(!dp_validate(P("home/Games/dump_good"), "13.0.4", &r));       /* --fonts is back */
    SECTION("dp_validate_picked: fonts the player named that work are kept (usable data, --pick-data)");
    put_fonts(P("home/Fonts/mine"));
    dp_set_cli(DP_FONTS, P("home/Fonts/mine"));
    dp_check("13.0.4", &was);
    CHECK(was.status == DP_OK && was.fonts_source == DP_SRC_CLI);
    CHECK(dp_validate_picked(P("home/Games/dump_nofonts"), "13.0.4", &was, &r) && !strcmp(r.fonts, real(P("home/Fonts/mine"))));
    dp_set_cli(DP_FONTS, NULL);
    unsetenv("SSBU_DATA");
    SECTION("dp_fonts_check: all 7, not empty, case-insensitive names; the first one missing is named");
    char miss[128];
    CHECK(dp_fonts_check(P("home/Fonts/mine"), miss, sizeof miss) && !miss[0]);
    CHECK(!dp_fonts_check(P("home/Games/dump_nofonts/sysdata/fonts"), miss, sizeof miss) && !strcmp(miss, FONTS[0]));
    CHECK(!dp_fonts_check(NULL, miss, sizeof miss) && !dp_fonts_check("", NULL, 0));
    put_fonts(P("home/Fonts/UPPER"));
    rename(P("home/Fonts/UPPER/nintendo_ext_003.ttf"), P("home/Fonts/UPPER/NINTENDO_EXT_003.TTF"));
    CHECK(dp_fonts_check(P("home/Fonts/UPPER"), NULL, 0));
    SECTION("--pick-data / SSBU_PICK_DATA: taken out of argv, asked for");
    char a0[] = "ssbu_native", a1[] = "--pick-data", a2[] = "main.elf";
    char *argv[] = {a0, a1, a2, NULL};
    int argc = 3;
    CHECK(dp_take_args(&argc, argv) == 0 && argc == 2 && !strcmp(argv[1], "main.elf") && dp_pick_requested());
    char *argv2[] = {a0, a2, NULL};
    argc = 2;
    CHECK(dp_take_args(&argc, argv2) == 0 && !dp_pick_requested());
    setenv("SSBU_PICK_DATA", "1", 1);
    CHECK(dp_pick_requested());
    setenv("SSBU_PICK_DATA", "0", 1);
    CHECK(!dp_pick_requested());
    unsetenv("SSBU_PICK_DATA");
}

static void t_input(void) {
    dpk_state s;
    SECTION("pad: a held direction repeats after 400 ms every 70 ms, until released");
    fresh();
    rmrf(P("home/many"));
    for (int i = 0; i < 50; i++) { char p[64]; snprintf(p, sizeof p, "home/many/d%03d", i); mkdirs(P("%s", p)); }
    was_check();
    dpk_init(&s, &g_was, 0, 0);
    dpk_do(&s, DPK_CONFIRM);
    CHECK(dpk_browse(&s, P("home/many")));
    int at = s.sel;
    dpk_press(&s, DPK_DOWN, 1000);
    CHECK(s.sel == at + 1 && s.device == DPK_DEV_PAD);
    dpk_tick(&s, 1399);
    CHECK(s.sel == at + 1);
    dpk_tick(&s, 1400); dpk_tick(&s, 1469);
    CHECK(s.sel == at + 2);
    dpk_tick(&s, 1470);
    CHECK(s.sel == at + 3);
    dpk_release(&s, DPK_UP);                                              /* another button's release: still held */
    dpk_tick(&s, 1540);
    CHECK(s.sel == at + 4);
    dpk_release(&s, DPK_DOWN);
    shot(&s, "13_pad_hints", CW, CH);
    CHECK(count_rgb(CW, CH, C_SEP, 0, CH - 19, 200, CH) == 0);          /* face-button diamonds, no key boxes */
    dpk_tick(&s, 5000);
    CHECK(s.sel == at + 4);
    dpk_press(&s, DPK_CONFIRM, 6000);                                     /* A does not repeat */
    dpk_tick(&s, 9000);
    CHECK(s.held == DPK_NONE);
    SECTION("mouse: moving over a row selects it, a click opens it, a right click goes back, the wheel scrolls");
    CHECK(dpk_browse(&s, P("home/many")));
    shot(&s, "13_mouse", CW, CH);
    const dpk_list_area *a = &s.area;
    CHECK(a->row_h > 0 && a->count == s.n);
    dpk_pointer(&s, a->x + 20, a->y + 5 * a->row_h + 2, 0);
    CHECK(s.sel == 5 && s.device == DPK_DEV_PAD);                         /* moving alone keeps the hints */
    dpk_pointer(&s, a->x + 20, a->y + 5 * a->row_h + 2, 1);
    CHECK(!strcmp(s.cwd, real(P("home/many/d002"))) && s.device == DPK_DEV_KEYS);
    dpk_pointer(&s, 3, 3, 2);
    CHECK(!strcmp(s.cwd, real(P("home/many"))));
    dpk_pointer(&s, a->x + 20, a->y - 5, 1);                              /* outside the list: nothing */
    CHECK(!strcmp(s.cwd, real(P("home/many"))));
    shot(&s, "13_mouse", CW, CH);
    int top = s.top;
    dpk_wheel(&s, -1);
    CHECK(s.sel >= 3 && s.top >= top);
    dpk_do(&s, DPK_FIRST);
    shot(&s, "13_mouse", CW, CH);
    dpk_pointer(&s, a->x + 20, a->y + 2, 1);                              /* the first row: "Use this folder" */
    CHECK(s.screen == DPK_SCR_STATUS && s.now.status == DP_MISSING_ROMFS);
    SECTION("mouse: every command is a row (use this folder, places, up) - no keyboard or pad needed");
    CHECK(dpk_browse(&s, P("home/many")));
    s.screen = DPK_SCR_BROWSE;
    shot(&s, "13_mouse", CW, CH);
    dpk_pointer(&s, s.area.x + 20, s.area.y + 1 * s.area.row_h + 1, 1);  /* "Places" */
    CHECK(s.screen == DPK_SCR_PLACES);
    shot(&s, "14_places", CW, CH);
    dpk_pointer(&s, s.area.x + 20, s.area.y + (s.nplaces - 1) * s.area.row_h + 1, 1);   /* "Back to the start screen" */
    CHECK(s.screen == DPK_SCR_STATUS);
    shot(&s, "14_status_again", CW, CH);
    dpk_pointer(&s, s.area.x + 20, s.area.y + 2 * 17 + 3, 1);             /* "Quit" */
    CHECK(s.result == DPK_QUITTED);
    dpk_free(&s);
}

static void t_places(void) {
    dpk_state s;
    SECTION("places: home, the drives (/run/media/<user>/<label> and /run/media/<label>), saved folders, the build, /");
    fresh();
    mkdirs(P("media/SDCARD"));
    mkdirs(P("media/tester/USB_STICK"));
    mkdirs(P("media/other_user"));
    make_dump(P("home/Games/dump_good"), 0x39c7e90, 1);
    mkdirs(P("build/work"));
    dp_set_base(P("build/work"));
    CHECK(dp_save_choice(P("home/Games/dump_good")) == 0);
    setenv("SSBU_DATA", P("home/Games"), 1);
    was_check();
    unsetenv("SSBU_DATA");
    dpk_init(&s, &g_was, 0, 0);
    dpk_open_browser(&s, DPK_FOR_DATA);
    dpk_do(&s, DPK_PLACES);
    CHECK(s.screen == DPK_SCR_PLACES);
    int sd = -1, usb = -1, saved = -1, other = -1, build = -1, root = -1;
    for (int i = 0; i < s.nplaces; i++) {
        if (!strcmp(s.place[i].label, "Drive: SDCARD")) sd = i;
        if (!strcmp(s.place[i].label, "Drive: USB_STICK")) usb = i;
        if (!strcmp(s.place[i].label, "Drive: other_user")) other = i;
        if (!strcmp(s.place[i].label, "Saved game data folder")) saved = i;
        if (!strcmp(s.place[i].label, "Folder of this game build")) build = i;
        if (!strcmp(s.place[i].path, "/")) root = i;
    }
    CHECK(!strcmp(s.place[0].label, "Home") && sd > 0 && usb > 0 && other > 0 && saved > usb && build > saved && root > build);
    CHECK(!strcmp(s.place[build].path, real(P("build"))));
    CHECK(!strcmp(s.place[s.nplaces - 1].label, "Back to the start screen"));
    shot(&s, "15_places", CW, CH);
    while (s.place_sel != usb) dpk_do(&s, DPK_DOWN);
    dpk_do(&s, DPK_CONFIRM);
    CHECK(s.screen == DPK_SCR_BROWSE && !strcmp(s.cwd, real(P("media/tester/USB_STICK"))));
    dpk_do(&s, DPK_PLACES);
    dpk_do(&s, DPK_BACK);                                                 /* B in places: back to the browser */
    CHECK(s.screen == DPK_SCR_BROWSE);
    dpk_do(&s, DPK_PLACES);
    while (s.place_sel != saved) dpk_do(&s, DPK_DOWN);
    dpk_do(&s, DPK_CONFIRM);
    CHECK(s.sel == 0);                                                    /* the saved dump: "Use this folder" first */
    dpk_do(&s, DPK_CONFIRM);
    CHECK(s.result == DPK_PICKED);
    dpk_free(&s);
    dp_set_base(NULL);
}

static void t_dialog(void) {
    dpk_state s;
    const char *start;
    dpk_target tg;
    int id, id2;
    SECTION("system dialog (Desktop Mode): requested once, starts beside the last folder; cancel -> start screen");
    fresh();
    make_dump(P("home/Games/dump_good"), 0x39c7e90, 1);
    setenv("SSBU_DATA", P("home/Games/dump_good"), 1);
    put(P("home/Games/dump_good/exefs/main"), "not an NSO", 10);          /* an unknown build */
    was_check();
    unsetenv("SSBU_DATA");
    CHECK(g_was.status == DP_UNKNOWN_VERSION);
    dpk_init(&s, &g_was, 0, 1);
    CHECK(s.menu[0] == DPK_M_DATA && s.menu[1] == DPK_M_DATA_HERE);
    shot(&s, "16_desktop_menu", CW, CH);
    CHECK(!dpk_dialog_take(&s, &tg, &start, &id));
    dpk_do(&s, DPK_CONFIRM);
    CHECK(s.screen == DPK_SCR_DIALOG);
    shot(&s, "17_dialog_wait", CW, CH);
    CHECK(dpk_dialog_take(&s, &tg, &start, &id) && tg == DPK_FOR_DATA && !strcmp(start, real(P("home/Games/dump_good"))));
    CHECK(!dpk_dialog_take(&s, &tg, &start, &id2));
    dpk_dialog_done(&s, id, NULL, 0);
    CHECK(s.screen == DPK_SCR_STATUS && !s.result);
    SECTION("system dialog: an answer that is not the latest is ignored; B while it is open -> the in-game browser");
    dpk_do(&s, DPK_CONFIRM);
    CHECK(dpk_dialog_take(&s, &tg, &start, &id2) && id2 == id + 1);
    dpk_dialog_done(&s, id, P("home/Games/dump_good"), 0);
    CHECK(s.screen == DPK_SCR_DIALOG && !s.result);
    dpk_do(&s, DPK_BACK);
    CHECK(s.screen == DPK_SCR_BROWSE);
    dpk_dialog_done(&s, id2, P("home/Games/dump_good"), 0);              /* too late */
    CHECK(s.screen == DPK_SCR_BROWSE && !s.result);
    dpk_do(&s, DPK_BACK); dpk_do(&s, DPK_BACK); dpk_do(&s, DPK_BACK);
    for (int i = 0; i < 64 && s.screen != DPK_SCR_STATUS; i++) dpk_do(&s, DPK_BACK);
    CHECK(s.screen == DPK_SCR_STATUS);
    SECTION("system dialog: it failed (no portal, no zenity) -> in-game browser from then on");
    s.menu_sel = 0;
    dpk_do(&s, DPK_CONFIRM);
    CHECK(dpk_dialog_take(&s, &tg, &start, &id));
    dpk_dialog_done(&s, id, NULL, 1);
    CHECK(s.screen == DPK_SCR_BROWSE && !s.dialog_ok && strstr(s.note, "could not be opened"));
    CHECK(has_menu(&s, DPK_M_DATA_HERE) < 0);
    SECTION("system dialog: a folder chosen there is checked like one chosen here");
    put_nso(P("home/Games/dump_good/exefs/main"), 0x39c7e90);
    s.dialog_ok = 1;
    s.screen = DPK_SCR_STATUS;
    build_menu(&s);
    dpk_do(&s, DPK_CONFIRM);
    CHECK(dpk_dialog_take(&s, &tg, &start, &id));
    dpk_dialog_done(&s, id, P("home/Games"), 0);                          /* not a dump: the reason, no choice saved */
    CHECK(s.screen == DPK_SCR_STATUS && s.now.status == DP_MISSING_ROMFS && !s.result);
    dpk_do(&s, DPK_CONFIRM);
    CHECK(dpk_dialog_take(&s, &tg, &start, &id));
    CHECK(!strcmp(start, real(P("home/Games"))));
    dpk_dialog_done(&s, id, P("home/Games/dump_good"), 0);
    CHECK(s.result == DPK_PICKED && !strcmp(cfg("data"), real(P("home/Games/dump_good"))));
    dpk_free(&s);
}

static void t_save_fail(void) {
    dpk_state s;
    SECTION("the choice cannot be saved (config folder is a file): said so, the picker stays open");
    fresh();
    make_dump(P("home/Games/dump_good"), 0x39c7e90, 1);
    put(P("xdg/config/ssbu-recomp"), "file, not a folder", 18);
    was_check();
    dpk_init(&s, &g_was, 0, 0);
    dpk_choose(&s, P("home/Games/dump_good"));
    CHECK(!s.result && s.screen == DPK_SCR_STATUS && strstr(s.note, "could not be saved"));
    shot(&s, "18_save_failed", CW, CH);
    CHECK(count_rgb(CW, CH, C_RED, 0, 200, CW, CH) > 30);
    dpk_free(&s);
}

/* ---- review regressions (WP-U5b review) */
static void t_review(void) {
    dpk_state s;
    SECTION("review: a mouse event after a key changed the screen (one event batch, no frame between) picks no stale row");
    fresh();
    for (int i = 0; i < 300; i++) mkdirs(P("home/stale/d%03d", i));
    was_check();
    dpk_init(&s, &g_was, 0, 0);
    CHECK(dpk_browse(&s, P("home/stale")));
    s.screen = DPK_SCR_BROWSE;
    dpk_do(&s, DPK_LAST);
    dpk_render(&s, g_px, CW, CH, CW);
    CHECK(s.area.first > DPK_MAX_PLACES);
    dpk_do(&s, DPK_PLACES);                                               /* Tab / Y */
    CHECK(s.screen == DPK_SCR_PLACES);
    dpk_pointer(&s, s.area.x + 5, s.area.y + 2, 0);                       /* the mouse moved, before the next frame */
    CHECKF(s.place_sel >= 0 && s.place_sel < s.nplaces, "place_sel %d of %d", s.place_sel, s.nplaces);
    if (s.place_sel >= 0 && s.place_sel < s.nplaces) {
        dpk_do(&s, DPK_CONFIRM);                                          /* Enter: the first place, home */
        CHECK(s.screen == DPK_SCR_BROWSE && !strcmp(s.cwd, real(P("home"))));
    }
    dpk_render(&s, g_px, CW, CH, CW);
    dpk_pointer(&s, s.area.x + 5, s.area.y + 2, 1);                       /* a click on the first row of home: "Use this folder" */
    CHECK(s.screen == DPK_SCR_STATUS);
    dpk_pointer(&s, s.area.x + 5, s.area.y + 2, 1);                       /* a second click in the same batch: the browser's area */
    CHECK(s.screen == DPK_SCR_STATUS && !s.result);
    dpk_free(&s);

    SECTION("review: more than 4096 files in a folder do not hide its folders (readdir's order is not the sorted one)");
    mkdirs(P("home/full"));
    for (int i = 0; i < 3 * DPK_MAX_ENTRIES; i++) { int fd = open(P("home/full/f%05d", i), O_CREAT | O_WRONLY, 0644); if (fd >= 0) close(fd); }
    for (int i = 0; i < 12; i++) mkdirs(P("home/full/dir%02d", i));
    make_dump(P("home/full/zz_dump"), 0x39c7e90, 1);
    dpk_init(&s, &g_was, 0, 0);
    CHECK(dpk_browse(&s, P("home/full")));
    int dirs = 0, files = 0;
    for (int i = 0; i < s.n; i++) { dirs += s.ent[i].kind == DPK_E_DIR; files += s.ent[i].kind == DPK_E_FILE; }
    CHECKF(dirs == 13 && find_entry(&s, "zz_dump") >= 0, "%d of 13 folders listed", dirs);
    CHECK(files == DPK_MAX_ENTRIES && s.truncated);
    shot(&s, "19_full_folder", CW, CH);
    dpk_free(&s);

    SECTION("review: --pick-data, the new folder cannot be saved: the screen shows the data the game will start with");
    fresh();
    make_dump(P("home/Games/dump_a"), 0x39c7e90, 1);
    make_dump(P("home/Games/dump_b"), 0x39c7e90, 1);
    setenv("SSBU_DATA", P("home/Games/dump_a"), 1);
    was_check();
    unsetenv("SSBU_DATA");
    CHECK(g_was.status == DP_OK);
    put(P("xdg/config/ssbu-recomp"), "file, not a folder", 18);
    dpk_init(&s, &g_was, 1, 0);
    dpk_choose(&s, P("home/Games/dump_b"));
    CHECK(!s.result && strstr(s.note, "could not be saved"));
    CHECKF(!strcmp(s.now.dir, g_was.dir), "shown %s, the game would start with %s", s.now.dir, g_was.dir);
    int m = has_menu(&s, DPK_M_START);
    CHECK(m == 0);
    s.menu_sel = m < 0 ? 0 : m;
    dpk_do(&s, DPK_CONFIRM);
    CHECK(s.result == DPK_KEEP);
    dpk_free(&s);
    SECTION("review: without --pick-data the same failure does not claim the game will start");
    dpk_init(&s, &g_was, 0, 0);                         /* as after a pick from unusable data: pick_mode 0 */
    s.pick_mode = 0;
    dpk_choose(&s, P("home/Games/dump_b"));
    CHECK(s.now.status == DP_OK && !s.result && has_menu(&s, DPK_M_START) < 0);
    CHECK(!strcmp(dpk_headline(&s.now, s.pick_mode), "Usable game data, but not saved"));
    dpk_free(&s);

    SECTION("review: --pick-data, a folder tried fails: the data found at the start can still be started");
    rmrf(P("xdg/config/ssbu-recomp"));
    mkdirs(P("home/Games/empty"));
    dpk_init(&s, &g_was, 1, 0);
    dpk_choose(&s, P("home/Games/empty"));
    CHECK(s.now.status == DP_MISSING_ROMFS && !s.result);
    m = has_menu(&s, DPK_M_START);
    CHECK(m >= 0 && !strcmp(menu_label(&s, DPK_M_START), "Start the game with the data found at the start"));
    s.menu_sel = m < 0 ? 0 : m;
    dpk_do(&s, DPK_CONFIRM);
    CHECK(s.result == DPK_KEEP);
    CHECK(!strcmp(cfg("data"), "-"));                   /* nothing saved */
    dpk_free(&s);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: test_picker <scratch folder> [shots folder]\n"); return 2; }
    char abs[PATH_MAX];
    mkdirs(argv[1]);
    if (!realpath(argv[1], abs)) { perror(argv[1]); return 2; }
    snprintf(T, sizeof T, "%s/picker_scratch", abs);
    snprintf(SHOTS, sizeof SHOTS, "%s", argc > 2 ? argv[2] : "");
    if (!SHOTS[0]) snprintf(SHOTS, sizeof SHOTS, "%s/shots", abs);
    mkdirs(SHOTS);
    setvbuf(stdout, NULL, _IONBF, 0);
    rmrf(T);
    mkdirs(T);
    clean_env();
    t_font();
    t_status();
    t_browse();
    t_fonts();
    t_datapaths();
    t_input();
    t_places();
    t_dialog();
    t_save_fail();
    t_review();
    if (!tp_fail) rmrf(T);
    else printf("scratch kept: %s\n", T);
    printf("shots: %s\n", SHOTS);
    return tp_done("picker");
}

/* tests/port/boot - where the player's game data are and whether they are usable (runtime/datapaths.c), boot.c's Linux
 * launch_defaults / check_romfs / default main.elf (boot.c compiled with -Dmain=ssbu_boot_main, run in forked children) and
 * nnmisc.c's add-on content list. Every input is a synthetic fixture made here (fake ELF / NSO headers, a one-byte
 * data.arc, empty font files with one byte): no game data is used or needed.
 *   test_boot <scratch folder>      the folder is created, and removed when every check passes */
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"
#include "hostpath.h"
#include "datapaths.h"
#include "tp.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- what boot.c and nnmisc.c reach in the rest of the runtime on the paths run here */
static ImportDef g_imp[512];
static int g_nimp;
void register_imports(const ImportDef *d, int n) { for (int i = 0; i < n && g_nimp < 512; i++) g_imp[g_nimp++] = d[i]; }
void import_set_address(const char *name, void *addr) { (void)name; (void)addr; }
void *import_data(const char *name) { (void)name; return (void *)1; }
ImportFn import_lookup(const char *name) { (void)name; return NULL; }
void trace_printf(const char *fmt, ...) { (void)fmt; }
void report_and_exit(int code) { printf("report_and_exit(%d)\n", code); fflush(stdout); _exit(code); }
GameData game_data;                              /* all zero: no SDK data symbols (hostrt.h macros sdk_data / sdk_data_count) */
uint64_t g_main_stack_base, g_main_stack_size;    /* boot.c data (defined in the real exe by hostrt.c) */
const DynSym dyn_syms[1];
const int dyn_sym_count = 0;
MainLayout g_main;
volatile LONG g_guest_frozen;
void cxxrt_install_data(void) { }
int nnhid_pad_selftest(void) { return 0; }
int nnpad_tool(int argc, char **argv) { (void)argc; (void)argv; return -1; }
static int g_hook_calls, g_hook_mode;           /* 0 give up, 1 choose the good dump, 2 say "chosen" without changing anything, 3 quit */
static char g_dump[1024];
void runtime_init(const char *elf, const char *trace) {          /* boot.c got past check_romfs: say what it set up, stop */
    (void)trace;
    const char *r = getenv("SSBU_ROMFS"), *f = getenv("SSBU_FONTS"), *u = getenv("SSBU_USERDATA");
    printf("RUNTIME_INIT elf=%s argv1=%s argc=%d romfs=%s fonts=%s userdata=%s hooks=%d\n", elf, __argc > 1 ? __argv[1] : "-", __argc,
           r ? r : "-", f ? f : "-", u ? u : "-", g_hook_calls);
    fflush(stdout);
    _exit(42);
}
int ssbu_boot_main(int argc, char **argv);       /* boot.c's main */
#if defined(__SANITIZE_ADDRESS__)
#define TP_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TP_ASAN 1
#endif
#endif
#ifndef TP_ASAN
#define TP_ASAN 0
#endif
void register_misc_imports(void);                /* nnmisc.c */

/* ---- fixtures */
static char T[PATH_MAX + 32];
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
static void puts_file(const char *path, const char *text) { put(path, text, strlen(text)); }
static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *ftw) { (void)st; (void)flag; (void)ftw; return remove(p); }
static void rmrf(const char *p) { nftw(p, rm_one, 16, FTW_DEPTH | FTW_PHYS); }
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static char *slurp(const char *path) {
    static char buf[1 << 16];
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = 0;
    return buf;
}
static void put_elf(const char *path, uint64_t text) {
    uint8_t b[64 + 2 * 56] = {0};
    uint64_t phoff = 64, v;
    uint16_t phnum = 2;
    uint32_t type;
    memcpy(b, "\x7f" "ELF", 4); b[4] = 2; b[5] = 1;
    memcpy(b + 0x20, &phoff, 8); memcpy(b + 0x38, &phnum, 2);
    type = 6; memcpy(b + 64, &type, 4);                 /* a PT_PHDR first: the first PT_LOAD is the one that counts */
    type = 1; memcpy(b + 64 + 56, &type, 4);
    memcpy(b + 64 + 56 + 32, &text, 8);
    v = text + 0x1000; memcpy(b + 64 + 56 + 40, &v, 8);
    put(path, b, sizeof b);
}
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
/* a dump folder as tools/extract/dump_folder.py makes it: exefs/main (NSO of the given code size), romfs/data.arc,
 * romfs/.nrr/a.nrr, sysdata/fonts/<7>, sysdata/zoneinfo/ */
static void make_dump(const char *d, uint32_t text) {
    char p[2100];
    if (text) { snprintf(p, sizeof p, "%s/exefs/main", d); put_nso(p, text); snprintf(p, sizeof p, "%s/exefs/main.npdm", d); put(p, "META", 4); }
    snprintf(p, sizeof p, "%s/romfs/data.arc", d); put(p, "A", 1);
    snprintf(p, sizeof p, "%s/romfs/.nrr/a.nrr", d); put(p, "N", 1);
    snprintf(p, sizeof p, "%s/sysdata/fonts", d); put_fonts(p);
    snprintf(p, sizeof p, "%s/sysdata/zoneinfo", d); mkdirs(p);
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
    setenv("XDG_CONFIG_HOME", P("xdg/config"), 1);
    setenv("XDG_DATA_HOME", P("xdg/data"), 1);
    setenv("XDG_STATE_HOME", P("xdg/state"), 1);
    for (int k = 0; k < DP_KINDS; k++) dp_set_cli((dp_kind)k, NULL);
}
/* a fresh tree: the build folder (exe in build/work, both main.elf builds), one good dump; no state.json, no config */
static void fresh(void) {
    rmrf(T);
    mkdirs(P("build/work"));
    mkdirs(P("home"));
    put_elf(P("build/binaries/13.0.4/main.elf"), 0x39c7e90);
    put_elf(P("build/binaries/13.0.5/main.elf"), 0x39c8440);
    make_dump(P("dump"), 0x39c7e90);
    clean_env();
    dp_set_base(P("build/work"));
}
static int same(const char *a, const char *b) {            /* the same file or folder */
    struct stat x, y;
    return a && b && !stat(a, &x) && !stat(b, &y) && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}
#define HAS(hay, needle) CHECKF(strstr((hay), (needle)) != NULL, "\"%s\" not in: %s", (needle), (hay))

/* ================================================================ datapaths.c */
static void t_paths(void) {
    char out[DP_PATH], v[DP_PATH];
    SECTION("path conversion: ..\\state.json against the exe folder, case-insensitive");
    fresh();
    CHECK(dp_exe_path("..\\state.json", out, sizeof out) == 0);       /* missing: still the path to create */
    HAS(out, "/build/work/../state.json");
    CHECK(strchr(out, '\\') == NULL);
    puts_file(P("build/STATE.Json"), "{\"game_version\": \"13.0.5\"}");
    CHECK(dp_exe_path("..\\state.json", out, sizeof out) == 1);
    CHECK(same(out, P("build/STATE.Json")));
    CHECK(dp_state_string("game_version", v, sizeof v) && !strcmp(v, "13.0.5"));
    SECTION("state.json strings: setup.py's json.dump (indent=1, \\uXXXX, escapes), null, a key's text inside a value");
    fresh();
    puts_file(P("build/state.json"), "{\n \"note\": \"romfs\",\n \"dump\": {\n  \"exefs\": \"/x/exefs\",\n"
              "  \"romfs\": \"/m\\u00fcde/\\ud83c\\udfae/r\\\\o\\/m\"\n },\n \"sysdata\": null,\n \"game_version\": \"13.0.4\"\n}\n");
    CHECK(dp_state_string("romfs", v, sizeof v));
    CHECKF(!strcmp(v, "/m\xc3\xbc" "de/\xf0\x9f\x8e\xae/r\\o/m"), "%s", v);
    CHECK(dp_state_string("sysdata", v, sizeof v) == 0);
    CHECK(dp_state_string("nothere", v, sizeof v) == 0);
    puts_file(P("build/state.json"), "{\"romfs\": \"bad \\u00zz\"}");
    CHECK(dp_state_string("romfs", v, sizeof v) == 0);
    puts_file(P("build/state.json"), "{\"romfs\": \"nul \\u0000 inside\"}");
    CHECK(dp_state_string("romfs", v, sizeof v) == 0);
    puts_file(P("build/state.json"), "{\"romfs\": \"cut off");
    CHECK(dp_state_string("romfs", v, sizeof v) == 0);
    SECTION("XDG folders: the variables, HOME, a relative value ignored");
    CHECK(dp_xdg_dir('c', out, sizeof out) && !strcmp(out, P("xdg/config/ssbu-recomp")));
    CHECK(dp_xdg_dir('d', out, sizeof out) && !strcmp(out, P("xdg/data/ssbu-recomp")));
    CHECK(dp_xdg_dir('s', out, sizeof out) && !strcmp(out, P("xdg/state/ssbu-recomp")));
    setenv("XDG_CONFIG_HOME", "relative/dir", 1);
    CHECK(dp_xdg_dir('c', out, sizeof out) && !strcmp(out, P("home/.config/ssbu-recomp")));
    unsetenv("XDG_DATA_HOME");
    CHECK(dp_xdg_dir('d', out, sizeof out) && !strcmp(out, P("home/.local/share/ssbu-recomp")));
}

static void t_builds(void) {
    char out[DP_PATH];
    unsigned ts;
    SECTION("builds: main.elf's first PT_LOAD, ExeFS main's NSO header");
    fresh();
    CHECK(dp_elf_build(P("build/binaries/13.0.4/main.elf")) && !strcmp(dp_elf_build(P("build/binaries/13.0.4/main.elf")), "13.0.4"));
    CHECK(dp_elf_build(P("build/binaries/13.0.5/main.elf")) && !strcmp(dp_elf_build(P("build/binaries/13.0.5/main.elf")), "13.0.5"));
    put_elf(P("other.elf"), 0x1234000);
    CHECK(dp_elf_build(P("other.elf")) == NULL);
    CHECK(dp_elf_build(P("nothere.elf")) == NULL && dp_elf_build(NULL) == NULL);
    put(P("short.elf"), "\x7f" "ELF", 4);
    CHECK(dp_elf_build(P("short.elf")) == NULL);
    CHECK(dp_nso_build(P("dump/exefs/main"), &ts) && !strcmp(dp_nso_build(P("dump/exefs/main"), NULL), "13.0.4") && ts == 0x39c7e90);
    put_nso(P("n5"), 0x39c8440);
    CHECK(dp_nso_build(P("n5"), NULL) && !strcmp(dp_nso_build(P("n5"), NULL), "13.0.5"));
    put_nso(P("nx"), 0x39c0000);
    CHECK(dp_nso_build(P("nx"), &ts) == NULL && ts == 0x39c0000);
    put(P("notnso"), "NSO1xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 36);
    CHECK(dp_nso_build(P("notnso"), &ts) == NULL && ts == 0);
    SECTION("default main.elf: state.json game_version, else the build present, never a path out of binaries/");
    CHECK(dp_default_elf(out, sizeof out) == 1 && same(out, P("build/binaries/13.0.4/main.elf")));
    puts_file(P("build/state.json"), "{\"game_version\": \"13.0.5\"}");
    CHECK(dp_default_elf(out, sizeof out) == 1 && same(out, P("build/binaries/13.0.5/main.elf")));
    puts_file(P("build/state.json"), "{\"game_version\": \"..\"}");
    CHECK(dp_default_elf(out, sizeof out) == 1 && same(out, P("build/binaries/13.0.4/main.elf")));
    puts_file(P("build/state.json"), "{\"game_version\": \"../../x\"}");
    CHECK(dp_default_elf(out, sizeof out) == 1 && same(out, P("build/binaries/13.0.4/main.elf")));
    rmrf(P("build/binaries/13.0.4"));
    remove(P("build/state.json"));
    CHECK(dp_default_elf(out, sizeof out) == 1 && same(out, P("build/binaries/13.0.5/main.elf")));
    rmrf(P("build/binaries"));
    CHECK(dp_default_elf(out, sizeof out) == 0);
    HAS(out, "binaries/13.0.4/main.elf");
}

static void t_args(void) {
    char out[DP_PATH];
    dp_source s;
    SECTION("command line: --data <v>, --fonts=<v>, --check-data taken out of argv; a missing value is an error");
    fresh();
    char *argv[] = {"exe", "--data", P("dump"), "main.elf", "--fonts=/ff", "5", "--check-data", "--userdata", "/u", "--dlc", NULL};
    int argc = 10;
    CHECK(dp_take_args(&argc, argv) == -1);
    CHECK(argc == 3 && !strcmp(argv[0], "exe") && !strcmp(argv[1], "main.elf") && !strcmp(argv[2], "5") && argv[3] == NULL);
    CHECK(dp_check_only());
    CHECK(dp_resolve(DP_DATA, out, sizeof out, &s) && s == DP_SRC_CLI && same(out, P("dump")));
    CHECK(dp_resolve(DP_FONTS, out, sizeof out, &s) && s == DP_SRC_CLI && !strcmp(out, "/ff"));
    CHECK(dp_resolve(DP_USERDATA, out, sizeof out, &s) && s == DP_SRC_CLI && !strcmp(out, "/u"));
    CHECK(dp_resolve(DP_DLC, out, sizeof out, &s) == 0 && s == DP_SRC_NONE);
    char *argv2[] = {"exe", "--database", "x", "--data=", NULL};      /* not ours / an empty value = not set */
    argc = 4;
    clean_env();
    CHECK(dp_take_args(&argc, argv2) == 0 && argc == 3 && !strcmp(argv2[1], "--database"));
    CHECK(dp_resolve(DP_DATA, out, sizeof out, &s) == 0);
}

static void t_config(void) {
    char out[DP_PATH], path[DP_PATH];
    SECTION("config: path, round trip, replace in place, other lines kept, removal, ~/ expansion, bad input");
    fresh();
    CHECK(dp_config_path(path, sizeof path) && !strcmp(path, P("xdg/config/ssbu-recomp/config")));
    CHECK(dp_config_get("data", out, sizeof out) == 0 && out[0] == 0);
    CHECK(dp_config_set("data", "/first") == 0);
    CHECK(dp_config_get("data", out, sizeof out) && !strcmp(out, "/first"));
    HAS(slurp(path), "# ssbu-recomp settings");
    CHECK(dp_config_set("fonts", "/fonts dir/with spaces") == 0);
    CHECK(dp_config_set("data", "/second") == 0);
    const char *c = slurp(path);
    CHECKF(strstr(c, "data=/second\n") && strstr(c, "fonts=") && strstr(c, "data=/second") < strstr(c, "fonts="), "%s", c);
    CHECK(!strstr(c, "/first"));
    CHECK(dp_config_get("fonts", out, sizeof out) && !strcmp(out, "/fonts dir/with spaces"));
    puts_file(path, "# mine\r\n  data =  ~/dump  \r\nfuture_key=kept\nnot a setting\n\ndlc=\n");
    CHECK(dp_config_get("data", out, sizeof out) && !strcmp(out, P("home/dump")));
    CHECK(dp_config_get("dlc", out, sizeof out) == 0);             /* key= is unset */
    CHECK(dp_config_get("future_key", out, sizeof out) && !strcmp(out, "kept"));
    CHECK(dp_config_set("userdata", "/saves") == 0);
    c = slurp(path);
    CHECKF(strstr(c, "# mine") && strstr(c, "future_key=kept") && strstr(c, "not a setting") && strstr(c, "userdata=/saves"), "%s", c);
    CHECK(dp_config_set("data", NULL) == 0);
    CHECK(dp_config_get("data", out, sizeof out) == 0 && !strstr(slurp(path), "~/dump"));
    puts_file(path, "data=/a\ndata=/b\n");                         /* the last line of a key wins; a set keeps one line */
    CHECK(dp_config_get("data", out, sizeof out) && !strcmp(out, "/b"));
    CHECK(dp_config_set("data", "/c") == 0 && !strcmp(slurp(path), "data=/c\n"));
    CHECK(dp_config_set("data", "two\nlines") == -EINVAL && dp_config_set("data", "cr\r") == -EINVAL);
    CHECK(dp_config_set("bad key", "x") == -EINVAL && dp_config_set("", "x") == -EINVAL && dp_config_set(NULL, "x") == -EINVAL);
    CHECK(!strcmp(slurp(path), "data=/c\n"));
    SECTION("config: atomic (no temporary left; a failed write leaves the old file whole)");
    char dir[DP_PATH];
    snprintf(dir, sizeof dir, "%s", path);
    *strrchr(dir, '/') = 0;
    int stray = 0;
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d));) stray += strstr(e->d_name, ".tmp") != NULL;
    if (d) closedir(d);
    CHECK(stray == 0);
    char tmp[DP_PATH + 32];
    snprintf(tmp, sizeof tmp, "%s.tmp%ld", path, (long)getpid());
    puts_file(tmp, "left by a crash");                             /* a temporary of a run that died: replaced, not reused */
    CHECK(dp_config_set("fonts", "/f") == 0 && !exists(tmp));
    if (geteuid() != 0) {
        chmod(dir, 0555);
        int r = dp_config_set("data", "/never");
        chmod(dir, 0755);
        CHECKF(r == -EACCES, "%d", r);
        CHECK(!strcmp(slurp(path), "data=/c\nfonts=/f\n"));
    }
    SECTION("config: corrupt (NUL bytes, > 64 KiB, an over-long line) = ignored, and a save writes it anew");
    put(path, "data=/x\0garbage\n", 16);
    CHECK(dp_config_get("data", out, sizeof out) == 0);
    CHECK(dp_config_set("data", "/y") == 0);
    c = slurp(path);
    CHECKF(!strstr(c, "garbage") && strstr(c, "data=/y\n") && strstr(c, "# ssbu-recomp settings"), "%s", c);
    static char big[70 * 1024];
    memset(big, '#', sizeof big);
    memcpy(big, "data=/big\n", 10);
    put(path, big, sizeof big);
    CHECK(dp_config_get("data", out, sizeof out) == 0);
    memset(big, 'x', 8000);
    memcpy(big, "fonts=", 6);
    memcpy(big + 8000, "\ndata=/after\n", 14);
    put(path, big, 8000 + 13);
    CHECK(dp_config_get("fonts", out, sizeof out) == 0);
    CHECK(dp_config_get("data", out, sizeof out) && !strcmp(out, "/after"));
    SECTION("config: dp_save_choice stores the absolute folder; dp_current_choice reads it back");
    remove(path);
    CHECK(chdir(T) == 0);
    CHECK(dp_save_choice("dump") == 0);
    dp_source s;
    CHECK(dp_current_choice(out, sizeof out, &s) && s == DP_SRC_CONFIG && !strcmp(out, P("dump")));
    CHECK(dp_save_choice(NULL) == 0 && !dp_current_choice(out, sizeof out, &s) && s == DP_SRC_NONE);
}

static void t_resolve(void) {
    char out[DP_PATH];
    dp_source s;
    SECTION("resolver DP_DATA: CLI > SSBU_DATA > SSBU_ROMFS > config > state.json (if data.arc) > <exe>/../romfs (if data.arc) > none");
    fresh();
    CHECK(dp_resolve(DP_DATA, out, sizeof out, &s) == 0 && s == DP_SRC_NONE && !out[0]);
    mkdirs(P("build/romfs"));                                   /* no data.arc: not taken */
    CHECK(dp_resolve(DP_DATA, out, sizeof out, &s) == 0);
    put(P("build/romfs/data.arc"), "A", 1);
    CHECK(dp_resolve(DP_DATA, out, sizeof out, &s) && s == DP_SRC_LAYOUT && same(out, P("build/romfs")));
    char st[3000];
    snprintf(st, sizeof st, "{\"dump\": {\"romfs\": \"%s\"}}", P("gone/romfs"));
    puts_file(P("build/state.json"), st);                       /* names a folder without data.arc (moved): not taken */
    CHECK(dp_resolve(DP_DATA, out, sizeof out, &s) && s == DP_SRC_LAYOUT);
    snprintf(st, sizeof st, "{\"dump\": {\"romfs\": \"%s\"}}", P("dump/romfs"));
    puts_file(P("build/state.json"), st);
    CHECK(dp_resolve(DP_DATA, out, sizeof out, &s) && s == DP_SRC_STATE && same(out, P("dump/romfs")));
    CHECK(dp_config_set("data", "/configured/but/missing") == 0);   /* explicit: returned even when unusable */
    CHECK(dp_resolve(DP_DATA, out, sizeof out, &s) && s == DP_SRC_CONFIG && !strcmp(out, "/configured/but/missing"));
    setenv("SSBU_ROMFS", P("dump/romfs"), 1);
    CHECK(dp_resolve(DP_DATA, out, sizeof out, &s) && s == DP_SRC_ENV && same(out, P("dump/romfs")));
    setenv("SSBU_DATA", "C:\\nope\\Dump", 1);                    /* backslashes become slashes */
    CHECK(dp_resolve(DP_DATA, out, sizeof out, &s) && s == DP_SRC_ENV && !strcmp(out, "C:/nope/Dump"));
    dp_set_cli(DP_DATA, P("DUMP"));                              /* the case of a folder copied from Windows */
    CHECK(dp_resolve(DP_DATA, out, sizeof out, &s) && s == DP_SRC_CLI && same(out, P("dump")));

    SECTION("resolver DP_FONTS: explicit > beside the dump (root or RomFS chosen) > <exe>/../sysdata/fonts > none");
    fresh();
    CHECK(dp_resolve(DP_FONTS, out, sizeof out, &s) == 0 && s == DP_SRC_NONE);
    put_fonts(P("build/sysdata/fonts"));
    CHECK(dp_resolve(DP_FONTS, out, sizeof out, &s) && s == DP_SRC_LAYOUT && same(out, P("build/sysdata/fonts")));
    setenv("SSBU_DATA", P("dump"), 1);
    CHECK(dp_resolve(DP_FONTS, out, sizeof out, &s) && s == DP_SRC_DUMP && same(out, P("dump/sysdata/fonts")));
    setenv("SSBU_DATA", P("dump/romfs"), 1);
    CHECK(dp_resolve(DP_FONTS, out, sizeof out, &s) && s == DP_SRC_DUMP && same(out, P("dump/sysdata/fonts")));
    CHECK(dp_config_set("fonts", P("cfgfonts")) == 0);
    CHECK(dp_resolve(DP_FONTS, out, sizeof out, &s) && s == DP_SRC_CONFIG && !strcmp(out, P("cfgfonts")));
    setenv("SSBU_FONTS", P("envfonts"), 1);
    CHECK(dp_resolve(DP_FONTS, out, sizeof out, &s) && s == DP_SRC_ENV && !strcmp(out, P("envfonts")));
    dp_set_cli(DP_FONTS, P("clifonts"));
    CHECK(dp_resolve(DP_FONTS, out, sizeof out, &s) && s == DP_SRC_CLI && !strcmp(out, P("clifonts")));

    SECTION("resolver DP_USERDATA: explicit > <exe>/../userdata (exists) > $XDG_DATA_HOME/ssbu-recomp/userdata");
    fresh();
    CHECK(dp_resolve(DP_USERDATA, out, sizeof out, &s) && s == DP_SRC_DEFAULT && !strcmp(out, P("xdg/data/ssbu-recomp/userdata")));
    mkdirs(P("build/userdata"));
    CHECK(dp_resolve(DP_USERDATA, out, sizeof out, &s) && s == DP_SRC_LAYOUT && same(out, P("build/userdata")));
    CHECK(dp_config_set("userdata", "~/saves") == 0);
    CHECK(dp_resolve(DP_USERDATA, out, sizeof out, &s) && s == DP_SRC_CONFIG && !strcmp(out, P("home/saves")));
    setenv("SSBU_USERDATA", "/env/saves", 1);
    CHECK(dp_resolve(DP_USERDATA, out, sizeof out, &s) && s == DP_SRC_ENV && !strcmp(out, "/env/saves"));
    dp_set_cli(DP_USERDATA, "/cli/saves");
    CHECK(dp_resolve(DP_USERDATA, out, sizeof out, &s) && s == DP_SRC_CLI && !strcmp(out, "/cli/saves"));

    SECTION("resolver DP_DLC: explicit > <exe>/../config/dlc.txt > $XDG_CONFIG_HOME/ssbu-recomp/dlc.txt > none");
    fresh();
    CHECK(dp_resolve(DP_DLC, out, sizeof out, &s) == 0 && s == DP_SRC_NONE);
    puts_file(P("xdg/config/ssbu-recomp/DLC.txt"), "1\n");
    CHECK(dp_resolve(DP_DLC, out, sizeof out, &s) && s == DP_SRC_DEFAULT && same(out, P("xdg/config/ssbu-recomp/DLC.txt")));
    puts_file(P("build/config/dlc.txt"), "2\n");
    CHECK(dp_resolve(DP_DLC, out, sizeof out, &s) && s == DP_SRC_LAYOUT && same(out, P("build/config/dlc.txt")));
    CHECK(dp_config_set("dlc", P("dlcdumps")) == 0);
    CHECK(dp_resolve(DP_DLC, out, sizeof out, &s) && s == DP_SRC_CONFIG && !strcmp(out, P("dlcdumps")));
    setenv("SSBU_DLC_LIST", "tests\\vk13_nodlc.txt", 1);
    CHECK(dp_resolve(DP_DLC, out, sizeof out, &s) && s == DP_SRC_ENV && !strcmp(out, "tests/vk13_nodlc.txt"));
    dp_set_cli(DP_DLC, "/cli/dlc.txt");
    CHECK(dp_resolve(DP_DLC, out, sizeof out, &s) && s == DP_SRC_CLI && !strcmp(out, "/cli/dlc.txt"));
}

static void t_validate(void) {
    dp_result r;
    SECTION("validate: ok (dump root; RomFS chosen directly; no exefs = warning; upper-case names from Windows)");
    fresh();
    CHECK(dp_validate(P("dump"), "13.0.4", &r) && r.status == DP_OK && !r.warnings);
    CHECK(same(r.romfs, P("dump/romfs")) && same(r.fonts, P("dump/sysdata/fonts")) && r.fonts_source == DP_SRC_DUMP && !strcmp(r.found, "13.0.4"));
    CHECK(r.romfs[0] == '/' && r.fonts[0] == '/');
    HAS(r.message, "13.0.4");
    CHECK(dp_validate(P("dump"), NULL, &r) && !strcmp(r.expected, "13.0.4 or 13.0.5"));
    CHECK(dp_validate(P("dump/romfs"), "13.0.4", &r) && same(r.romfs, P("dump/romfs")) && !strcmp(r.found, "13.0.4"));
    CHECK(dp_validate(P("dump/romfs/"), "13.0.4", &r) && r.status == DP_OK);
    rmrf(P("dump/exefs"));
    CHECK(dp_validate(P("dump"), "13.0.5", &r) && r.status == DP_OK && r.warnings == DP_WARN_NO_EXEFS);
    rmrf(T); mkdirs(P("build/work")); clean_env();
    put(P("W/ROMFS/DATA.ARC"), "A", 1); put(P("W/ROMFS/.NRR/X.NRR"), "N", 1); put_nso(P("W/EXEFS/MAIN"), 0x39c8440);
    put_fonts(P("W/SYSDATA/FONTS"));
    CHECK(dp_validate(P("w"), "13.0.5", &r) && r.status == DP_OK && !strcmp(r.found, "13.0.5"));

    SECTION("validate: every failure has its status, the missing path and a message");
    fresh();
    CHECK(!dp_validate("", "13.0.4", &r) && r.status == DP_NOT_CONFIGURED);
    HAS(r.message, "dump_folder.py");
    CHECK(!dp_validate(NULL, "13.0.4", &r) && r.status == DP_NOT_CONFIGURED);
    CHECK(!dp_validate(P("nothere"), "13.0.4", &r) && r.status == DP_NOT_FOUND && !strcmp(r.missing, P("nothere")));
    HAS(r.message, P("nothere"));
    put(P("afile"), "x", 1);
    CHECK(!dp_validate(P("afile"), "13.0.4", &r) && r.status == DP_NOT_FOUND);
    put(P("pk/Super Smash Bros. Ultimate [01006A800016E000][v0].XCI"), "x", 1);
    CHECK(!dp_validate(P("pk"), "13.0.4", &r) && r.status == DP_PACKAGES_ONLY);
    HAS(r.message, "dump_folder.py"); HAS(r.message, "prod.keys");
    mkdirs(P("empty"));
    CHECK(!dp_validate(P("empty"), "13.0.4", &r) && r.status == DP_MISSING_ROMFS && !strcmp(r.missing, P("empty/romfs/data.arc")));
    mkdirs(P("noarc/romfs"));
    CHECK(!dp_validate(P("noarc"), "13.0.4", &r) && r.status == DP_MISSING_ROMFS && !strcmp(r.missing, P("noarc/romfs/data.arc")));
    HAS(r.message, "data.arc");
    put(P("zero/romfs/data.arc"), "", 0);
    CHECK(!dp_validate(P("zero"), "13.0.4", &r) && r.status == DP_MISSING_ROMFS);
    rmrf(P("dump/romfs/.nrr/a.nrr"));
    put(P("dump/romfs/.nrr/readme.txt"), "x", 1);
    CHECK(!dp_validate(P("dump"), "13.0.4", &r) && r.status == DP_MISSING_NRR && !strcmp(r.missing, P("dump/romfs/.nrr")));
    HAS(r.message, ".nrr");
    put(P("dump/romfs/.nrr/a.nrr"), "N", 1);
    put_nso(P("dump/exefs/main"), 0x39c8440);
    CHECK(!dp_validate(P("dump"), "13.0.4", &r) && r.status == DP_WRONG_VERSION && !strcmp(r.found, "13.0.5") && !strcmp(r.expected, "13.0.4"));
    HAS(r.message, "version 13.0.5"); HAS(r.message, "made from version 13.0.4");
    CHECK(!strcmp(r.missing, P("dump/exefs/main")));
    CHECK(dp_validate(P("dump"), NULL, &r) && r.status == DP_OK);   /* nothing expected: either build */
    put_nso(P("dump/exefs/main"), 0x39c0000);
    CHECK(!dp_validate(P("dump"), NULL, &r) && r.status == DP_UNKNOWN_VERSION && !strcmp(r.found, "code size 0x39c0000"));
    HAS(r.message, "13.0.4 and 13.0.5");
    put(P("dump/exefs/main"), "MOD0", 4);
    CHECK(!dp_validate(P("dump"), NULL, &r) && r.status == DP_UNKNOWN_VERSION && !strcmp(r.found, "not an NSO file"));
    put_nso(P("dump/exefs/main"), 0x39c7e90);
    put(P("dump/sysdata/fonts/nintendo_ext_003.ttf"), "", 0);
    CHECK(!dp_validate(P("dump"), "13.0.4", &r) && r.status == DP_MISSING_FONTS && !strcmp(r.missing, P("dump/sysdata/fonts/nintendo_ext_003.ttf")));
    HAS(r.message, "nintendo_ext_003.ttf"); HAS(r.message, "fw_fonts.py");
    remove(P("dump/sysdata/fonts/nintendo_ext_003.ttf"));
    CHECK(!dp_validate(P("dump"), "13.0.4", &r) && r.status == DP_MISSING_FONTS);
    rmrf(P("dump/sysdata"));
    CHECK(!dp_validate(P("dump"), "13.0.4", &r) && r.status == DP_MISSING_FONTS && !strcmp(r.missing, P("dump/sysdata/fonts")));
    put_fonts(P("build/sysdata/fonts"));                        /* the build's copy (setup.py convert) */
    CHECK(dp_validate(P("dump"), "13.0.4", &r) && r.fonts_source == DP_SRC_LAYOUT && same(r.fonts, P("build/sysdata/fonts")));
    setenv("SSBU_FONTS", P("nofonts"), 1);                      /* an explicit choice wins, even a bad one */
    CHECK(!dp_validate(P("dump"), "13.0.4", &r) && r.status == DP_MISSING_FONTS && r.fonts_source == DP_SRC_ENV);

    SECTION("dp_check: the resolved folder and its source; dp_apply exports the result");
    fresh();
    CHECK(!dp_check("13.0.4", &r) && r.status == DP_NOT_CONFIGURED && r.source == DP_SRC_NONE);
    CHECK(dp_config_set("data", P("dump")) == 0);
    CHECK(dp_check("13.0.4", &r) && r.source == DP_SRC_CONFIG);
    dp_apply(&r);
    CHECK(same(getenv("SSBU_ROMFS"), P("dump/romfs")) && same(getenv("SSBU_FONTS"), P("dump/sysdata/fonts")));
    CHECK(same(getenv("SSBU_TZDIR"), P("dump/sysdata/zoneinfo")));
    CHECK(getenv("SSBU_USERDATA") && !strcmp(getenv("SSBU_USERDATA"), P("xdg/data/ssbu-recomp/userdata")) && exists(P("xdg/data/ssbu-recomp/userdata")));
    setenv("SSBU_TZDIR", "/mine", 1);
    dp_apply(&r);
    CHECK(!strcmp(getenv("SSBU_TZDIR"), "/mine"));
    SECTION("unattended runs: SSBU_SEQ / PLAY / PRESS / SCRIPT, SSBU_ERROR_DIALOG=0");
    clean_env();
    CHECK(!dp_unattended());
    setenv("SSBU_ERROR_DIALOG", "1", 1); CHECK(!dp_unattended());
    setenv("SSBU_ERROR_DIALOG", "0", 1); CHECK(dp_unattended());
    clean_env(); setenv("SSBU_SEQ", "A", 1); CHECK(dp_unattended());
    clean_env(); setenv("SSBU_SCRIPT", "x", 1); CHECK(dp_unattended());
    clean_env();
}

static void t_dlc_scan(void) {
    int idx[64];
    SECTION("DLC: ids in the names of the player's DLC dumps (dlc_list.py's rule), sorted and unique");
    fresh();
    put(P("dlc/Piranha Plant [01006A800016F001][v0].nsp"), "x", 1);
    put(P("dlc/Joker [01006a800016f00b][v65536].NSP"), "x", 1);
    mkdirs(P("dlc/Hero [01006A800016F00C]"));                       /* an extracted folder */
    put(P("dlc/Piranha copy [01006A800016F001].nsz"), "x", 1);      /* the same id twice */
    put(P("dlc/Base game [01006A800016E000][v0].nsp"), "x", 1);     /* the application: not an add-on */
    put(P("dlc/Update [01006A800016E800][v2031616].nsp"), "x", 1);  /* the update id */
    put(P("dlc/Other game [0100000000011001].nsp"), "x", 1);
    put(P("dlc/Short [01006A800016F01].nsp"), "x", 1);
    put(P("dlc/Two in one [01006A800016F002][01006A800016F0FF].nsp"), "x", 1);
    int n = dp_dlc_scan(P("dlc"), idx, 64);
    CHECKF(n == 5 && idx[0] == 1 && idx[1] == 2 && idx[2] == 11 && idx[3] == 12 && idx[4] == 255, "n=%d", n);
    CHECK(dp_dlc_scan(P("dlc"), idx, 2) == 2);
    CHECK(dp_dlc_scan(P("nothere"), idx, 64) == 0);
}

/* nnmisc.c's nn::aoc on the result (one load per process: each case in a child; the exit code is the count) */
static ImportFn H(const char *sub) {
    for (int i = g_nimp; i-- > 0;) if (strstr(g_imp[i].name, sub)) return g_imp[i].fn;
    fprintf(stderr, "no import *%s*\n", sub); exit(2);
}
static int aoc_child(int listed[8]) {
    char out[PATH_MAX + 64];
    snprintf(out, sizeof out, "%s/aoc.txt", T);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (!freopen(out, "w", stdout)) _exit(99);
        g_nimp = 0;
        register_misc_imports();
        static Ctx c;
        H("17CountAddOnContentEv")(&c);
        int n = (int)c.x[0], got[8] = {0};
        memset(&c, 0, sizeof c);
        c.x[0] = (uint64_t)(uintptr_t)got; c.x[1] = 0; c.x[2] = 8;
        H("16ListAddOnContentEPiii")(&c);
        printf("\nLIST %d:", (int)c.x[0]);
        for (int i = 0; i < (int)c.x[0]; i++) printf(" %d", got[i]);
        printf("\n");
        fflush(stdout);
        _exit(n);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    const char *t = slurp(out), *l = strstr(t, "LIST ");
    for (int i = 0; i < 8; i++) listed[i] = 0;
    if (l) { int k = 0, off = 0; sscanf(l, "LIST %d:%n", &k, &off); l += off; for (int i = 0; i < k && i < 8; i++) { int m = 0; sscanf(l, " %d%n", &listed[i], &m); l += m; } }
    if (!strstr(t, "nn::aoc:")) fprintf(stderr, "aoc child said: %s\n", t);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static void t_aoc(void) {
    int got[8];
    SECTION("nnmisc.c nn::aoc: a DLC dump folder, a dlc.txt list, the layout file, nothing");
    fresh();
    CHECK(aoc_child(got) == 0);
    HAS(slurp(P("aoc.txt")), "no dlc.txt");
    put(P("dlc/A [01006A800016F003].nsp"), "x", 1);
    put(P("dlc/B [01006A800016F001].nsp"), "x", 1);
    setenv("SSBU_DLC_LIST", P("dlc"), 1);
    CHECK(aoc_child(got) == 2 && got[0] == 1 && got[1] == 3);
    HAS(slurp(P("aoc.txt")), "the DLC dump names in");
    puts_file(P("list.txt"), "# owned\n010\n0x0b\n01006A800016F00C   # Hero\nnonsense\n");
    setenv("SSBU_DLC_LIST", P("list.txt"), 1);
    CHECK(aoc_child(got) == 3 && got[0] == 10 && got[1] == 11 && got[2] == 12);
    unsetenv("SSBU_DLC_LIST");
    puts_file(P("build/config/DLC.TXT"), "7\n");
    CHECK(aoc_child(got) == 1 && got[0] == 7);
    HAS(slurp(P("aoc.txt")), "/config/DLC.TXT");
}

/* ================================================================ boot.c (Linux launch_defaults / check_romfs / default ELF) */
static int hook(const dp_result *r) {
    g_hook_calls++;
    printf("HOOK %d status=%s\n", g_hook_calls, dp_status_name(r->status));
    if (g_hook_mode == 1) return dp_save_choice(g_dump) == 0;
    if (g_hook_mode == 3) return -1;
    return g_hook_mode == 2;
}
/* boot.c's main in a child: stdout to a file; returns the exit code, *out = what it printed */
static int boot_child(const char **out, int hook_mode, char *const argv_in[]) {
    char log[PATH_MAX + 64];
    snprintf(log, sizeof log, "%s/boot.txt", T);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[16];
        int argc = 0;
        if (!freopen(log, "w", stdout)) _exit(99);
        dup2(1, 2);
        for (; argv_in[argc] && argc < 15; argc++) argv[argc] = argv_in[argc];
        argv[argc] = NULL;
        __argc = argc; __argv = argv;
        g_hook_mode = hook_mode;
        dp_picker_hook = hook_mode >= 0 ? hook : NULL;
        _exit(ssbu_boot_main(argc, argv));
    }
    int st = 0;
    waitpid(pid, &st, 0);
    *out = slurp(log);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
static void t_boot(void) {
    const char *o;
    static char e4[PATH_MAX + 64], e5[PATH_MAX + 64];                /* P() buffers rotate: keep these */
    snprintf(e4, sizeof e4, "%s", P("build/binaries/13.0.4/main.elf"));
    snprintf(e5, sizeof e5, "%s", P("build/binaries/13.0.5/main.elf"));
    snprintf(g_dump, sizeof g_dump, "%s", P("dump"));
    SECTION("boot --check-data: ok -> 0 with the report; nothing configured -> 9");
    fresh();
    CHECK(boot_child(&o, -1, (char *[]){"ssbu_native", "--data", g_dump, e4, "--check-data", NULL}) == 0);
    HAS(o, "game data: ok"); HAS(o, "(command line)"); HAS(o, "dump 13.0.4, this exe 13.0.4");
    CHECK(boot_child(&o, -1, (char *[]){"ssbu_native", "--check-data", NULL}) == DP_EXIT_DATA);
    HAS(o, "game data: not configured"); HAS(o, "dump_folder.py");
    SECTION("boot: good data -> past check_romfs with SSBU_ROMFS / SSBU_FONTS / SSBU_USERDATA exported");
    char want[3000];
    CHECK(boot_child(&o, -1, (char *[]){"ssbu_native", e4, "0", "--data", g_dump, NULL}) == 42);
    snprintf(want, sizeof want, "elf=%s argv1=%s argc=3 romfs=%s/romfs fonts=%s/sysdata/fonts userdata=%s", e4, e4, g_dump, g_dump, P("xdg/data/ssbu-recomp/userdata"));
    HAS(o, want);
    HAS(o, "game data: "); HAS(o, "(command line)");
    SECTION("boot: no main.elf named -> the build's own (state.json game_version), also in __argv");
    puts_file(P("build/state.json"), "{\"game_version\": \"13.0.5\"}");
    put_nso(P("dump/exefs/main"), 0x39c8440);
    setenv("SSBU_DATA", g_dump, 1);
    CHECK(boot_child(&o, -1, (char *[]){"ssbu_native", NULL}) == 42);
    snprintf(want, sizeof want, "elf=%s argv1=%s argc=2", P("build/work/../binaries/13.0.5/main.elf"), P("build/work/../binaries/13.0.5/main.elf"));
    HAS(o, want);
    SECTION("boot: a dump of the other build -> 9 with both versions named");
    CHECK(boot_child(&o, -1, (char *[]){"ssbu_native", e4, NULL}) == DP_EXIT_DATA);
    HAS(o, "This dump is version 13.0.5, but this build of the port was made from version 13.0.4");
    CHECK(boot_child(&o, -1, (char *[]){"ssbu_native", e5, NULL}) == 42);
    SECTION("boot: unusable data, attended: the picker hook chooses -> checked again -> runs; gives up -> 9");
    fresh();
    CHECK(boot_child(&o, 1, (char *[]){"ssbu_native", e4, NULL}) == 42);
    HAS(o, "HOOK 1 status=not configured"); HAS(o, "hooks=1"); HAS(o, "(saved choice)");
    dp_result r;
    CHECK(dp_check("13.0.4", &r) && r.source == DP_SRC_CONFIG);    /* the choice was saved for the next start */
    CHECK(dp_save_choice(NULL) == 0);
    CHECK(boot_child(&o, 0, (char *[]){"ssbu_native", e4, NULL}) == DP_EXIT_DATA);
    HAS(o, "HOOK 1"); HAS(o, "No game data folder is set");
    CHECK(boot_child(&o, 2, (char *[]){"ssbu_native", e4, NULL}) == DP_EXIT_DATA);   /* "chosen" but still bad: bounded */
    HAS(o, "HOOK 16 "); CHECK(!strstr(o, "HOOK 17"));
    SECTION("boot: unusable data, unattended (SSBU_SEQ) or no picker linked -> 9, the hook never called");
    setenv("SSBU_SEQ", "A 1", 1);
    CHECK(boot_child(&o, 1, (char *[]){"ssbu_native", e4, NULL}) == DP_EXIT_DATA);
    CHECK(!strstr(o, "HOOK")); HAS(o, "No game data folder is set");
    unsetenv("SSBU_SEQ");
    CHECK(boot_child(&o, -1, (char *[]){"ssbu_native", e4, NULL}) == DP_EXIT_DATA);
    SECTION("boot: self-test modes need no game data; a missing option value is a usage error");
    setenv("SSBU_MISC_SELFTEST", "1", 1);
    CHECK(boot_child(&o, 1, (char *[]){"ssbu_native", e4, NULL}) == 42);
    CHECK(!strstr(o, "HOOK"));
    unsetenv("SSBU_MISC_SELFTEST");
    CHECK(boot_child(&o, -1, (char *[]){"ssbu_native", e4, "--fonts", NULL}) == 1);
    HAS(o, "--fonts needs a folder");
    SECTION("boot: broken RomFS (no .nrr) named by SSBU_ROMFS -> 9 naming the folder");
    rmrf(P("dump/romfs/.nrr"));
    setenv("SSBU_ROMFS", P("dump/romfs"), 1);
    CHECK(boot_child(&o, -1, (char *[]){"ssbu_native", e4, NULL}) == DP_EXIT_DATA);
    HAS(o, "has data.arc but no .nrr");
    SECTION("boot: the picker's choice wins over the folder that failed (saved, --data, SSBU_DATA) and over what was exported for it");
    static char bad[PATH_MAX + 64];
    snprintf(bad, sizeof bad, "%s", P("dump5"));
    const char *const why[] = {"wrong version", "missing nrr", "missing fonts"};
    for (int c = 0; c < 3; c++) {
        fresh();
        make_dump(bad, c == 0 ? 0x39c8440 : 0x39c7e90);
        if (c == 1) rmrf(P("dump5/romfs/.nrr"));
        if (c == 2) remove(P("dump5/sysdata/fonts/nintendo_ext2_003.ttf"));
        CHECK(dp_save_choice(bad) == 0);
        CHECK(boot_child(&o, 1, (char *[]){"ssbu_native", e4, NULL}) == 42);
        char h[64];
        snprintf(h, sizeof h, "HOOK 1 status=%s", why[c]);
        HAS(o, h); HAS(o, "hooks=1"); HAS(o, "(saved choice)");
        snprintf(want, sizeof want, "romfs=%s/romfs fonts=%s/sysdata/fonts", g_dump, g_dump);
        HAS(o, want);
    }
    fresh();
    make_dump(bad, 0x39c8440);
    CHECK(boot_child(&o, 1, (char *[]){"ssbu_native", e4, "--data", bad, NULL}) == 42);
    HAS(o, "hooks=1");
    fresh();
    make_dump(bad, 0x39c8440);
    setenv("SSBU_DATA", bad, 1);
    CHECK(boot_child(&o, 1, (char *[]){"ssbu_native", e4, NULL}) == 42);
    HAS(o, "hooks=1");
    unsetenv("SSBU_DATA");
    SECTION("boot --pick-data / SSBU_PICK_DATA=1: the picker opens with usable data too; keep -> runs, quit -> 0, chosen -> checked");
    fresh();
    CHECK(boot_child(&o, 0, (char *[]){"ssbu_native", e4, "--pick-data", "--data", g_dump, NULL}) == 42);
    HAS(o, "HOOK 1 status=ok"); HAS(o, "hooks=1"); HAS(o, "argc=2"); HAS(o, "(command line)");
    CHECK(boot_child(&o, 3, (char *[]){"ssbu_native", e4, "--pick-data", "--data", g_dump, NULL}) == 0);
    HAS(o, "HOOK 1 status=ok"); HAS(o, "the player quit in the data picker"); CHECK(!strstr(o, "RUNTIME_INIT"));
    make_dump(bad, 0x39c7e90);
    CHECK(boot_child(&o, 1, (char *[]){"ssbu_native", e4, "--pick-data", "--data", bad, NULL}) == 42);   /* the pick wins over --data */
    HAS(o, "hooks=1"); HAS(o, "(saved choice)");
    snprintf(want, sizeof want, "romfs=%s/romfs", g_dump);
    HAS(o, want);
    setenv("SSBU_PICK_DATA", "1", 1);
    CHECK(boot_child(&o, 0, (char *[]){"ssbu_native", e4, NULL}) == 42);
    HAS(o, "HOOK 1 status=ok"); HAS(o, "hooks=1");
    setenv("SSBU_PICK_DATA", "0", 1);
    CHECK(boot_child(&o, 0, (char *[]){"ssbu_native", e4, NULL}) == 42);
    CHECK(!strstr(o, "HOOK"));
    unsetenv("SSBU_PICK_DATA");
    setenv("SSBU_SEQ", "A 1", 1);                   /* unattended: --pick-data is ignored, the game starts */
    CHECK(boot_child(&o, 3, (char *[]){"ssbu_native", e4, "--pick-data", NULL}) == 42);
    CHECK(!strstr(o, "HOOK"));
    unsetenv("SSBU_SEQ");
    CHECK(boot_child(&o, -1, (char *[]){"ssbu_native", e4, "--pick-data", NULL}) == 42);     /* no picker linked: starts */
    SECTION("boot: unusable data, the player quits in the picker -> 9 with the reason");
    fresh();
    CHECK(boot_child(&o, 3, (char *[]){"ssbu_native", e4, NULL}) == DP_EXIT_DATA);
    HAS(o, "HOOK 1"); CHECK(!strstr(o, "HOOK 2")); HAS(o, "No game data folder is set");
    SECTION("boot --check-data: the userdata source as found (not the SSBU_USERDATA boot exports), nothing created");
    fresh();
    CHECK(boot_child(&o, -1, (char *[]){"ssbu_native", "--data", g_dump, e4, "--check-data", NULL}) == 0);
    HAS(o, "ssbu-recomp/userdata (default)");
    CHECK(!exists(P("xdg/data/ssbu-recomp/userdata")));
    SECTION("config: a FIFO in place of the config file is ignored, not waited on");
    mkdirs(P("xdg/config/ssbu-recomp"));
    CHECK(mkfifo(P("xdg/config/ssbu-recomp/config"), 0600) == 0);
    char v[64];
    alarm(20);                                  /* the old code blocked here forever */
    CHECK(!dp_config_get("data", v, sizeof v));
    alarm(0);
    remove(P("xdg/config/ssbu-recomp/config"));
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: test_boot <scratch folder>\n"); return 2; }
    char abs[PATH_MAX];
    mkdirs(argv[1]);
    if (!realpath(argv[1], abs)) { perror(argv[1]); return 2; }
    snprintf(T, sizeof T, "%s/boot_scratch", abs);
    setvbuf(stdout, NULL, _IONBF, 0);
    t_paths();
    t_builds();
    t_args();
    t_config();
    t_resolve();
    t_validate();
    t_dlc_scan();
    t_aoc();
    t_boot();
    int rc = tp_done("boot");
    if (!rc) rmrf(T);
    else printf("scratch kept: %s\n", T);
    return rc;
}

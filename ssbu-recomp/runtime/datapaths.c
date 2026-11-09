/* datapaths.c - the player's game data on Linux: where they are (CLI, environment, saved choice, build layout), whether
 * they are usable, and the saved choice itself. See datapaths.h. Linux only; the Windows build does not compile it. */
#ifdef __linux__
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "hostpath.h"
#include "datapaths.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"     /* the messages are cut at their buffer on purpose; paths are checked with fits() */
#endif

int (*dp_picker_hook)(const dp_result *r);

static char g_cli[DP_KINDS][DP_PATH];
static int g_check_only, g_pick;
static char g_base[DP_PATH];

#define AOC_APP 0x01006A800016E000ULL           /* the application the add-on contents belong to (nnmisc.c AOC_APP_ID) */
#define CONFIG_MAX (64 * 1024)                  /* a bigger config file is not ours: ignored */
static const struct { unsigned text; const char *name; } g_builds[] = {{0x39c7e90, "13.0.4"}, {0x39c8440, "13.0.5"}};   /* setup.py GAME_BUILDS */
/* nnpl.c font_files: the shared fonts nn::pl hands to the game (the game stops at start without them) */
static const char *const g_fonts[] = {
    "nintendo_udsg-r_std_003.ttf", "nintendo_udsg-r_org_zh-cn_003.ttf", "nintendo_udsg-r_ext_zh-cn_003.ttf",
    "nintendo_udjxh-db_zh-tw_003.ttf", "nintendo_udsg-r_ko_003.ttf", "nintendo_ext_003.ttf", "nintendo_ext2_003.ttf",
};
static const char *const g_cli_names[DP_KINDS] = {"--data", "--fonts", "--dlc", "--userdata"};
static const char *const g_cfg_keys[DP_KINDS] = {"data", "fonts", "dlc", "userdata"};

/* ------------------------------------------------------------------------------------------------ small helpers */
static const char *base_dir(void) { return g_base[0] ? g_base : host_exe_dir(); }
void dp_set_base(const char *exe_dir) {
    if (!exe_dir || !exe_dir[0]) { g_base[0] = 0; return; }
    size_t l = strlen(exe_dir);
    snprintf(g_base, sizeof g_base, "%s%s", exe_dir, exe_dir[l - 1] == '/' ? "" : "/");
}
static int fits(int k, size_t n) { return k >= 0 && (size_t)k < n; }
static int join(char *out, size_t n, const char *a, const char *b) {          /* a/b; 0 = too long */
    size_t l = strlen(a);
    return fits(snprintf(out, n, "%s%s%s", a, l && a[l - 1] == '/' ? "" : "/", b), n);
}
static int stat_kind(const char *p, int want_dir, off_t *size) {               /* 1 = exists and is a folder (want_dir) / a regular file */
    struct stat st;
    if (!p[0] || stat(p, &st)) return 0;
    if (size) *size = st.st_size;
    return want_dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode);
}
int dp_is_dir(const char *path) { return path && stat_kind(path, 1, NULL); }
/* path a/b resolved case-insensitively ('\\' too) into out; 1 = it exists as the kind asked for */
static int find(char *out, size_t n, const char *a, const char *b, int want_dir) {
    char tmp[DP_PATH];
    if (b ? !join(tmp, sizeof tmp, a, b) : !fits(snprintf(tmp, sizeof tmp, "%s", a), sizeof tmp)) { out[0] = 0; return 0; }
    host_path_resolve(tmp, out, n);
    return stat_kind(out, want_dir, NULL);
}
static void absolute(char *p, size_t n) {                                      /* realpath in place when it exists */
    char r[PATH_MAX];
    if (realpath(p, r) && strlen(r) < n) strcpy(p, r);
}
static void parent_of(const char *p, char *out, size_t n) {                    /* the folder above an absolute path */
    snprintf(out, n, "%s", p);
    char *s = strrchr(out, '/');
    if (s && s != out) *s = 0; else snprintf(out, n, "/");
}
/* a leading ~/ is the home folder (a config file edited by hand) */
static void expand_home(const char *in, char *out, size_t n) {
    const char *home = getenv("HOME");
    if (in[0] == '~' && (in[1] == '/' || !in[1]) && home && home[0]) snprintf(out, n, "%s%s", home, in + 1);
    else snprintf(out, n, "%s", in);
}

int dp_exe_path(const char *rel, char *out, size_t n) {
    char tmp[DP_PATH];
    if (!fits(snprintf(tmp, sizeof tmp, "%s%s", base_dir(), rel), sizeof tmp)) { out[0] = 0; return 0; }
    return host_path_resolve(tmp, out, n);
}

int dp_xdg_dir(int which, char *out, size_t n) {
    const char *var = which == 'c' ? "XDG_CONFIG_HOME" : which == 'd' ? "XDG_DATA_HOME" : "XDG_STATE_HOME";
    const char *dflt = which == 'c' ? ".config" : which == 'd' ? ".local/share" : ".local/state";
    const char *v = getenv(var), *home = getenv("HOME");
    if (v && v[0] == '/') return fits(snprintf(out, n, "%s/ssbu-recomp", v), n);      /* the spec: a relative value is ignored */
    if (!home || !home[0]) { struct passwd *pw = getpwuid(getuid()); home = pw ? pw->pw_dir : NULL; }
    if (!home || !home[0]) { out[0] = 0; return 0; }
    return fits(snprintf(out, n, "%s/%s/ssbu-recomp", home, dflt), n);
}

const char *dp_source_name(dp_source s) {
    static const char *const names[] = {"none", "command line", "environment", "saved choice", "state.json of the build", "build folder", "beside the dump", "default"};
    return (unsigned)s < sizeof names / sizeof names[0] ? names[s] : "?";
}
const char *dp_status_name(dp_status s) {
    static const char *const names[] = {"ok", "not configured", "not found", "packages only", "missing romfs", "missing nrr", "unknown version", "wrong version", "missing fonts"};
    return (unsigned)s < sizeof names / sizeof names[0] ? names[s] : "?";
}
int dp_unattended(void) {                       /* boot.c romfs_fail's rule (and nnmisc.c err_dialog's): tests never wait on a screen */
    const char *off = getenv("SSBU_ERROR_DIALOG");
    return (off && off[0] == '0') || getenv("SSBU_SEQ") || getenv("SSBU_PLAY") || getenv("SSBU_PRESS") || getenv("SSBU_SCRIPT");
}

/* ------------------------------------------------------------------------------------------------ command line */
void dp_set_cli(dp_kind k, const char *value) {
    if ((unsigned)k >= DP_KINDS) return;
    snprintf(g_cli[k], sizeof g_cli[k], "%s", value ? value : "");
}
int dp_check_only(void) { return g_check_only; }
int dp_pick_requested(void) {
    const char *e = getenv("SSBU_PICK_DATA");
    return g_pick || (e && e[0] && strcmp(e, "0"));
}
int dp_take_args(int *argc, char **argv) {
    int out = 1, bad = 0;
    g_check_only = g_pick = 0;
    for (int i = 1; i < *argc; i++) {
        const char *a = argv[i];
        int k = -1;
        size_t l = 0;
        if (!strcmp(a, "--check-data")) { g_check_only = 1; continue; }
        if (!strcmp(a, "--pick-data")) { g_pick = 1; continue; }
        for (int j = 0; j < DP_KINDS && k < 0; j++) {
            l = strlen(g_cli_names[j]);
            if (!strncmp(a, g_cli_names[j], l) && (a[l] == 0 || a[l] == '=')) k = j;
        }
        if (k < 0) { argv[out++] = argv[i]; continue; }
        if (a[l] == '=') dp_set_cli((dp_kind)k, a + l + 1);
        else if (i + 1 < *argc) dp_set_cli((dp_kind)k, argv[++i]);
        else { fprintf(stderr, "%s needs a folder (or a file, for --dlc)\n", a); bad = 1; }
    }
    for (int i = out; i < *argc; i++) argv[i] = NULL;
    *argc = out;
    return bad ? -1 : 0;
}

/* ------------------------------------------------------------------------------------------------ state.json (setup.py) */
static char *put_utf8(char *o, unsigned cp) {
    if (cp < 0x80) *o++ = (char)cp;
    else if (cp < 0x800) { *o++ = (char)(0xc0 | cp >> 6); *o++ = (char)(0x80 | (cp & 0x3f)); }
    else if (cp < 0x10000) { *o++ = (char)(0xe0 | cp >> 12); *o++ = (char)(0x80 | (cp >> 6 & 0x3f)); *o++ = (char)(0x80 | (cp & 0x3f)); }
    else { *o++ = (char)(0xf0 | cp >> 18); *o++ = (char)(0x80 | (cp >> 12 & 0x3f)); *o++ = (char)(0x80 | (cp >> 6 & 0x3f)); *o++ = (char)(0x80 | (cp & 0x3f)); }
    return o;
}
static unsigned hex4(const char *p) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int c = p[i], d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
        if (d < 0) return ~0u;
        v = v << 4 | (unsigned)d;
    }
    return v;
}
static const char *blank(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }
/* the JSON string at q (its opening quote) -> UTF-8; json.dump writes non-ASCII as \uXXXX (surrogate pairs too). 0 = bad */
static int json_string(const char *q, char *out, size_t n) {
    char *o = out, *end = out + n - 5;
    const char *p = q + 1;
    for (; *p && *p != '"'; p++) {
        unsigned cp = (unsigned char)*p;
        if (o >= end) return 0;
        if (cp == '\\') {
            cp = (unsigned char)*++p;
            switch (cp) {
            case 'n': cp = '\n'; break;
            case 't': cp = '\t'; break;
            case 'r': cp = '\r'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case '"': case '\\': case '/': break;
            case 'u':
                if ((cp = hex4(p + 1)) == ~0u) return 0;
                p += 4;
                if (cp >= 0xd800 && cp < 0xdc00 && p[1] == '\\' && p[2] == 'u') {
                    unsigned lo = hex4(p + 3);
                    if (lo >= 0xdc00 && lo < 0xe000) { cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00); p += 6; }
                }
                if (!cp) return 0;                      /* a NUL cannot be in a path */
                break;
            default: return 0;
            }
        }
        o = put_utf8(o, cp);
    }
    if (*p != '"') return 0;
    *o = 0;
    return 1;
}
int dp_state_string(const char *key, char *out, size_t n) {
    char path[DP_PATH], pat[64];
    static char buf[256 * 1024];
    out[0] = 0;
    if (!dp_exe_path("..\\state.json", path, sizeof path)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = 0;
    if (!fits(snprintf(pat, sizeof pat, "\"%s\"", key), sizeof pat)) return 0;
    for (const char *p = strstr(buf, pat); p; p = strstr(p + 1, pat)) {
        const char *v = blank(p + strlen(pat));
        if (*v++ != ':') continue;                      /* a value that happens to be the key's text */
        v = blank(v);
        if (*v != '"') return 0;                        /* null or a number */
        if (!json_string(v, out, n)) { out[0] = 0; return 0; }
        return out[0] != 0;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ config file */
int dp_config_path(char *out, size_t n) {
    char d[DP_PATH];
    return dp_xdg_dir('c', d, sizeof d) && join(out, n, d, "config");
}
/* the whole file, NUL-terminated; -1 = none, unreadable or not ours (too big, NUL bytes) */
static long config_read(char *buf, size_t cap) {
    char path[DP_PATH], extra;
    if (!dp_config_path(path, sizeof path)) return -1;
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);  /* O_NONBLOCK: a FIFO put there must not hang the start */
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return -1; }
    size_t len = 0;
    ssize_t r = 0;
    while (len < cap - 1 && ((r = read(fd, buf + len, cap - 1 - len)) > 0 || (r < 0 && errno == EINTR))) if (r > 0) len += (size_t)r;
    int big = len == cap - 1 && read(fd, &extra, 1) > 0;   /* more than CONFIG_MAX bytes: not ours */
    close(fd);
    if (r < 0 || big) return -1;
    buf[len] = 0;
    if (memchr(buf, 0, len)) return -1;                     /* NUL bytes: not a text file */
    return (long)len;
}
/* the key of the line [p, e) when it is `key=value` (blanks around the key allowed): its value in v[0..vl) */
static int config_line(const char *p, const char *e, const char **k, size_t *kl, const char **v, size_t *vl) {
    if (e - p > DP_PATH + 64) return 0;                 /* an over-long line: ignored */
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    if (p == e || *p == '#') return 0;
    const char *eq = memchr(p, '=', (size_t)(e - p));
    if (!eq) return 0;
    const char *ke = eq;
    while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
    if (ke == p) return 0;
    const char *vs = eq + 1, *ve = e;
    while (vs < ve && (*vs == ' ' || *vs == '\t')) vs++;
    while (ve > vs && (ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t')) ve--;
    *k = p; *kl = (size_t)(ke - p); *v = vs; *vl = (size_t)(ve - vs);
    return 1;
}
int dp_config_get(const char *key, char *out, size_t n) {
    static char buf[CONFIG_MAX + 1];
    char raw[DP_PATH];
    out[0] = 0;
    long len = config_read(buf, sizeof buf);
    if (len < 0) return 0;
    int found = 0;
    for (const char *p = buf, *e; *p; p = *e ? e + 1 : e) {
        const char *k, *v;
        size_t kl, vl;
        e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        if (!config_line(p, e, &k, &kl, &v, &vl) || kl != strlen(key) || memcmp(k, key, kl)) continue;
        if (vl >= sizeof raw) continue;
        memcpy(raw, v, vl);
        raw[vl] = 0;
        found = vl > 0;                                 /* the last line of a key wins; `key=` unsets it */
        if (found) expand_home(raw, out, n); else out[0] = 0;
    }
    return found;
}
static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return w < 0 ? -errno : -EIO;
        p += w; n -= (size_t)w;
    }
    return 0;
}
int dp_config_set(const char *key, const char *value) {
    static char old[CONFIG_MAX + 1], neu[CONFIG_MAX + DP_PATH + 256];
    char path[DP_PATH], tmp[DP_PATH + 32];
    if (!key || !key[0]) return -EINVAL;
    for (const char *c = key; *c; c++) if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-') return -EINVAL;
    if (value && (strchr(value, '\n') || strchr(value, '\r') || strlen(value) >= DP_PATH)) return -EINVAL;
    if (!dp_config_path(path, sizeof path)) return -ENAMETOOLONG;
    long len = config_read(old, sizeof old);           /* none or not ours: written anew */
    size_t at = 0;
    int done = 0;
    if (len <= 0) at = (size_t)snprintf(neu, sizeof neu, "# ssbu-recomp settings: key=value lines (data, fonts, dlc, userdata); written by the game\n");
    for (const char *p = len > 0 ? old : "", *e; *p; p = *e ? e + 1 : e) {
        const char *k, *v;
        size_t kl, vl;
        e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        if (config_line(p, e, &k, &kl, &v, &vl) && kl == strlen(key) && !memcmp(k, key, kl)) {
            if (!done && value) at += (size_t)snprintf(neu + at, sizeof neu - at, "%s=%s\n", key, value);
            done = 1;                                   /* the first line of the key takes the new value, later ones go */
            continue;
        }
        if (at + (size_t)(e - p) + 2 >= sizeof neu) return -EFBIG;
        memcpy(neu + at, p, (size_t)(e - p));
        at += (size_t)(e - p);
        neu[at++] = '\n';
    }
    if (!done && value) at += (size_t)snprintf(neu + at, sizeof neu - at, "%s=%s\n", key, value);
    if (at >= sizeof neu) return -EFBIG;
    int r = host_make_parents(path);
    if (r) return r;
    if (!fits(snprintf(tmp, sizeof tmp, "%s.tmp%ld", path, (long)getpid()), sizeof tmp)) return -ENAMETOOLONG;
    unlink(tmp);                                        /* left by a run that died here */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return -errno;
    r = write_all(fd, neu, at);
    if (!r && fsync(fd)) r = -errno;
    if (close(fd) && !r) r = -errno;
    if (!r && rename(tmp, path)) r = -errno;
    if (r) { unlink(tmp); return r; }
    host_fsync_parent(path);                            /* the rename itself reaches the disk */
    return 0;
}
int dp_save_choice(const char *dir) {
    char abs[DP_PATH];
    if (!dir || !dir[0]) return dp_config_set("data", NULL);
    host_path_resolve(dir, abs, sizeof abs);
    absolute(abs, sizeof abs);
    return dp_config_set("data", abs);
}

/* ------------------------------------------------------------------------------------------------ builds */
static const char *build_of(unsigned text) {
    for (size_t i = 0; i < sizeof g_builds / sizeof g_builds[0]; i++) if (g_builds[i].text == text) return g_builds[i].name;
    return NULL;
}
const char *dp_elf_build(const char *elf) {
    uint8_t eh[64], ph[56];
    FILE *f = elf ? fopen(elf, "rb") : NULL;
    const char *b = NULL;
    if (!f) return NULL;
    if (fread(eh, 1, sizeof eh, f) == sizeof eh && !memcmp(eh, "\x7f" "ELF", 4)) {
        uint64_t phoff; uint16_t phnum;
        memcpy(&phoff, eh + 0x20, 8); memcpy(&phnum, eh + 0x38, 2);
        for (int i = 0; i < phnum && i < 64; i++) {
            uint32_t type; uint64_t filesz;
            if (fseek(f, (long)(phoff + (uint64_t)i * 56), SEEK_SET) || fread(ph, 1, sizeof ph, f) != sizeof ph) break;
            memcpy(&type, ph, 4);
            if (type != 1) continue;                    /* the first PT_LOAD is the code (hostrt.c load_layout) */
            memcpy(&filesz, ph + 32, 8);
            b = filesz >> 32 ? NULL : build_of((unsigned)filesz);
            break;
        }
    }
    fclose(f);
    return b;
}
const char *dp_nso_build(const char *nso, unsigned *text_size) {
    uint8_t h[0x20];
    FILE *f = nso ? fopen(nso, "rb") : NULL;
    if (text_size) *text_size = 0;
    if (!f) return NULL;
    size_t got = fread(h, 1, sizeof h, f);
    fclose(f);
    if (got != sizeof h || memcmp(h, "NSO0", 4)) return NULL;
    unsigned size;
    memcpy(&size, h + 0x18, 4);                         /* text segment: file offset, memory offset, size (nso2elf.py) */
    if (text_size) *text_size = size;
    return build_of(size);
}
int dp_default_elf(char *out, size_t n) {
    char v[32], rel[96];
    if (dp_state_string("game_version", v, sizeof v) && !strchr(v, '/') && !strchr(v, '\\') && strcmp(v, "..")) {
        snprintf(rel, sizeof rel, "../binaries/%s/main.elf", v);
        if (dp_exe_path(rel, out, n)) return 1;
    }
    for (size_t i = 0; i < sizeof g_builds / sizeof g_builds[0]; i++) {
        snprintf(rel, sizeof rel, "../binaries/%s/main.elf", g_builds[i].name);
        if (dp_exe_path(rel, out, n)) return 1;
    }
    dp_exe_path("../binaries/13.0.4/main.elf", out, n);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ resolving */
static int explicit_value(dp_kind k, char *out, size_t n, dp_source *src) {
    static const char *const env1[DP_KINDS] = {"SSBU_DATA", "SSBU_FONTS", "SSBU_DLC_LIST", "SSBU_USERDATA"};
    char raw[DP_PATH];
    const char *e;
    if (g_cli[k][0]) { *src = DP_SRC_CLI; expand_home(g_cli[k], raw, sizeof raw); }
    else if ((e = getenv(env1[k])) && e[0]) { *src = DP_SRC_ENV; snprintf(raw, sizeof raw, "%s", e); }
    else if (k == DP_DATA && (e = getenv("SSBU_ROMFS")) && e[0]) { *src = DP_SRC_ENV; snprintf(raw, sizeof raw, "%s", e); }
    else if (dp_config_get(g_cfg_keys[k], raw, sizeof raw)) *src = DP_SRC_CONFIG;
    else return 0;
    host_path_resolve(raw, out, n);                     /* '\\' and the case of a dump copied from Windows */
    return 1;
}
/* the dump's root for a data folder: the folder itself, or the one above a RomFS folder that was chosen directly */
static void dump_root(const char *data, char *root, size_t n) {
    char p[DP_PATH];
    snprintf(root, n, "%s", data);
    absolute(root, n);
    if (find(p, sizeof p, root, "data.arc", 0)) { snprintf(p, sizeof p, "%s", root); parent_of(p, root, n); }
}
static int fonts_for(const char *data, char *out, size_t n, dp_source *src) {
    char root[DP_PATH];
    if (explicit_value(DP_FONTS, out, n, src)) return 1;
    if (data && data[0]) {
        dump_root(data, root, sizeof root);
        if (find(out, n, root, "sysdata/fonts", 1)) { *src = DP_SRC_DUMP; return 1; }
    }
    if (dp_exe_path("../sysdata/fonts", out, n) && dp_is_dir(out)) { *src = DP_SRC_LAYOUT; return 1; }
    out[0] = 0;
    *src = DP_SRC_NONE;
    return 0;
}
int dp_resolve(dp_kind k, char *out, size_t n, dp_source *src) {
    dp_source s = DP_SRC_NONE;
    char tmp[DP_PATH], st[DP_PATH];
    int ok = 0;
    out[0] = 0;
    if ((unsigned)k >= DP_KINDS) return 0;
    if (k == DP_FONTS) {
        char data[DP_PATH];
        ok = explicit_value(k, out, n, &s) || fonts_for(dp_resolve(DP_DATA, data, sizeof data, NULL) ? data : NULL, out, n, &s);
    } else if (explicit_value(k, out, n, &s)) ok = 1;
    else if (k == DP_DATA) {
        /* the build's own layout, only when it holds the data (Windows launch_defaults / fs_roots) */
        char arc[DP_PATH];
        if (dp_state_string("romfs", st, sizeof st) && host_path_resolve(st, tmp, sizeof tmp) && find(arc, sizeof arc, tmp, "data.arc", 0)) {
            snprintf(out, n, "%s", tmp);
            s = DP_SRC_STATE; ok = 1;
        } else if (dp_exe_path("../romfs/data.arc", tmp, sizeof tmp) && stat_kind(tmp, 0, NULL)) {
            dp_exe_path("../romfs", out, n);
            s = DP_SRC_LAYOUT; ok = 1;
        }
    } else if (k == DP_DLC) {
        if (dp_exe_path("../config/dlc.txt", out, n)) { s = DP_SRC_LAYOUT; ok = 1; }
        else if (dp_xdg_dir('c', tmp, sizeof tmp) && find(out, n, tmp, "dlc.txt", 0)) { s = DP_SRC_DEFAULT; ok = 1; }
    } else if (k == DP_USERDATA) {
        if (dp_exe_path("../userdata", out, n) && dp_is_dir(out)) { s = DP_SRC_LAYOUT; ok = 1; }
        else if (dp_xdg_dir('d', tmp, sizeof tmp) && join(out, n, tmp, "userdata")) { s = DP_SRC_DEFAULT; ok = 1; }
    }
    if (!ok) out[0] = 0;
    if (src) *src = ok ? s : DP_SRC_NONE;
    return ok;
}
int dp_current_choice(char *out, size_t n, dp_source *src) { return dp_resolve(DP_DATA, out, n, src); }

/* ------------------------------------------------------------------------------------------------ DLC */
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
int dp_dlc_scan(const char *dir, int *idx, int max) {
    DIR *d = opendir(dir);
    int n = 0;
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d)) && n < max;) {
        for (const char *p = strchr(e->d_name, '['); p && n < max; p = strchr(p + 1, '[')) {
            unsigned long long id = 0;
            int i = 1;
            for (; i <= 16; i++) {
                int c = p[i], v = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
                if (v < 0) break;
                id = id << 4 | (unsigned)v;
            }
            if (i != 17 || p[17] != ']') continue;
            if (id <= AOC_APP + 0x1000 || id >= AOC_APP + 0x2000) continue;
            int v = (int)(id - AOC_APP - 0x1000), k = 0;
            while (k < n && idx[k] != v) k++;
            if (k == n) idx[n++] = v;                   /* `max` counts different ids */
        }
    }
    closedir(d);
    qsort(idx, (size_t)n, sizeof *idx, cmp_int);
    return n;
}

/* ------------------------------------------------------------------------------------------------ validation */
static int has_packages(const char *dir) {
    DIR *d = opendir(dir);
    int found = 0;
    if (!d) return 0;
    for (struct dirent *e; !found && (e = readdir(d));) {
        const char *dot = strrchr(e->d_name, '.');
        found = dot && (!strcasecmp(dot, ".xci") || !strcasecmp(dot, ".nsp") || !strcasecmp(dot, ".nsz") || !strcasecmp(dot, ".xcz"));
    }
    closedir(d);
    return found;
}
static int has_nrr(const char *romfs) {
    char nrr[DP_PATH];
    if (!find(nrr, sizeof nrr, romfs, ".nrr", 1)) return 0;
    DIR *d = opendir(nrr);
    int found = 0;
    if (!d) return 0;
    for (struct dirent *e; !found && (e = readdir(d));) {
        size_t l = strlen(e->d_name);
        found = l > 4 && !strcasecmp(e->d_name + l - 4, ".nrr");
    }
    closedir(d);
    return found;
}
static const char how_dump[] =
    "The game runs from YOUR OWN dump: the folder tools/extract/dump_folder.py makes from your cartridge (.xci) or eShop copy "
    "(.nsp) and the 13.0.4 or 13.0.5 update, with your prod.keys and the system fonts of your firmware (--firmware):\n"
    "    <dump>/exefs/  <dump>/romfs/data.arc  <dump>/sysdata/fonts/\n"
    "Choose that folder in the game, or start with  --data <folder>  or  SSBU_DATA=<folder>.";
static int fail(dp_result *r, dp_status s, const char *missing, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int fail(dp_result *r, dp_status s, const char *missing, const char *fmt, ...) {
    va_list ap;
    r->status = s;
    snprintf(r->missing, sizeof r->missing, "%s", missing ? missing : "");
    va_start(ap, fmt);
    vsnprintf(r->message, sizeof r->message, fmt, ap);
    va_end(ap);
    return 0;
}
/* The one place that decides whether a folder holds the system fonts (dp_validate, dp_fonts_check, the picker's tags): all 7
 * files, none empty, names matched case-insensitively. *missing = the first one absent (its name). */
static int fonts_complete(const char *dir, const char **missing) {
    char p[DP_PATH];
    off_t size = 0;
    for (size_t i = 0; i < sizeof g_fonts / sizeof g_fonts[0]; i++)
        if (!find(p, sizeof p, dir, g_fonts[i], 0) || (stat_kind(p, 0, &size), size == 0)) { *missing = g_fonts[i]; return 0; }
    *missing = NULL;
    return 1;
}
int dp_fonts_check(const char *dir, char *missing, size_t n) {
    char d[DP_PATH];
    const char *m = g_fonts[0];
    int ok = 0;
    if (dir && dir[0]) { host_path_resolve(dir, d, sizeof d); ok = fonts_complete(d, &m); }
    if (missing && n) snprintf(missing, n, "%s", ok ? "" : m);
    return ok;
}
int dp_validate(const char *dir, const char *expect, dp_result *r) {
    char d[DP_PATH], root[DP_PATH], p[DP_PATH], e[DP_PATH];
    off_t size = 0;
    memset(r, 0, sizeof *r);
    snprintf(r->dir, sizeof r->dir, "%s", dir ? dir : "");
    snprintf(r->expected, sizeof r->expected, "%s", expect ? expect : "13.0.4 or 13.0.5");
    if (!dir || !dir[0]) return fail(r, DP_NOT_CONFIGURED, NULL, "No game data folder is set.\n\n%s", how_dump);
    host_path_resolve(dir, d, sizeof d);
    if (!dp_is_dir(d)) return fail(r, DP_NOT_FOUND, d, "The game data folder %s does not exist (or is not a folder).\n\n%s", d, how_dump);
    absolute(d, sizeof d);
    if (find(p, sizeof p, d, "data.arc", 0)) { snprintf(r->romfs, sizeof r->romfs, "%s", d); parent_of(d, root, sizeof root); }
    else if (find(p, sizeof p, d, "romfs", 1)) { snprintf(r->romfs, sizeof r->romfs, "%s", p); snprintf(root, sizeof root, "%s", d); }
    else if (has_packages(d))
        return fail(r, DP_PACKAGES_ONLY, d, "%s holds game packages (.xci / .nsp / .nsz), not an extracted dump.\n\n"
                    "Extract them first, with your own prod.keys in ~/.switch/prod.keys:\n"
                    "    python3 tools/extract/dump_folder.py <base .xci/.nsp> <update .nsp> <dump folder> --firmware <firmware folder>\n"
                    "(.nsz: tools/extract/nsz_to_nsp.py first), then choose the dump folder it made.", d);
    else {
        join(p, sizeof p, d, "romfs/data.arc");
        return fail(r, DP_MISSING_ROMFS, p, "%s is not a game dump: it has no romfs folder (romfs/data.arc).\n\n%s", d, how_dump);
    }
    absolute(r->romfs, sizeof r->romfs);
    if (!find(p, sizeof p, r->romfs, "data.arc", 0) || (stat_kind(p, 0, &size), size == 0)) {
        join(p, sizeof p, r->romfs, "data.arc");
        return fail(r, DP_MISSING_ROMFS, p, "data.arc was not found in the RomFS folder %s (or it is empty).\n\n"
                    "Copy the whole RomFS of your dump (update applied), or extract it again with tools/extract/dump_folder.py.", r->romfs);
    }
    if (!has_nrr(r->romfs)) {
        join(p, sizeof p, r->romfs, ".nrr");
        return fail(r, DP_MISSING_NRR, p, "The RomFS folder %s has data.arc but no .nrr/*.nrr files.\n\nThe game would refuse to load its "
                    "script modules and stop on the loading screen before the first fight. Copy the whole RomFS folder of your dump "
                    "(the .nrr folder sits beside data.arc; file managers hide names that start with a dot).", r->romfs);
    }
    if (find(p, sizeof p, root, "exefs/main", 0)) {
        unsigned text;
        const char *b = dp_nso_build(p, &text);
        if (!b) {
            if (text) snprintf(r->found, sizeof r->found, "code size %#x", text); else snprintf(r->found, sizeof r->found, "not an NSO file");
            return fail(r, DP_UNKNOWN_VERSION, p, "%s is not a build of Super Smash Bros. Ultimate this port handles (%s).\n\n"
                        "The port handles 13.0.4 and 13.0.5 (the update applied: the update's ExeFS). Make the dump again "
                        "with tools/extract/dump_folder.py from your base game and the 13.0.4 or 13.0.5 update.", p, r->found);
        }
        snprintf(r->found, sizeof r->found, "%s", b);
        if (expect && strcmp(b, expect))
            return fail(r, DP_WRONG_VERSION, p, "This dump is version %s, but this build of the port was made from version %s.\n\n"
                        "Choose a dump of %s, or build the port again from this dump (scripts/setup.py --target linux-arm64 --dump %s).",
                        b, expect, expect, root);
    } else r->warnings |= DP_WARN_NO_EXEFS;
    if (!fonts_for(d, r->fonts, sizeof r->fonts, &r->fonts_source)) {
        join(p, sizeof p, root, "sysdata/fonts");
        return fail(r, DP_MISSING_FONTS, p, "The system fonts were not found (%s).\n\nThe game needs the 7 shared fonts of the console "
                    "(nintendo_*_003.ttf). Get them from your own firmware dump: python3 tools/extract/fw_fonts.py <firmware folder> "
                    "%s  (or dump_folder.py --firmware), or start with --fonts <folder> / SSBU_FONTS=<folder>.", p, p);
    }
    absolute(r->fonts, sizeof r->fonts);
    const char *font;
    if (!fonts_complete(r->fonts, &font)) {
        join(e, sizeof e, r->fonts, font);
        return fail(r, DP_MISSING_FONTS, e, "The system font %s is missing from %s (or empty).\n\nThe game needs all 7 shared fonts of "
                    "the console. Extract them again from your own firmware dump: python3 tools/extract/fw_fonts.py <firmware folder> %s",
                    font, r->fonts, r->fonts);
    }
    r->status = DP_OK;
    snprintf(r->message, sizeof r->message, "game data: %s (build %s), fonts %s", r->romfs,
             r->warnings & DP_WARN_NO_EXEFS ? "not checked: no exefs/main" : r->found, r->fonts);
    return 1;
}
int dp_check(const char *expect, dp_result *r) {
    char dir[DP_PATH];
    dp_source s;
    if (!dp_resolve(DP_DATA, dir, sizeof dir, &s)) { dir[0] = 0; s = DP_SRC_NONE; }
    int ok = dp_validate(dir, expect, r);
    r->source = s;
    return ok;
}

void dp_report(FILE *out, const dp_result *r) {
    char p[DP_PATH], cfg[DP_PATH];
    dp_source s;
    fprintf(out, "game data: %s\n", r->status == DP_OK ? "ok" : dp_status_name(r->status));
    fprintf(out, "  data     %s (%s)\n", r->dir[0] ? r->dir : "-", dp_source_name(r->source));
    fprintf(out, "  romfs    %s\n", r->romfs[0] ? r->romfs : "-");
    fprintf(out, "  fonts    %s (%s)\n", r->fonts[0] ? r->fonts : "-", dp_source_name(r->fonts_source));
    fprintf(out, "  build    dump %s, this exe %s\n", r->found[0] ? r->found : r->warnings & DP_WARN_NO_EXEFS ? "not checked (no exefs/main)" : "-", r->expected);
    fprintf(out, "  userdata %s", dp_resolve(DP_USERDATA, p, sizeof p, &s) ? p : "-"); fprintf(out, " (%s)\n", dp_source_name(s));
    fprintf(out, "  dlc      %s", dp_resolve(DP_DLC, p, sizeof p, &s) ? p : "none owned"); fprintf(out, " (%s)\n", dp_source_name(s));
    fprintf(out, "  config   %s\n", dp_config_path(cfg, sizeof cfg) ? cfg : "-");
    if (r->status != DP_OK) fprintf(out, "\n%s\n", r->message);
}

/* ------------------------------------------------------------------------------------------------ export */
static char g_tz_ours[DP_PATH];                         /* the SSBU_TZDIR dp_apply set (a dump's zoneinfo), "" = none */
void dp_apply(const dp_result *r) {
    char p[DP_PATH], root[DP_PATH];
    if (r && r->romfs[0]) {
        setenv("SSBU_ROMFS", r->romfs, 1);
        parent_of(r->romfs, root, sizeof root);
        const char *tz = getenv("SSBU_TZDIR");
        if ((!tz || (g_tz_ours[0] && !strcmp(tz, g_tz_ours))) && find(p, sizeof p, root, "sysdata/zoneinfo", 1)) {
            setenv("SSBU_TZDIR", p, 1);
            snprintf(g_tz_ours, sizeof g_tz_ours, "%s", p);
        }
    }
    if (r && r->fonts[0]) setenv("SSBU_FONTS", r->fonts, 1);
    if (dp_resolve(DP_USERDATA, p, sizeof p, NULL)) {
        char probe[DP_PATH + 4];
        snprintf(probe, sizeof probe, "%s/x", p);
        host_make_parents(probe);                       /* the folder itself (and those above it) */
        absolute(p, sizeof p);
        setenv("SSBU_USERDATA", p, 1);
    }
}
/* The player chose through dp_picker_hook and the choice was saved (config "data", perhaps "fonts"): from now on it wins over
 * --data / SSBU_DATA / SSBU_ROMFS, and over what dp_apply exported for the folder that failed (SSBU_ROMFS, SSBU_FONTS found
 * beside it or in the build, SSBU_TZDIR). Fonts the player named himself (--fonts, SSBU_FONTS) are kept unless they were the
 * problem. Without this the next dp_check finds the old folder again and the picker is asked 16 times in vain. */
static int fonts_dropped(const dp_result *r) {         /* dp_picked drops SSBU_FONTS (2: --fonts too) for this result */
    if (!r) return 0;
    if (r->status == DP_MISSING_FONTS) return 2;
    return r->fonts_source == DP_SRC_DUMP || r->fonts_source == DP_SRC_LAYOUT || r->fonts_source == DP_SRC_CONFIG;
}
void dp_picked(const dp_result *r) {
    int f = fonts_dropped(r);
    dp_set_cli(DP_DATA, NULL);
    unsetenv("SSBU_DATA");
    unsetenv("SSBU_ROMFS");
    if (f == 2) dp_set_cli(DP_FONTS, NULL);
    if (f) unsetenv("SSBU_FONTS");
    const char *tz = getenv("SSBU_TZDIR");
    if (g_tz_ours[0] && tz && !strcmp(tz, g_tz_ours)) unsetenv("SSBU_TZDIR");
    g_tz_ours[0] = 0;
}
/* The picker checks a folder the way boot.c will after dp_picked(was): without the fonts overrides dp_picked drops (the
 * data overrides do not matter, `dir` is named). Nothing is changed for the caller: --fonts and SSBU_FONTS are restored. */
int dp_validate_picked(const char *dir, const char *expect, const dp_result *was, dp_result *r) {
    static char cli[DP_PATH], env[DP_PATH];
    const char *e = getenv("SSBU_FONTS");
    int f = fonts_dropped(was), had = e != NULL, ok;
    snprintf(cli, sizeof cli, "%s", g_cli[DP_FONTS]);
    snprintf(env, sizeof env, "%s", e ? e : "");
    if (f == 2) g_cli[DP_FONTS][0] = 0;
    if (f) unsetenv("SSBU_FONTS");
    ok = dp_validate(dir, expect, r);
    snprintf(g_cli[DP_FONTS], sizeof g_cli[DP_FONTS], "%s", cli);
    if (f && had) setenv("SSBU_FONTS", env, 1);
    return ok;
}
#endif /* !_WIN32 */

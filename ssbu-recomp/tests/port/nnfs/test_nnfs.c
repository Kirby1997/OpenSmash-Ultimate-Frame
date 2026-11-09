/* test_nnfs.c - the file services of the game on the rebuilt Linux layer: nn::fs (nnfs.c: a romfs-like tree, journaled save
 * data with Commit and its recovery, the guest file systems' fallbacks), nnfsdir.c (OpenDirectory / ReadDirectory order),
 * the Win32 file calls under them (plat_posix.c: CreateFileA, ReadFile / WriteFile / OVERLAPPED, SetFilePointerEx,
 * SetEndOfFile, MoveFileExA, CopyFileA, FindFirstFileA in NTFS order), the case-insensitive lookup (host_path_resolve), and
 * the config and font paths (gfx_options.c, nnpl.c). The real handlers run, through their import tables.
 * Save commit crash safety: a child process is SIGKILLed before every mutating file call of Commit (and of the roll-forward at
 * the next mount); the committed folder must then be the complete old or the complete new state. A Commit whose apply fails after
 * the decision (a move, a deletion, the marker's removal) stays pending: the game sees the decided state at once, and Commit
 * again, SIGKILL, Unmount, uncommitted changes, or more changes and a Commit (each also SIGKILLed before every call) end in the
 * decided state or a later decided one, never with an uncommitted change. Two processes: A's apply fails and A dies, B mounts
 * while it still fails and commits (B SIGKILLed before every call). Markers written by earlier builds are still applied.
 * The calls nnfs.c makes are renamed to fi_* in that one unit (fault.h, forced in with -include) and fault_wrap.c passes them on
 * after telling a hook: linux-arm64's hostpath.c had the hook inside its file layer, plat_posix.c has none.
 * Everything happens in a fresh folder under argv[1], else $TMPDIR, else /tmp (removed when all checks pass).
 * Env: FS_TRACE=1 shows nnfs's trace and the children's output, FS_NO_KILL=1 skips the crash runs, FS_KEEP=1 keeps the folder.
 * Was tests/port/fs/test_fs.c of linux-arm64 (WP4). Dropped: its own hostpath.c (plat_posix.c's file layer is tests/port/fs),
 * the fsync/rename order inside MoveFileExA (invisible from here: plat's), SetFileSize through nnfs_handle_fd (nnfs.c's handler
 * uses the plat handle). */
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"
#include "hostpath.h"
#include "gfx_options.h"
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <ftw.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int mt_fail, mt_checks;
#define CHECK(c) do { mt_checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); mt_fail++; } } while (0)
#define CHECKF(c, ...) do { mt_checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, #c); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); mt_fail++; } } while (0)
#define SECTION(s) do { printf("  - %s\n", s); fflush(stdout); } while (0)
static int mt_done(const char *name) { printf("%-14s %s (%d checks, %d failed)\n", name, mt_fail ? "FAIL" : "PASS", mt_checks, mt_fail); fflush(stdout); return mt_fail ? 1 : 0; }

/* ---- what nnfs.c / nnfsdir.c / nnpl.c / nnlibc2.c reach in the rest of the runtime */
static ImportDef g_imp[512];
static int g_nimp;
void register_imports(const ImportDef *d, int n) { for (int i = 0; i < n && g_nimp < 512; i++) g_imp[g_nimp++] = d[i]; }
void *import_data(const char *name) { (void)name; return (void *)1; }
void trace_printf(const char *fmt, ...) { if (getenv("FS_TRACE")) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); } }
int g_hitch_ms;
void hitch_note(const char *s) { (void)s; }
void report_and_exit(int code) { printf("report_and_exit(%d)\n", code); fflush(stdout); _exit(code); }
void call_guest(Ctx *c, uint64_t addr) { (void)c; (void)addr; abort(); }
void guest_trap(const char *what, uint64_t addr) { fprintf(stderr, "guest_trap: %s (at %llx)\n", what, (unsigned long long)addr); abort(); }
uint64_t main_export(const char *name) { (void)name; return 0; }
void r1_wrote(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
void r1_read_retry_note(void) { }
void register_fs_imports(void);
void register_nnfsdir_imports(void);
void register_pl_imports(void);
int nnfs_selftest(Ctx *c);
int nnfsdir_selftest(Ctx *c);
extern void (*fi_hook)(const char *op, const char *path);          /* fault_wrap.c */

static ImportFn H(const char *sub) {               /* the last registration wins (nnfsdir.c replaces OpenDirectory) */
    for (int i = g_nimp; i-- > 0;) if (strstr(g_imp[i].name, sub)) return g_imp[i].fn;
    fprintf(stderr, "no import *%s*\n", sub); exit(2);
}
static Ctx C;
#define G(p) ((uint64_t)(uintptr_t)(p))
static uint32_t gc(const char *imp, int n, ...) {  /* a guest call: x0.. = the arguments, the result is x0 */
    va_list ap; va_start(ap, n);
    memset(&C, 0, sizeof C);
    for (int i = 0; i < n; i++) C.x[i] = va_arg(ap, uint64_t);
    va_end(ap);
    H(imp)(&C);
    return (uint32_t)C.x[0];
}
#define MOUNT_ROM(n) gc("8MountRomEPKcPvm", 3, G(n), 0ull, 0ull)
#define MOUNT_SAVE(n) gc("13MountSaveDataEPKc", 2, G(n), G(g_uid))
#define UNMOUNT(n) gc("7UnmountEPKc", 1, G(n))
#define COMMIT(n) gc("6CommitEPKc", 1, G(n))
#define OPEN(h, p, m) gc("8OpenFileEPNS0_10FileHandleEPKci", 3, G(h), G(p), (uint64_t)(m))
#define CLOSE(h) gc("9CloseFileENS0_10FileHandleE", 1, (uint64_t)(h))
#define READ(h, off, buf, n) gc("8ReadFileENS0_10FileHandleElPvm", 4, (uint64_t)(h), (uint64_t)(off), G(buf), (uint64_t)(n))
#define READN(got, h, off, buf, n) gc("8ReadFileEPmNS0_10FileHandleElPvm", 5, G(got), (uint64_t)(h), (uint64_t)(off), G(buf), (uint64_t)(n))
#define WRITE(h, off, buf, n) gc("9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", 5, (uint64_t)(h), (uint64_t)(off), G(buf), (uint64_t)(n), G(&g_flush))
#define SIZE(out, h) gc("11GetFileSizeEPlNS0_10FileHandleE", 2, G(out), (uint64_t)(h))
#define CREATE(p, n) gc("10CreateFileEPKcl", 2, G(p), (uint64_t)(n))
#define DELETE(p) gc("10DeleteFileEPKc", 1, G(p))
#define MKDIR(p) gc("15CreateDirectoryEPKc", 1, G(p))
#define RMDIR(p) gc("15DeleteDirectoryEPKc", 1, G(p))
#define ENTRY_TYPE(out, p) gc("12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", 2, G(out), G(p))
#define SETSIZE(h, n) gc("11SetFileSizeENS0_10FileHandleEl", 2, (uint64_t)(h), (uint64_t)(n))
static uint64_t g_uid[2] = {1, 0};
static uint32_t g_flush = 1;                       /* WriteOption::Flush */
#define R_PATH_NOT_FOUND 0x202u
#define R_ALREADY_EXISTS 0x402u
#define R_NAME_EXISTS 0x7802u
#define R_OUT_OF_RANGE 0x177a02u
#define R_NO_APPEND 0x307202u
#define R_WRITE_NOT_PERMITTED 0x307602u
#define R_PERMISSION_DENIED 0x320002u
#define R_WRITE_FILE_OPEN 0x327202u
#define R_NOT_MOUNTED 0x35f202u
#define R_GENERIC_FAILURE 0x3e802u

/* ---- plain POSIX helpers (independent of the layer under test) */
static char T[1024];                               /* the scratch folder */
static char *tp(const char *rel) { static char b[8][2048]; static int i; i = (i + 1) & 7; snprintf(b[i], sizeof b[i], "%s/%s", T, rel); return b[i]; }
static void mkp(const char *path) {                /* mkdir -p of the folder that holds path */
    char t[2048]; snprintf(t, sizeof t, "%s", path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(t, 0777); *p = '/'; }
}
static void put(const char *path, const char *s) {
    mkp(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0 || write(fd, s, strlen(s)) != (ssize_t)strlen(s)) { fprintf(stderr, "put %s: %s\n", path, strerror(errno)); exit(2); }
    close(fd);
}
static long get(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    buf[n < 0 ? 0 : n] = 0;
    return (long)n;
}
static long fsize(const char *path) { struct stat st; return stat(path, &st) == 0 ? (long)st.st_size : -1; }
static int rm_one(const char *p, const struct stat *s, int f, struct FTW *w) { (void)s; (void)f; (void)w; chmod(p, 0777); return remove(p); }
static int rw_one(const char *p, const struct stat *s, int f, struct FTW *w) { (void)s; (void)f; (void)w; chmod(p, 0777); return 0; }
static void rm_rf(const char *p) { nftw(p, rw_one, 16, FTW_PHYS); nftw(p, rm_one, 16, FTW_DEPTH | FTW_PHYS); }   /* (a folder left read-only by a test first) */
static void snap(const char *dir, const char *rel, char *out, size_t cap) {  /* "rel/;" per folder, "rel=content;" per file, sorted */
    char p[2048], r[1024], buf[256];
    struct dirent **nl;
    snprintf(p, sizeof p, "%s%s%s", dir, rel[0] ? "/" : "", rel);
    int n = scandir(p, &nl, NULL, alphasort);
    for (int i = 0; i < n; i++) {
        if (strcmp(nl[i]->d_name, ".") && strcmp(nl[i]->d_name, "..")) {
            struct stat st;
            snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", nl[i]->d_name);
            snprintf(p, sizeof p, "%s/%s", dir, r);
            size_t l = strlen(out);
            if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) { snprintf(out + l, cap - l, "%s/;", r); snap(dir, r, out, cap); }
            else { get(p, buf, sizeof buf); snprintf(out + l, cap - l, "%s=%s;", r, buf); }
        }
        free(nl[i]);
    }
    if (n >= 0) free(nl);
}
static void join_names(char *out, size_t cap, const char *name) { size_t l = strlen(out); snprintf(out + l, cap - l, "%s%s", l ? "," : "", name); }

/* ---- fault hook: an op log, and SIGKILL before the n-th mutating call */
static char g_log[256][300];
static int g_nlog, g_ops, g_kill_at;
static void hook(const char *op, const char *path) {
    if (g_nlog < 256) snprintf(g_log[g_nlog++], sizeof g_log[0], "%s %s", op, path ? path : "-");
    if (++g_ops == g_kill_at) kill(getpid(), SIGKILL);
}
static void hook_on(int kill_at) { g_nlog = g_ops = 0; g_kill_at = kill_at; fi_hook = hook; }
static void hook_off(void) { fi_hook = NULL; }
static int log_find(const char *prefix) { for (int i = 0; i < g_nlog; i++) if (!strncmp(g_log[i], prefix, strlen(prefix))) return i; return -1; }
static int log_is(int i, const char *op, const char *rel) { char w[2048]; snprintf(w, sizeof w, "%s %s", op, tp(rel)); return i >= 0 && i < g_nlog && !strcmp(g_log[i], w); }

/* ================================================================ 1. romfs-like tree */
static void t_romfs(void) {
    SECTION("romfs tree through nn::fs: MountRom, OpenFile (any case, '\\\\'), ReadFile both overloads, refusals, GetEntryType");
    char big[1001], buf[200];
    for (int i = 0; i < 1000; i++) big[i] = (char)('A' + i % 26);
    big[1000] = 0;
    put(tp("romfs/Data/Sub/File.BIN"), big);
    put(tp("romfs/Data/small.txt"), "hello");
    uint64_t h = 0, got = 0, cache = 0;
    int64_t size = 0;
    uint32_t type = 9;
    CHECK(MOUNT_ROM("rom") == 0);
    CHECK(MOUNT_ROM("rom") == R_NAME_EXISTS);
    CHECK(OPEN(&h, "rom:/data\\SUB/file.bin", 1) == 0 && h);
    CHECK(SIZE(&size, h) == 0 && size == 1000);
    CHECK(READ(h, 10, buf, 100) == 0 && !memcmp(buf, big + 10, 100));
    CHECK(READ(h, 950, buf, 100) == R_OUT_OF_RANGE);              /* this overload reads exactly `size` bytes */
    CHECK(READN(&got, h, 950, buf, 100) == 0 && got == 50 && !memcmp(buf, big + 950, 50));
    CHECK(READN(&got, h, 1000, buf, 100) == 0 && got == 0);
    CHECK(READN(&got, h, 1 << 20, buf, 100) == 0 && got == 0);
    CHECK(WRITE(h, 0, "x", 1) == R_WRITE_NOT_PERMITTED);
    CLOSE(h);
    CHECK(OPEN(&h, "rom:/Data/small.txt", 3) == R_PERMISSION_DENIED);
    CHECK(CREATE("rom:/Data/new.bin", 0) == R_PERMISSION_DENIED);
    CHECK(OPEN(&h, "rom:/Data/missing.bin", 1) == R_PATH_NOT_FOUND && h == 0);
    CHECK(OPEN(&h, "rom:/Data/Sub", 1) == R_PATH_NOT_FOUND);       /* a folder is not a file */
    CHECK(OPEN(&h, "nosuch:/x", 1) == R_NOT_MOUNTED);
    CHECK(ENTRY_TYPE(&type, "rom:/DATA") == 0 && type == 0);
    CHECK(ENTRY_TYPE(&type, "rom:\\data\\SMALL.TXT") == 0 && type == 1);
    CHECK(ENTRY_TYPE(&type, "rom:/nothing") == R_PATH_NOT_FOUND);
    /* 3 folders (root, Data, Sub): 4*3 + 3*0x18 + 4 ("Data") + 4 ("Sub"); 2 files: 4*3 + 2*0x20 + 8 + 12 */
    CHECK(gc("22QueryMountRomCacheSizeEPm", 1, G(&cache)) == 0);
    CHECKF(cache == 12 + 0x48 + 8 + 12 + 0x40 + 20, "QueryMountRomCacheSize %llu", (unsigned long long)cache);
}

/* ================================================================ 2. read / write / seek / truncate */
static void t_rw(void) {
    SECTION("Win32 file layer: CreateFileA dispositions, ReadFile/WriteFile (plain, OVERLAPPED), SetFilePointerEx, SetEndOfFile");
    char p[2048], buf[9000];
    DWORD n = 0;
    LARGE_INTEGER d, pos, sz;
    snprintf(p, sizeof p, "%s\\RW\\f.bin", T);
    CHECK(CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL) == INVALID_HANDLE_VALUE && (GetLastError() == ERROR_PATH_NOT_FOUND || GetLastError() == ERROR_FILE_NOT_FOUND));   /* (Windows: PATH_NOT_FOUND; plat gives the errno's) */
    mkdir(tp("rw"), 0777);                         /* "RW" in the path, "rw" on disk */
    HANDLE h = CreateFileA(p, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE && fsize(tp("rw/f.bin")) == 0);
    CHECK(CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL) == INVALID_HANDLE_VALUE && (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS));   /* (Windows: FILE_EXISTS) */
    CHECK(CreateFileA(tp("rw/none"), GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND);
    memset(buf, 'w', 100);
    CHECK(WriteFile(h, buf, 100, &n, NULL) && n == 100);
    d.QuadPart = -50;
    CHECK(SetFilePointerEx(h, d, &pos, FILE_CURRENT) && pos.QuadPart == 50);
    CHECK(SetEndOfFile(h) && fsize(tp("rw/f.bin")) == 50);
    d.QuadPart = 4096;
    CHECK(SetFilePointerEx(h, d, NULL, FILE_BEGIN) && SetEndOfFile(h) && GetFileSizeEx(h, &sz) && sz.QuadPart == 4096);
    OVERLAPPED ov = {0};
    ov.Offset = 8000;
    CHECK(WriteFile(h, "0123456789", 10, &n, &ov) && n == 10 && fsize(tp("rw/f.bin")) == 8010);
    ov.Offset = 40;
    CHECK(ReadFile(h, buf, 20, &n, &ov) && n == 20 && !memcmp(buf, "wwwwwwwwww", 10) && buf[10] == 0 && buf[19] == 0);  /* grown part = zeros */
    ov.Offset = 8005;
    CHECK(ReadFile(h, buf, 100, &n, &ov) && n == 5 && !memcmp(buf, "56789", 5));
    ov.Offset = 9000;
    CHECK((n = 77, !ReadFile(h, buf, 100, &n, &ov) ? (n == 0 && GetLastError() == ERROR_HANDLE_EOF) : n == 0));        /* (Windows: FALSE + HANDLE_EOF; plat: TRUE, 0 bytes) */
    d.QuadPart = 0;
    CHECK(SetFilePointerEx(h, d, &pos, FILE_END) && pos.QuadPart == 8010);
    CHECK(ReadFile(h, buf, 100, &n, NULL) && n == 0);                 /* plain read at the end: TRUE, 0 bytes */
    int fd = plat_handle_fd(h);
    CHECK(fd >= 0 && lseek(fd, 0, SEEK_CUR) == 8010);
    CHECK(CloseHandle(h));
    CHECK(!ReadFile(h, buf, 1, &n, NULL) && GetLastError() == ERROR_INVALID_HANDLE);
    h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE && fsize(tp("rw/f.bin")) == 0);                                   /* (Windows also sets ERROR_ALREADY_EXISTS here) */
    CloseHandle(h);

    SECTION("nn::fs save data: CreateFile, append rule, Write/Read, SetFileSize, Commit refusal and apply");
    uint64_t g = 0, got = 0;
    int64_t size = 0;
    CHECK(gc("14EnsureSaveData", 1, G(g_uid)) == 0);
    CHECK(MOUNT_SAVE("sv") == 0);
    CHECK(CREATE("sv:/Dir1\\f.bin", 0) == R_PATH_NOT_FOUND);
    CHECK(MKDIR("sv:/Dir1") == 0 && MKDIR("sv:/dir1") == R_ALREADY_EXISTS);
    CHECK(CREATE("sv:/dir1/F.bin", 16) == 0 && CREATE("sv:/DIR1/f.BIN", 0) == R_ALREADY_EXISTS);
    CHECK(OPEN(&g, "sv:/dir1/f.bin", 3) == 0);                      /* read + write, no append */
    CHECK(SIZE(&size, g) == 0 && size == 16);
    CHECK(WRITE(g, 0, "0123456789abcdef", 16) == 0 && WRITE(g, 10, "0123456789abcdef", 16) == R_NO_APPEND);
    CHECK(COMMIT("sv") == R_WRITE_FILE_OPEN);
    CLOSE(g);
    CHECK(OPEN(&g, "sv:/dir1/f.bin", 7) == 0 && WRITE(g, 16, "ghij", 4) == 0 && SIZE(&size, g) == 0 && size == 20);
    CHECK(READ(g, 14, buf, 6) == 0 && !memcmp(buf, "efghij", 6));
    CHECK(plat_handle_fd((HANDLE)g) >= 0);
    CHECK(SETSIZE(g, 5) == 0 && SIZE(&size, g) == 0 && size == 5);
    CHECK(SETSIZE(g, 70000) == 0 && SIZE(&size, g) == 0 && size == 70000);
    CHECK(READN(&got, g, 0, buf, 8) == 0 && got == 8 && !memcmp(buf, "01234\0\0\0", 8));
    CHECK(SETSIZE(g, -1) == R_OUT_OF_RANGE);
    CLOSE(g);
    CHECK(fsize(tp("ud/save/Dir1/F.bin")) == -1);                   /* not committed yet */
    CHECK(COMMIT("sv") == 0 && fsize(tp("ud/save/Dir1/F.bin")) == 70000);
    UNMOUNT("sv");
}

/* ================================================================ 3. case-insensitive lookup */
static void t_case(void) {
    SECTION("case-insensitive lookup: host_path_resolve ('\\\\', '.', '..'), twins, non-ASCII, the listing cache, the file calls");
    char in[2048], out[4096], p[2048], a[2048], b[2048], s[512] = "";
    put(tp("Case/Sub Dir/Alpha.bin"), "a");
    put(tp("Case/u\xc3\xa9.bin"), "e");
    put(tp("Case/dup.bin"), "lower");
    put(tp("Case/DUP.bin"), "upper");
#define RES(rel, want_ret, want) do { snprintf(in, sizeof in, "%s\\%s", T, rel); int r_ = host_path_resolve(in, out, sizeof out); \
        CHECKF(r_ == (want_ret) && !strcmp(out, tp(want)), "%s -> %d \"%s\"", rel, r_, out); } while (0)
    RES("cASE\\sub dir\\ALPHA.BIN", 1, "Case/Sub Dir/Alpha.bin");
    RES("case/./SUB DIR/../Sub dir\\alpha.bin", 1, "Case/./Sub Dir/../Sub Dir/Alpha.bin");
    RES("CASE\\sub dir\\", 1, "Case/Sub Dir");                       /* trailing separator dropped */
    RES("case\\missing\\deeper.bin", 0, "Case/missing/deeper.bin");  /* the rest is kept as given */
    RES("case\\U\xc3\xa9.BIN", 1, "Case/u\xc3\xa9.bin");             /* ASCII folds, the UTF-8 bytes match exactly */
    RES("case\\U\xc3\x89.BIN", 0, "Case/U\xc3\x89.BIN");             /* E-acute vs e-acute: no Unicode folding (DIFF) */
    RES("case\\DUP.bin", 1, "Case/DUP.bin");                         /* twins: the exact name first */
    RES("case\\dup.bin", 1, "Case/dup.bin");
    RES("case\\Dup.bin", 1, "Case/DUP.bin");                         /* else the first in NTFS order */
    snprintf(p, sizeof p, "%s\\CASE\\SUB DIR\\alpha.BIN", T);
    CHECK(GetFileAttributesA(p) == FILE_ATTRIBUTE_ARCHIVE);
    WIN32_FILE_ATTRIBUTE_DATA ad;
    CHECK(GetFileAttributesExA(p, GetFileExInfoStandard, &ad) && ad.nFileSizeLow == 1);
    FILE *f = fopen(p, "rb");
    CHECK(f && fgetc(f) == 'a');
    if (f) fclose(f);
    snprintf(p, sizeof p, "%s\\case\\new.bin", T);                   /* a new name goes into the existing folder */
    f = fopen(p, "wb");
    CHECK(f != NULL && fsize(tp("Case/new.bin")) == 0 && fsize(tp("case")) == -1);
    if (f) fclose(f);
    snprintf(p, sizeof p, "%s\\CASE\\NEW.BIN", T);
    CHECK(DeleteFileA(p) && fsize(tp("Case/new.bin")) == -1);
    /* the listing cache: a folder unchanged for > 2 s is cached; a change (new mtime) reads it again */
    put(tp("Cache/One.bin"), "1");
    sleep(3);
    RES("cache\\ONE.BIN", 1, "Cache/One.bin");
    RES("cache\\ONE.BIN", 1, "Cache/One.bin");                       /* from the cache */
    put(tp("Cache/Two.bin"), "2");
    RES("cache\\two.bin", 1, "Cache/Two.bin");
    snprintf(a, sizeof a, "%s\\CACHE\\one.bin", T); snprintf(b, sizeof b, "%s\\cache\\Three.bin", T);
    CHECK(MoveFileExA(a, b, 0) && fsize(tp("Cache/Three.bin")) == 1);
    RES("cache\\one.bin", 0, "Cache/one.bin");
    snprintf(a, sizeof a, "%s\\cache\\TWO.BIN", T); snprintf(b, sizeof b, "%s\\cache\\THREE.bin", T);
    CHECK(MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING));            /* replaces Three.bin: no second file */
    snap(tp("Cache"), "", s, sizeof s);
    CHECKF(!strcmp(s, "Three.bin=2;"), "%s", s);
#undef RES
}

/* ================================================================ 4. directory enumeration order */
static const char *const k_ui[] = {"b.bin", "A.bin", "_x", "a1", "Z", "10", "9", "[1].txt", "Menu/"};
static const char *const k_ui_order[] = {"10", "9", "A.bin", "a1", "b.bin", "Menu/", "Z", "[1].txt", "_x"};   /* NTFS: upper-case ordinal */
static int list(const char *guest, int mode, int chunk, char *out, size_t cap) {   /* names joined by ',' (folders end in '/'), or -1 */
    uint64_t dh = 0;
    int64_t count = -1, got = 0, total = 0;
    uint8_t e[4 * 0x310];
    char name[0x310];
    out[0] = 0;
    if (gc("13OpenDirectoryEPNS0_15DirectoryHandleEPKci", 3, G(&dh), G(guest), (uint64_t)mode)) return -1;
    gc("22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", 2, G(&count), dh);
    do {
        gc("13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", 4, G(&got), G(e), dh, (uint64_t)chunk);
        for (int i = 0; i < got; i++) { snprintf(name, sizeof name, "%s%s", (char *)e + i * 0x310, e[i * 0x310 + 0x304] ? "" : "/"); join_names(out, cap, name); }
        total += got;
    } while (got > 0);
    gc("14CloseDirectoryENS0_15DirectoryHandleE", 1, dh);
    return total == count ? (int)total : -2;
}
static void find_all(const char *pattern, char *out, size_t cap) {
    WIN32_FIND_DATAA fd;
    out[0] = 0;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do join_names(out, cap, fd.cFileName); while (FindNextFileA(h, &fd));
    FindClose(h);
}
static void t_order(void) {
    SECTION("directory enumeration order: FindFirstFileA (NTFS order, patterns), nnfsdir OpenDirectory/ReadDirectory, journal view");
    char p[2048], want[512] = ".,..", got[512];
    mkdir(tp("romfs/ui"), 0777);
    for (size_t i = 0; i < 9; i++) {
        snprintf(p, sizeof p, "romfs/ui/%s", k_ui[i]);
        if (strchr(k_ui[i], '/')) mkdir(tp(p), 0777); else put(tp(p), "x");
    }
    for (size_t i = 0; i < 9; i++) { snprintf(p, sizeof p, "%s", k_ui_order[i]); p[strcspn(p, "/")] = 0; join_names(want, sizeof want, p); }
    snprintf(p, sizeof p, "%s\\ROMFS\\UI\\*", T);
    find_all(p, got, sizeof got);
    CHECKF(!strcmp(got, want), "FindFirstFileA *: %s", got);
#define FIND(pat, expect) do { snprintf(p, sizeof p, "%s/romfs/ui/%s", T, pat); find_all(p, got, sizeof got); CHECKF(!strcmp(got, expect), "FindFirstFileA %s: \"%s\"", pat, got); } while (0)
    FIND("*.BIN", "A.bin,b.bin");
    FIND("[1]*", "[1].txt");                       /* '[' is literal in Win32 (fnmatch alone would match "10") */
    FIND("A?", "a1");
    FIND("menu", "Menu");
    FIND("*.*", ".,..,10,9,A.bin,a1,b.bin,Menu,Z,[1].txt,_x");
    FIND("nothing*", "");
    CHECK(GetLastError() == ERROR_FILE_NOT_FOUND);
#undef FIND
    want[0] = 0;                                   /* nnfsdir: the same order without "." and "..", in chunks, per mode */
    for (size_t i = 0; i < 9; i++) join_names(want, sizeof want, k_ui_order[i]);
    CHECK(list("rom:/UI", 3, 4, got, sizeof got) == 9);
    CHECKF(!strcmp(got, want), "ReadDirectory: %s", got);
    CHECK(list("rom:/ui", 1, 4, got, sizeof got) == 1 && !strcmp(got, "Menu/"));
    CHECK(list("rom:\\ui\\", 2, 1, got, sizeof got) == 8 && !strstr(got, "Menu"));
    CHECK(list("rom:/nothing", 3, 4, got, sizeof got) == -1 && C.x[0] == R_PATH_NOT_FOUND);
    /* journaled save data: the working folder first, then the committed folder minus what was deleted or replaced */
    put(tp("ud/save/lst/c1"), "c"); put(tp("ud/save/lst/B2"), "b"); put(tp("ud/save/lst/zz"), "z");
    uint64_t g = 0;
    CHECK(MOUNT_SAVE("ls") == 0);
    CHECK(CREATE("ls:/lst/a3", 0) == 0 && DELETE("ls:/lst/c1") == 0);
    CHECK(OPEN(&g, "ls:/lst/b2", 7) == 0 && WRITE(g, 0, "B", 1) == 0);   /* copy-on-write under the guest's spelling */
    CLOSE(g);
    CHECK(list("ls:/lst", 3, 4, got, sizeof got) == 3);
    CHECKF(!strcmp(got, "a3,b2,zz"), "journal view: %s", got);
    CHECK(DELETE("ls:/lst/a3") == 0 && DELETE("ls:/lst/B2") == 0 && DELETE("ls:/lst/zz") == 0);
    CHECK(RMDIR("ls:/lst") == 0 && list("ls:/lst", 3, 4, got, sizeof got) == -1);
    UNMOUNT("ls");
    CHECK(fsize(tp("ud/save/lst/zz")) == 1);                         /* not committed: unchanged */
}

/* ================================================================ 5. MoveFileExA, CopyFileA */
static void t_writethrough(void) {
    SECTION("MoveFileExA (also WRITE_THROUGH, which only adds fsyncs): REPLACE_EXISTING, other case, error codes; CopyFileA");
    char a[2048], b[2048], s[512] = "";
    put(tp("wt/a/x.bin"), "x"); put(tp("wt/b/y.bin"), "old");
    snprintf(a, sizeof a, "%s\\WT\\A\\x.bin", T); snprintf(b, sizeof b, "%s\\wt\\B\\Y.BIN", T);
    CHECK(!MoveFileExA(a, b, 0) && GetLastError() == ERROR_ALREADY_EXISTS && fsize(tp("wt/a/x.bin")) == 1);
    CHECK(MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    snap(tp("wt"), "", s, sizeof s);
    CHECKF(!strcmp(s, "a/;b/;b/y.bin=x;"), "%s", s);
    snprintf(a, sizeof a, "%s/wt/b/y.bin", T); snprintf(b, sizeof b, "%s/wt/b/z.bin", T);
    CHECK(MoveFileExA(a, b, MOVEFILE_WRITE_THROUGH));                                                    /* one folder */
    CHECK(MoveFileExA(b, a, 0) && fsize(a) == 1 && fsize(b) == -1);
    snprintf(s, sizeof s, "%s/wt/b/none.bin", T);
    CHECK(!MoveFileExA(b, s, 0) && GetLastError() == ERROR_FILE_NOT_FOUND);                              /* the source is gone */
    CHECK(CopyFileA(a, b, TRUE) && fsize(b) == 1 && !CopyFileA(a, b, TRUE) && (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS));
}

/* ================================================================ 6. save commit with kill -9 */
static int in_child(int (*fn)(void));
static char g_old[2048], g_new[2048];
static void old_state(void) {
    rm_rf(tp("ud"));
    put(tp("ud/save/a.bin"), "old-a"); put(tp("ud/save/b.bin"), "old-b"); put(tp("ud/save/d.bin"), "old-d");
    put(tp("ud/save/olddir/o1.bin"), "o1"); put(tp("ud/save/olddir/o2.bin"), "o2");
    put(tp("ud/save/re/x.bin"), "old-x"); put(tp("ud/save/keep.bin"), "keep");
}
static void wr(const char *path, const char *text) {
    uint64_t g = 0;
    if (OPEN(&g, path, 7) || WRITE(g, 0, text, strlen(text))) _exit(4);
    CLOSE(g);
}
static void changes(void) {                        /* every kind of change, in the guest's spelling (other case, '\\') */
    if (MOUNT_SAVE("save")) _exit(4);
    wr("save:/A.BIN", "new-a-longer");                                     /* modified (copy-on-write) */
    if (DELETE("save:\\b.bin")) _exit(4);                                  /* deleted */
    if (MKDIR("save:/dir1") || CREATE("save:/dir1/c.bin", 0)) _exit(4);
    wr("save:/dir1/c.bin", "new-c");                                       /* new folder and file */
    if (DELETE("save:/d.bin") || CREATE("save:/D.bin", 0)) _exit(4);
    wr("save:/D.bin", "new-d");                                            /* deleted and created again */
    if (DELETE("save:/olddir/o1.bin") || DELETE("save:/olddir/o2.bin") || RMDIR("save:/olddir")) _exit(4);   /* folder deleted */
    if (DELETE("save:/re/x.bin") || RMDIR("save:/re") || MKDIR("save:/RE") || CREATE("save:/re/y.bin", 0)) _exit(4);
    wr("save:/re/y.bin", "new-y");                                         /* folder deleted and created again */
}
static int run_child(int what, int kill_at) {      /* what 0: changes + Commit, 1: mount (= recovery) + unmount; -> wait status */
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        if (!getenv("FS_TRACE")) { int dn = open("/dev/null", O_WRONLY); dup2(dn, 1); }
        if (what == 0) changes();
        hook_on(kill_at);
        uint32_t r = what == 0 ? COMMIT("save") : MOUNT_SAVE("save");
        hook_off();
        if (what == 0) {
            FILE *f = fopen(tp("oplog"), "w");
            for (int i = 0; f && i < g_nlog; i++) fprintf(f, "%s\n", g_log[i]);
            if (f) fclose(f);
        } else UNMOUNT("save");
        _exit(r ? 3 : 0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return st;
}
static int killed_(int st) { return WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL; }       /* (a function: macOS's WIF* macros take an lvalue) */
static int done_(int st) { return WIFEXITED(st) && WEXITSTATUS(st) == 0; }
#define KILLED(st) killed_(st)
#define DONE(st) done_(st)
static int leftovers(void) {                       /* save.commit / save.journal.* / save.work.* after a recovery */
    DIR *d = opendir(tp("ud"));
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d))) n += !strncmp(e->d_name, "save.", 5);
    if (d) closedir(d);
    return n;
}
static void t_commit_kill(void) {
    SECTION("save Commit, SIGKILL before each mutating file operation (and again inside the roll-forward): old or new, never partial");
    char s[2048];
    old_state();
    g_old[0] = 0; snap(tp("ud/save"), "", g_old, sizeof g_old);
    int st = run_child(0, 0);
    CHECK(DONE(st));
    g_new[0] = 0; snap(tp("ud/save"), "", g_new, sizeof g_new);
    CHECKF(!strcmp(g_new, "D.bin=new-d;RE/;RE/y.bin=new-y;a.bin=new-a-longer;dir1/;dir1/c.bin=new-c;keep.bin=keep;"), "new state %s", g_new);
    CHECK(DONE(run_child(1, 0)) && leftovers() == 0);              /* the next mount drops the dead child's (now empty) working folder */
    s[0] = 0; snap(tp("ud/save"), "", s, sizeof s);
    CHECK(!strcmp(s, g_new));
    /* the call log of a whole commit: the journal is flushed, then the move that decides; after it only repeatable steps, the marker's removal (and its folder's fsync) last */
    g_nlog = 0;
    FILE *f = fopen(tp("oplog"), "r");
    while (f && g_nlog < 256 && fgets(g_log[g_nlog], sizeof g_log[0], f)) { g_log[g_nlog][strcspn(g_log[g_nlog], "\n")] = 0; g_nlog++; }
    if (f) fclose(f);
    int nops = g_nlog;
    int dec = -1, moves = 0;
    for (int i = 0; i < nops; i++) {
        if (dec < 0 && !strncmp(g_log[i], "move ", 5) && strstr(g_log[i], ".commit")) dec = i;
        if (dec >= 0 && i > dec && !strncmp(g_log[i], "move ", 5)) moves++;
    }
    CHECKF(dec > 0 && !strncmp(g_log[dec - 1], "fsync ", 6) && nops > dec + 1, "the deciding move is at %d of %d, after a flush", dec, nops);
    CHECKF(moves == 4, "%d file moves after the decision (a.bin, dir1/c.bin, D.bin, re/y.bin)", moves);
    int unmark = -1, touched_root = 0, cleanup_ok = 1;
    for (int i = 0; i < nops; i++) {
        if (!strncmp(g_log[i], "unlink ", 7) && strstr(g_log[i], ".commit")) unmark = i;
        if (i < dec && (strstr(g_log[i], "ud\\save\\") || strstr(g_log[i], "ud/save/"))) touched_root = 1;       /* R before the decision */
    }
    for (int i = unmark + 2; unmark >= 0 && i < nops; i++)            /* after the marker: only the stage folder goes, the working folder is emptied */
        if (!(strstr(g_log[i], ".stage.") || strstr(g_log[i], ".work.")) || !(!strncmp(g_log[i], "unlink ", 7) || !strncmp(g_log[i], "rmdir ", 6) || !strncmp(g_log[i], "mkdir ", 6))) cleanup_ok = 0;
    CHECKF(!touched_root, "the committed folder was changed before the decision");
    CHECKF(unmark > dec && unmark + 1 < nops && !strncmp(g_log[unmark + 1], "fsync-dir ", 10) && cleanup_ok,
           "the marker is removed after the moves, then its folder fsynced, then only the stage and working folders are cleaned (marker at %d of %d)", unmark, nops);
    printf("    one commit = %d mutating file calls (%d before the decision, the marker removed at %d)\n", nops, dec, unmark);
    /* kill before op k; then recover (if the commit was decided, also kill the recovery before each of its ops, then recover) */
    int runs = 0, as_old = 0, as_new = 0, rolled = 0, bad = 0;
    for (int k = 1; k <= nops; k++) {
        for (int r = 0;; r++) {
            old_state();
            st = run_child(0, k);
            if (!KILLED(st)) { CHECKF(0, "commit not killed at op %d (status %x)", k, st); bad++; break; }
            int decided = fsize(tp("ud/save.commit")) >= 0;
            int rst = r ? run_child(1, r) : 0, st2 = run_child(1, 0);
            runs++;
            s[0] = 0; snap(tp("ud/save"), "", s, sizeof s);
            int is_old = !strcmp(s, g_old), is_new = !strcmp(s, g_new), left = leftovers();
            if (!DONE(st2) || left || !(is_new || (is_old && !decided))) {
                bad++;
                CHECKF(0, "kill before op %d (%s), recovery killed before op %d: %s, %d leftovers, state %s", k, g_log[k - 1], r, decided ? "decided" : "not decided", left, s);
            }
            as_old += is_old; as_new += is_new; rolled += decided && !r;
            if (!decided || (r && !KILLED(rst))) break;           /* nothing to roll forward, or the recovery ran to its end */
        }
    }
    CHECKF(as_old > 0 && rolled > 0, "both outcomes reached (old %d, rolled forward %d)", as_old, rolled);
    printf("    %d crash runs: %d ended in the old state, %d in the new state (%d kill points after the decision), %d bad\n", runs, as_old, as_new, rolled, bad);
}
/* ---- a Commit whose apply fails after the decision: the k-th file move, the k-th deletion, or the removal of the marker
 * (the folder that holds the target is made read-only just before that call; on Windows: a full disk, a read-only file,
 * an antivirus holding the file). The commit is then PENDING (see nnfs.c, "THE COMMIT PROTOCOL"): the game must see the
 * decided state at once, and every way on must end in exactly that state, never the old one, a mix, or a write made after
 * the failed Commit without a Commit of its own. */
enum { FAIL_MOVE, FAIL_DELETE, FAIL_MARKER };
static const char *const k_fail_kind[] = {"move", "deletion", "marker removal"};
static int g_fail_kind, g_fail_at, g_fail_n, g_skip_fail;
static char g_ro[2048];
static void fail_hook(const char *op, const char *path) {
    char hp[2048];
    if (!path) return;
    host_path_resolve(path, hp, sizeof hp);              /* (0 when the target is not there yet: the path is the same) */
    int hit = g_fail_kind == FAIL_MOVE ? !strcmp(op, "move") && strstr(hp, "/ud/save/") != NULL
            : g_fail_kind == FAIL_DELETE ? (!strcmp(op, "unlink") || !strcmp(op, "rmdir")) && strstr(hp, "/ud/save/") != NULL
            : !strcmp(op, "unlink") && strlen(hp) > 12 && !strcmp(hp + strlen(hp) - 12, "/save.commit");
    if (hit && ++g_fail_n == g_fail_at) { snprintf(g_ro, sizeof g_ro, "%s", hp); *strrchr(g_ro, '/') = 0; chmod(g_ro, 0555); }
}
static int fail_commit(int kind, int k) {          /* in a child, after changes(): 1 = Commit failed at that call */
    g_fail_kind = kind; g_fail_at = k; g_fail_n = 0; g_ro[0] = 0; fi_hook = fail_hook;
    uint32_t r = COMMIT("save");
    fi_hook = NULL;
    return r && g_ro[0];
}
static int rd_is(const char *guest, const char *want) {   /* the guest's view of one file (want NULL: it does not exist) */
    uint64_t g = 0, got = 0;
    char buf[64];
    if (OPEN(&g, guest, 1)) return want == NULL;
    READN(&got, g, 0, buf, sizeof buf - 1);
    CLOSE(g);
    buf[got] = 0;
    return want && !strcmp(buf, want);
}
static int view_is_new(void) {                     /* the decided state of changes(), as the game reads it */
    return rd_is("save:/a.bin", "new-a-longer") && rd_is("save:/b.bin", NULL) && rd_is("save:/d.bin", "new-d") && rd_is("save:/dir1/c.bin", "new-c")
        && rd_is("save:/re/y.bin", "new-y") && rd_is("save:/re/x.bin", NULL) && rd_is("save:/olddir/o1.bin", NULL) && rd_is("save:/keep.bin", "keep");
}
static void more_changes(void) {                   /* after a failed Commit: delete a file that exists only in the pending commit, change another */
    if (DELETE("save:/dir1/c.bin")) _exit(4);
    wr("save:/keep.bin", "keep3");
}
static void uncommitted(void) {                    /* after a failed Commit: changes that are never committed */
    wr("save:/A.BIN", "UNCOMMITTED");
    if (CREATE("save:/unc.bin", 0)) _exit(4);
    wr("save:/unc.bin", "unc");
    if (DELETE("save:/re/y.bin")) _exit(4);
}
static const char *const k_new = "D.bin=new-d;RE/;RE/y.bin=new-y;a.bin=new-a-longer;dir1/;dir1/c.bin=new-c;keep.bin=keep;";
static const char *const k_more = "D.bin=new-d;RE/;RE/y.bin=new-y;a.bin=new-a-longer;dir1/;keep.bin=keep3;";
enum { THEN_COMMIT, THEN_KILL, THEN_UNMOUNT, THEN_UNMOUNT_FAILING, THEN_UNCOMMITTED_UNMOUNT, THEN_UNCOMMITTED_KILL, THEN_MORE_COMMIT, THEN_N };
static const char *const k_then[] = {"Commit again", "SIGKILL", "Unmount", "Unmount while it still fails", "uncommitted changes, Unmount",
                                     "uncommitted changes, SIGKILL", "more changes (one deletes a file only the pending commit has), Commit"};
/* changes, a Commit that fails at the k-th call of `kind`, then `then`; with kill_at, the follow-up is SIGKILLed before its
 * kill_at-th mutating call (-1: count them into "opcount"). Exit 5: the failure could not be made (root?), 6: the game's view
 * after the failed Commit is not the decided state, 7: no pending marker. */
static int run_fail_child(int kind, int k, int then, int kill_at) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        if (!getenv("FS_TRACE")) { int dn = open("/dev/null", O_WRONLY); dup2(dn, 1); }
        changes();
        if (!fail_commit(kind, k)) _exit(5);
        if (fsize(tp("ud/save.commit")) < 0) _exit(7);
        if (then != THEN_UNMOUNT_FAILING) chmod(g_ro, 0777);
        if (!view_is_new()) _exit(6);
        if (kill_at) hook_on(kill_at < 0 ? 0 : kill_at);
        uint32_t r = 0;
        switch (then) {
        case THEN_COMMIT: r = COMMIT("save"); break;
        case THEN_KILL: kill(getpid(), SIGKILL); break;
        case THEN_UNMOUNT: UNMOUNT("save"); break;
        case THEN_UNMOUNT_FAILING: UNMOUNT("save"); chmod(g_ro, 0777); break;
        case THEN_UNCOMMITTED_UNMOUNT: uncommitted(); UNMOUNT("save"); break;
        case THEN_UNCOMMITTED_KILL: uncommitted(); hook_off(); kill(getpid(), SIGKILL); break;
        case THEN_MORE_COMMIT: more_changes(); r = COMMIT("save"); if (!r && !rd_is("save:/dir1/c.bin", NULL)) r = 8; break;
        }
        hook_off();
        if (kill_at < 0) { FILE *f = fopen(tp("opcount"), "w"); if (f) { fprintf(f, "%d\n", g_ops); fclose(f); } }
        _exit(r ? 3 : 0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return st;
}
static void t_commit_fail(void) {
    SECTION("save Commit whose apply fails (a move, a deletion, the marker's removal), then 7 ways on, next mount: the decided state");
    static const int ks[3][3] = {{1, 2, 4}, {1, 5, 7}, {1, 0, 0}};     /* move 1..4; deletion 1 = b.bin, 5 = rmdir olddir, 7 = rmdir re; the marker */
    int runs = 0, bad = 0;
    if (geteuid() == 0) { printf("    root: a read-only folder does not make a call fail: skipped\n"); g_skip_fail = 1; return; }
    for (int kind = 0; kind < 3; kind++)
        for (int ki = 0; ki < 3 && ks[kind][ki]; ki++)
            for (int t = 0; t < THEN_N; t++) {
                char s[2048] = "";
                const char *want = t == THEN_MORE_COMMIT ? k_more : k_new;
                old_state();
                int st = run_fail_child(kind, ks[kind][ki], t, 0);
                int st2 = run_child(1, 0);
                snap(tp("ud/save"), "", s, sizeof s);
                runs++;
                int killed = t == THEN_KILL || t == THEN_UNCOMMITTED_KILL;
                if (!(killed ? KILLED(st) : DONE(st)) || !DONE(st2) || leftovers() || strcmp(s, want)) {
                    bad++;
                    CHECKF(0, "%s %d fails, then %s: status %x/%x%s, %d leftovers, state %s", k_fail_kind[kind], ks[kind][ki], k_then[t], st, st2,
                           WIFEXITED(st) && WEXITSTATUS(st) == 5 ? " (Commit reported success)" : WIFEXITED(st) && WEXITSTATUS(st) == 6 ? " (the game's view after the failed Commit was not the decided state)" : "", leftovers(), s);
                }
            }
    CHECKF(bad == 0, "%d of %d failed-apply runs bad", bad, runs);
    printf("    %d failed-apply runs, %d bad\n", runs, bad);
}
/* the same, with the follow-up SIGKILLed before each of its mutating calls (the pending commit's APPLY inside Commit, inside
 * Unmount, the new commit's stage / decision / apply), then the next mount */
static void t_commit_fail_kill(void) {
    SECTION("pending commit: SIGKILL before each call of Commit again / uncommitted changes + Unmount / more changes + Commit");
    static const int kinds[3][2] = {{FAIL_MOVE, 2}, {FAIL_DELETE, 1}, {FAIL_MARKER, 1}};
    static const int thens[3] = {THEN_COMMIT, THEN_UNCOMMITTED_UNMOUNT, THEN_MORE_COMMIT};
    int runs = 0, bad = 0, as_more = 0;
    for (int ki = 0; ki < 3; ki++)
        for (int ti = 0; ti < 3; ti++) {
            int kind = kinds[ki][0], k = kinds[ki][1], t = thens[ti], nops = 0;
            char s[2048];
            old_state();
            if (!DONE(run_fail_child(kind, k, t, -1))) { CHECKF(0, "%s %d, then %s: the counting run failed", k_fail_kind[kind], k, k_then[t]); continue; }
            FILE *f = fopen(tp("opcount"), "r");
            if (f) { if (fscanf(f, "%d", &nops) != 1) nops = 0; fclose(f); }
            CHECKF(nops > 3, "%s, then %s: %d calls", k_fail_kind[kind], k_then[t], nops);
            for (int kk = 1; kk <= nops; kk++) {
                old_state();
                int st = run_fail_child(kind, k, t, kk);
                chmod(tp("ud/save"), 0777); chmod(tp("ud"), 0777);
                int st2 = run_child(1, 0);
                s[0] = 0; snap(tp("ud/save"), "", s, sizeof s);
                runs++;
                int ok = KILLED(st) && DONE(st2) && !leftovers() && (!strcmp(s, k_new) || (t == THEN_MORE_COMMIT && !strcmp(s, k_more)));
                as_more += !strcmp(s, k_more);
                if (!ok) { bad++; CHECKF(0, "%s %d fails, then %s killed before call %d: status %x/%x, %d leftovers, state %s", k_fail_kind[kind], k, k_then[t], kk, st, st2, leftovers(), s); }
            }
        }
    CHECKF(bad == 0 && as_more > 0, "%d of %d runs bad, %d reached the later commit", bad, runs, as_more);
    printf("    %d crash runs after a failed apply: %d bad (%d ended in the later commit's state, the rest in the pending one's)\n", runs, bad, as_more);
}
/* Two processes: A's Commit fails and A dies; B mounts while the APPLY still fails (the save folder read-only), must see A's
 * decided state, deletes a file that exists only in it, changes another and commits. B is SIGKILLed before each of its
 * mutating calls (its mount's APPLY attempt and copy, its Commit's APPLY of A's commit, its own stage, decision and apply).
 * Before this was fixed B's Commit replaced A's marker and A's unmoved files were dropped: the save was neither state. */
static int g_twoproc_kind;
static int child_a(void) {
    if (!getenv("FS_TRACE")) { int dn = open("/dev/null", O_WRONLY); dup2(dn, 1); }
    changes();
    if (!fail_commit(g_twoproc_kind, 1)) return 5;
    chmod(g_ro, 0777);
    kill(getpid(), SIGKILL);
    return 0;
}
static int run_b(int kill_at) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        if (!getenv("FS_TRACE")) { int dn = open("/dev/null", O_WRONLY); dup2(dn, 1); }
        hook_on(kill_at < 0 ? 0 : kill_at);
        chmod(tp("ud/save"), 0555);
        uint32_t r = MOUNT_SAVE("save");
        chmod(tp("ud/save"), 0777);
        if (r) _exit(3);
        if (fsize(tp("ud/save.commit")) < 0) _exit(7);                   /* (the APPLY at the mount must have failed) */
        if (!view_is_new()) _exit(6);
        more_changes();
        r = COMMIT("save");
        if (!r && !rd_is("save:/dir1/c.bin", NULL)) r = 8;
        UNMOUNT("save");
        hook_off();
        if (kill_at < 0) { FILE *f = fopen(tp("opcount"), "w"); if (f) { fprintf(f, "%d\n", g_ops); fclose(f); } }
        _exit(r ? 3 : 0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return st;
}
static void t_two_process(void) {
    SECTION("two processes: A's apply fails and A dies, B mounts (its APPLY fails too), sees A's state, commits; SIGKILL B before each call");
    int runs = 0, bad = 0, as_more = 0;
    for (int kind = FAIL_MOVE; kind <= FAIL_DELETE; kind++) {
        char s[2048];
        int nops = 0;
        g_twoproc_kind = kind;
        old_state();
        if (in_child(child_a) != 99) { CHECKF(0, "%s: A did not fail and die", k_fail_kind[kind]); continue; }
        int st = run_b(-1);
        s[0] = 0; snap(tp("ud/save"), "", s, sizeof s);
        CHECKF(DONE(st) && !strcmp(s, k_more), "%s: B status %x, state %s", k_fail_kind[kind], st, s);
        FILE *f = fopen(tp("opcount"), "r");
        if (f) { if (fscanf(f, "%d", &nops) != 1) nops = 0; fclose(f); }
        for (int kk = 1; kk <= nops; kk++) {
            old_state();
            if (in_child(child_a) != 99) { bad++; continue; }
            st = run_b(kk);
            chmod(tp("ud/save"), 0777);
            int st2 = run_child(1, 0);
            s[0] = 0; snap(tp("ud/save"), "", s, sizeof s);
            runs++;
            as_more += !strcmp(s, k_more);
            if (!KILLED(st) || !DONE(st2) || leftovers() || (strcmp(s, k_new) && strcmp(s, k_more))) {
                bad++;
                CHECKF(0, "%s: B killed before call %d: status %x/%x, %d leftovers, state %s", k_fail_kind[kind], kk, st, st2, leftovers(), s);
            }
        }
    }
    CHECKF(bad == 0 && as_more > 0, "%d of %d two-process runs bad", bad, runs);
    printf("    %d two-process crash runs: %d bad (%d ended in B's state, the rest in A's)\n", runs, bad, as_more);
}
/* Markers of earlier builds name their own working folder (R.work.<pid>) as W. They are applied as before; one still
 * pending is started from by a mount; and when its pid is this process's own (a reused pid) the mount works elsewhere and
 * leaves that folder alone. */
static void old_marker(const char *wpid) {         /* the decided commit of changes() as an earlier build wrote it */
    char w[2048], m[4096];
    snprintf(w, sizeof w, "ud/save.work.%s", wpid);
    char p[2048];
    snprintf(p, sizeof p, "%s/a.bin", w); put(tp(p), "new-a-longer");
    snprintf(p, sizeof p, "%s/D.bin", w); put(tp(p), "new-d");
    snprintf(p, sizeof p, "%s/dir1/c.bin", w); put(tp(p), "new-c");
    snprintf(p, sizeof p, "%s/RE/y.bin", w); put(tp(p), "new-y");
    snprintf(m, sizeof m, "W\t%s\\save.work.%s\nR\t%s\\save\nD\tb.bin\nD\td.bin\nD\tolddir\nD\tre\nF\ta.bin\nF\tD.bin\nM\tdir1\nF\tdir1\\c.bin\nM\tRE\nF\tRE\\y.bin\n",
             tp("ud"), wpid, tp("ud"));
    put(tp("ud/save.commit"), m);
}
static int child_old_reused(void) {
    if (!getenv("FS_TRACE")) { int dn = open("/dev/null", O_WRONLY); dup2(dn, 1); }
    char pid[32];
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    old_marker(pid);
    chmod(tp("ud/save"), 0555);                                  /* its APPLY fails at the mount */
    uint32_t r = MOUNT_SAVE("save");
    chmod(tp("ud/save"), 0777);
    if (r || !view_is_new()) return 6;
    char w[2048]; snprintf(w, sizeof w, "ud/save.work.%s/dir1/c.bin", pid);
    if (fsize(tp(w)) < 0) return 7;                              /* the pending commit's folder was not wiped by the mount */
    more_changes();
    r = COMMIT("save");
    UNMOUNT("save");
    return r ? 3 : 0;
}
static void t_old_marker(void) {
    SECTION("markers of earlier builds (W = their working folder): applied at the mount, started from while pending, a reused pid");
    char s[2048];
    old_state(); old_marker("4294967290");                       /* a dead process */
    CHECK(DONE(run_child(1, 0)) && !leftovers());
    s[0] = 0; snap(tp("ud/save"), "", s, sizeof s);
    CHECKF(!strcmp(s, k_new), "applied at the mount: %s", s);
    old_state(); old_marker("4294967290");
    chmod(tp("ud/save"), 0555);
    CHECK(DONE(run_child(1, 0)));                                /* APPLY fails: the marker and its folder stay */
    chmod(tp("ud/save"), 0777);
    CHECK(fsize(tp("ud/save.commit")) >= 0 && fsize(tp("ud/save.work.4294967290/dir1/c.bin")) == 5);
    CHECK(DONE(run_child(1, 0)) && !leftovers());
    s[0] = 0; snap(tp("ud/save"), "", s, sizeof s);
    CHECKF(!strcmp(s, k_new), "applied at the next mount: %s", s);
    old_state();
    CHECK(in_child(child_old_reused) == 0);
    CHECK(DONE(run_child(1, 0)) && !leftovers());
    s[0] = 0; snap(tp("ud/save"), "", s, sizeof s);
    CHECKF(!strcmp(s, k_more), "a reused pid: %s", s);
}

/* ================================================================ 7. config and font paths, exe dir, the built-in self-tests */
static int in_child(int (*fn)(void)) { fflush(NULL); pid_t p = fork(); if (!p) _exit(fn()); int st; waitpid(p, &st, 0); return WIFEXITED(st) ? WEXITSTATUS(st) : 99; }
static int gfx_existing(void) {                    /* an existing config in another case, named with backslashes */
    char e[2048];
    put(tp("conf/GRAPHICS.INI"), "width = 1000\r\n");
    snprintf(e, sizeof e, "%s\\Conf\\graphics.ini", T);
    setenv("SSBU_GRAPHICS_CONFIG", e, 1);
    const GfxOptions *o = gfx_options();
    return !(o->width == 1000);                       /* read from the file in the other case; the path string stays as given */
}
static int gfx_new(void) {                         /* none yet: the folders are made and the defaults written */
    char e[2048];
    snprintf(e, sizeof e, "%s\\newconf\\deep\\graphics.ini", T);
    setenv("SSBU_GRAPHICS_CONFIG", e, 1);
    const GfxOptions *o = gfx_options();
    return !(o->width == 1280 && fsize(tp("newconf/deep/graphics.ini")) > 100);
}
static int font_any_case(void) {
    char e[2048];
    put(tp("Fonts/NINTENDO_UDSG-R_STD_003.TTF"), "font bytes");
    snprintf(e, sizeof e, "%s\\fonts", T);
    setenv("SSBU_FONTS", e, 1);
    register_pl_imports();
    uint32_t n = gc("17GetSharedFontSizeENS0_14SharedFontTypeE", 1, 0ull);
    gc("20GetSharedFontAddressENS0_14SharedFontTypeE", 1, 0ull);
    return !(n == 10 && C.x[0] && !memcmp((void *)(uintptr_t)C.x[0], "font bytes", 10));
}
static void t_misc(void) {
    SECTION("graphics.ini and shared-font paths (any case, '\\\\'), host_exe_dir, nnfs_selftest, nnfsdir_selftest");
    CHECK(in_child(gfx_existing) == 0);
    CHECK(in_child(gfx_new) == 0);
    CHECK(in_child(font_any_case) == 0);
#ifdef __linux__
    char exe[2048];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = 0;
    if (strrchr(exe, '/')) {
        *(strrchr(exe, '/') + 1) = 0;
        CHECKF(!strcmp(host_exe_dir(), exe), "%s", host_exe_dir());
    }
#endif
    mkdir(tp("selftest"), 0777);
    setenv("TMPDIR", tp("selftest"), 1);
    printf("    nnfs_selftest\n"); fflush(stdout);
    CHECK(nnfs_selftest(&C) == 0);
    printf("    nnfsdir_selftest\n"); fflush(stdout);
    CHECK(nnfsdir_selftest(&C) == 0);
}

int main(int argc, char **argv) {
    const char *base = argc > 1 ? argv[1] : getenv("TMPDIR") && getenv("TMPDIR")[0] ? getenv("TMPDIR") : "/tmp";
    snprintf(T, sizeof T, "%s/test_fs.XXXXXX", base);
    if (!mkdtemp(T)) { printf("mkdtemp %s: %s\n", T, strerror(errno)); return 2; }
    printf("test_fs: scratch folder %s\n", T);
    char env[2048];
    snprintf(env, sizeof env, "%s\\romfs", T); setenv("SSBU_ROMFS", env, 1);   /* a backslash in the variable */
    snprintf(env, sizeof env, "%s/ud", T); setenv("SSBU_USERDATA", env, 1);
    register_fs_imports();
    register_nnfsdir_imports();
    t_romfs();
    t_rw();
    {   /* the case-insensitive layer needs a file system that tells "dup.bin" and "DUP.bin" apart (macOS: run.sh makes a case-sensitive volume) */
        put(tp("probe.low"), "x");
        int ci = fsize(tp("PROBE.LOW")) >= 0;
        rm_rf(tp("probe.low"));
        if (ci) printf("  - case-insensitive lookup: this scratch folder is on a case-insensitive file system: skipped\n");
        else t_case();
    }
    t_order();
    t_writethrough();
    if (!getenv("FS_NO_KILL")) t_commit_kill();
    t_commit_fail();
    if (!getenv("FS_NO_KILL") && !g_skip_fail) { t_commit_fail_kill(); t_two_process(); }
    if (!g_skip_fail) t_old_marker();
    t_misc();
    if (!mt_fail && !getenv("FS_KEEP")) rm_rf(T); else printf("test_fs: kept %s\n", T);
    return mt_done("test_fs");
}

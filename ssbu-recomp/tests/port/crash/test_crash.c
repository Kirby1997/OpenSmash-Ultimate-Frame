/* C4 crash-report test: runtime/plat_crash.c. Every crash happens in a forked child; the parent checks how the child ended, the
 * report file and what the child wrote to its stderr. Builds and runs natively on aarch64 Linux (the Steam Frame) and on macOS
 * arm64 (a development Mac: same code, Mach-O module snapshot, no /proc/self/maps). Standalone: needs only plat_crash.c.
 * Usage: test_crash [scratch-dir] */
#define _GNU_SOURCE
#include "plat_crash.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECKF(c, ...) do { g_checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, #c); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); g_fail++; } } while (0)
#ifdef __APPLE__
#define ON_MAC 1
#else
#define ON_MAC 0
#endif

static char g_dir[512];
static const char *g_prog = "test_crash";

/* ---------------------------------------------------------------------------------------------- harness */
typedef struct { int status; char err[65536]; char file[65536]; int has_file; double secs; } Run;
static void slurp(const char *path, char *out, size_t n, int *has) {
    FILE *f = fopen(path, "rb");
    out[0] = 0;
    if (has) *has = f != NULL;
    if (!f) return;
    size_t k = fread(out, 1, n - 1, f);
    out[k] = 0;
    fclose(f);
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }
/* runs body(path) in a child with stderr captured; close_err: the capture pipe's reader is closed before the child runs (SIGPIPE) */
static void run_child(Run *r, const char *name, void (*body)(const char *path), const char *envk, const char *envv, int keep_file) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s_crash.txt", g_dir, name);
    if (!keep_file) unlink(path);
    int p[2];
    if (pipe(p)) { perror("pipe"); exit(2); }
    double t0 = now();
    fflush(NULL);
    pid_t pid = fork();
    if (!pid) {
        dup2(p[1], 2); close(p[0]); close(p[1]);
        if (envk) setenv(envk, envv, 1);
        body(path);
        _exit(77);                                       /* the body returned: no crash */
    }
    close(p[1]);
    size_t len = 0;
    for (;;) { ssize_t k = read(p[0], r->err + len, sizeof r->err - 1 - len); if (k <= 0) break; len += (size_t)k; }
    r->err[len] = 0;
    close(p[0]);
    waitpid(pid, &r->status, 0);
    r->secs = now() - t0;
    slurp(path, r->file, sizeof r->file, &r->has_file);
}
static int died_of(const Run *r, int sig) { return WIFSIGNALED(r->status) && WTERMSIG(r->status) == sig; }
static const char *g_mod;                                /* this executable's name as the report gives it */
static int count_str(const char *s, const char *needle) { int n = 0; for (const char *p = s; (p = strstr(p, needle)); p += strlen(needle)) n++; return n; }

/* ---------------------------------------------------------------------------------------------- crash makers */
static volatile int g_sink;
static void callback_marker(const crash_info *ci) { (void)ci; static const char m[] = "CALLBACK RAN\n"; write(2, m, sizeof m - 1); }
static void section_marker(const crash_info *ci) { crash_put("SECTION pc="); crash_put_hex(ci->pc, 0); crash_put(" npcs="); crash_put_dec(ci->npcs); crash_put("\n"); }
static void section_faults(const crash_info *ci) { (void)ci; crash_put("before the fault\n"); g_sink = *(volatile int *)16; crash_put("not reached\n"); }
static void callback_faults(const crash_info *ci) { (void)ci; g_sink = *(volatile int *)16; }
static void callback_hangs(const crash_info *ci) { (void)ci; static const char m[] = "CALLBACK HANGS\n"; write(2, m, sizeof m - 1); for (;;) pause(); }
static void callback_exit(const crash_info *ci) { (void)ci; _exit(33); }

static __attribute__((noinline)) int recurse(int n) { if (n == 0) { *(volatile int *)8 = 1; return 0; } return recurse(n - 1) + (int)g_sink + 1; }
static __attribute__((noinline)) int overflow(int n) { volatile char pad[256]; pad[0] = (char)n; g_sink += pad[0]; return overflow(n + 1) + pad[(n & 15)]; }

static void b_null_write(const char *p) { crash_install(p, section_marker, callback_marker, 4); *(volatile int *)0 = 1; }
static void b_read(const char *p) { crash_install(p, NULL, NULL, 4); g_sink = *(volatile int *)8; }
static void b_recurse(const char *p) { crash_install(p, NULL, NULL, 4); recurse(6); }
static void b_overflow_main(const char *p) { crash_install(p, section_marker, NULL, 4); overflow(0); }
static void *thr_overflow(void *a) { (void)a; crash_thread_init(); overflow(0); return NULL; }
static void b_overflow_thread(const char *p) { crash_install(p, NULL, NULL, 4); pthread_t t; pthread_create(&t, NULL, thr_overflow, NULL); pthread_join(t, NULL); }
static void b_abort(const char *p) { crash_install(p, NULL, callback_marker, 4); abort(); }
static void b_section_faults(const char *p) { crash_install(p, section_faults, NULL, 4); *(volatile int *)0 = 1; }
static void b_callback_faults(const char *p) { crash_install(p, NULL, callback_faults, 4); *(volatile int *)0 = 1; }
static void b_callback_hangs(const char *p) { crash_install(p, NULL, callback_hangs, 4); *(volatile int *)0 = 1; }
static void b_callback_exits(const char *p) { crash_install(p, NULL, callback_exit, 4); *(volatile int *)0 = 1; }
static void b_exit_mode(const char *p) { crash_install(p, NULL, NULL, 4); *(volatile int *)0 = 1; }
static void b_no_crash(const char *p) { crash_install(p, NULL, NULL, 4); _exit(0); }
static void b_bad_dir(const char *p) { (void)p; crash_install("/nonexistent-dir/x_crash.txt", NULL, NULL, 4); *(volatile int *)0 = 1; }
static void b_illegal(const char *p) { crash_install(p, NULL, NULL, 4); __builtin_trap(); }
static void b_fpe_sent(const char *p) { crash_install(p, NULL, NULL, 4); raise(SIGFPE); }
static void *thr_stdio_lock(void *a) { (void)a; flockfile(stdout); flockfile(stderr); for (;;) pause(); return NULL; }
static void b_stdio_locked(const char *p) { crash_install(p, section_marker, NULL, 4); pthread_t t; pthread_create(&t, NULL, thr_stdio_lock, NULL); usleep(100000); *(volatile int *)0 = 1; }
static void *thr_crash(void *a) { crash_thread_init(); usleep(50000); *(volatile int *)(uintptr_t)a = 1; return NULL; }
static void b_two_threads(const char *p) { crash_install(p, NULL, NULL, 4); pthread_t t[2]; pthread_create(&t[0], NULL, thr_crash, (void *)0); pthread_create(&t[1], NULL, thr_crash, (void *)8);
    for (int i = 0; i < 2; i++) pthread_join(t[i], NULL); }
static void b_emfile(const char *p) {
    crash_install(p, NULL, NULL, 4);
    struct rlimit rl; getrlimit(RLIMIT_NOFILE, &rl);
    int top = 0; while (top < 4096) { int fd = dup(0); if (fd < 0) break; top = fd; }       /* fill the table */
    (void)rl;
    g_sink = *(volatile int *)0;
}
static uint8_t *g_page;
static int unprotect(void *addr) {
    if ((uint8_t *)addr < g_page || (uint8_t *)addr >= g_page + 65536) return 0;
    mprotect(g_page, 65536, PROT_READ | PROT_WRITE);
    return 1;
}
static void b_filter(const char *p) {
    crash_install(p, NULL, NULL, 4);
    crash_set_fault_filter(unprotect);
    g_page = mmap(NULL, 65536, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_page[8] = 42;                                      /* faults, the filter makes the page writable, the store runs again */
    if (g_page[8] != 42) _exit(1);
    _exit(78);
}
static char g_maps_path[700];
static void b_maps_in(const char *p) { crash_install(p, NULL, NULL, 4); crash_set_maps_file(g_maps_path); *(volatile int *)0x7f00002000ULL = 1; }
static void b_maps_gap(const char *p) { crash_install(p, NULL, NULL, 4); crash_set_maps_file(g_maps_path); *(volatile int *)0x7f00006000ULL = 1; }
static void b_closed_console(const char *p) { crash_install(p, NULL, NULL, 4); *(volatile int *)0 = 1; }

/* ---------------------------------------------------------------------------------------------- pure parts */
static void test_pure(void) {
    printf("  - pure parts\n");
    uint8_t res[64];
    memset(res, 0, sizeof res);
    /* a reserved area: an fpsimd record (0x46508001, 0x210 bytes would not fit: use a small 16-byte one) then the ESR record */
    uint32_t h[4] = {0x46508001u, 16, 0, 0};
    memcpy(res, h, 16);
    uint32_t e[2] = {0x45535201u, 16}; uint64_t esr = (uint64_t)0x24 << 26 | 1 << 6;        /* data abort, write */
    memcpy(res + 16, e, 8); memcpy(res + 24, &esr, 8);
    CHECK(crash_esr_access(res, sizeof res) == 'w');
    esr = (uint64_t)0x25 << 26;                          /* data abort at the same EL, read */
    memcpy(res + 24, &esr, 8);
    CHECK(crash_esr_access(res, sizeof res) == 'r');
    esr = (uint64_t)0x20 << 26 | 1 << 6;                 /* instruction abort: not a data access */
    memcpy(res + 24, &esr, 8);
    CHECK(crash_esr_access(res, sizeof res) == 0);
    CHECK(crash_esr_access(res, 20) == 0);               /* the record does not fit */
    memset(res, 0, sizeof res);
    CHECK(crash_esr_access(res, sizeof res) == 0);       /* terminated list, no ESR */
    uint32_t bad[2] = {0x46508001u, 4};                  /* size smaller than a header: stops */
    memcpy(res, bad, 8);
    CHECK(crash_esr_access(res, sizeof res) == 0);
    CHECK(crash_esr_access(res, 0) == 0);

    crash_mod tab[3] = {{0x1000, 0x2000, 0x1000, "a.so"}, {0x5000, 0x6000, 0x4000, "b.so"}, {0x7000, 0x7001, 0, "c"}};
    const crash_mod *m = NULL;
    CHECK(crash_mod_find(tab, 3, 0x1000, &m) && m == &tab[0]);
    CHECK(crash_mod_find(tab, 3, 0x1fff, &m) && m == &tab[0]);
    CHECK(!crash_mod_find(tab, 3, 0x2000, &m));
    CHECK(crash_mod_find(tab, 3, 0x5800, &m) && m == &tab[1]);
    CHECK(crash_mod_find(tab, 3, 0x7000, &m) && m == &tab[2]);
    CHECK(!crash_mod_find(tab, 3, 0, &m) && !crash_mod_find(tab, 0, 0x1000, &m));

    char name[64]; uint64_t off = 0;
    const char *l1 = "ffff8a1b0000-ffff8a1d0000 r-xp 00010000 fe:01 1234567                    /usr/lib/aarch64-linux-gnu/libfoo.so.1";
    CHECK(crash_maps_line(l1, 0xffff8a1b0100ULL, name, sizeof name, &off) && !strcmp(name, "libfoo.so.1") && off == 0x10100);
    CHECK(!crash_maps_line(l1, 0xffff8a1d0000ULL, name, sizeof name, &off));
    CHECK(!crash_maps_line(l1, 0xffff8a1aff00ULL, name, sizeof name, &off));
    const char *l2 = "aaaabbbb0000-aaaabbbc0000 rw-p 00000000 00:00 0 ";            /* anonymous */
    CHECK(!crash_maps_line(l2, 0xaaaabbbb1000ULL, name, sizeof name, &off));
    CHECK(!crash_maps_line("garbage", 0, name, sizeof name, &off));
    CHECK(!crash_maps_line("12-34", 0x20, name, sizeof name, &off));
    const char *l3 = "400000-410000 r-xp 00000000 08:01 99 /home/user/my game/ssbu";  /* a space in the name: kept whole */
    CHECK(crash_maps_line(l3, 0x400010, name, sizeof name, &off) && !strcmp(name, "ssbu"));
    CHECK(strstr(crash_signal_text(SIGSEGV, SEGV_MAPERR), "not mapped"));
    CHECK(!strcmp(crash_signal_text(SIGABRT, 0), "SIGABRT, abort()"));
    CHECK(!strcmp(crash_signal_text(99, 0), "?"));
    CHECK(crash_strip_pac(0xaa007f123456789aULL) == 0x00007f123456789aULL);
    CHECK(crash_strip_pac(0x00007f123456789aULL) == 0x00007f123456789aULL);
}

static void test_read(void) {
    printf("  - crash_read\n");
    crash_install(NULL, NULL, NULL, 4);                  /* creates the pipe; no signal is raised here */
    char buf[64];
    static const char src[32] = "0123456789abcdef0123456789abcde";
    CHECK(crash_read((uint64_t)(uintptr_t)src, buf, 32) && !memcmp(buf, src, 32));
    CHECK(crash_read((uint64_t)(uintptr_t)src, buf, 0));
    CHECK(!crash_read(0, buf, 8));
    CHECK(!crash_read(8, buf, 8));
    CHECK(!crash_read(~0ULL - 3, buf, 8));               /* wraps */
    CHECK(!crash_read((uint64_t)(uintptr_t)src, buf, 4096));    /* over the cap */
    size_t P = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *pg = mmap(NULL, 2 * P, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(pg + P, P);                             /* the second page is gone: a read across the boundary fails, short of it works */
    CHECK(crash_read((uint64_t)(uintptr_t)pg + P - 16, buf, 16));
    CHECK(!crash_read((uint64_t)(uintptr_t)pg + P - 8, buf, 16));
    CHECK(crash_read((uint64_t)(uintptr_t)src, buf, 8) && !memcmp(buf, src, 8));       /* still works after failures */
    munmap(pg, P);
}

/* ---------------------------------------------------------------------------------------------- scenarios */
static void check_report(const Run *r, const char *what) {
    CHECKF(r->has_file, "%s: report file", what);
    CHECKF(strstr(r->file, "=== end of crash report"), "%s: end marker", what);
    CHECKF(!strcmp(r->file, r->err) || strstr(r->err, r->file) != NULL, "%s: console has the file's text", what);
    CHECKF(count_str(r->file, "HOST SIGNAL") == 1, "%s: one report", what);
}
static void test_scenarios(void) {
    Run *r = calloc(1, sizeof *r);
    printf("  - null write on the main thread\n");
    run_child(r, "nullw", b_null_write, NULL, NULL, 0);
    CHECK(died_of(r, SIGSEGV));
    check_report(r, "nullw");
    CHECK(strstr(r->file, "HOST SIGNAL 11 (SIGSEGV"));
    CHECK(strstr(r->file, "address 0x0\n"));
    CHECK(strstr(r->file, "SECTION pc="));
    CHECK(strstr(r->file, "host registers:") && strstr(r->file, " x30 "));
    CHECK(strstr(r->file, "host stack from sp-0x40:"));
    CHECK(strstr(r->file, "instruction at pc: "));
    CHECK(strstr(r->file, "\n  #0 "));
    CHECK(strstr(r->file, "UTC"));
    CHECK(strstr(r->err, "CALLBACK RAN"));                // the callback runs after the report
    CHECK(strstr(r->file, g_mod));                       // module name in module+offset
    { char mk[128]; snprintf(mk, sizeof mk, "%s+0x", g_mod); CHECKF(strstr(r->file, mk), "module+offset for %s", g_mod); }
    CHECK(!strstr(r->file, "CALLBACK RAN"));
    if (!ON_MAC) CHECK(strstr(r->file, "writing address"));    // ESR record: Linux aarch64 kernels write one; macOS gives the ESR in the context
    else CHECK(strstr(r->file, "writing address") || strstr(r->file, "accessing address"));

    printf("  - read fault, call stack depth\n");
    run_child(r, "read", b_read, NULL, NULL, 0);
    CHECK(died_of(r, SIGSEGV));
    CHECK(strstr(r->file, "reading address 0x8") || strstr(r->file, "accessing address 0x8"));
    run_child(r, "recurse", b_recurse, NULL, NULL, 0);
    CHECK(died_of(r, SIGSEGV));
    CHECK(count_str(r->file, "\n  #") >= 7);              // recurse(6) .. recurse(0), run_child's caller chain

    printf("  - stack overflow on the main thread and on a thread\n");
    run_child(r, "ovf_main", b_overflow_main, NULL, NULL, 0);
    CHECK(died_of(r, SIGSEGV) || died_of(r, SIGBUS));
    check_report(r, "ovf_main");
    CHECK(strstr(r->file, "stack overflow"));
    CHECK(strstr(r->file, "SECTION pc="));
    run_child(r, "ovf_thread", b_overflow_thread, NULL, NULL, 0);
    CHECK(died_of(r, SIGSEGV) || died_of(r, SIGBUS));
    check_report(r, "ovf_thread");
    CHECK(strstr(r->file, "stack overflow"));

    printf("  - abort, trap, sent signal\n");
    run_child(r, "abort", b_abort, NULL, NULL, 0);
    CHECK(died_of(r, SIGABRT));
    check_report(r, "abort");
    CHECK(strstr(r->file, "SIGABRT, abort()"));
    CHECK(strstr(r->err, "CALLBACK RAN"));
    run_child(r, "trap", b_illegal, NULL, NULL, 0);
    CHECK(WIFSIGNALED(r->status) && (WTERMSIG(r->status) == SIGILL || WTERMSIG(r->status) == SIGTRAP));
    CHECK(WTERMSIG(r->status) != SIGTRAP ? r->has_file : 1);
    run_child(r, "sentfpe", b_fpe_sent, NULL, NULL, 0);
    CHECK(died_of(r, SIGFPE));
    check_report(r, "sentfpe");

    printf("  - a fault inside the program section, the callback\n");
    run_child(r, "sec_fault", b_section_faults, NULL, NULL, 0);
    CHECK(died_of(r, SIGSEGV));
    check_report(r, "sec_fault");
    CHECK(strstr(r->file, "before the fault"));
    CHECK(strstr(r->file, "a fault inside the program part"));
    CHECK(!strstr(r->file, "not reached"));
    run_child(r, "cb_fault", b_callback_faults, NULL, NULL, 0);
    CHECK(died_of(r, SIGSEGV));
    check_report(r, "cb_fault");
    CHECK(strstr(r->err, "a fault inside the summaries"));
    run_child(r, "cb_exit", b_callback_exits, NULL, NULL, 0);        // the callback ends the process itself (report_and_exit -> ExitProcess)
    CHECK(WIFEXITED(r->status) && WEXITSTATUS(r->status) == 33);
    check_report(r, "cb_exit");

    printf("  - timeout of a callback that hangs (SSBU_CRASH_TIMEOUT=1)\n");
    run_child(r, "cb_hang", b_callback_hangs, "SSBU_CRASH_TIMEOUT", "1", 0);
    CHECK(died_of(r, SIGSEGV));
    CHECK(r->secs > 0.9 && r->secs < 10);
    check_report(r, "cb_hang");
    CHECK(strstr(r->err, "CALLBACK HANGS") && strstr(r->err, "did not finish in time"));

    printf("  - SSBU_CRASH_EXIT=1: the exit code\n");
    run_child(r, "exit_mode", b_exit_mode, "SSBU_CRASH_EXIT", "1", 0);
    CHECK(WIFEXITED(r->status) && WEXITSTATUS(r->status) == 4);
    check_report(r, "exit_mode");

    printf("  - no crash, no file\n");
    run_child(r, "clean", b_no_crash, NULL, NULL, 0);
    CHECK(WIFEXITED(r->status) && WEXITSTATUS(r->status) == 0);
    CHECK(!r->has_file && r->err[0] == 0);

    printf("  - report file cannot be created: console only, says so\n");
    run_child(r, "baddir", b_bad_dir, NULL, NULL, 0);
    CHECK(died_of(r, SIGSEGV));
    CHECK(strstr(r->err, "=== end of crash report") && strstr(r->err, "report file could not be opened: errno 2"));

    printf("  - appended, one report per crash\n");
    run_child(r, "append", b_null_write, NULL, NULL, 0);
    run_child(r, "append", b_read, NULL, NULL, 1);
    CHECK(count_str(r->file, "HOST SIGNAL") == 2 && count_str(r->file, "=== end of crash report") == 2);

    printf("  - stdio locks held by another thread do not stop the report\n");
    run_child(r, "locked", b_stdio_locked, NULL, NULL, 0);
    CHECK(died_of(r, SIGSEGV));
    CHECK(r->has_file && strstr(r->file, "=== end of crash report") && strstr(r->file, "SECTION pc="));

    printf("  - two threads crash at once: one report\n");
    run_child(r, "two", b_two_threads, NULL, NULL, 0);
    CHECK(died_of(r, SIGSEGV));
    check_report(r, "two");

    printf("  - no descriptor left: the spare opens the file\n");
    run_child(r, "emfile", b_emfile, NULL, NULL, 0);
    CHECK(died_of(r, SIGSEGV));
    CHECK(r->has_file && strstr(r->file, "=== end of crash report"));

    printf("  - fault filter takes a write-watch fault\n");
    run_child(r, "filter", b_filter, NULL, NULL, 0);
    CHECK(WIFEXITED(r->status) && WEXITSTATUS(r->status) == 78);
    CHECK(!r->has_file);

    printf("  - mapping around the fault address and modules from a maps file\n");
    {   snprintf(g_maps_path, sizeof g_maps_path, "%s/maps.txt", g_dir);
        FILE *f = fopen(g_maps_path, "w");
        fputs("7f00000000-7f00004000 rw-p 00000000 00:00 0                          [heap]\n"
              "7f00004000-7f00005000 r-xp 00003000 fe:01 77   /home/user/lib/libx.so.2\n"
              "7f00008000-7f00009000 r--p 00000000 fe:01 78   /opt/some dir/liby.so\n", f);
        fclose(f);
        run_child(r, "maps_in", b_maps_in, NULL, NULL, 0);
        CHECK(r->has_file && strstr(r->file, "mapping holding the address: 0000007f00000000-0000007f00004000 rw-p [heap]"));
        run_child(r, "maps_gap", b_maps_gap, NULL, NULL, 0);
        CHECK(strstr(r->file, "the address is not mapped"));
        CHECK(strstr(r->file, "mapping before it: 0000007f00004000-0000007f00005000 r-xp libx.so.2"));
        CHECK(strstr(r->file, "mapping after it:  0000007f00008000-0000007f00009000 r--p liby.so"));
        crash_set_maps_file(g_maps_path);
        char name[64]; uint64_t off = 0;
        CHECK(crash_module_of(0x7f00004010ULL, name, sizeof name, &off) && !strcmp(name, "libx.so.2") && off == 0x3010);
        CHECK(!crash_module_of(0x7f00001000ULL, name, sizeof name, &off));        // [heap]: not a module
        CHECK(!crash_module_of(0x7f00006000ULL, name, sizeof name, &off));
        crash_set_maps_file("/nonexistent/maps");
        CHECK(!crash_module_of(0x7f00004010ULL, name, sizeof name, &off));
        crash_set_maps_file("/proc/self/maps");
    }

    printf("  - console closed (SIGPIPE): still reported to the file, still dies of the fault\n");
    {   char path[600]; snprintf(path, sizeof path, "%s/closed_crash.txt", g_dir); unlink(path);
        int p[2]; if (pipe(p)) exit(2);
        fflush(NULL);
        pid_t pid = fork();
        if (!pid) { close(p[0]); dup2(p[1], 2); close(p[1]); b_closed_console(path); _exit(77); }
        close(p[0]); close(p[1]);
        int st; waitpid(pid, &st, 0);
        char txt[65536]; int has;
        slurp(path, txt, sizeof txt, &has);
        CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
        CHECK(has && strstr(txt, "=== end of crash report"));
    }
    free(r);
}

int main(int argc, char **argv) {
    g_prog = argv[0];
    const char *b = strrchr(argv[0], '/');
    g_mod = b ? b + 1 : argv[0];
    snprintf(g_dir, sizeof g_dir, "%s", argc > 1 ? argv[1] : "/tmp");
    char tmpl[600];
    snprintf(tmpl, sizeof tmpl, "%s/crash_test_XXXXXX", g_dir);
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
    snprintf(g_dir, sizeof g_dir, "%s", tmpl);
    printf("test_crash: scratch %s\n", g_dir);
    test_pure();
    crash_modules_refresh();
    test_scenarios();
    test_read();                                         /* last: installs in this process (the pipe) */
    printf("%-14s %s (%d checks, %d failed)\n", "crash", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

/* test_fiber.c (C3b) - plat_posix.c's fibers on aarch64 Linux: fiber_switch_a64 keeps x19-x28 / d8-d15 on both sides
 * (called raw through blr, and through SwitchToFiber), FPCR per fiber, 1e6 switches, a fiber resumed by another thread,
 * the Win32 shims, stacks below 2^47 and unmapped by DeleteFiber, the guard page and a returning entry (child
 * processes). TP_BTI (run.sh bti): PROT_BTI on the switch's page. Build / run: tests/port/threads/run.sh. */
#include "tp.h"
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>

uint64_t tp_regcheck(void (*fn)(void *, void *), void *a0, void *a1, uint64_t seed);       /* regcheck_a64.S */
void fiber_switch_a64(void *from, void *to);                                              /* plat_posix.c */
static uint64_t (*volatile regcheck_p)(void (*)(void *, void *), void *, void *, uint64_t) = tp_regcheck;
static void switch_to(void *to, void *unused) { (void)unused; SwitchToFiber(to); }

static uint64_t rd_fpcr(void) { uint64_t v; __asm__ __volatile__("mrs %0, fpcr" : "=r"(v)); return v; }
static void wr_fpcr(uint64_t v) { __asm__ __volatile__("msr fpcr, %0" :: "r"(v)); }
#define FPCR_RZ (3ull << 22)     /* RMode: toward zero */
#define FPCR_FZ (1ull << 24)
#define N_SWITCH 1000000
static int maps_lines(void) {    /* Linux: mappings of this process (0 elsewhere) */
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    int n = 0, ch;
    while ((ch = fgetc(f)) != EOF) n += ch == '\n';
    fclose(f);
    return n;
}

/* ---------------------------------------------------------------- the raw switch, entered through blr */
typedef struct { uint64_t r[13], d[8], fpcr; } Regs;       /* plat_posix.c FiberRegs */
static Regs g_rmain, g_rf;
static uint64_t g_raw_bad, g_raw_iters;
static void raw_entry(void) {                              /* "returned" into by the first switch */
    for (uint64_t i = 0;; i++) {
        g_raw_bad |= regcheck_p((void (*)(void *, void *))fiber_switch_a64, &g_rf, &g_rmain, 0x5000000000ull + i * 0x1000);
        g_raw_iters++;
    }
}
static void test_raw(void) {
    SECTION("fiber_switch_a64 through blr: x19-x28 and d8-d15 kept on both sides, 1e6 round trips");
    size_t sz = 1 << 20;
    uint8_t *stk = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(&g_rf, 0, sizeof g_rf);
    g_rf.r[11] = (uint64_t)(uintptr_t)raw_entry;
    g_rf.r[12] = ((uint64_t)(uintptr_t)(stk + sz)) & ~15ull;
    g_rf.fpcr = rd_fpcr();
    uint64_t bad = 0;
    int64_t t0 = tp_now_us();
    for (uint64_t i = 0; i < N_SWITCH; i++) bad |= regcheck_p((void (*)(void *, void *))fiber_switch_a64, &g_rmain, &g_rf, 0x7000000000ull + i * 0x2000);
    int64_t dt = tp_now_us() - t0;
    CHECKF(bad == 0, "main side mask %llx", (unsigned long long)bad);
    CHECKF(g_raw_bad == 0, "fiber side mask %llx", (unsigned long long)g_raw_bad);
    CHECK(g_raw_iters == N_SWITCH - 1);
    printf("    raw: %.1f ns per switch\n", dt * 1000.0 / (2.0 * N_SWITCH));
    munmap(stk, sz);
}

/* ---------------------------------------------------------------- SwitchToFiber: registers and FPCR per fiber */
static void *g_main, *g_f;
static uint64_t g_fbad, g_fiters;
static int g_ffpcr_bad, g_fdata_bad;
static void WINAPI pong(LPVOID arg) {
    wr_fpcr(rd_fpcr() | FPCR_RZ | FPCR_FZ);               /* this fiber rounds toward zero and flushes denormals */
    volatile double third = 1.0 / 3.0;
    for (uint64_t i = 0;; i++) {
        g_fbad |= regcheck_p(switch_to, g_main, NULL, 0x5100000000ull + i * 0x1000);
        if ((rd_fpcr() & (FPCR_RZ | FPCR_FZ)) != (FPCR_RZ | FPCR_FZ)) g_ffpcr_bad++;
        if (GetFiberData() != arg || GetCurrentFiber() != g_f) g_fdata_bad++;
        if (third != 1.0 / 3.0) g_fbad |= 1ull << 40;     /* a value held on the fiber's stack */
        g_fiters++;
    }
}
static void test_switch(void) {
    SECTION("SwitchToFiber: registers kept on both sides, FPCR per fiber, GetFiberData / GetCurrentFiber, 1e6 round trips");
    uint64_t fpcr0 = rd_fpcr();
    g_main = ConvertThreadToFiber((void *)0x1234);
    CHECK(g_main && GetCurrentFiber() == g_main && GetFiberData() == (void *)0x1234);
    CHECK(ConvertThreadToFiber(NULL) == g_main);           /* upstream: a second call gives the same fiber */
    g_f = CreateFiberEx(1 << 20, (SIZE_T)256 << 20, 0, pong, (void *)0x777);
    CHECK(g_f != NULL);
    uint64_t bad = 0;
    int fpcr_bad = 0;
    int64_t t0 = tp_now_us();
    for (uint64_t i = 0; i < N_SWITCH; i++) {
        bad |= regcheck_p(switch_to, g_f, NULL, 0x7100000000ull + i * 0x2000);
        if (rd_fpcr() != fpcr0) fpcr_bad++;
    }
    int64_t dt = tp_now_us() - t0;
    CHECKF(bad == 0, "main side mask %llx", (unsigned long long)bad);
    CHECKF(g_fbad == 0, "fiber side mask %llx", (unsigned long long)g_fbad);
    CHECKF(fpcr_bad == 0 && g_ffpcr_bad == 0, "%d %d", fpcr_bad, g_ffpcr_bad);
    CHECK(g_fdata_bad == 0 && g_fiters == N_SWITCH - 1 && GetCurrentFiber() == g_main);
    printf("    SwitchToFiber: %.1f ns per switch\n", dt * 1000.0 / (2.0 * N_SWITCH));
    DeleteFiber(g_f);
}

/* ---------------------------------------------------------------- stacks: below 2^47, unmapped by DeleteFiber */
static uintptr_t g_local;
static void WINAPI where(LPVOID back) { int v; g_local = (uintptr_t)&v; for (;;) SwitchToFiber(back); }
static void test_stacks(void) {
    SECTION("fiber stacks: below 2^47 (aarch64 Linux), 2000 created and deleted leave no mappings behind");
    void *f = CreateFiberEx(0, (SIZE_T)256 << 20, 0, where, g_main);
    SwitchToFiber(f);
    printf("    a fiber's stack: %016llx\n", (unsigned long long)g_local);
#if defined(__linux__) && defined(__aarch64__)
    CHECK(g_local < (1ull << 47) && g_local >= 0x7600000000ull);   /* low_place's range */
#endif
    DeleteFiber(f);
    int m0 = maps_lines();
    for (int i = 0; i < 2000; i++) { void *g = CreateFiberEx(0, (SIZE_T)256 << 20, 0, where, g_main); SwitchToFiber(g); DeleteFiber(g); }
    int m1 = maps_lines();
    CHECKF(m1 <= m0 + 2, "mappings %d -> %d", m0, m1);
}

/* ---------------------------------------------------------------- one fiber resumed by another thread */
static __thread void *t_home;                              /* the running thread's own fiber */
static __attribute__((noinline)) void *home(void) { __asm__ __volatile__("" ::: "memory"); return t_home; }
static DWORD g_tids[4];
static int g_resumes, g_mig_bad;
static void WINAPI migrant(LPVOID arg) {
    double acc = 1.5;                                      /* lives on the fiber stack across threads */
    for (int k = 0;; k++) {
        g_tids[g_resumes++ & 3] = GetCurrentThreadId();
        if (acc != 1.5 * (1 << k) || GetFiberData() != arg) g_mig_bad++;
        acc *= 2;
        SwitchToFiber(home());                             /* the home of the thread running us NOW */
    }
}
static void *g_migrant;
static DWORD WINAPI runner(LPVOID arg) {
    int times = (int)(intptr_t)arg;
    t_home = ConvertThreadToFiber(NULL);
    for (int i = 0; i < times; i++) SwitchToFiber(g_migrant);
    ConvertFiberToThread();
    return GetCurrentThreadId();
}
static void test_migration(void) {
    SECTION("a fiber that left thread A is resumed by thread B");
    g_migrant = CreateFiberEx(0, 1 << 20, 0, migrant, (void *)0x55);
    DWORD a, b;
    HANDLE ta = CreateThread(NULL, 0, runner, (void *)(intptr_t)1, 0, NULL);
    WaitForSingleObject(ta, INFINITE); GetExitCodeThread(ta, &a); CloseHandle(ta);
    HANDLE tb = CreateThread(NULL, 0, runner, (void *)(intptr_t)2, 0, NULL);
    WaitForSingleObject(tb, INFINITE); GetExitCodeThread(tb, &b); CloseHandle(tb);
    CHECK(g_resumes == 3 && a != b);
    CHECK(g_tids[0] == a && g_tids[1] == b && g_tids[2] == b);
    CHECK(g_mig_bad == 0);
    DeleteFiber(g_migrant);
}

/* ---------------------------------------------------------------- the guard page and a returning entry, in children */
static int depth(int n) { volatile char buf[1024]; buf[0] = (char)n; return n ? depth(n - 1) + buf[0] : 0; }
static void WINAPI overflow(LPVOID arg) { (void)arg; depth(1 << 20); }
static void WINAPI returns(LPVOID arg) { (void)arg; }
static int child(void (*body)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) { int dn = open("/dev/null", O_WRONLY); dup2(dn, 2); body(); _exit(0); }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFSIGNALED(st) ? WTERMSIG(st) : -WEXITSTATUS(st);
}
static void body_overflow(void) { ConvertThreadToFiber(NULL); SwitchToFiber(CreateFiberEx(0, 64 << 10, 0, overflow, NULL)); }
static void body_return(void) { ConvertThreadToFiber(NULL); SwitchToFiber(CreateFiberEx(0, 64 << 10, 0, returns, NULL)); }
static void test_failures(void) {
    SECTION("an overflow hits the guard page (SIGSEGV; SIGBUS on macOS), a returning entry ends the process (exit code 3)");
    int s = child(body_overflow);
    CHECKF(s == SIGSEGV || s == SIGBUS, "%d", s);
    s = child(body_return);
    CHECKF(s == -3, "%d", s);
}

#ifdef TP_BTI
#ifndef PROT_BTI
#define PROT_BTI 0x10
#endif
static void bti_guard(const void *fn) { CHECK(mprotect((void *)((uintptr_t)fn & ~(uintptr_t)4095), 4096, PROT_READ | PROT_EXEC | PROT_BTI) == 0); }
#endif
int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
#ifdef TP_BTI
    SECTION("BTI: PROT_BTI on the pages of fiber_switch_a64, SwitchToFiber and the checker (indirect calls land on bti c)");
    bti_guard((const void *)fiber_switch_a64);
    bti_guard((const void *)SwitchToFiber);
    bti_guard((const void *)tp_regcheck);
#endif
    test_raw();
    test_switch();
    test_stacks();
    test_migration();
    test_failures();
    return tp_done("test_fiber");
}

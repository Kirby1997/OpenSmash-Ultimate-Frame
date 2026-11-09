/* test_fiber.c - aarch64 fiber switch: register preservation, FPCR, 1e6 switches, migration between threads,
 * guard page, Win32 shims */
#include "tp.h"
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>

uint64_t tp_switch_check(plat_fiber *from, plat_fiber *to, uint64_t seed);
/* called through a volatile pointer: an indirect branch (blr), checked by BTI on guarded pages */
static uint64_t (*volatile tp_switch_check_p)(plat_fiber *, plat_fiber *, uint64_t) = tp_switch_check;
#include <sys/mman.h>
#ifndef PROT_BTI
#define PROT_BTI 0x10
#endif

static uint64_t rd_fpcr(void) { uint64_t v; __asm__ __volatile__("mrs %0, fpcr" : "=r"(v)); return v; }
static void wr_fpcr(uint64_t v) { __asm__ __volatile__("msr fpcr, %0" :: "r"(v)); }
#define FPCR_RZ (3ull << 22)     /* RMode = round toward zero */
#define FPCR_FZ (1ull << 24)

/* ---------------------------------------------------------------- raw API: registers and FPCR, 1e6 switches */
#define N_SWITCH 1000000
static plat_fiber *g_main, *g_f;
static uint64_t g_fiber_bad, g_fiber_iters;
static int g_fiber_fpcr_bad;
#ifdef TP_BTI
#define PAGE_ALONE __attribute__((aligned(4096)))   /* guarded page starts here: no PLT/crt code on it */
#else
#define PAGE_ALONE
#endif
static PAGE_ALONE void pong(void *arg) {
    (void)arg;
    wr_fpcr(rd_fpcr() | FPCR_RZ);                         /* this fiber rounds toward zero */
    volatile double third = 1.0 / 3.0;
    for (uint64_t i = 0;; i++) {
        g_fiber_bad |= tp_switch_check(g_f, g_main, 0x5000000000ull + i * 0x1000);
        if ((rd_fpcr() & FPCR_RZ) != FPCR_RZ) g_fiber_fpcr_bad++;
        g_fiber_iters++;
        if (third != 1.0 / 3.0) g_fiber_bad |= 1ull << 40;   /* stack-held FP value survives */
    }
}
static void test_raw(void) {
    SECTION("raw switch: x19-x28, d8-d15 preserved on both sides, FPCR per fiber, 1e6 round trips");
    uint64_t fpcr0 = rd_fpcr();
    g_main = plat_fiber_convert_thread(NULL);
    g_f = plat_fiber_create(1 << 20, pong, NULL);
    CHECK(g_main && g_f);
    uint64_t bad = 0;
    int fpcr_bad = 0;
    int64_t t0 = tp_now_us();
    for (uint64_t i = 0; i < N_SWITCH; i++) {
        bad |= tp_switch_check_p(g_main, g_f, 0x7000000000ull + i * 0x2000);
        if (rd_fpcr() != fpcr0) fpcr_bad++;
    }
    int64_t dt = tp_now_us() - t0;
    CHECKF(bad == 0, "main side register mask %llx", (unsigned long long)bad);
    CHECKF(g_fiber_bad == 0, "fiber side register mask %llx", (unsigned long long)g_fiber_bad);
    CHECK(fpcr_bad == 0 && g_fiber_fpcr_bad == 0);
    CHECK(g_fiber_iters == N_SWITCH - 1);
    printf("    %d round trips in %lld us: %.1f ns per switch\n", N_SWITCH, (long long)dt, dt * 1000.0 / (2.0 * N_SWITCH));
    plat_fiber_delete(g_f);
    plat_fiber_delete(g_main);
}

/* ---------------------------------------------------------------- Win32 shims */
static void *g_thread_fiber;
static int g_count;
static PAGE_ALONE void WINAPI counter_fiber(LPVOID arg) {
    for (;;) {
        if (GetFiberData() != arg || GetCurrentFiber() == g_thread_fiber) g_count = -1000000000;
        g_count++;
        SwitchToFiber(g_thread_fiber);
    }
}
static void test_win32(void) {
    SECTION("Win32 shims: ConvertThreadToFiber, CreateFiberEx, SwitchToFiber x1e6, GetFiberData, DeleteFiber");
    CHECK(!IsThreadAFiber() && GetCurrentFiber() == NULL);
    g_thread_fiber = ConvertThreadToFiber((void *)0x1234);
    CHECK(g_thread_fiber && IsThreadAFiber() && GetCurrentFiber() == g_thread_fiber && GetFiberData() == (void *)0x1234);
    CHECK(!ConvertThreadToFiber(NULL) && GetLastError() == ERROR_ALREADY_FIBER);
    void *f = CreateFiberEx(1 << 20, (SIZE_T)256 << 20, 0, counter_fiber, (void *)0x777);
    CHECK(f != NULL);
    int64_t t0 = tp_now_us();
    for (int i = 0; i < N_SWITCH; i++) SwitchToFiber(f);
    int64_t dt = tp_now_us() - t0;
    CHECK(g_count == N_SWITCH && GetCurrentFiber() == g_thread_fiber);
    printf("    SwitchToFiber: %.1f ns per switch\n", dt * 1000.0 / (2.0 * N_SWITCH));
    DeleteFiber(f);
    for (int i = 0; i < 2000; i++) DeleteFiber(CreateFiber(0, counter_fiber, NULL));   /* stacks are unmapped */
    CHECK(ConvertFiberToThread() && !IsThreadAFiber());
}

/* ---------------------------------------------------------------- migration: one fiber resumed by another thread */
static _Thread_local void *t_home;                     /* this thread's own (converted) fiber */
PLAT_TLS_ACCESSORS(void *, t_home)
static DWORD g_tids[4];
static int g_resumes, g_mig_bad;
static PAGE_ALONE void WINAPI migrant(LPVOID arg) {
    double acc = 1.5;                                     /* lives on the fiber stack across threads */
    for (int k = 0;; k++) {
        g_tids[g_resumes++ & 3] = GetCurrentThreadId();   /* out of line: right thread every time */
        if (acc != 1.5 * (1 << k) || GetFiberData() != arg) g_mig_bad++;
        acc *= 2;
        SwitchToFiber(t_home_get());                      /* noinline accessor: home of the thread running us NOW */
    }
}
static void *g_migrant;
static DWORD WINAPI runner(LPVOID arg) {
    int times = (int)(intptr_t)arg;
    t_home_set(ConvertThreadToFiber(NULL));
    for (int i = 0; i < times; i++) SwitchToFiber(g_migrant);
    ConvertFiberToThread();
    return GetCurrentThreadId();
}
static void test_migration(void) {
    SECTION("migration: a fiber suspended on thread A resumes on thread B");
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
    /* ping-pong between two threads handing the fiber over many times */
}

/* ---------------------------------------------------------------- failure modes, in child processes */
static int depth(int n) { volatile char buf[1024]; buf[0] = (char)n; return n ? depth(n - 1) + buf[0] : 0; }
static void WINAPI overflow(LPVOID arg) { (void)arg; depth(1 << 20); }
static void WINAPI returns(LPVOID arg) { (void)arg; }
static int child_signal(void (*body)(void)) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", 1);
        dup2(devnull, 2);
        body();
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFSIGNALED(st) ? WTERMSIG(st) : -WEXITSTATUS(st);
}
static void body_overflow(void) { ConvertThreadToFiber(NULL); SwitchToFiber(CreateFiberEx(0, 64 << 10, 0, overflow, NULL)); }
static void body_return(void) { ConvertThreadToFiber(NULL); SwitchToFiber(CreateFiberEx(0, 64 << 10, 0, returns, NULL)); }
static __attribute__((unused)) void test_failures(void) {
    SECTION("guard page (SIGSEGV), entry returning (SIGABRT)");
    CHECK(child_signal(body_overflow) == SIGSEGV);
    CHECK(child_signal(body_return) == SIGABRT);
}

#ifdef TP_BTI
/* BTI build: turn on BTI for the pages holding the switch, the trampoline, the register checker and the fiber entry
 * functions (PROT_BTI works on any executable mapping, even when the program as a whole is not BTI-marked; SteamOS'
 * crt objects are not, so -z force-bti cannot be used). Any indirect branch into them without a landing pad: SIGILL. */
static void bti_guard(const void *fn) {
    uintptr_t p = (uintptr_t)fn & ~(uintptr_t)4095;
    CHECK(mprotect((void *)p, 4096, PROT_READ | PROT_EXEC | PROT_BTI) == 0);
}
#endif
int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
#ifdef TP_BTI
    SECTION("BTI: PROT_BTI on the fiber code pages (and PAC-signed C callers on both sides of each switch)");
    bti_guard((const void *)plat_fiber_switch_raw);
    bti_guard((const void *)plat_fiber_trampoline);
    bti_guard((const void *)tp_switch_check);
    bti_guard((const void *)pong);
    bti_guard((const void *)counter_fiber);
    bti_guard((const void *)migrant);
#endif
    test_raw();
    test_win32();
    test_migration();
#ifndef TP_TSAN
    test_failures();
#endif
#ifdef TP_BTI
    _exit(tp_done("test_fiber"));   /* the guarded pages may hold crtbegin's (non-BTI) destructor helper, called at exit */
#endif
    return tp_done("test_fiber");
}

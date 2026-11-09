/* test_os_threads.c - runtime/os_threads.c's nn::os threads on plat_posix.c (Linux paths), through the import handlers:
 * Create / SetName / Start / Wait (twice) / Destroy, the thread block and GetCurrentThread, the host thread's name (15
 * characters on Linux), the thread's stack below 2^47 and its alternate signal stack (crash reports of a thread that
 * overflows), the console core model (SSBU_OS_CONSOLE_CORES=1: one processor per core number), reuse of one ThreadType,
 * 8 threads starting 4 nn::os threads each, nn::os::SleepThread's accuracy and std::condition_variable's real-time deadline.
 * The nn::os fibers are test_os_fibers.c. "Guest" functions are host functions void f(Ctx *) (stubs.c: call_guest calls
 * them). Was the nn::os part of tests/port/threads/test_os_threads.c of linux-arm64, which also tested a host_thread_*
 * API that os_threads.c does not have since the Frame was rebuilt on upstream's layer (plat_posix.c CreateThread). */
#include "tp.h"
#include "runtime.h"
#include "hostrt.h"
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#ifdef __linux__
#include <sched.h>
#endif

ImportFn tst_import(const char *name);

#define OS(n) "_ZN2nn2os" n
#define I_CREATE OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi")
#define I_CREATE_CORE OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii")
#define I_START OS("11StartThreadEPNS0_10ThreadTypeE")
#define I_WAIT OS("10WaitThreadEPNS0_10ThreadTypeE")
#define I_DESTROY OS("13DestroyThreadEPNS0_10ThreadTypeE")
#define I_NAME OS("13SetThreadNameEPNS0_10ThreadTypeEPKc")
#define I_CUR OS("16GetCurrentThreadEv")
#define I_CORE OS("20GetCurrentCoreNumberEv")
#define I_SLEEP OS("11SleepThreadENS_8TimeSpanE")
#define I_STD_TIMED "_ZNSt3__118condition_variable15__do_timed_waitERNS_11unique_lockINS_5mutexEEENS_6chrono10time_pointINS5_12system_clockENS5_8durationIxNS_5ratioILl1ELl1000000000EEEEEEE"
#define I_STD_LOCK "_ZNSt3__15mutex4lockEv"
#define I_STD_UNLOCK "_ZNSt3__15mutex6unlockEv"

static uint64_t imp(const char *name, Ctx *c, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[4] = a4; c->x[5] = a5; c->x[6] = a6;
    tst_import(name)(c);
    return c->x[0];
}
#define IMP(name, c, ...) imp(name, c, __VA_ARGS__)
static uint64_t P(const void *p) { return (uint64_t)(uintptr_t)p; }
/* below 2^47: aarch64 Linux only (the guest is handed host addresses, which must stay in 47 bits) */
static int low_addr(const void *p) {
#if defined(__linux__) && defined(__aarch64__)
    return P(p) < (1ull << 47);
#else
    (void)p; return 1;
#endif
}
static int has_altstack(void) { stack_t ss; return sigaltstack(NULL, &ss) == 0 && !(ss.ss_flags & SS_DISABLE) && ss.ss_size >= 0x10000; }

typedef struct {
    uint64_t self_tt, block_tt;           /* GetCurrentThread, thread block +0x1f8 */
    char name[64];
    int core, wait_name, alt, ncpus;
    const void *stack;
    volatile int go;
} gt_rec;
static void gt_fn(Ctx *c) {
    gt_rec *r = (gt_rec *)(uintptr_t)c->x[0];
    int local;
    r->stack = &local;
    r->alt = has_altstack();                 /* crash_thread_init in the thread start (aarch64 Linux) */
    r->block_tt = rd64(c->tpidr + 0x1f8);
    r->self_tt = IMP(I_CUR, c, 0, 0, 0, 0, 0, 0, 0);
    while (r->wait_name && !LOAD(r->go)) Sleep(1);
    pthread_getname_np(pthread_self(), r->name, sizeof r->name);
#ifdef __linux__
    cpu_set_t s;
    CPU_ZERO(&s);
    sched_getaffinity(0, sizeof s, &s);
    r->ncpus = CPU_COUNT(&s);
#endif
    r->core = (int)IMP(I_CORE, c, 0, 0, 0, 0, 0, 0, 0);
}
static uint8_t g_tt[16][0x200];
static uint8_t g_gstack[0x10000];
static void run_guest_thread(int idx, gt_rec *r, int prio, int core, const char *name_before, const char *name_after) {
    Ctx c;
    memset(&c, 0, sizeof c);
    if (core < 0) IMP(I_CREATE, &c, P(g_tt[idx]), P(gt_fn), P(r), P(g_gstack), sizeof g_gstack, (uint64_t)prio, 0);
    else IMP(I_CREATE_CORE, &c, P(g_tt[idx]), P(gt_fn), P(r), P(g_gstack), sizeof g_gstack, (uint64_t)prio, (uint64_t)core);
    if (name_before) IMP(I_NAME, &c, P(g_tt[idx]), P(name_before), 0, 0, 0, 0, 0);
    r->wait_name = name_after != NULL;
    IMP(I_START, &c, P(g_tt[idx]), 0, 0, 0, 0, 0, 0);
    if (name_after) { IMP(I_NAME, &c, P(g_tt[idx]), P(name_after), 0, 0, 0, 0, 0); __atomic_store_n(&r->go, 1, __ATOMIC_RELEASE); }
    IMP(I_WAIT, &c, P(g_tt[idx]), 0, 0, 0, 0, 0, 0);
    IMP(I_WAIT, &c, P(g_tt[idx]), 0, 0, 0, 0, 0, 0);              /* WaitThread twice */
    IMP(I_DESTROY, &c, P(g_tt[idx]), 0, 0, 0, 0, 0, 0);
}
/* the host name of a thread: os_threads.c gives it on aarch64 Linux only (the kernel keeps 15 characters) */
#if defined(__linux__) && defined(__aarch64__)
#define HOST_NAMES 1
static int name_is(const char *got, const char *want) {
    size_t n = strlen(want);
    if (n > 15) n = 15;
    return strlen(got) == n && !strncmp(got, want, n);
}
#else
#define HOST_NAMES 0
static int name_is(const char *got, const char *want) { (void)got; (void)want; return 1; }
#endif

static void test_guest_threads(void) {
    SECTION("nn::os threads: Create / SetName / Start / Wait x2 / Destroy, thread block, GetCurrentThread, names, stack, alternate stack");
    gt_rec r;
    memset(&r, 0, sizeof r);
    run_guest_thread(0, &r, 16, -1, "GuestWorkerThreadLongName", NULL);
    CHECK(r.self_tt == P(g_tt[0]) && r.block_tt == P(g_tt[0]));
    CHECKF(name_is(r.name, "GuestWorkerThreadLongName"), "name '%s'", r.name);
    CHECKF(low_addr(r.stack), "host stack %p", r.stack);
#if defined(__linux__) && defined(__aarch64__)
    CHECK(r.alt);
#endif
    memset(&r, 0, sizeof r);
    run_guest_thread(1, &r, 0, -1, NULL, "RenamedAfterStart");          /* a name set after StartThread reaches the running thread */
    CHECKF(name_is(r.name, "RenamedAfterStart"), "name '%s'", r.name);
    memset(&r, 0, sizeof r);
    run_guest_thread(2, &r, 31, -1, NULL, NULL);                        /* the lowest priority: Windows has fewer levels; nothing may fail */
    CHECK(r.self_tt == P(g_tt[2]));
    /* reuse of one ThreadType, 64 times: a stack is not kept alive, every run is a fresh thread with its own block */
    for (int i = 0; i < 64; i++) {
        memset(&r, 0, sizeof r);
        run_guest_thread(6, &r, 16, -1, "reuse", NULL);
        CHECK(r.self_tt == P(g_tt[6]) && r.block_tt == P(g_tt[6]));
    }
}

#ifdef __linux__
/* console core model (SSBU_OS_CONSOLE_CORES=1, set in main): a thread created for core n is pinned to one processor, and
 * GetCurrentCoreNumber answers n. Only when the process may use at least three processors (else the model is off). */
static void test_console_cores(void) {
    SECTION("console core model: a thread created on core 0, 1, 2 runs on one processor of its own and reports that core");
    cpu_set_t all;
    CPU_ZERO(&all);
    sched_getaffinity(0, sizeof all, &all);
    if (CPU_COUNT(&all) < 3) { printf("    fewer than 3 usable processors: the model is off, skipped\n"); return; }
    for (int core = 0; core < 3; core++) {
        gt_rec r;
        memset(&r, 0, sizeof r);
        run_guest_thread(3 + core, &r, 16, core, "core", NULL);
        CHECKF(r.ncpus == 1, "core %d: %d processors allowed", core, r.ncpus);
        CHECKF(r.core == core, "core %d: GetCurrentCoreNumber %d", core, r.core);
    }
}
#endif

/* ---------------------------------------------------------------- concurrent starts */
static DWORD WINAPI starter_fn(LPVOID p) {
    int base = (int)(intptr_t)p;
    for (int i = 0; i < 4; i++) {
        gt_rec r;
        memset(&r, 0, sizeof r);
        Ctx c;
        memset(&c, 0, sizeof c);
        static uint8_t tts[8][4][0x200];
        uint8_t *tt = tts[base][i];
        IMP(I_CREATE, &c, P(tt), P(gt_fn), P(&r), P(g_gstack), sizeof g_gstack, 16, 0);
        IMP(I_START, &c, P(tt), 0, 0, 0, 0, 0, 0);
        IMP(I_WAIT, &c, P(tt), 0, 0, 0, 0, 0, 0);
        IMP(I_DESTROY, &c, P(tt), 0, 0, 0, 0, 0, 0);
        if (r.self_tt != P(tt)) return 1;
    }
    return 0;
}
static void test_concurrent_starts(void) {
    SECTION("8 host threads each starting 4 nn::os threads at once");
    HANDLE h[8];
    for (int i = 0; i < 8; i++) h[i] = CreateThread(NULL, 0, starter_fn, (LPVOID)(intptr_t)i, 0, NULL);
    for (int i = 0; i < 8; i++) { DWORD code = 9; CHECK(WaitForSingleObject(h[i], 20000) == WAIT_OBJECT_0); GetExitCodeThread(h[i], &code); CHECK(code == 0); CloseHandle(h[i]); }
}

/* ---------------------------------------------------------------- sleeps and the real-time deadline */
static int cmp64(const void *a, const void *b) { int64_t x = *(const int64_t *)a, y = *(const int64_t *)b; return x < y ? -1 : x > y; }
static DWORD WINAPI sleep_fn(LPVOID p) {
    (void)p;
    Ctx c;
    memset(&c, 0, sizeof c);
    static const int64_t asks[] = {100000, 500000, 1000000, 2000000, 5000000};
    int bad = 0;
    for (int k = 0; k < 5; k++) {
        int64_t over[40];
        for (int i = 0; i < 40; i++) {
            LARGE_INTEGER a, b, f;
            QueryPerformanceCounter(&a);
            IMP(I_SLEEP, &c, (uint64_t)asks[k], 0, 0, 0, 0, 0, 0);
            QueryPerformanceCounter(&b);
            QueryPerformanceFrequency(&f);
            over[i] = (b.QuadPart - a.QuadPart) * 1000000000LL / f.QuadPart - asks[k];
            if (over[i] < -50000) bad++;                   /* woke more than 50 us early */
        }
        qsort(over, 40, sizeof over[0], cmp64);
        printf("    sleep %5lld us: late by p50 %4lld us, p90 %4lld us, max %5lld us\n", (long long)asks[k] / 1000, (long long)over[20] / 1000,
               (long long)over[36] / 1000, (long long)over[39] / 1000);
        if (over[20] > 4000000) bad += 1000;               /* the median more than 4 ms late */
    }
    return (DWORD)bad;
}
static void test_sleep(void) {
    SECTION("nn::os::SleepThread accuracy + std::condition_variable real-time deadline");
    HANDLE h = CreateThread(NULL, 0, sleep_fn, NULL, 0, NULL);
    CHECK(WaitForSingleObject(h, 60000) == WAIT_OBJECT_0);
    DWORD code = 99;
    GetExitCodeThread(h, &code);
    CHECKF(code == 0, "early wake-ups or a late median: %u", (unsigned)code);
    CloseHandle(h);
    /* real-time deadline: system_clock ns since 1970 */
    static uint8_t cv[64], mtx[64];
    uint64_t lock = P(mtx);                                  /* unique_lock<mutex>: the first word is the mutex pointer */
    Ctx c;
    memset(&c, 0, sizeof c);
    for (int k = 0; k < 2; k++) {
        int64_t after = k ? -5000000 : 30000000;             /* 30 ms ahead; 5 ms in the past */
        IMP(I_STD_LOCK, &c, P(mtx), 0, 0, 0, 0, 0, 0);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        int64_t deadline = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec + after;
        int64_t t0 = tp_now_us();
        IMP(I_STD_TIMED, &c, P(cv), P(&lock), (uint64_t)deadline, 0, 0, 0, 0);
        int64_t dt = tp_now_us() - t0;
        IMP(I_STD_UNLOCK, &c, P(mtx), 0, 0, 0, 0, 0, 0);
        clock_gettime(CLOCK_REALTIME, &ts);
        int64_t now = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
        printf("    std::condition_variable deadline %+lld ms: returned after %.2f ms, %s the deadline\n", (long long)after / 1000000, dt / 1000.0, now >= deadline ? "at/after" : "BEFORE");
        CHECK(now >= deadline);
        CHECK(dt < (k ? 5000 : 60000));
    }
}

int main(void) {
    setenv("SSBU_OS_CONSOLE_CORES", "1", 1);
    test_guest_threads();
#ifdef __linux__
    test_console_cores();
#endif
    test_concurrent_starts();
    test_sleep();
    return tp_done("test_os_threads");
}

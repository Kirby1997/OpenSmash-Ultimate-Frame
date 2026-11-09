/* test_os_fibers.c (C3b) - runtime/os_threads.c's nn::os fibers on plat_posix.c (Linux paths):
 *   - one guest fiber resumed alternately by two nn::os threads: each time it runs on the resuming host thread, sees
 *     that thread's thread block and nn::os GetCurrentThread, its own GetCurrentFiber, keeps its FLS value (gaps.c G3),
 *     and locks a guest mutex on one thread that it unlocks on the other (G1b); its host stack is below 2^47 (aarch64
 *     Linux); a thread's own context always comes back on its own thread;
 *   - two guest fibers on one host thread keep their own FLS values and the thread its own (G2); FinalizeFiber runs
 *     the FLS callback.
 * "Guest" functions are host functions void f(Ctx *) (stubs.c: call_guest calls them). Build / run: run.sh. */
#include "tp.h"
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "hostrt.h"

ImportFn tst_import(const char *name);
#define OS(n) "_ZN2nn2os" n
#define I_CREATE OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi")
#define I_START OS("11StartThreadEPNS0_10ThreadTypeE")
#define I_WAIT OS("10WaitThreadEPNS0_10ThreadTypeE")
#define I_DESTROY OS("13DestroyThreadEPNS0_10ThreadTypeE")
#define I_CUR OS("16GetCurrentThreadEv")
#define I_MTX_LOCK OS("9LockMutexEPNS0_9MutexTypeE")
#define I_MTX_UNLOCK OS("11UnlockMutexEPNS0_9MutexTypeE")
#define I_MTX_TRY OS("12TryLockMutexEPNS0_9MutexTypeE")
#define I_FIB_INIT OS("15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi")
#define I_FIB_FINI OS("13FinalizeFiberEPNS0_9FiberTypeE")
#define I_FIB_SWITCH OS("13SwitchToFiberEPNS0_9FiberTypeE")
#define I_FIB_CUR OS("15GetCurrentFiberEv")
static uint64_t imp(const char *name, Ctx *c, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[4] = a4; c->x[5] = a5;
    tst_import(name)(c);
    return c->x[0];
}
static uint64_t P(const void *p) { return (uint64_t)(uintptr_t)p; }
static uint64_t thread_block_owner(Ctx *c) { return *(uint64_t *)(uintptr_t)(c->tpidr + 0x1f8); }
static uint8_t g_gstack[0x10000];
typedef struct { int calls; } Rec;
static void WINAPI cb_rec(PVOID v) { __atomic_add_fetch(&((Rec *)v)->calls, 1, __ATOMIC_RELAXED); }
static int calls(Rec *r) { return __atomic_load_n(&r->calls, __ATOMIC_ACQUIRE); }
static DWORD g_ix;

/* ---------------------------------------------------------------- one fiber, two threads */
#define PP_ROUNDS 2000
static uint8_t g_fib[0x100], g_ptt[2][0x400], g_fmtx[0x40];
static volatile int g_turn;
static volatile DWORD g_resumer_tid;
static volatile uint64_t g_resumer_tt;
static int g_fbad, g_runs, g_returns, g_low_bad;
static Rec g_frec;
static void fiber_fn(Ctx *c) {
    if (FlsGetValue(g_ix) == NULL) FlsSetValue(g_ix, &g_frec);           /* the first start; a restart is the same host fiber */
    for (int j = 0;; j++) {
        int local;
#if defined(__linux__) && defined(__aarch64__)
        if ((uint64_t)(uintptr_t)&local >= (1ull << 47)) g_low_bad++;
#endif
        (void)local;
        if (GetCurrentThreadId() != g_resumer_tid) g_fbad |= 1;
        if (thread_block_owner(c) != g_resumer_tt) g_fbad |= 2;          /* the thread block of the resuming thread */
        if (imp(I_CUR, c, 0, 0, 0, 0, 0, 0) != g_resumer_tt) g_fbad |= 4; /* nn::os GetCurrentThread: the host thread running us */
        if (imp(I_FIB_CUR, c, 0, 0, 0, 0, 0, 0) != P(g_fib)) g_fbad |= 8;
        if (FlsGetValue(g_ix) != &g_frec) g_fbad |= 0x100;               /* FLS travels with the fiber */
        if (g_runs++ % 2 == 0) imp(I_MTX_LOCK, c, P(g_fmtx), 0, 0, 0, 0, 0);   /* locked here, unlocked by the next run, on the other thread */
        else imp(I_MTX_UNLOCK, c, P(g_fmtx), 0, 0, 0, 0, 0);
        if (j % 7 == 6) { g_returns++; c->x[0] = 0; return; }             /* returns: fiber_main switches to the current thread */
        imp(I_FIB_SWITCH, c, 0, 0, 0, 0, 0, 0);                           /* back to the thread that resumed us */
    }
}
static void pp_thread(Ctx *c) {
    int me = (int)c->x[0];
    for (int k = 0; k < PP_ROUNDS; k++) {
        for (int spin = 0; __atomic_load_n(&g_turn, __ATOMIC_ACQUIRE) != me; spin++) if (spin > 1000) Sleep(0);
        DWORD tid = GetCurrentThreadId();
        g_resumer_tid = tid;
        g_resumer_tt = P(g_ptt[me]);
        imp(I_FIB_SWITCH, c, P(g_fib), 0, 0, 0, 0, 0);
        if (GetCurrentThreadId() != tid) g_fbad |= 16;                    /* a thread's own context comes back on that thread */
        if (imp(I_FIB_CUR, c, 0, 0, 0, 0, 0, 0) != 0) g_fbad |= 32;
        if (thread_block_owner(c) != P(g_ptt[me])) g_fbad |= 64;
        __atomic_store_n(&g_turn, 1 - me, __ATOMIC_RELEASE);
    }
}
static void test_two_threads(void) {
    SECTION("one nn::os fiber resumed alternately by two nn::os threads: thread, thread block, FLS, a guest mutex across threads");
    Ctx c;
    memset(&c, 0, sizeof c);
    imp(I_FIB_INIT, &c, P(g_fib), P(fiber_fn), 0, P(g_gstack), sizeof g_gstack, 0);
    for (int i = 0; i < 2; i++) imp(I_CREATE, &c, P(g_ptt[i]), P(pp_thread), (uint64_t)i, P(g_gstack), sizeof g_gstack, 16);
    for (int i = 0; i < 2; i++) imp(I_START, &c, P(g_ptt[i]), 0, 0, 0, 0, 0);
    int64_t t0 = tp_now_us();
    for (int i = 0; i < 2; i++) imp(I_WAIT, &c, P(g_ptt[i]), 0, 0, 0, 0, 0);
    int64_t dt = tp_now_us() - t0;
    for (int i = 0; i < 2; i++) imp(I_DESTROY, &c, P(g_ptt[i]), 0, 0, 0, 0, 0);
    printf("    %d resumes (%d by the function returning) in %.1f ms\n", g_runs, g_returns, dt / 1000.0);
    CHECKF(g_fbad == 0, "mask %x", g_fbad);
    CHECK(g_low_bad == 0);
    CHECK(g_runs == 2 * PP_ROUNDS && g_returns > 0);
    CHECK(imp(I_MTX_TRY, &c, P(g_fmtx), 0, 0, 0, 0, 0) == 1);           /* an even number of runs: the mutex is free */
    imp(I_MTX_UNLOCK, &c, P(g_fmtx), 0, 0, 0, 0, 0);
    imp(I_FIB_FINI, &c, P(g_fib), 0, 0, 0, 0, 0);
    CHECKF(calls(&g_frec) == 1, "%d", calls(&g_frec));                   /* FinalizeFiber deletes the host fiber: its value's callback */
}

/* ---------------------------------------------------------------- two fibers, one thread */
static uint8_t g_gf[2][0x100], g_ott[0x400];
static Rec g_grec[2], g_gthread;
static uint64_t g_gnext[2];
static int g_gbad, g_tbad;
static void gfib_fn(Ctx *c) {
    int i = (int)c->x[0];
    if (FlsGetValue(g_ix) != NULL) g_gbad |= 1;                           /* a new guest fiber starts empty */
    FlsSetValue(g_ix, &g_grec[i]);
    for (;;) {
        if (FlsGetValue(g_ix) != &g_grec[i]) g_gbad |= 2;
        if (imp(I_FIB_CUR, c, 0, 0, 0, 0, 0, 0) != P(g_gf[i])) g_gbad |= 4;
        imp(I_FIB_SWITCH, c, g_gnext[i], 0, 0, 0, 0, 0);
    }
}
static void one_thread_fn(Ctx *c) {
    FlsSetValue(g_ix, &g_gthread);                                         /* before the first switch: the thread's own */
    for (int i = 0; i < 2; i++) imp(I_FIB_INIT, c, P(g_gf[i]), P(gfib_fn), (uint64_t)i, P(g_gstack), sizeof g_gstack, 0);
    for (int k = 0; k < 50; k++) {
        g_gnext[0] = k & 1 ? P(g_gf[1]) : 0;                                /* fiber 0 -> fiber 1 directly every other round */
        g_gnext[1] = 0;
        imp(I_FIB_SWITCH, c, P(g_gf[0]), 0, 0, 0, 0, 0);
        if (!(k & 1)) imp(I_FIB_SWITCH, c, P(g_gf[1]), 0, 0, 0, 0, 0);
        if (FlsGetValue(g_ix) != &g_gthread) g_tbad |= 1;                  /* the thread's value, carried by its own fiber */
        if (imp(I_FIB_CUR, c, 0, 0, 0, 0, 0, 0) != 0) g_tbad |= 2;
    }
    for (int i = 0; i < 2; i++) imp(I_FIB_FINI, c, P(g_gf[i]), 0, 0, 0, 0, 0);
    if (calls(&g_grec[0]) != 1 || calls(&g_grec[1]) != 1 || calls(&g_gthread) != 0) g_tbad |= 4;
}
static void test_one_thread(void) {
    SECTION("two nn::os fibers on one nn::os thread: own FLS values, the thread's kept, FinalizeFiber callbacks, the thread's at its end");
    Ctx c;
    memset(&c, 0, sizeof c);
    imp(I_CREATE, &c, P(g_ott), P(one_thread_fn), 0, P(g_gstack), sizeof g_gstack, 16);
    imp(I_START, &c, P(g_ott), 0, 0, 0, 0, 0);
    imp(I_WAIT, &c, P(g_ott), 0, 0, 0, 0, 0);
    imp(I_DESTROY, &c, P(g_ott), 0, 0, 0, 0, 0);
    for (int i = 0; i < 2000 && calls(&g_gthread) < 1; i++) Sleep(1);     /* the thread's end runs after WaitThread returns */
    CHECKF(g_gbad == 0 && g_tbad == 0, "%x %x", g_gbad, g_tbad);
    CHECK(calls(&g_gthread) == 1);
}
int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_ix = FlsAlloc(cb_rec);
    test_two_threads();
    test_one_thread();
    return tp_done("test_os_fibers");
}

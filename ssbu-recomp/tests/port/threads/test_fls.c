/* test_fls.c (C3b) - plat_posix.c's fiber-local storage on Linux (FlsAlloc / FlsFree / FlsGetValue / FlsSetValue):
 *   - the API: indexes, invalid ones, running out, FlsFree;
 *   - fibers on one thread: a new fiber starts empty, each keeps its own value (tests/cspike/gaps.c G2),
 *     ConvertThreadToFiber / ConvertFiberToThread keep the thread's values, DeleteFiber runs the callback (non-NULL only);
 *   - a fiber resumed by another thread brings its values (G3);
 *   - the end of a thread: CreateThread (before the handle is signalled), a pthread, a converted thread, a callback
 *     that sets a value again;
 *   - FlsFree: the callback once for each fiber's and thread's value, the index empty when handed out again;
 *   - soak: 10,000 fibers and 3,000 threads each with a value: every callback runs, no value set is left.
 * Build / run: tests/port/threads/run.sh. */
#include "tp.h"
#include <stdint.h>
#include <pthread.h>

size_t plat_fls_sets(void);                            /* plat_posix.c (Linux): live value sets */
#define ERROR_NO_MORE_ITEMS 259
typedef struct { int calls; } Rec;
static void WINAPI cb_rec(PVOID v) { __atomic_add_fetch(&((Rec *)v)->calls, 1, __ATOMIC_RELAXED); }
static int calls(Rec *r) { return __atomic_load_n(&r->calls, __ATOMIC_ACQUIRE); }

/* ---------------------------------------------------------------- the API */
static DWORD g_ix;
static void test_api(void) {
    SECTION("FlsAlloc / FlsGetValue / FlsSetValue / FlsFree: indexes, invalid ones, running out");
    g_ix = FlsAlloc(cb_rec);
    CHECK(g_ix != FLS_OUT_OF_INDEXES && g_ix != 0 && g_ix < FLS_MAXIMUM_AVAILABLE);
    CHECK(FlsGetValue(g_ix) == NULL);
    SetLastError(0); CHECK(FlsGetValue(0) == NULL && GetLastError() == ERROR_INVALID_PARAMETER);
    SetLastError(0); CHECK(FlsGetValue(FLS_OUT_OF_INDEXES) == NULL && GetLastError() == ERROR_INVALID_PARAMETER);
    SetLastError(0); CHECK(!FlsSetValue(5000, (void *)1) && GetLastError() == ERROR_INVALID_PARAMETER);
    size_t sets0 = plat_fls_sets();
    CHECK(FlsSetValue(g_ix, NULL) && plat_fls_sets() == sets0);      /* NULL into nothing: no value set is made */
    DWORD ix[FLS_MAXIMUM_AVAILABLE + 2];
    int n = 0;
    while (n < FLS_MAXIMUM_AVAILABLE + 1 && (ix[n] = FlsAlloc(NULL)) != FLS_OUT_OF_INDEXES) n++;
    CHECKF(n == FLS_MAXIMUM_AVAILABLE - 2 && GetLastError() == ERROR_NO_MORE_ITEMS, "%d more indexes", n);   /* 0 is never used, g_ix is taken */
    int distinct = 1;
    for (int i = 0; i < n; i++) for (int j = 0; j < i; j++) if (ix[i] == ix[j] || ix[i] == g_ix) distinct = 0;
    CHECK(distinct);
    CHECK(FlsSetValue(ix[n - 1], (void *)0x55) && FlsGetValue(ix[n - 1]) == (void *)0x55 && plat_fls_sets() == sets0 + 1);
    int freed = 1;
    for (int i = 0; i < n; i++) freed &= FlsFree(ix[i]) != 0;
    CHECK(freed);
    SetLastError(0); CHECK(!FlsFree(ix[0]) && GetLastError() == ERROR_INVALID_PARAMETER);    /* twice */
    SetLastError(0); CHECK(FlsGetValue(ix[n - 1]) == NULL && GetLastError() == ERROR_INVALID_PARAMETER);
    DWORD again = FlsAlloc(NULL);
    CHECK(again != FLS_OUT_OF_INDEXES && FlsGetValue(again) == NULL);   /* 0x55 went with FlsFree */
    FlsFree(again);
    FlsSetValue(ix[n - 1], NULL);
}

/* ---------------------------------------------------------------- fibers on one thread */
static LPVOID g_main_fiber;
static void *g_seen_first[2];
static int g_bad_resume, g_clear1;
static Rec g_wr[2];
static void WINAPI w32_fiber(LPVOID arg) {
    int i = (int)(uintptr_t)arg;
    g_seen_first[i] = FlsGetValue(g_ix);                              /* a new fiber: nothing yet */
    FlsSetValue(g_ix, &g_wr[i]);
    for (;;) {
        SwitchToFiber(g_main_fiber);
        if (FlsGetValue(g_ix) != &g_wr[i] || GetCurrentFiber() == g_main_fiber) g_bad_resume++;
        if (i == 1 && g_clear1) FlsSetValue(g_ix, NULL);               /* fiber 1 ends with no value: no callback */
    }
}
static void test_fibers(void) {
    SECTION("fibers on one thread: own values, ConvertThreadToFiber / ConvertFiberToThread keep the thread's, DeleteFiber callback");
    static Rec t0;
    CHECK(FlsSetValue(g_ix, &t0) && FlsGetValue(g_ix) == &t0);
    size_t sets0 = plat_fls_sets();
    g_main_fiber = ConvertThreadToFiber(NULL);
    CHECK(g_main_fiber && GetCurrentFiber() == g_main_fiber && FlsGetValue(g_ix) == &t0);     /* handed over */
    LPVOID f[2];
    for (int i = 0; i < 2; i++) f[i] = CreateFiber(1 << 16, w32_fiber, (LPVOID)(uintptr_t)i);
    for (int i = 0; i < 2; i++) SwitchToFiber(f[i]);
    CHECK(g_seen_first[0] == NULL && g_seen_first[1] == NULL);
    CHECKF(FlsGetValue(g_ix) == &t0 && plat_fls_sets() == sets0 + 2, "%zu -> %zu", sets0, plat_fls_sets());
    for (int k = 0; k < 3; k++) for (int i = 0; i < 2; i++) SwitchToFiber(f[i]);
    g_clear1 = 1;
    SwitchToFiber(f[1]);
    CHECK(g_bad_resume == 0 && FlsGetValue(g_ix) == &t0);
    CHECK(calls(&g_wr[0]) == 0 && calls(&g_wr[1]) == 0);
    DeleteFiber(f[0]);
    DeleteFiber(f[1]);
    CHECKF(calls(&g_wr[0]) == 1 && calls(&g_wr[1]) == 0, "%d %d", calls(&g_wr[0]), calls(&g_wr[1]));
    CHECK(ConvertFiberToThread() && GetCurrentFiber() == NULL && FlsGetValue(g_ix) == &t0 && calls(&t0) == 0);
    CHECK(plat_fls_sets() == sets0);
    FlsSetValue(g_ix, NULL);
}

/* ---------------------------------------------------------------- a fiber resumed by another thread */
static __thread void *t_home;
static __attribute__((noinline)) void *home(void) { __asm__ __volatile__("" ::: "memory"); return t_home; }
static Rec g_mrec, g_thr_rec[2];
static int g_mbad, g_mruns;
static void WINAPI mig_fiber(LPVOID p) {
    (void)p;
    FlsSetValue(g_ix, &g_mrec);
    for (;;) {
        g_mruns++;
        if (FlsGetValue(g_ix) != &g_mrec) g_mbad++;
        SwitchToFiber(home());
    }
}
static LPVOID g_mf;
static HANDLE g_turn[2];
static DWORD WINAPI mig_thread(LPVOID p) {
    int me = (int)(intptr_t)p;
    FlsSetValue(g_ix, &g_thr_rec[me]);
    t_home = ConvertThreadToFiber(NULL);
    for (int k = 0; k < 100; k++) {
        WaitForSingleObject(g_turn[me], INFINITE);
        SwitchToFiber(g_mf);
        if (FlsGetValue(g_ix) != &g_thr_rec[me]) g_mbad += 1000;      /* the thread's own value, back with its own fiber */
        SetEvent(g_turn[1 - me]);
    }
    return 0;
}
static void test_migration(void) {
    SECTION("a fiber resumed alternately by two threads keeps its value; each thread keeps its own");
    g_mf = CreateFiber(1 << 16, mig_fiber, NULL);
    for (int i = 0; i < 2; i++) g_turn[i] = CreateEventA(NULL, FALSE, i == 0, NULL);
    HANDLE t[2];
    for (int i = 0; i < 2; i++) t[i] = CreateThread(NULL, 0, mig_thread, (LPVOID)(intptr_t)i, 0, NULL);
    CHECK(WaitForMultipleObjects(2, t, TRUE, 20000) == WAIT_OBJECT_0);
    for (int i = 0; i < 2; i++) { CloseHandle(t[i]); CloseHandle(g_turn[i]); }
    CHECKF(g_mbad == 0 && g_mruns == 200, "bad %d runs %d", g_mbad, g_mruns);
    CHECK(calls(&g_thr_rec[0]) == 1 && calls(&g_thr_rec[1]) == 1 && calls(&g_mrec) == 0);   /* the threads' at their end */
    DeleteFiber(g_mf);
    CHECK(calls(&g_mrec) == 1);
}

/* ---------------------------------------------------------------- the end of a thread */
static Rec g_rt, g_rp, g_rconv_thread, g_rconv_fiber, g_rx, g_ry;
static DWORD g_ix2;
static volatile int g_rt_at_signal = -1;
static DWORD WINAPI plat_thread_fn(LPVOID p) { (void)p; FlsSetValue(g_ix, &g_rt); return 0; }
static void *pthread_fn(void *p) { (void)p; FlsSetValue(g_ix, &g_rp); return NULL; }
static LPVOID g_conv_main, g_conv_other;
static void WINAPI conv_fiber(LPVOID p) { (void)p; FlsSetValue(g_ix, &g_rconv_fiber); for (;;) SwitchToFiber(g_conv_main); }
static void *conv_thread(void *p) {                    /* converts, values in its own fiber and in another, ends converted */
    (void)p;
    FlsSetValue(g_ix, &g_rconv_thread);
    g_conv_main = ConvertThreadToFiber(NULL);
    g_conv_other = CreateFiber(1 << 16, conv_fiber, NULL);
    SwitchToFiber(g_conv_other);
    return NULL;
}
static void WINAPI cb_again(PVOID v) {                 /* the first call sets a value again: the next round frees it */
    cb_rec(v);
    if (v == &g_rx) FlsSetValue(g_ix2, &g_ry);
}
static void *again_thread(void *p) { (void)p; FlsSetValue(g_ix2, &g_rx); return NULL; }
static void test_thread_end(void) {
    SECTION("the end of a thread: CreateThread (before its handle is signalled), pthread, converted thread, a callback that sets again");
    size_t sets0 = plat_fls_sets();
    HANDLE h = CreateThread(NULL, 0, plat_thread_fn, NULL, 0, NULL);
    CHECK(WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0 && calls(&g_rt) == 1);
    CloseHandle(h);
#ifdef __linux__
    /* threads plat did not make: the pthread key's destructor. (Not on macOS: its __thread variables are gone by then.) */
    pthread_t t;
    pthread_create(&t, NULL, pthread_fn, NULL); pthread_join(t, NULL);
    CHECK(calls(&g_rp) == 1);
    pthread_create(&t, NULL, conv_thread, NULL); pthread_join(t, NULL);
    CHECK(calls(&g_rconv_thread) == 1 && calls(&g_rconv_fiber) == 0);   /* the other fiber's value stays with it */
    DeleteFiber(g_conv_other);
    CHECK(calls(&g_rconv_fiber) == 1);
    g_ix2 = FlsAlloc(cb_again);
    pthread_create(&t, NULL, again_thread, NULL); pthread_join(t, NULL);
    CHECKF(calls(&g_rx) == 1 && calls(&g_ry) == 1, "%d %d", calls(&g_rx), calls(&g_ry));
    FlsFree(g_ix2);
#endif
    CHECKF(plat_fls_sets() == sets0, "%zu -> %zu", sets0, plat_fls_sets());
}

/* ---------------------------------------------------------------- FlsFree */
static Rec g_fm, g_ff[3], g_flive;
static DWORD g_ixf;
static LPVOID g_ffib[3], g_ffmain;
static void *g_after_free[3];
static void WINAPI free_fiber(LPVOID p) {
    int i = (int)(uintptr_t)p;
    if (i < 2) FlsSetValue(g_ixf, &g_ff[i]);           /* fiber 2 never sets one */
    SwitchToFiber(g_ffmain);
    for (;;) { g_after_free[i] = FlsGetValue(g_ixf); SwitchToFiber(g_ffmain); }
}
static HANDLE g_live_go, g_live_set;
static DWORD WINAPI live_thread(LPVOID p) { (void)p; FlsSetValue(g_ixf, &g_flive); SetEvent(g_live_set); WaitForSingleObject(g_live_go, INFINITE); return 0; }
static void test_fls_free(void) {
    SECTION("FlsFree: the callback once for each fiber's and thread's non-NULL value, the index empty when handed out again");
    size_t sets0 = plat_fls_sets();
    g_ixf = FlsAlloc(cb_rec);
    FlsSetValue(g_ixf, &g_fm);
    g_ffmain = ConvertThreadToFiber(NULL);
    for (int i = 0; i < 3; i++) { g_ffib[i] = CreateFiber(1 << 16, free_fiber, (LPVOID)(uintptr_t)i); SwitchToFiber(g_ffib[i]); }
    g_live_go = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_live_set = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE th = CreateThread(NULL, 0, live_thread, NULL, 0, NULL);
    WaitForSingleObject(g_live_set, INFINITE);
    CHECK(FlsFree(g_ixf));
    CHECKF(calls(&g_fm) == 1 && calls(&g_ff[0]) == 1 && calls(&g_ff[1]) == 1 && calls(&g_flive) == 1, "%d %d %d %d",
           calls(&g_fm), calls(&g_ff[0]), calls(&g_ff[1]), calls(&g_flive));
    SetLastError(0); CHECK(FlsGetValue(g_ixf) == NULL && GetLastError() == ERROR_INVALID_PARAMETER);
    DWORD re = FlsAlloc(cb_rec);                       /* very likely the same index */
    for (int i = 0; i < 3; i++) { g_after_free[i] = (void *)1; SwitchToFiber(g_ffib[i]); }
    CHECK(g_after_free[0] == NULL && g_after_free[1] == NULL && g_after_free[2] == NULL && FlsGetValue(re) == NULL);
    SetEvent(g_live_go);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    for (int i = 0; i < 3; i++) DeleteFiber(g_ffib[i]);
    ConvertFiberToThread();
    CHECK(calls(&g_fm) == 1 && calls(&g_ff[0]) == 1 && calls(&g_ff[1]) == 1 && calls(&g_flive) == 1);   /* nothing twice */
    FlsFree(re);
    CloseHandle(g_live_go); CloseHandle(g_live_set);
    CHECKF(plat_fls_sets() == sets0, "%zu -> %zu", sets0, plat_fls_sets());
}

/* ---------------------------------------------------------------- soak */
static DWORD g_ixs;
static volatile int g_soak_calls;
static LPVOID g_smain;
static void WINAPI cb_count(PVOID v) { (void)v; __atomic_add_fetch(&g_soak_calls, 1, __ATOMIC_RELAXED); }
static void WINAPI soak_fiber(LPVOID p) { FlsSetValue(g_ixs, p); for (;;) SwitchToFiber(g_smain); }
static DWORD WINAPI soak_thread(LPVOID p) { FlsSetValue(g_ixs, p); return 0; }
static void test_soak(void) {
    SECTION("soak: 10,000 fibers and 3,000 threads, each with a value");
    g_ixs = FlsAlloc(cb_count);
    size_t sets0 = plat_fls_sets();
    g_smain = ConvertThreadToFiber(NULL);
    for (int i = 0; i < 10000; i++) { LPVOID f = CreateFiber(1 << 16, soak_fiber, (LPVOID)(uintptr_t)(i + 1)); SwitchToFiber(f); DeleteFiber(f); }
    ConvertFiberToThread();
    CHECKF(g_soak_calls == 10000, "%d callbacks", g_soak_calls);
    for (int i = 0; i < 3000; i++) {
        HANDLE h = CreateThread(NULL, 1 << 16, soak_thread, (LPVOID)(uintptr_t)(i + 1), 0, NULL);
        if (!h) { CHECKF(0, "CreateThread %d", i); break; }
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    CHECKF(g_soak_calls == 13000, "%d callbacks", g_soak_calls);
    CHECKF(plat_fls_sets() == sets0, "%zu -> %zu", sets0, plat_fls_sets());
    FlsFree(g_ixs);
}
int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_api();
    test_fibers();
    test_migration();
    test_thread_end();
    test_fls_free();
    test_soak();
    return tp_done("test_fls");
}

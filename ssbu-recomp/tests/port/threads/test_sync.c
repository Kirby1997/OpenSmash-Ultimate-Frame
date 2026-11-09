/* test_sync.c (C3b) - plat_posix.c's CRITICAL_SECTION on Linux, with Windows' semantics: recursion, owner thread id,
 * TryEnter, contention (spin, then futex), a Leave on another thread than the Enter, a fiber that takes the lock on one
 * thread and leaves it on another (tests/cspike/gaps.c G1a / G1b), a zeroed section; SRW locks and condition variables
 * with either lock (timeouts, wake one / all, producer / consumer). Build / run: tests/port/threads/run.sh. */
#include "tp.h"
#include <stdint.h>

#define NT 8
static CRITICAL_SECTION g_cs;
static volatile LONG g_flag;
static int64_t g_count;
static DWORD owner_of(CRITICAL_SECTION *cs) { return (DWORD)(uintptr_t)cs->OwningThread; }
static DWORD WINAPI try_cs(LPVOID arg) { (void)arg; return TryEnterCriticalSection(&g_cs) ? (LeaveCriticalSection(&g_cs), 1u) : 0u; }
static DWORD WINAPI leave_cs(LPVOID arg) { (void)arg; LeaveCriticalSection(&g_cs); return 0; }
static DWORD WINAPI hold_cs(LPVOID arg) {
    (void)arg;
    EnterCriticalSection(&g_cs);
    InterlockedExchange(&g_flag, 1);
    while (InterlockedCompareExchange(&g_flag, 0, 0) != 2) Sleep(1);
    DWORD ok = owner_of(&g_cs) == GetCurrentThreadId();
    LeaveCriticalSection(&g_cs);
    return ok;
}
static DWORD WINAPI cs_hammer(LPVOID arg) {
    int slow = arg != NULL;
    for (int i = 0; i < 100000; i++) {
        EnterCriticalSection(&g_cs);
        EnterCriticalSection(&g_cs);
        int64_t v = g_count;
        if (slow && i % 1000 == 0) Sleep(1);               /* held across a sleep: the others go to the futex */
        g_count = v + 1;
        LeaveCriticalSection(&g_cs);
        LeaveCriticalSection(&g_cs);
    }
    return 0;
}
static void run_threads(int n, LPTHREAD_START_ROUTINE fn, void *arg, DWORD *codes) {
    HANDLE h[64] = {0};
    for (int i = 0; i < n; i++) h[i] = CreateThread(NULL, 0, fn, arg, 0, NULL);
    CHECK(WaitForMultipleObjects((DWORD)n, h, TRUE, 120000) == WAIT_OBJECT_0);
    for (int i = 0; i < n; i++) { if (codes) GetExitCodeThread(h[i], &codes[i]); CloseHandle(h[i]); }
}
static void test_cs(void) {
    SECTION("critical section: recursion, owner, TryEnter, contention, Leave on another thread, a zeroed one");
    DWORD code;
    InitializeCriticalSection(&g_cs);
    CHECK(owner_of(&g_cs) == 0 && g_cs.RecursionCount == 0);
    EnterCriticalSection(&g_cs);
    EnterCriticalSection(&g_cs);
    CHECK(TryEnterCriticalSection(&g_cs));
    CHECK(g_cs.RecursionCount == 3 && owner_of(&g_cs) == GetCurrentThreadId());
    run_threads(1, try_cs, NULL, &code);
    CHECK(code == 0);                                      /* another thread cannot get it */
    LeaveCriticalSection(&g_cs);
    LeaveCriticalSection(&g_cs);
    CHECK(g_cs.RecursionCount == 1 && owner_of(&g_cs) == GetCurrentThreadId());
    LeaveCriticalSection(&g_cs);
    CHECK(owner_of(&g_cs) == 0 && g_cs.RecursionCount == 0 && g_cs.Lock == 0);
    run_threads(1, try_cs, NULL, &code);
    CHECK(code == 1);
    g_flag = 0;                                            /* the owner field names the other thread while it holds it */
    HANDLE t = CreateThread(NULL, 0, hold_cs, NULL, 0, NULL);
    while (InterlockedCompareExchange(&g_flag, 0, 0) != 1) Sleep(1);
    CHECK(owner_of(&g_cs) == GetThreadId(t) && !TryEnterCriticalSection(&g_cs));
    InterlockedExchange(&g_flag, 2);
    CHECK(WaitForSingleObject(t, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(t, &code) && code == 1);
    CloseHandle(t);
    EnterCriticalSection(&g_cs);                           /* G1a: Enter here, Leave on another thread, as Windows allows */
    run_threads(1, leave_cs, NULL, NULL);
    CHECK(owner_of(&g_cs) == 0 && g_cs.RecursionCount == 0);
    run_threads(1, try_cs, NULL, &code);
    CHECK(code == 1);
    LeaveCriticalSection(&g_cs);                           /* not entered: said once on stderr, nothing changes */
    CHECK(g_cs.RecursionCount == 0 && g_cs.Lock == 0);
    g_count = 0;
    run_threads(NT, cs_hammer, NULL, NULL);
    CHECK(g_count == NT * 100000);
    g_count = 0;
    run_threads(4, cs_hammer, (void *)1, NULL);
    CHECK(g_count == 4 * 100000);
    static CRITICAL_SECTION zeroed;                        /* never initialised: free, as with SRWLOCK_INIT */
    EnterCriticalSection(&zeroed); LeaveCriticalSection(&zeroed);
    CHECK(zeroed.Lock == 0 && zeroed.OwningThread == 0);
    DeleteCriticalSection(&g_cs);
}

/* ---------------------------------------------------------------- G1b: a fiber holding the lock moves to another thread */
static void *g_back[2], *g_mfiber;
static volatile LONG g_step;
static DWORD g_tid_enter, g_tid_leave;
static void WINAPI holder(LPVOID p) {
    (void)p;
    EnterCriticalSection(&g_cs);
    EnterCriticalSection(&g_cs);
    g_tid_enter = GetCurrentThreadId();
    InterlockedExchange(&g_step, 1);
    SwitchToFiber(g_back[0]);
    g_tid_leave = GetCurrentThreadId();                    /* resumed by thread B */
    LeaveCriticalSection(&g_cs);
    LeaveCriticalSection(&g_cs);
    for (;;) SwitchToFiber(g_back[1]);
}
static DWORD WINAPI thread_a(LPVOID p) {
    (void)p;
    g_back[0] = ConvertThreadToFiber(NULL);
    SwitchToFiber(g_mfiber);
    ConvertFiberToThread();
    return 0;
}
static DWORD WINAPI thread_b(LPVOID p) {
    (void)p;
    g_back[1] = ConvertThreadToFiber(NULL);
    while (InterlockedCompareExchange(&g_step, 0, 0) < 1) Sleep(1);
    SwitchToFiber(g_mfiber);
    ConvertFiberToThread();
    return 0;
}
static void test_cs_migration(void) {
    SECTION("a fiber that entered a critical section (twice) on thread A leaves it on thread B");
    InitializeCriticalSection(&g_cs);
    g_mfiber = CreateFiberEx(0, 1 << 20, 0, holder, NULL);
    HANDLE a = CreateThread(NULL, 0, thread_a, NULL, 0, NULL);
    CHECK(WaitForSingleObject(a, 5000) == WAIT_OBJECT_0);  /* A has ended; the fiber still holds the lock */
    CHECK(owner_of(&g_cs) == g_tid_enter && g_cs.RecursionCount == 2);
    HANDLE b = CreateThread(NULL, 0, thread_b, NULL, 0, NULL);
    CHECK(WaitForSingleObject(b, 5000) == WAIT_OBJECT_0);
    CloseHandle(a); CloseHandle(b);
    DWORD code;
    run_threads(1, try_cs, NULL, &code);
    CHECK(g_tid_enter != g_tid_leave && code == 1 && owner_of(&g_cs) == 0);
    DeleteFiber(g_mfiber);
}

/* ---------------------------------------------------------------- SRW */
static SRWLOCK g_srw = SRWLOCK_INIT;
static volatile LONG g_readers;
static DWORD WINAPI srw_hammer(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 100000; i++) { AcquireSRWLockExclusive(&g_srw); g_count++; ReleaseSRWLockExclusive(&g_srw); }
    return 0;
}
static DWORD WINAPI srw_reader(LPVOID arg) {
    (void)arg;
    AcquireSRWLockShared(&g_srw);
    InterlockedIncrement(&g_readers);
    int64_t t0 = tp_now_us();
    while (InterlockedCompareExchange(&g_readers, 0, 0) < 2 && tp_now_us() - t0 < 2000000) Sleep(1);   /* both inside at once */
    DWORD both = InterlockedCompareExchange(&g_readers, 0, 0) >= 2;
    ReleaseSRWLockShared(&g_srw);
    return both;
}
static void test_srw(void) {
    SECTION("SRW lock: exclusive, shared, try");
    DWORD codes[2];
    g_count = 0;
    run_threads(NT, srw_hammer, NULL, NULL);
    CHECK(g_count == NT * 100000);
    g_readers = 0;
    run_threads(2, srw_reader, NULL, codes);
    CHECK(codes[0] == 1 && codes[1] == 1);
    AcquireSRWLockShared(&g_srw);
    CHECK(TryAcquireSRWLockShared(&g_srw));
    ReleaseSRWLockShared(&g_srw);
    CHECK(!TryAcquireSRWLockExclusive(&g_srw));
    ReleaseSRWLockShared(&g_srw);
    CHECK(TryAcquireSRWLockExclusive(&g_srw));
    CHECK(!TryAcquireSRWLockShared(&g_srw));
    ReleaseSRWLockExclusive(&g_srw);
}

/* ---------------------------------------------------------------- condition variables */
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static int g_queue[1024], g_qn;
static int64_t g_sum;
static volatile LONG g_done, g_woken;
static DWORD WINAPI consumer(LPVOID arg) {
    (void)arg;
    EnterCriticalSection(&g_cs);
    for (;;) {
        while (!g_qn && !g_done) SleepConditionVariableCS(&g_cv, &g_cs, INFINITE);
        if (!g_qn && g_done) break;
        g_sum += g_queue[--g_qn];
        WakeAllConditionVariable(&g_cv);                   /* the producer waits for room */
    }
    LeaveCriticalSection(&g_cs);
    return 0;
}
static DWORD WINAPI waiter_cs(LPVOID arg) {
    (void)arg;
    EnterCriticalSection(&g_cs);
    BOOL r = SleepConditionVariableCS(&g_cv, &g_cs, 5000);
    InterlockedIncrement(&g_woken);
    LeaveCriticalSection(&g_cs);
    return (DWORD)r;
}
static void test_cv(void) {
    SECTION("condition variables: timeouts (CS, SRW exclusive / shared), wake one / all, producer / consumer");
    InitializeCriticalSection(&g_cs);
    EnterCriticalSection(&g_cs);
    int64_t t0 = tp_now_us();
    SetLastError(0);
    BOOL r = SleepConditionVariableCS(&g_cv, &g_cs, 50);
    int64_t dt = tp_now_us() - t0;
    CHECK(!r && GetLastError() == 1460);                    /* ERROR_TIMEOUT */
    CHECKF(dt >= 50000 && dt < 500000, "%lld us", (long long)dt);
    CHECK(g_cs.RecursionCount == 1 && owner_of(&g_cs) == GetCurrentThreadId());   /* entered again after the wait */
#ifdef __linux__                                           /* (macOS: os_sync_wait_on_address takes no zero timeout) */
    CHECK(!SleepConditionVariableCS(&g_cv, &g_cs, 0) && GetLastError() == 1460);
#endif
    LeaveCriticalSection(&g_cs);
    CHECK(owner_of(&g_cs) == 0);
    AcquireSRWLockExclusive(&g_srw);
    r = SleepConditionVariableSRW(&g_cv, &g_srw, 30, 0);
    CHECK(!r && GetLastError() == 1460);
    CHECK(!TryAcquireSRWLockShared(&g_srw));               /* exclusive again */
    ReleaseSRWLockExclusive(&g_srw);
    AcquireSRWLockShared(&g_srw);
    r = SleepConditionVariableSRW(&g_cv, &g_srw, 20, CONDITION_VARIABLE_LOCKMODE_SHARED);
    CHECK(!r && GetLastError() == 1460);
    CHECK(TryAcquireSRWLockShared(&g_srw));                /* shared again */
    ReleaseSRWLockShared(&g_srw);
    ReleaseSRWLockShared(&g_srw);
    HANDLE w[3];                                           /* wake one wakes one of three waiters; wake all the rest */
    g_woken = 0;
    for (int i = 0; i < 3; i++) w[i] = CreateThread(NULL, 0, waiter_cs, NULL, 0, NULL);
    Sleep(100);
    WakeConditionVariable(&g_cv);
    Sleep(100);
    CHECKF(LOAD(g_woken) == 1, "woken %d", (int)LOAD(g_woken));
    WakeAllConditionVariable(&g_cv);
    CHECK(WaitForMultipleObjects(3, w, TRUE, 5000) == WAIT_OBJECT_0);
    CHECK(g_woken == 3);
    for (int i = 0; i < 3; i++) { DWORD c; GetExitCodeThread(w[i], &c); CHECK(c == 1); CloseHandle(w[i]); }
    HANDLE c[4];                                           /* producer / consumer, a Wake (one) per item */
    g_qn = 0; g_sum = 0; g_done = 0;
    for (int i = 0; i < 4; i++) c[i] = CreateThread(NULL, 0, consumer, NULL, 0, NULL);
    int64_t want = 0;
    for (int i = 1; i <= 200000; i++) {
        EnterCriticalSection(&g_cs);
        while (g_qn == 1024) SleepConditionVariableCS(&g_cv, &g_cs, INFINITE);
        g_queue[g_qn++] = i;
        want += i;
        WakeConditionVariable(&g_cv);
        LeaveCriticalSection(&g_cs);
    }
    EnterCriticalSection(&g_cs);
    g_done = 1;
    WakeAllConditionVariable(&g_cv);
    LeaveCriticalSection(&g_cs);
    CHECK(WaitForMultipleObjects(4, c, TRUE, 60000) == WAIT_OBJECT_0);
    for (int i = 0; i < 4; i++) CloseHandle(c[i]);
    CHECK(g_sum == want);
}
int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_cs();
    test_cs_migration();
    test_srw();
    test_cv();
    return tp_done("test_sync");
}

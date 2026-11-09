/* C-spike: test_sync.c (linux-arm64 tests/plat) retargeted to upstream's layer: CS fields read from glibc's mutex */
/* test_sync.c - CRITICAL_SECTION, SRWLOCK, CONDITION_VARIABLE, Interlocked, INIT_ONCE, time, CRT shims */
#include "tp.h"
#include <stdint.h>

#define NT 8

/* ---------------------------------------------------------------- critical section */
static CRITICAL_SECTION g_cs;
static volatile LONG g_flag;
static DWORD WINAPI try_cs(LPVOID arg) { return TryEnterCriticalSection(&g_cs) ? (LeaveCriticalSection(&g_cs), 1u) : 0u; (void)arg; }
static DWORD WINAPI hold_cs(LPVOID arg) {
    (void)arg;
    EnterCriticalSection(&g_cs);
    InterlockedExchange(&g_flag, 1);
    while (InterlockedCompareExchange(&g_flag, 0, 0) != 2) Sleep(1);
    DWORD owner = (DWORD)(uintptr_t)CS_OWNER(g_cs);
    LeaveCriticalSection(&g_cs);
    return owner == GetCurrentThreadId();
}
static __attribute__((unused)) DWORD WINAPI leave_other(LPVOID arg) { (void)arg; LeaveCriticalSection(&g_cs); return 0; }
static int64_t g_count;
static DWORD WINAPI cs_hammer(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 100000; i++) {
        EnterCriticalSection(&g_cs);
        EnterCriticalSection(&g_cs);
        g_count++;
        LeaveCriticalSection(&g_cs);
        LeaveCriticalSection(&g_cs);
    }
    return 0;
}
static void run_threads(int n, LPTHREAD_START_ROUTINE fn, void *arg, DWORD *codes) {
    HANDLE h[64] = {0};
    for (int i = 0; i < n; i++) h[i] = CreateThread(NULL, 0, fn, arg, 0, NULL);
    CHECK(WaitForMultipleObjects((DWORD)n, h, TRUE, 60000) == WAIT_OBJECT_0);
    for (int i = 0; i < n; i++) { if (codes) GetExitCodeThread(h[i], &codes[i]); CloseHandle(h[i]); }
}
static void test_cs(void) {
    SECTION("critical section: recursion, owner, trylock, contention");
    DWORD code;
    InitializeCriticalSection(&g_cs);
    CHECK(CS_OWNER(g_cs) == 0 && 1);
    EnterCriticalSection(&g_cs);
    EnterCriticalSection(&g_cs);
    CHECK(TryEnterCriticalSection(&g_cs));
    CHECK(CS_RECURSION(g_cs) == 3);
    CHECK((DWORD)(uintptr_t)CS_OWNER(g_cs) == GetCurrentThreadId());
    run_threads(1, try_cs, NULL, &code);
    CHECK(code == 0);                                     /* another thread cannot get it */
    LeaveCriticalSection(&g_cs);
    LeaveCriticalSection(&g_cs);
    CHECK(CS_RECURSION(g_cs) == 1 && (DWORD)(uintptr_t)CS_OWNER(g_cs) == GetCurrentThreadId());
    LeaveCriticalSection(&g_cs);
    CHECK(CS_OWNER(g_cs) == 0 && CS_RECURSION(g_cs) == 0 && 1);
    run_threads(1, try_cs, NULL, &code);
    CHECK(code == 1);
    /* the owner field names the other thread while it holds the lock */
    g_flag = 0;
    HANDLE t = CreateThread(NULL, 0, hold_cs, NULL, 0, NULL);
    while (InterlockedCompareExchange(&g_flag, 0, 0) != 1) Sleep(1);
    CHECK((DWORD)(uintptr_t)CS_OWNER(g_cs) == GetThreadId(t));
    CHECK(!TryEnterCriticalSection(&g_cs));
    InterlockedExchange(&g_flag, 2);
    CHECK(WaitForSingleObject(t, 5000) == WAIT_OBJECT_0);
    GetExitCodeThread(t, &code);
    CHECK(code == 1);
    CloseHandle(t);
#ifndef TP_TSAN
    /* Enter on one thread, Leave on another (a fiber holding a guest mutex migrated): works as on Windows */
    EnterCriticalSection(&g_cs);
    run_threads(1, leave_other, NULL, NULL);
    CHECK(CS_OWNER(g_cs) == 0);
    run_threads(1, try_cs, NULL, &code);
    CHECK(code == 1);
#endif
    g_count = 0;
    run_threads(NT, cs_hammer, NULL, NULL);
    CHECK(g_count == NT * 100000);
    DeleteCriticalSection(&g_cs);
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
    SRWLOCK l;
    InitializeSRWLock(&l);
    AcquireSRWLockExclusive(&l);
    ReleaseSRWLockExclusive(&l);
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
        WakeAllConditionVariable(&g_cv);                  /* producer waits for room */
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
    SECTION("condition variables: timeouts (CS, SRW excl/shared), wake one/all, producer/consumer");
    InitializeCriticalSection(&g_cs);
    EnterCriticalSection(&g_cs);
    EnterCriticalSection(&g_cs);                          /* entered twice: the recursion survives the wait */
    int64_t t0 = tp_now_us();
    SetLastError(0);
    BOOL r = SleepConditionVariableCS(&g_cv, &g_cs, 50);
    int64_t dt = tp_now_us() - t0;
    CHECK(!r && GetLastError() == ERROR_TIMEOUT);
    CHECKF(dt >= 50000 && dt < 500000, "%lld us", (long long)dt);
    CHECK(CS_RECURSION(g_cs) == 2 && (DWORD)(uintptr_t)CS_OWNER(g_cs) == GetCurrentThreadId());
    CHECK(!SleepConditionVariableCS(&g_cv, &g_cs, 0) && GetLastError() == ERROR_TIMEOUT);
    LeaveCriticalSection(&g_cs);
    LeaveCriticalSection(&g_cs);

    AcquireSRWLockExclusive(&g_srw);
    t0 = tp_now_us();
    r = SleepConditionVariableSRW(&g_cv, &g_srw, 30, 0);
    dt = tp_now_us() - t0;
    CHECK(!r && GetLastError() == ERROR_TIMEOUT);
    CHECKF(dt >= 30000 && dt < 500000, "%lld us", (long long)dt);
    CHECK(!TryAcquireSRWLockShared(&g_srw));              /* re-acquired exclusively */
    ReleaseSRWLockExclusive(&g_srw);
    AcquireSRWLockShared(&g_srw);
    r = SleepConditionVariableSRW(&g_cv, &g_srw, 20, CONDITION_VARIABLE_LOCKMODE_SHARED);
    CHECK(!r && GetLastError() == ERROR_TIMEOUT);
    CHECK(TryAcquireSRWLockShared(&g_srw));               /* re-acquired shared */
    ReleaseSRWLockShared(&g_srw);
    ReleaseSRWLockShared(&g_srw);

    /* wake one wakes exactly one of three waiters; wake all the rest */
    HANDLE w[3];
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

    /* producer / consumer with Wake (one) per item */
    HANDLE c[4];
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
    CHECK(WaitForMultipleObjects(4, c, TRUE, 30000) == WAIT_OBJECT_0);
    for (int i = 0; i < 4; i++) CloseHandle(c[i]);
    CHECK(g_sum == want);
    CONDITION_VARIABLE cv2;
    InitializeConditionVariable(&cv2);
    WakeConditionVariable(&cv2);
    WakeAllConditionVariable(&cv2);
}

/* ---------------------------------------------------------------- Interlocked */
static volatile LONG a32, d32, add32, xadd32, cas32;
static volatile LONG64 a64, add64, xadd64, cas64;
static volatile long long ll64;                           /* `long long` objects work too (not only LONG64) */
static volatile char or8;
static void *volatile g_ptr;
static volatile LONG g_spin;
static int64_t g_plain, g_plain2;
static DWORD WINAPI hammer(LPVOID arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < 200000; i++) {
        InterlockedIncrement(&a32);
        InterlockedDecrement(&d32);
        InterlockedAdd(&add32, 3);
        InterlockedExchangeAdd(&xadd32, 2);
        for (LONG v = LOAD(cas32); InterlockedCompareExchange(&cas32, v + 1, v) != v; v = LOAD(cas32)) {}
        InterlockedIncrement64(&a64);
        InterlockedAdd64(&add64, 5);
        InterlockedExchangeAdd64(&xadd64, 7);
        for (LONG64 v = LOAD(cas64); InterlockedCompareExchange64(&cas64, v + 1, v) != v; v = LOAD(cas64)) {}
        InterlockedIncrement64(&ll64);
        if ((i & 1023) == 0) {                            /* a pointer spin lock + exchange */
            while (InterlockedCompareExchangePointer(&g_ptr, (void *)&g_plain, NULL) != NULL) YieldProcessor();
            g_plain++;
            CHECK(InterlockedExchangePointer(&g_ptr, NULL) == (void *)&g_plain);
            while (InterlockedExchange(&g_spin, 1)) YieldProcessor();
            g_plain2++;
            InterlockedExchange(&g_spin, 0);
        }
    }
    InterlockedOr8(&or8, (char)(1 << id));
    return 0;
}
static void test_interlocked(void) {
    SECTION("Interlocked: 8 threads");
    LONG x = 5;
    CHECK(InterlockedIncrement(&x) == 6 && InterlockedDecrement(&x) == 5);
    CHECK(InterlockedExchange(&x, 9) == 5 && x == 9);
    CHECK(InterlockedExchangeAdd(&x, 1) == 9 && InterlockedAdd(&x, 1) == 11);
    CHECK(InterlockedCompareExchange(&x, 1, 3) == 11 && x == 11);
    CHECK(InterlockedCompareExchange(&x, 1, 11) == 11 && x == 1);
    LONG64 y = (LONG64)1 << 40;
    CHECK(InterlockedIncrement64(&y) == ((LONG64)1 << 40) + 1);
    CHECK(InterlockedCompareExchange64(&y, 2, ((LONG64)1 << 40) + 1) == ((LONG64)1 << 40) + 1 && y == 2);
    CHECK(InterlockedExchange64(&y, -1) == 2 && InterlockedDecrement64(&y) == -2);
    char c8 = 1;
    CHECK(InterlockedOr8(&c8, 6) == 1 && c8 == 7);
    void *p = NULL;
    CHECK(InterlockedCompareExchangePointer(&p, &x, NULL) == NULL && p == &x);
    CHECK(InterlockedCompareExchangePointer((void *volatile *)&p, NULL, &y) == &x && p == &x);
    for (int i = 0; i < NT; i++) {}
    HANDLE h[NT];
    for (int i = 0; i < NT; i++) h[i] = CreateThread(NULL, 0, hammer, (void *)(intptr_t)i, 0, NULL);
    CHECK(WaitForMultipleObjects(NT, h, TRUE, 120000) == WAIT_OBJECT_0);
    for (int i = 0; i < NT; i++) CloseHandle(h[i]);
    const long n = NT * 200000L;
    CHECK(a32 == n && d32 == -n && add32 == 3 * n && xadd32 == 2 * n && cas32 == n);
    CHECK(a64 == n && add64 == 5 * n && xadd64 == 7 * n && cas64 == n && ll64 == n);
    CHECK((unsigned char)or8 == 0xff);
    CHECK(g_plain == NT * (200000 / 1024 + 1) && g_plain2 == g_plain);
}

/* ---------------------------------------------------------------- INIT_ONCE */
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT, g_once_fail = INIT_ONCE_STATIC_INIT;
static volatile LONG g_once_calls, g_fail_calls;
static int g_ctx_obj;
static BOOL CALLBACK once_fn(PINIT_ONCE o, PVOID param, PVOID *ctx) {
    (void)o; (void)param;
    InterlockedIncrement(&g_once_calls);
    Sleep(50);
    *ctx = &g_ctx_obj;
    return TRUE;
}
static BOOL CALLBACK fail_fn(PINIT_ONCE o, PVOID param, PVOID *ctx) {
    (void)o; (void)ctx;
    return InterlockedIncrement(&g_fail_calls) >= (LONG)(intptr_t)param;
}
static DWORD WINAPI once_thread(LPVOID arg) {
    (void)arg;
    void *ctx = NULL;
    return InitOnceExecuteOnce(&g_once, once_fn, NULL, &ctx) && ctx == &g_ctx_obj;
}
static void test_once(void) {
    SECTION("InitOnceExecuteOnce: 8 racing threads, failure then retry");
    DWORD codes[NT];
    run_threads(NT, once_thread, NULL, codes);
    for (int i = 0; i < NT; i++) CHECK(codes[i] == 1);
    CHECK(g_once_calls == 1);
    CHECK(!InitOnceExecuteOnce(&g_once_fail, fail_fn, (PVOID)2, NULL));
    CHECK(InitOnceExecuteOnce(&g_once_fail, fail_fn, (PVOID)2, NULL));
    CHECK(InitOnceExecuteOnce(&g_once_fail, fail_fn, (PVOID)2, NULL));
    CHECK(g_fail_calls == 2);
}

/* ---------------------------------------------------------------- time, errors, CRT */
static DWORD WINAPI err_thread(LPVOID arg) { (void)arg; SetLastError(1234); return GetLastError(); }
static void test_misc(void) {
    SECTION("time, last error, aligned malloc, strings, paths");
    LARGE_INTEGER f, a, b;
    CHECK(QueryPerformanceFrequency(&f) && f.QuadPart == 1000000000LL);
    QueryPerformanceCounter(&a);
    Sleep(20);
    QueryPerformanceCounter(&b);
    CHECKF(b.QuadPart - a.QuadPart >= 20000000 && b.QuadPart - a.QuadPart < 500000000, "%lld ns", (long long)(b.QuadPart - a.QuadPart));
    ULONGLONG t0 = GetTickCount64();
    Sleep(30);
    CHECK(GetTickCount64() - t0 >= 29 && (DWORD)(GetTickCount() - (DWORD)t0) >= 29);
    Sleep(0);
    CHECK(SwitchToThread());
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    CHECK(ft.dwHighDateTime > 0x01d00000u);              /* after 2014 */
    SYSTEMTIME st;
    GetSystemTime(&st);
    CHECK(st.wYear >= 2024 && st.wMonth >= 1 && st.wMonth <= 12);
    GetLocalTime(&st);
    CHECK(st.wYear >= 2024);

    SetLastError(77);
    DWORD code;
    run_threads(1, err_thread, NULL, &code);
    CHECK(code == 1234 && GetLastError() == 77);
    CHECK(GetCurrentThreadId() == (DWORD)gettid() && GetCurrentProcessId() == (DWORD)getpid());

    uint8_t *p = _aligned_malloc(100, 64);
    CHECK(p && ((uintptr_t)p & 63) == 0);
    for (int i = 0; i < 100; i++) p[i] = (uint8_t)i;
    p = _aligned_realloc(p, 100000, 64);
    CHECK(p && ((uintptr_t)p & 63) == 0 && p[99] == 99 && _aligned_msize(p, 64, 0) >= 100000);
    _aligned_free(p);
    p = _aligned_realloc(NULL, 10, 16);
    CHECK(p && ((uintptr_t)p & 15) == 0);
    p = _aligned_realloc(p, 1000, 16);
    CHECK(p && ((uintptr_t)p & 15) == 0);
    _aligned_free(p);

    CHECK(_stricmp("Hello", "hELLO") == 0 && _stricmp("a", "B") < 0 && _strnicmp("ABCx", "abcy", 3) == 0);
    char s[] = "MiXeD";
    CHECK(strcmp(_strlwr(s), "mixed") == 0);
    char *d = _strdup("x");
    CHECK(d && !strcmp(d, "x"));
    free(d);
    CHECK(_putenv("TP_ENV=1") == 0 && getenv("TP_ENV") && !strcmp(getenv("TP_ENV"), "1"));
    CHECK(_putenv("TP_ENV=") == 0 && !getenv("TP_ENV"));
    char path[MAX_PATH];
    CHECK(_fullpath(path, "/a/b/../c/./d//e", sizeof path) && !strcmp(path, "/a/c/d/e"));
    CHECK(_fullpath(path, "/..", sizeof path) && !strcmp(path, "/"));
    CHECK(_fullpath(path, "x/y", sizeof path) && path[0] == '/' && strstr(path, "/x/y"));
    CHECK(!_fullpath(path, "/abc", 3));
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    CHECK(n > 0 && n < sizeof path && strstr(path, "test_sync"));
    char tiny[4];
    CHECK(GetModuleFileNameA(NULL, tiny, 4) == 4 && GetLastError() == ERROR_INSUFFICIENT_BUFFER && tiny[3] == 0);
    n = GetTempPathA(sizeof path, path);
    CHECK(n > 0 && path[n - 1] == '/');
    HMODULE self = GetModuleHandleA(NULL);
    CHECK(self && GetProcAddress(self, "test_sync_exported") != NULL);
    CHECK(!LoadLibraryA("/nonexistent/x.so") && GetLastError() == ERROR_MOD_NOT_FOUND && 1);
    uint64_t rnd[4] = {0};
    CHECK(SystemFunction036(rnd, sizeof rnd) && (rnd[0] | rnd[1] | rnd[2] | rnd[3]));
    CHECK(__argc == 1 && __argv && strstr(__argv[0], "test_sync"));
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    CHECK(si.dwPageSize == 4096 && si.dwNumberOfProcessors >= 1 && si.dwAllocationGranularity == 65536);
}
__declspec(dllexport) int test_sync_exported(void) { return 1; }
static __declspec(thread) int t_declspec = 5;

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    CHECK(t_declspec == 5);
    test_cs();
    test_srw();
    test_cv();
    test_interlocked();
    test_once();
    test_misc();
    return tp_done("test_sync");
}

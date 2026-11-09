/* C-spike: test_threads.c (linux-arm64 tests/plat) on upstream's layer; the custom-handle section is ours only */
/* test_threads.c - threads, events, semaphores, mutexes, timers, processes, WaitForMultipleObjects */
#include "tp.h"
#include <sys/wait.h>

static volatile LONG g_ran;
static DWORD WINAPI slow42(LPVOID arg) { Sleep((DWORD)(uintptr_t)arg); return 42; }
static DWORD WINAPI waiter(LPVOID arg) { return WaitForSingleObject((HANDLE)arg, INFINITE); }
static DWORD WINAPI mark(LPVOID arg) { (void)arg; InterlockedExchange(&g_ran, 1); return GetCurrentThreadId(); }

static void test_thread_wait(void) {
    SECTION("thread handle: two waiters, timeouts, exit code");
    DWORD tid = 0, code = 0;
    HANDLE t = CreateThread(NULL, 0, slow42, (void *)(uintptr_t)200, 0, &tid);
    CHECK(t && tid && GetThreadId(t) == tid && tid != GetCurrentThreadId());
    CHECK(GetExitCodeThread(t, &code) && code == STILL_ACTIVE);
    HANDLE w1 = CreateThread(NULL, 0, waiter, t, 0, NULL), w2 = CreateThread(NULL, 0, waiter, t, 0, NULL);
    int64_t t0 = tp_now_us();
    CHECK(WaitForSingleObject(t, 20) == WAIT_TIMEOUT);
    CHECK(tp_now_us() - t0 >= 20000);
    CHECK(WaitForSingleObject(w1, 5000) == WAIT_OBJECT_0 && WaitForSingleObject(w2, 5000) == WAIT_OBJECT_0);
    CHECK(GetExitCodeThread(w1, &code) && code == WAIT_OBJECT_0);
    CHECK(GetExitCodeThread(w2, &code) && code == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(t, 0) == WAIT_OBJECT_0 && WaitForSingleObject(t, INFINITE) == WAIT_OBJECT_0);   /* stays signalled */
    CHECK(GetExitCodeThread(t, &code) && code == 42);
    CHECK(CloseHandle(t) && CloseHandle(w1) && CloseHandle(w2));
    CHECK(!CloseHandle(NULL) && GetLastError() == ERROR_INVALID_HANDLE);
    CHECK(WaitForSingleObject(NULL, 0) == WAIT_FAILED);

    SECTION("CREATE_SUSPENDED, ResumeThread, big stack, fire-and-forget");
    g_ran = 0;
    t = CreateThread(NULL, (SIZE_T)256 << 20, mark, NULL, CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
    CHECK(t && GetThreadId(t) == tid && tid);
    CHECK(SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL) && GetThreadPriority(t) == THREAD_PRIORITY_ABOVE_NORMAL);
    Sleep(50);
    CHECK(LOAD(g_ran) == 0 && WaitForSingleObject(t, 0) == WAIT_TIMEOUT);
    CHECK(SuspendThread(t) == 1);                         /* still suspended since creation: counts */
    CHECK(ResumeThread(t) == 2 && ResumeThread(t) == 1);
    CHECK(WaitForSingleObject(t, 5000) == WAIT_OBJECT_0 && g_ran == 1);
    CHECK(GetExitCodeThread(t, &code) && code == tid);
    CHECK(SuspendThread(t) == (DWORD)-1 && GetLastError() == ERROR_NOT_SUPPORTED);
    CloseHandle(t);
    for (int i = 0; i < 200; i++) CloseHandle(CreateThread(NULL, 0, slow42, (void *)(uintptr_t)(i % 3), 0, NULL));

    SECTION("current thread: pseudo handle, priority, DuplicateHandle, affinity");
    CHECK(GetThreadId(GetCurrentThread()) == GetCurrentThreadId());
    CHECK(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL) && GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_BELOW_NORMAL);
    CHECK(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL));   /* TRUE even without CAP_SYS_NICE */
    CHECK(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL) && GetThreadPriority(GetCurrentThread()) == 0);
    HANDLE dup = NULL;
    CHECK(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS));
    CHECK(dup && GetThreadId(dup) == GetCurrentThreadId() && WaitForSingleObject(dup, 0) == WAIT_TIMEOUT);
    CloseHandle(dup);
    DWORD_PTR pm = 0, sm = 0;
    CHECK(GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm) && pm && (pm & sm) == pm);
    DWORD_PTR first = pm & (~pm + 1);
    DWORD_PTR old = SetThreadAffinityMask(GetCurrentThread(), first);
    CHECK(old == pm && GetCurrentProcessorNumber() == (DWORD)__builtin_ctzll(first));
    CHECK(SetThreadAffinityMask(GetCurrentThread(), pm) == first);
}

static HANDLE g_ev;
static volatile LONG g_got;
static DWORD WINAPI ev_waiter(LPVOID arg) {
    DWORD r = WaitForSingleObject(g_ev, (DWORD)(uintptr_t)arg);
    if (r == WAIT_OBJECT_0) InterlockedIncrement(&g_got);
    return r;
}
static void test_events(void) {
    SECTION("events: auto-reset wakes one, manual wakes all");
    g_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_got = 0;
    HANDLE w[2] = {CreateThread(NULL, 0, ev_waiter, (void *)(uintptr_t)3000, 0, NULL), CreateThread(NULL, 0, ev_waiter, (void *)(uintptr_t)3000, 0, NULL)};
    Sleep(50);
    CHECK(SetEvent(g_ev));
    Sleep(100);
    CHECK(LOAD(g_got) == 1);
    CHECK(WaitForSingleObject(g_ev, 0) == WAIT_TIMEOUT);  /* consumed by the waiter */
    SetEvent(g_ev);
    CHECK(WaitForMultipleObjects(2, w, TRUE, 3000) == WAIT_OBJECT_0 && g_got == 2);
    CloseHandle(w[0]); CloseHandle(w[1]);
    SetEvent(g_ev);
    CHECK(WaitForSingleObject(g_ev, 0) == WAIT_OBJECT_0 && WaitForSingleObject(g_ev, 0) == WAIT_TIMEOUT);
    CloseHandle(g_ev);

    g_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_got = 0;
    w[0] = CreateThread(NULL, 0, ev_waiter, (void *)(uintptr_t)3000, 0, NULL);
    w[1] = CreateThread(NULL, 0, ev_waiter, (void *)(uintptr_t)3000, 0, NULL);
    Sleep(50);
    SetEvent(g_ev);
    CHECK(WaitForMultipleObjects(2, w, TRUE, 3000) == WAIT_OBJECT_0 && g_got == 2);
    CHECK(WaitForSingleObject(g_ev, 0) == WAIT_OBJECT_0 && WaitForSingleObject(g_ev, 0) == WAIT_OBJECT_0);
    ResetEvent(g_ev);
    CHECK(WaitForSingleObject(g_ev, 10) == WAIT_TIMEOUT);
    CloseHandle(w[0]); CloseHandle(w[1]); CloseHandle(g_ev);
    HANDLE on = CreateEventA(NULL, TRUE, TRUE, NULL);
    CHECK(WaitForSingleObject(on, 0) == WAIT_OBJECT_0);
    CloseHandle(on);

    SECTION("WaitForMultipleObjects: any (lowest index), all (atomic), threads + events");
    HANDLE e[3];
    for (int i = 0; i < 3; i++) e[i] = CreateEventA(NULL, i != 1, FALSE, NULL);   /* e1 auto-reset */
    CHECK(WaitForMultipleObjects(3, e, FALSE, 10) == WAIT_TIMEOUT);
    SetEvent(e[2]);
    CHECK(WaitForMultipleObjects(3, e, FALSE, 0) == WAIT_OBJECT_0 + 2);
    SetEvent(e[1]);
    CHECK(WaitForMultipleObjects(3, e, FALSE, 0) == WAIT_OBJECT_0 + 1);
    CHECK(WaitForMultipleObjects(3, e, FALSE, 0) == WAIT_OBJECT_0 + 2);  /* e1 consumed, e2 manual */
    SetEvent(e[1]);
    CHECK(WaitForMultipleObjects(3, e, TRUE, 20) == WAIT_TIMEOUT);       /* e0 not set ... */
    CHECK(WaitForSingleObject(e[1], 0) == WAIT_OBJECT_0);                /* ... and e1 was NOT consumed */
    SetEvent(e[1]);
    SetEvent(e[0]);
    CHECK(WaitForMultipleObjects(3, e, TRUE, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(e[1], 0) == WAIT_TIMEOUT);                 /* consumed by the wait-all */
    HANDLE mix[2] = {CreateThread(NULL, 0, slow42, (void *)(uintptr_t)100, 0, NULL), e[0]};
    ResetEvent(e[0]);
    CHECK(WaitForMultipleObjects(2, mix, TRUE, 10) == WAIT_TIMEOUT);
    SetEvent(e[0]);
    int64_t t0 = tp_now_us();
    CHECK(WaitForMultipleObjects(2, mix, TRUE, 5000) == WAIT_OBJECT_0);
    CHECK(tp_now_us() - t0 > 30000);
    CHECK(WaitForMultipleObjects(2, mix, FALSE, 0) == WAIT_OBJECT_0);
    CloseHandle(mix[0]);
    /* a waiter blocked in wait-any is woken by a later SetEvent of any member */
    HANDLE late = CreateThread(NULL, 0, slow42, (void *)(uintptr_t)0, CREATE_SUSPENDED, NULL);
    ResetEvent(e[0]); ResetEvent(e[2]);
    HANDLE any[3] = {e[0], e[2], late};
    ResumeThread(late);
    CHECK(WaitForMultipleObjects(3, any, FALSE, 5000) == WAIT_OBJECT_0 + 2);
    CloseHandle(late);
    for (int i = 0; i < 3; i++) CloseHandle(e[i]);
    CHECK(WaitForMultipleObjects(0, e, FALSE, 0) == WAIT_FAILED);
}

static HANDLE g_mx;
static DWORD WINAPI try_mutex(LPVOID arg) {
    DWORD r = WaitForSingleObject(g_mx, (DWORD)(uintptr_t)arg);
    BOOL rel_ok = r == WAIT_OBJECT_0 ? ReleaseMutex(g_mx) : 0;
    return r == WAIT_OBJECT_0 ? (rel_ok ? 1 : 2) : 0;
}
static DWORD WINAPI release_foreign(LPVOID arg) { (void)arg; return ReleaseMutex(g_mx) ? 1 : GetLastError(); }
/* another process (fresh exec, so nothing shared but the lock file) waits on the named mutex */
static int mutex_child(int ms) {
    pid_t pid = fork();
    if (pid == 0) {
        char arg[16];
        snprintf(arg, sizeof arg, "%d", ms);
        execl("/proc/self/exe", "test_threads", "--mutex-child", arg, (char *)NULL);
        _exit(99);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 98;
}
static void test_sem_mutex_timer(void) {
    SECTION("semaphore");
    HANDLE s = CreateSemaphoreA(NULL, 2, 3, NULL);
    CHECK(WaitForSingleObject(s, 0) == WAIT_OBJECT_0 && WaitForSingleObject(s, 0) == WAIT_OBJECT_0 && WaitForSingleObject(s, 0) == WAIT_TIMEOUT);
    LONG prev = -1;
    CHECK(ReleaseSemaphore(s, 3, &prev) && prev == 0);
    CHECK(!ReleaseSemaphore(s, 1, NULL) && GetLastError() == ERROR_TOO_MANY_POSTS);
    CloseHandle(s);
    CHECK(!CreateSemaphoreA(NULL, 2, 1, NULL));

    SECTION("mutex: recursive, owner-only release, named");
    DWORD code;
    g_mx = CreateMutexA(NULL, TRUE, NULL);
    CHECK(WaitForSingleObject(g_mx, 0) == WAIT_OBJECT_0); /* recursive */
    HANDLE t = CreateThread(NULL, 0, try_mutex, (void *)(uintptr_t)30, 0, NULL);
    WaitForSingleObject(t, INFINITE); GetExitCodeThread(t, &code); CloseHandle(t);
    CHECK(code == 0);
    t = CreateThread(NULL, 0, release_foreign, NULL, 0, NULL);
    WaitForSingleObject(t, INFINITE); GetExitCodeThread(t, &code); CloseHandle(t);
    CHECK(code == ERROR_NOT_OWNER);
    CHECK(ReleaseMutex(g_mx) && ReleaseMutex(g_mx) && !ReleaseMutex(g_mx));
    t = CreateThread(NULL, 0, try_mutex, (void *)(uintptr_t)1000, 0, NULL);
    WaitForSingleObject(t, INFINITE); GetExitCodeThread(t, &code); CloseHandle(t);
    CHECK(code == 1);
    CloseHandle(g_mx);
    HANDLE n1 = CreateMutexA(NULL, FALSE, "tp_named_mutex");
    CHECK(n1 && GetLastError() == 0);
    HANDLE n2 = CreateMutexA(NULL, FALSE, "tp_named_mutex");
    CHECK(n2 == n1 && GetLastError() == ERROR_ALREADY_EXISTS);
    CHECK(WaitForSingleObject(n1, 1000) == WAIT_OBJECT_0);
    int st = 0;
    CHECK(mutex_child(100) == 11);                       /* another process cannot take it while we hold it */
    CHECK(ReleaseMutex(n2));
    CHECK(mutex_child(1000) == 10);
    pid_t pid;
    CloseHandle(n1); CloseHandle(n2);

    SECTION("waitable timer: relative, absolute, periodic, cancel");
    HANDLE tm = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -200000;                               /* 20 ms */
    int64_t t0 = tp_now_us();
    CHECK(SetWaitableTimer(tm, &due, 0, NULL, NULL, FALSE));
    CHECK(WaitForSingleObject(tm, INFINITE) == WAIT_OBJECT_0);
    int64_t dt = tp_now_us() - t0;
    CHECKF(dt >= 20000 && dt < 60000, "%lld us", (long long)dt);
    CHECK(WaitForSingleObject(tm, 10) == WAIT_TIMEOUT);  /* synchronization timer: consumed */
    due.QuadPart = -10000;                                /* 1 ms precision check */
    t0 = tp_now_us();
    SetWaitableTimer(tm, &due, 0, NULL, NULL, FALSE);
    WaitForSingleObject(tm, INFINITE);
    dt = tp_now_us() - t0;
    CHECKF(dt >= 1000 && dt < 5000, "%lld us", (long long)dt);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    due.QuadPart = (LONGLONG)(((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) + 300000);   /* absolute: now + 30 ms */
    t0 = tp_now_us();
    SetWaitableTimer(tm, &due, 0, NULL, NULL, FALSE);
    CHECK(WaitForSingleObject(tm, 2000) == WAIT_OBJECT_0);
    dt = tp_now_us() - t0;
    CHECKF(dt >= 25000 && dt < 80000, "%lld us", (long long)dt);
    due.QuadPart = -100000;
    SetWaitableTimer(tm, &due, 10, NULL, NULL, FALSE);    /* 10 ms, then every 10 ms */
    t0 = tp_now_us();
    for (int i = 0; i < 5; i++) CHECK(WaitForSingleObject(tm, 1000) == WAIT_OBJECT_0);
    dt = tp_now_us() - t0;
    CHECKF(dt >= 45000 && dt < 120000, "%lld us", (long long)dt);
    /* C-spike: upstream has no CancelWaitableTimer / CreateWaitableTimerW (the runtime uses neither) */
    CloseHandle(tm);
#if 0
    tm = CreateWaitableTimerW(NULL, TRUE, NULL);
    due.QuadPart = -1;
    SetWaitableTimer(tm, &due, 0, NULL, NULL, FALSE);
    CHECK(WaitForSingleObject(tm, 100) == WAIT_OBJECT_0 && WaitForSingleObject(tm, 0) == WAIT_OBJECT_0);   /* manual: stays */
    CloseHandle(tm);
#endif

    SECTION("OpenProcess: alive / gone");
    HANDLE me = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
    CHECK(me && WaitForSingleObject(me, 0) == WAIT_TIMEOUT);
    CloseHandle(me);
    pid = fork();
    if (pid == 0) { usleep(100000); _exit(0); }
    HANDLE ch = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    CHECK(ch && WaitForSingleObject(ch, 0) == WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(ch, 3000) == WAIT_OBJECT_0);   /* exited (zombie until reaped) */
    waitpid(pid, &st, 0);
    CloseHandle(ch);
    CHECK(!OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid) && GetLastError() == ERROR_INVALID_PARAMETER);
}

#if 0   /* C-spike: plat_handle_* is our extension (no upstream equivalent) */
/* custom handle kind (for the file layer) */
static int g_destroyed;
static void destroy_payload(void *p) { g_destroyed += *(int *)p; }
static const plat_handle_ops k_ops = {"test", destroy_payload};
static void test_custom(void) {
    SECTION("custom handle kinds");
    static int payload = 7;
    HANDLE h = plat_handle_create(&k_ops, &payload);
    CHECK(plat_handle_payload(h, &k_ops) == &payload);
    CHECK(WaitForSingleObject(h, 0) == WAIT_FAILED);
    HANDLE d;
    CHECK(DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS));
    CloseHandle(h);
    CHECK(g_destroyed == 0);
    CloseHandle(d);
    CHECK(g_destroyed == 7);
}
#endif

/* many threads waiting/signalling at once */
static HANDLE g_gate;
static volatile LONG g_left;
static HANDLE g_all_done;
static DWORD WINAPI gate_worker(LPVOID arg) {
    (void)arg;
    if (WaitForSingleObject(g_gate, 10000) != WAIT_OBJECT_0) return 1;
    if (InterlockedDecrement(&g_left) == 0) SetEvent(g_all_done);
    return 0;
}
static void test_stress(void) {
    SECTION("stress: 64 threads on a manual gate (shader-worker pattern)");
    for (int round = 0; round < 20; round++) {
        HANDLE th[64];
        g_gate = CreateEventA(NULL, TRUE, FALSE, NULL);
        g_all_done = CreateEventA(NULL, TRUE, FALSE, NULL);
        g_left = 64;
        for (int i = 0; i < 64; i++) th[i] = CreateThread(NULL, 0, gate_worker, NULL, 0, NULL);
        SetEvent(g_gate);
        CHECK(WaitForSingleObject(g_all_done, 10000) == WAIT_OBJECT_0);
        CHECK(WaitForMultipleObjects(64, th, TRUE, 10000) == WAIT_OBJECT_0);
        for (int i = 0; i < 64; i++) { DWORD c; GetExitCodeThread(th[i], &c); CHECK(c == 0); CloseHandle(th[i]); }
        CloseHandle(g_gate); CloseHandle(g_all_done);
    }
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "--mutex-child")) {
        HANDLE m = CreateMutexA(NULL, FALSE, "tp_named_mutex");
        DWORD r = WaitForSingleObject(m, (DWORD)atoi(argv[2]));
        return r == WAIT_OBJECT_0 ? 10 : r == WAIT_TIMEOUT ? 11 : 12;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_thread_wait();
    test_events();
    test_sem_mutex_timer();
    test_stress();
    return tp_done("test_threads");
}

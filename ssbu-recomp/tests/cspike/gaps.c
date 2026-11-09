/* C-spike: measure, on the Frame, the aarch64 / Win32-semantics gaps UPSTREAM_LINUX_COMPARE.md predicted for upstream's
 * Linux layer (runtime/plat/windows.h + plat_posix.c). Each probe prints what Windows would give and what this layer gives;
 * "GAP" lines are the differences. Nothing is fixed here. No game data.
 *   G1 CRITICAL_SECTION left by another thread (direct, and by a fiber that migrated while holding it)
 *   G2 FLS per fiber on one thread;  G3 FLS of a fiber that migrated to another thread
 *   G4 __thread address cached across SwitchToFiber when the fiber resumes on another thread (clang -O1 on aarch64)
 *   G5 SwitchToFiber round-trip cost
 *   G6 where host memory handed to a guest would land (>= 2^47?)
 *   G7 VirtualQuery of host memory;  G8 CTR_EL0 (I-cache coherency: is FlushInstructionCache as a no-op safe?);  G9 page size
 * Build: upstream flags (-include plat_pre.h -I runtime/plat) + plat_posix.c. Run with a ulimit -u cap and a timeout. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <SDL3/SDL.h>

static int gaps;
static void verdict(const char *id, int ok, const char *what) { printf("%s %-3s %s\n", ok ? "same" : "GAP ", id, what); if (!ok) gaps++; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* ---------------------------------------------------------------- G1 */
static CRITICAL_SECTION g_cs;
static DWORD WINAPI enter_cs(LPVOID a) { (void)a; EnterCriticalSection(&g_cs); return 0; }        /* returns holding it */
static DWORD WINAPI leave_cs(LPVOID a) { (void)a; LeaveCriticalSection(&g_cs); return 0; }
static DWORD WINAPI try_cs(LPVOID a) { (void)a; BOOL ok = TryEnterCriticalSection(&g_cs); if (ok) LeaveCriticalSection(&g_cs); return ok; }
static DWORD run(LPTHREAD_START_ROUTINE fn, void *arg) {
    HANDLE t = CreateThread(NULL, 0, fn, arg, 0, NULL); DWORD code = 99;
    if (WaitForSingleObject(t, 5000) != WAIT_OBJECT_0) { printf("     (thread did not finish in 5 s)\n"); return 98; }
    GetExitCodeThread(t, &code); CloseHandle(t); return code;
}

/* fibers that migrate: a "host" thread owns a fiber, runs it, and a later thread resumes it */
static void *g_mig_fiber;
static volatile int g_mig_step;
static DWORD g_fls;
static __thread int t_tls = 0;
static int *volatile g_tls_seen[4];
static volatile DWORD g_tid_seen[2];
static void *g_back[2];                      /* the thread fiber to return to, set by whoever switches in */

static __attribute__((noinline)) int *tls_addr(void) { return &t_tls; }
static void WINAPI mig_fiber(LPVOID p) {
    (void)p;
    /* first run, on thread A */
    EnterCriticalSection(&g_cs);
    FlsSetValue(g_fls, (void *)0xB);
    int *before = &t_tls;                                 /* the address as this function computes it (may be CSE'd) */
    t_tls = 1;
    g_tls_seen[0] = before; g_tid_seen[0] = GetCurrentThreadId();
    g_mig_step = 1;
    SwitchToFiber(g_back[0]);
    /* resumed, on thread B */
    g_tls_seen[1] = &t_tls;                               /* same function after the opaque call: re-read or cached? */
    g_tls_seen[2] = tls_addr();                           /* through a call: always this thread's */
    g_tid_seen[1] = GetCurrentThreadId();
    t_tls = 2;                                            /* written through whichever address the compiler kept */
    g_mig_step = 2;
    g_tls_seen[3] = (int *)FlsGetValue(g_fls);            /* Windows: 0xB (this fiber's); a per-thread key: B's */
    LeaveCriticalSection(&g_cs);                          /* Windows: releases it (owner = this fiber's thread now) */
    SwitchToFiber(g_back[1]);
    for (;;) SwitchToFiber(g_back[1]);
}
static int *volatile g_tls_of[2];
static volatile int g_tls_val_after[2];
static DWORD WINAPI thread_a(LPVOID p) {
    (void)p;
    g_back[0] = ConvertThreadToFiber(NULL);
    g_tls_of[0] = tls_addr();
    g_mig_fiber = CreateFiber(256 << 10, mig_fiber, NULL);
    SwitchToFiber(g_mig_fiber);                            /* returns after step 1 */
    while (g_mig_step < 2) Sleep(1);                       /* keep thread A alive while B runs the fiber */
    g_tls_val_after[0] = *g_tls_of[0];
    ConvertFiberToThread();
    return 0;
}
static DWORD WINAPI thread_b(LPVOID p) {
    (void)p;
    g_back[1] = ConvertThreadToFiber(NULL);
    g_tls_of[1] = tls_addr();
    FlsSetValue(g_fls, (void *)0xC);
    while (g_mig_step < 1) Sleep(1);
    SwitchToFiber(g_mig_fiber);
    g_tls_val_after[1] = *g_tls_of[1];
    ConvertFiberToThread();
    return 0;
}

/* ---------------------------------------------------------------- G2 */
static void *g2_main, *g2_f;
static void WINAPI g2_fiber(LPVOID p) { (void)p; FlsSetValue(g_fls, (void *)0xF1); SwitchToFiber(g2_main); for (;;) SwitchToFiber(g2_main); }
/* ---------------------------------------------------------------- G5 */
static void *g5_main, *g5_f;
static void WINAPI g5_fiber(LPVOID p) { (void)p; for (;;) SwitchToFiber(g5_main); }
/* ---------------------------------------------------------------- G6 */
static uintptr_t g_fiber_stack_var, g_thread_stack_var;
static void WINAPI g6_fiber(LPVOID p) { int v; g_fiber_stack_var = (uintptr_t)&v; SwitchToFiber(p); }
static DWORD WINAPI g6_thread(LPVOID p) { int v; (void)p; g_thread_stack_var = (uintptr_t)&v; return 0; }

int main(void) {
    printf("page size %ld\n", sysconf(_SC_PAGESIZE));
    verdict("G9", sysconf(_SC_PAGESIZE) == 4096, "page size is the 4096 plat_posix.c hard-codes");

    /* G1a: Enter on thread X (which exits holding it), Leave on thread Y, TryEnter on thread Z */
    InitializeCriticalSection(&g_cs);
    run(enter_cs, NULL); run(leave_cs, NULL);
    DWORD got = run(try_cs, NULL);
    printf("     G1a: TryEnterCriticalSection after a Leave from another thread: %s\n", got ? "acquired" : "still held");
    verdict("G1a", got == 1, "CRITICAL_SECTION released by LeaveCriticalSection on another thread (Windows: yes)");

    /* G1b, G3, G4: one fiber, entered on thread A, resumed on thread B */
    InitializeCriticalSection(&g_cs);
    g_fls = FlsAlloc(NULL);
    HANDLE ta = CreateThread(NULL, 0, thread_a, NULL, 0, NULL), tb = CreateThread(NULL, 0, thread_b, NULL, 0, NULL);
    if (WaitForSingleObject(tb, 5000) != WAIT_OBJECT_0 || WaitForSingleObject(ta, 5000) != WAIT_OBJECT_0) printf("     migration threads hung\n");
    got = run(try_cs, NULL);
    printf("     G1b: lock taken by the fiber on thread %lu, left on thread %lu: %s\n", (unsigned long)g_tid_seen[0], (unsigned long)g_tid_seen[1], got ? "free" : "still held");
    verdict("G1b", got == 1, "a fiber that migrated while holding a CRITICAL_SECTION can leave it");
    printf("     G3: FLS read by the migrated fiber: %p (it set 0xb on thread A; thread B's own value is 0xc)\n", (void *)g_tls_seen[3]);
    verdict("G3", g_tls_seen[3] == (int *)0xB, "FLS follows the fiber to another thread");
    printf("     G4: &t_tls thread A %p, thread B %p; fiber saw %p before, %p after the move (via a call: %p); tid %lu -> %lu\n",
           (void *)g_tls_of[0], (void *)g_tls_of[1], (void *)g_tls_seen[0], (void *)g_tls_seen[1], (void *)g_tls_seen[2],
           (unsigned long)g_tid_seen[0], (unsigned long)g_tid_seen[1]);
    printf("     G4: after the fiber wrote 2: thread A's t_tls = %d, thread B's = %d\n", g_tls_val_after[0], g_tls_val_after[1]);
    verdict("G4", g_tls_seen[1] == g_tls_of[1] && g_tls_val_after[1] == 2 && g_tls_val_after[0] == 1,
            "__thread used in the same function after SwitchToFiber is the new thread's (no stale TLS base)");
    verdict("G4b", g_tid_seen[0] != g_tid_seen[1], "GetCurrentThreadId changes after the move");

    /* G2: two fibers on one thread */
    g2_main = ConvertThreadToFiber(NULL);
    FlsSetValue(g_fls, (void *)0xA0);
    g2_f = CreateFiber(64 << 10, g2_fiber, NULL);
    SwitchToFiber(g2_f);
    void *v = FlsGetValue(g_fls);
    printf("     G2: main fiber set 0xa0, the other fiber set 0xf1; main reads %p\n", v);
    verdict("G2", v == (void *)0xA0, "FLS is per fiber (two fibers on one thread keep their own value)");

    /* G5: switch cost */
    g5_main = g2_main;
    g5_f = CreateFiber(64 << 10, g5_fiber, NULL);
    for (int i = 0; i < 10000; i++) SwitchToFiber(g5_f);
    const int N = 1000000;
    double t0 = now();
    for (int i = 0; i < N; i++) SwitchToFiber(g5_f);
    double ns = (now() - t0) * 1e9 / N;
    printf("     G5: SwitchToFiber round trip %.0f ns (%.0f ns per switch); fiber_aarch64.S on the Frame: no system call\n", ns, ns / 2);

    /* G6: addresses */
    void *vf = CreateFiber(64 << 10, g6_fiber, g2_main); SwitchToFiber(vf);
    run(g6_thread, NULL);
    int local;
    struct { const char *what; uintptr_t a; } m[] = {
        {"malloc(16)", (uintptr_t)malloc(16)}, {"malloc(256 KiB)", (uintptr_t)malloc(256 << 10)}, {"malloc(64 MiB)", (uintptr_t)malloc(64 << 20)},
        {"_aligned_malloc(64,16)", (uintptr_t)_aligned_malloc(64, 16)}, {"VirtualAlloc(NULL, 1 MiB)", (uintptr_t)VirtualAlloc(NULL, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)},
        {"main thread stack", (uintptr_t)&local}, {"CreateThread stack", g_thread_stack_var}, {"CreateFiber stack", g_fiber_stack_var},
        {"__thread variable", (uintptr_t)tls_addr()}, {"exe code (main)", (uintptr_t)&main}, {"libSDL3 code", (uintptr_t)&SDL_GetVersion},
        {"libc code (malloc)", (uintptr_t)&malloc},
    };
    int high = 0;
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        int h = m[i].a >= (1ull << 47);
        high += h;
        printf("     G6: %-26s %016llx %s\n", m[i].what, (unsigned long long)m[i].a, h ? ">= 2^47" : "");
    }
    printf("     G6: %d of %zu above 2^47 (Windows x64 user space ends at 2^47: never)\n", high, sizeof m / sizeof m[0]);
    verdict("G6", high == 0, "host memory that can be handed to the guest stays below 2^47");

    /* G7 */
    MEMORY_BASIC_INFORMATION mi;
    SIZE_T q1 = VirtualQuery(&local, &mi, sizeof mi), q2 = VirtualQuery((void *)m[0].a, &mi, sizeof mi);
    printf("     G7: VirtualQuery(stack) = %zu, VirtualQuery(malloc) = %zu\n", (size_t)q1, (size_t)q2);
    verdict("G7", q1 && q2, "VirtualQuery answers for host stack / heap");

    /* G8 */
#if defined(__aarch64__)
    uint64_t ctr; __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    int dic = (int)(ctr >> 29) & 1, idc = (int)(ctr >> 28) & 1;
    printf("     G8: CTR_EL0 %016llx: IDC %d, DIC %d\n", (unsigned long long)ctr, idc, dic);
    verdict("G8", dic && idc, "I-cache coherent with data writes on this CPU (FlushInstructionCache no-op is safe here only)");
#endif
    printf("%d gap(s)\n", gaps);
    return 0;
}

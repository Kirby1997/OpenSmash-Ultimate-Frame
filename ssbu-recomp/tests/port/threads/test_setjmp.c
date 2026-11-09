/* test_setjmp.c - the setjmp/longjmp patterns of nnunwind.c (nro_catch_call: setjmp in a frame, longjmp from deeper on
 * the same stack once a handler ran) and sdklift_rt.c (sdk_jmp_record / sdk_longjmp_target: a jmp_buf kept in a table)
 * on plat_posix.c's fibers, on glibc aarch64 with and without _FORTIFY_SOURCE (longjmp -> __longjmp_chk, which refuses to
 * jump DOWN the stack; run.sh builds this file twice, the second time as test_setjmp_chk at -O2 -D_FORTIFY_SOURCE=2):
 *   - always upward on the same stack: a plain thread stack, a fiber's stack;
 *   - a fiber that did setjmp, migrated to another host thread, then longjmps there (the jmp_buf mangles sp / lr with the
 *     process-wide pointer guard, so it stays valid; what does NOT follow the fiber is a per-host-thread lookup table
 *     like sdklift_rt.c's t_jmp - shown at the end, as the reason that table is keyed the way it is);
 *   - volatile locals survive, nested records unwind in order.
 * Was tests/port/crash/test_setjmp.c of linux-arm64 (plat_fiber_* -> the Win32 fiber calls of plat_posix.c). */
#include "tp.h"
#include <setjmp.h>
#include <string.h>

typedef struct { uint64_t cfa; jmp_buf *jb; } Slot;      /* nnunwind.c CatchSlot, simplified */
static Slot g_slot[16];
static int g_nslot;
static void put(uint64_t cfa, jmp_buf *jb) { g_slot[g_nslot].cfa = cfa; g_slot[g_nslot].jb = jb; g_nslot++; }
static jmp_buf *take(uint64_t cfa) { for (int i = g_nslot - 1; i >= 0; i--) if (g_slot[i].cfa == cfa) { jmp_buf *j = g_slot[i].jb; g_slot[i] = g_slot[--g_nslot]; return j; } return NULL; }

static int g_body_ran, g_after;
__attribute__((noinline)) void unwinder(uint64_t cfa, int depth) {     /* eh_raise: deeper frames, then longjmp up */
    volatile char pad[512];
    pad[0] = (char)depth;
    if (depth > 0) { unwinder(cfa, depth - 1); __asm__ volatile("" :: "r"(pad[0])); return; }
    jmp_buf *jb = take(cfa);
    if (jb) longjmp(*jb, 1);
    printf("unwinder: no record\n");
}
__attribute__((noinline)) int catch_call(uint64_t cfa, int depth) {   /* nro_catch_call */
    jmp_buf jb;
    volatile int local = 1234;
    volatile int phase = 0;
    put(cfa, &jb);
    if (setjmp(jb) == 0) {
        phase = 1;
        local = 5678;
        g_body_ran++;
        unwinder(cfa, depth);
        take(cfa);
        return -1;                                            /* the body returned normally: not expected here */
    }
    g_after++;
    return local == 5678 && phase == 1 ? 1 : 0;
}
__attribute__((noinline)) int nested(void) {                   /* an inner catch frame, then the outer one */
    int a = catch_call(0x1000, 3);
    int b = catch_call(0x2000, 50);
    return a == 1 && b == 1;
}

/* ---- fibers */
static LPVOID g_home[2], g_fib;
static jmp_buf g_fjb;
static volatile DWORD g_tid_set, g_tid_jump;
static volatile int g_fstate, g_fresult;
static __attribute__((noinline)) void deep_jump(int d) { volatile char pad[256]; pad[0] = 1; if (d) { deep_jump(d - 1); __asm__ volatile("" :: "r"(pad[0])); return; } longjmp(g_fjb, 7); }
static void fiber_body(LPVOID arg) {
    (void)arg;
    volatile int local = 42;
    int r = setjmp(g_fjb);
    if (r == 0) {
        g_tid_set = GetCurrentThreadId();
        g_fstate = 1;
        SwitchToFiber(g_home[0]);                            /* suspend on thread A ... */
        g_tid_jump = GetCurrentThreadId();                   /* ... resumed on thread B */
        deep_jump(20);
    }
    g_fresult = (r == 7 && local == 42) ? 1 : -1;
    g_fstate = 2;
    SwitchToFiber(g_home[1]);
    for (;;) Sleep(1000);                                    /* not resumed again */
}
static DWORD WINAPI thread_a(LPVOID p) {
    (void)p;
    g_home[0] = ConvertThreadToFiber(NULL);
    g_fib = CreateFiber(256 << 10, fiber_body, NULL);
    SwitchToFiber(g_fib);
    return 0;
}
static DWORD WINAPI thread_b(LPVOID p) {
    (void)p;
    g_home[1] = ConvertThreadToFiber(NULL);
    SwitchToFiber(g_fib);
    return 0;
}

/* ---- a per-host-thread record table (sdklift_rt.c): the record is found through the thread's own slot */
static __thread jmp_buf *t_rec;
static __attribute__((noinline)) jmp_buf *lookup_rec(void) { return t_rec; }
static jmp_buf *volatile g_seen = (jmp_buf *)1;
static DWORD WINAPI peek(LPVOID p) { (void)p; g_seen = lookup_rec(); return 0; }

int main(void) {
    SECTION("longjmp upward on the thread stack (nro_catch_call pattern), volatile locals kept, nested records");
    CHECK(catch_call(0x10, 0) == 1);
    CHECK(catch_call(0x20, 200) == 1);
    CHECK(nested() == 1);
    CHECK(g_body_ran == 4 && g_after == 4);
    CHECK(g_nslot == 0);
#if defined(_FORTIFY_SOURCE)
    printf("    (_FORTIFY_SOURCE=%d: longjmp is __longjmp_chk on glibc)\n", _FORTIFY_SOURCE);
#endif

    SECTION("setjmp on a fiber, migrate the fiber to another thread, longjmp there");
    HANDLE a = CreateThread(NULL, 0, thread_a, NULL, 0, NULL);
    WaitForSingleObject(a, INFINITE);
    CHECK(g_fstate == 1);
    HANDLE b = CreateThread(NULL, 0, thread_b, NULL, 0, NULL);
    WaitForSingleObject(b, INFINITE);
    CHECK(g_fstate == 2);
    CHECK(g_fresult == 1);
    CHECKF(g_tid_set != g_tid_jump && g_tid_jump != 0, "tids %u %u", (unsigned)g_tid_set, (unsigned)g_tid_jump);
    CloseHandle(a); CloseHandle(b);

    SECTION("caveat: a per-host-thread record table (sdklift_rt.c) does not follow a migrated fiber");
    static jmp_buf jb;
    t_rec = &jb;
    HANDLE h = CreateThread(NULL, 0, peek, NULL, 0, NULL);   /* another thread's view of the slot: what a lookup finds after a migration */
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    CHECK(lookup_rec() == &jb);
    CHECK(g_seen == NULL);                                     /* -> "longjmp to a jmp_buf no lifted setjmp filled on this thread" trap */
    return tp_done("test_setjmp");
}

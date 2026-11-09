/* test_virtual.c - plat_posix.c's VirtualAlloc / VirtualFree / VirtualProtect / VirtualQuery as the runtime uses them: the
 * reservations and commits the guest image, the NRO modules, nn::os memory blocks and fiber stacks go through, fixed
 * addresses and collisions, a commit/decommit/recommit cycle (zero pages), the page runs VirtualQuery reports (what the
 * debug and memory walks of debug_mode.c and nvn_vk_m4.c read), host memory seen through it (heap, stack, code, data), a
 * walk of the whole address space, and 8 threads allocating, committing, protecting, querying and releasing in parallel.
 * tests/plat_selftest.c has the single-call checks; this is the rest of what linux-arm64's tests/plat/test_vmem.c (its own
 * vmem.c, not in this tree) tested. Where plat_posix.c is looser than Windows (a size with MEM_RELEASE, a commit's return
 * value is the address asked, not the page below it) nothing is asserted: no caller depends on the Windows answer. */
#include "tp.h"
#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>

static sigjmp_buf g_jb;
static void on_segv(int sig, siginfo_t *si, void *uc) { (void)sig; (void)si; (void)uc; siglongjmp(g_jb, 1); }
static int write_faults(volatile uint8_t *p) { if (sigsetjmp(g_jb, 1)) return 1; *p = 0x5a; return 0; }
static int read_faults(volatile uint8_t *p) { if (sigsetjmp(g_jb, 1)) return 1; (void)*p; return 0; }
static int prot_ok(uint8_t *p, int wr) { return wr ? !write_faults(p) : !read_faults(p); }

#ifdef __APPLE__
#define FIXED 0x7300000000ull      /* macOS keeps 0x6000000000 for itself; the guest window of the port is 0x7100000000.. */
#else
#define FIXED 0x6000000000ull      /* below 2^47, free in a fresh process on aarch64 Linux */
#endif
#define HP 0x4000                  /* protection is exact to this size on every host: macOS arm64 has 16 KiB host pages (plat keeps 4 KiB bookkeeping) */

static void test_alloc(void) {
    SECTION("reserve + commit anywhere: 64 KiB aligned, below 2^47, page runs, release");
    MEMORY_BASIC_INFORMATION mi;
    uint8_t *p = VirtualAlloc(NULL, 100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(p && ((uintptr_t)p & 0xffff) == 0 && (uint64_t)(uintptr_t)p < (1ull << 47));
    memset(p, 1, 100000);
    CHECK(VirtualQuery(p + 4096 * 3, &mi, sizeof mi) == sizeof mi && mi.State == MEM_COMMIT && mi.AllocationBase == p && mi.Protect == PAGE_READWRITE &&
          mi.BaseAddress == p + 4096 * 3 && mi.RegionSize == 0x19000 - 0x3000);
    CHECK(VirtualFree(p, 0, MEM_RELEASE));
    CHECK(VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_FREE);
    CHECK(!VirtualAlloc(NULL, 0, MEM_RESERVE, PAGE_NOACCESS) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(!VirtualFree((void *)(uintptr_t)(FIXED + 0x123000000ull), 0, MEM_RELEASE));          /* nothing there */

    SECTION("a stack reservation with a guard, as boot.c and os_threads.c build guest stacks");
    uint8_t *res = VirtualAlloc(NULL, (1 << 20) + 0x20000, MEM_RESERVE, PAGE_NOACCESS);
    uint8_t *stk = VirtualAlloc(res + 0x10000, 1 << 20, MEM_COMMIT, PAGE_READWRITE);
    CHECK(res && stk == res + 0x10000);
    CHECK(VirtualQuery(res, &mi, sizeof mi) && mi.State == MEM_RESERVE && mi.RegionSize == 0x10000 && mi.AllocationBase == res);
    CHECK(VirtualQuery(stk, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.RegionSize == 1 << 20 && mi.AllocationBase == res);
    CHECK(VirtualQuery(stk + (1 << 20), &mi, sizeof mi) && mi.State == MEM_RESERVE && mi.RegionSize == 0x10000);
    CHECK(!IsBadWritePtr(stk, 1 << 20) && IsBadReadPtr(res, 1) && IsBadReadPtr(stk + (1 << 20) - 1, 2));
    CHECK(VirtualFree(res, 0, MEM_RELEASE));
}

static void test_fixed(void) {
    SECTION("fixed addresses: reserve at an address, a second reservation over it fails, a commit outside any reservation fails");
    MEMORY_BASIC_INFORMATION mi;
    uint8_t *f = VirtualAlloc((void *)(uintptr_t)FIXED, 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(f == (uint8_t *)(uintptr_t)FIXED);
    if (f != (uint8_t *)(uintptr_t)FIXED) { printf("    (0x%llx not free in this process: skipped)\n", (unsigned long long)FIXED); return; }
    memset(f, 0xab, 0x100000);
    CHECK(!VirtualAlloc((void *)(uintptr_t)(FIXED + 0x80000), 0x1000, MEM_RESERVE, PAGE_NOACCESS) && GetLastError() == ERROR_INVALID_ADDRESS);   /* inside it */
    CHECK(!VirtualAlloc((void *)(uintptr_t)(FIXED - 0x10000), 0x20000, MEM_RESERVE, PAGE_NOACCESS));                                         /* overlaps its start */
    CHECK(!VirtualAlloc((void *)(uintptr_t)(FIXED + 0x200000), 0x1000, MEM_COMMIT, PAGE_READWRITE) && GetLastError() == ERROR_INVALID_ADDRESS);   /* free */
    CHECK(!VirtualAlloc((void *)(uintptr_t)(FIXED + 0xff000), 0x2000, MEM_COMMIT, PAGE_READWRITE));                                          /* runs past it */
    CHECK(f[0x80000] == 0xab);
    CHECK(VirtualFree(f, 0, MEM_RELEASE));
    CHECK(VirtualQuery(f, &mi, sizeof mi) && mi.State == MEM_FREE);
    f = VirtualAlloc((void *)(uintptr_t)FIXED, 0x10000, MEM_RESERVE, PAGE_NOACCESS);             /* free again */
    CHECK(f == (uint8_t *)(uintptr_t)FIXED);
    if (f) VirtualFree(f, 0, MEM_RELEASE);
}

static void test_commit_cycle(void) {
    SECTION("module-style reservation: commit, recommit (contents kept), protect, decommit (zero pages again), partial decommit");
    MEMORY_BASIC_INFORMATION mi;
    DWORD old = 0;
    uint8_t *nro = VirtualAlloc(NULL, 16 << 20, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(nro && VirtualQuery(nro, &mi, sizeof mi) && mi.State == MEM_RESERVE && mi.Protect == 0 && mi.RegionSize == 16 << 20);
    CHECK(IsBadReadPtr(nro, 1));
    uint8_t *c = VirtualAlloc(nro + 0x10000, 0x4000, MEM_COMMIT, PAGE_READWRITE);
    CHECK(c == nro + 0x10000);
    CHECK(VirtualQuery(c, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.RegionSize == 0x4000 && mi.AllocationBase == nro && mi.AllocationProtect == PAGE_NOACCESS);
    memset(c, 7, 0x4000);
    CHECK(VirtualAlloc(c, 0x4000, MEM_COMMIT, PAGE_READWRITE) == c && c[0x3fff] == 7);        /* recommit keeps the contents */
    CHECK(VirtualProtect(c, 0x1000, PAGE_READONLY, &old) && old == PAGE_READWRITE);
    CHECK(VirtualQuery(c, &mi, sizeof mi) && mi.Protect == PAGE_READONLY && mi.RegionSize == 0x1000);
    CHECK(VirtualQuery(c + 0x1000, &mi, sizeof mi) && mi.Protect == PAGE_READWRITE && mi.RegionSize == 0x3000);
    CHECK(VirtualProtect(c, 0x1000, PAGE_READWRITE, &old) && old == PAGE_READONLY);
    CHECK(VirtualFree(c, 0x4000, MEM_DECOMMIT));
    CHECK(VirtualQuery(c, &mi, sizeof mi) && mi.State == MEM_RESERVE && mi.RegionSize == (16 << 20) - 0x10000);   /* reserved again up to the end */
    CHECK(IsBadReadPtr(c, 1));
    CHECK(VirtualAlloc(c, 0x4000, MEM_COMMIT, PAGE_READWRITE) == c && c[0] == 0 && c[0x3fff] == 0);   /* fresh zero pages */
    memset(c, 9, 0x4000);
    CHECK(VirtualFree(c + 0x1000, 0x1000, MEM_DECOMMIT));                                      /* one page out of the middle */
    CHECK(c[0] == 9 && c[0x2000] == 9 && c[0x3fff] == 9);
    CHECK(VirtualQuery(c + 0x1000, &mi, sizeof mi) && mi.State == MEM_RESERVE && mi.RegionSize == 0x1000);
    CHECK(VirtualQuery(c + 0x2000, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.RegionSize == 0x2000);
    CHECK(VirtualFree(nro, 0, MEM_RELEASE));
    CHECK(VirtualQuery(nro, &mi, sizeof mi) && mi.State == MEM_FREE);
    uint8_t *d = VirtualAlloc((void *)nro, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);   /* the same address again */
    CHECK(d == nro && d[0] == 0);
    CHECK(VirtualFree(d, 0, MEM_DECOMMIT) && VirtualQuery(d, &mi, sizeof mi) && mi.State == MEM_RESERVE);   /* the whole allocation */
    CHECK(VirtualFree(d, 0, MEM_RELEASE));
}

static void test_faults(void) {
    SECTION("access rights are real: a read-only page faults on write, a NOACCESS page on read, a decommitted page on both");
    struct sigaction sa, osa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, &osa);
#ifdef __APPLE__
    struct sigaction osb;
    sigaction(SIGBUS, &sa, &osb);
#endif
    DWORD old;
    uint8_t *p = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    memset(p, 0xab, 0x10000);
    CHECK(VirtualProtect(p, HP, PAGE_READONLY, &old));
    CHECK(write_faults(p + 0x100) == 1 && p[0x100] == 0xab && read_faults(p + 0x100) == 0);
    CHECK(write_faults(p + HP) == 0 && p[HP] == 0x5a);
    CHECK(VirtualProtect(p + 2 * HP, HP, PAGE_NOACCESS, &old));
    CHECK(read_faults(p + 2 * HP) == 1 && write_faults(p + 2 * HP + 0x800) == 1);
    CHECK(VirtualFree(p + 3 * HP, HP, MEM_DECOMMIT));
    CHECK(read_faults(p + 3 * HP) == 1);
    CHECK(VirtualProtect(p, HP, PAGE_READWRITE, &old) && old == PAGE_READONLY && write_faults(p) == 0);
    CHECK(prot_ok(p + HP + 0x100, 1));
    CHECK(VirtualFree(p, 0, MEM_RELEASE));
    CHECK(read_faults(p) == 1);                                                               /* unmapped */
    sigaction(SIGSEGV, &osa, NULL);
#ifdef __APPLE__
    sigaction(SIGBUS, &osb, NULL);
#endif
}

static int g_global = 5;
static void test_query_host(void) {
    SECTION("memory plat did not map, through VirtualQuery: heap, stack, code, data, the null page, a PROT_NONE page, beyond 2^47");
    MEMORY_BASIC_INFORMATION mi;
    char *heap = malloc(64);
    int local = 1;
    CHECK(VirtualQuery(heap, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE);
    CHECK((char *)mi.BaseAddress <= heap && (char *)mi.BaseAddress + mi.RegionSize >= heap + 64);
    CHECK(VirtualQuery(&local, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE);
    CHECK(VirtualQuery((void *)(uintptr_t)&test_query_host, &mi, sizeof mi) && mi.State == MEM_COMMIT && (mi.Protect == PAGE_EXECUTE_READ || mi.Protect == PAGE_EXECUTE_READWRITE));
    CHECK(VirtualQuery(&g_global, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE);
    CHECK(VirtualQuery(NULL, &mi, sizeof mi) && mi.State == MEM_FREE);
    CHECK(VirtualQuery((void *)(uintptr_t)(1ull << 48), &mi, sizeof mi) == 0);
    CHECK(VirtualQuery(&local, &mi, sizeof mi - 1) == 0);                                      /* a buffer that is too small */
    CHECK(!IsBadReadPtr(heap, 64) && !IsBadReadPtr(&local, sizeof local) && IsBadReadPtr(NULL, 1) && IsBadReadPtr((void *)(uintptr_t)0x1000, 8));
    free(heap);
    uint8_t *m = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    if (m != MAP_FAILED) {
        CHECK(VirtualQuery(m, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE && !IsBadReadPtr(m, 0x4000));
        munmap(m, 0x4000);
        CHECK(IsBadReadPtr(m, 1));                                                              /* and unmapped again: no stale answer */
    }
    SECTION("the walk debug_mode.c / nvn_vk_m4.c do over the whole address space, with an allocation in it");
    uint8_t *mark = VirtualAlloc(NULL, 0x30000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int regions = 0, monotonic = 1, saw = 0;
    for (uint8_t *p = si.lpMinimumApplicationAddress; p < (uint8_t *)si.lpMaximumApplicationAddress && VirtualQuery(p, &mi, sizeof mi); regions++) {
        uint8_t *next = (uint8_t *)mi.BaseAddress + mi.RegionSize;
        if (next <= p) { monotonic = 0; break; }
        if (mark >= p && mark < next && mi.State == MEM_COMMIT) saw = 1;
        p = next;
    }
    CHECK(monotonic && regions > 5 && saw);
    printf("    walk: %d regions up to %p\n", regions, si.lpMaximumApplicationAddress);
    VirtualFree(mark, 0, MEM_RELEASE);
}

/* thread safety: 8 threads allocating, committing, protecting, querying and releasing in parallel */
static DWORD WINAPI churn(LPVOID arg) {
    int id = (int)(intptr_t)arg, bad = 0;
    for (int i = 0; i < 1000; i++) {
        uint8_t *a = VirtualAlloc(NULL, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        uint8_t *g = VirtualAlloc(NULL, 0x40000, MEM_RESERVE, PAGE_NOACCESS);
        if (!a || !g) { bad++; continue; }
        a[0] = (uint8_t)(i + id);
        if (VirtualAlloc(g + 0x10000, 0x10000, MEM_COMMIT, PAGE_READWRITE) != g + 0x10000) bad++;
        g[0x10000] = 1;
        DWORD old;
        if (!VirtualProtect(g + 0x10000, 0x1000, PAGE_READONLY, &old)) bad++;
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(g + 0x10000, &mi, sizeof mi) || mi.Protect != PAGE_READONLY || mi.AllocationBase != g) bad++;
        if (IsBadReadPtr(g + 0x10000, 0x10000) || !IsBadReadPtr(g, 0x10001)) bad++;
        if (!VirtualFree(a, 0, MEM_RELEASE) || !VirtualFree(g, 0, MEM_RELEASE)) bad++;
    }
    return (DWORD)bad;
}
static void test_threads(void) {
    SECTION("8 threads churning the region map");
    HANDLE h[8];
    for (int i = 0; i < 8; i++) h[i] = CreateThread(NULL, 0, churn, (void *)(intptr_t)i, 0, NULL);
    CHECK(WaitForMultipleObjects(8, h, TRUE, 120000) == WAIT_OBJECT_0);
    for (int i = 0; i < 8; i++) { DWORD c; GetExitCodeThread(h[i], &c); CHECKF(c == 0, "thread %d: %u failures", i, c); CloseHandle(h[i]); }
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_alloc();
    test_fixed();
    test_commit_cycle();
    test_faults();
    test_query_host();
    test_threads();
    return tp_done("test_virtual");
}

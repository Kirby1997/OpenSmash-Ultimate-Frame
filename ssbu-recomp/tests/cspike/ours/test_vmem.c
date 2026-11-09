/* test_vmem.c - guest window, region table, Virtual* shims, readable checks */
#include "tp.h"
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>

#define GUEST_BASE 0x7100000000ull
#define NRO_BASE 0x7200000000ull

static sigjmp_buf g_jb;
static volatile sig_atomic_t g_faults;
static volatile void *g_fault_addr;
static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)uc;
    g_faults++;
    g_fault_addr = si->si_addr;
    siglongjmp(g_jb, 1);
}
static int write_faults(volatile uint8_t *p) {
    if (sigsetjmp(g_jb, 1)) return 1;
    *p = 0x5a;
    return 0;
}
static int read_faults(volatile uint8_t *p) {
    if (sigsetjmp(g_jb, 1)) return 1;
    (void)*p;
    return 0;
}
static int maps_has(uint64_t lo, uint64_t hi) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int found = 0;
    while (f && fgets(line, sizeof line, f)) {
        unsigned long long a, b;
        if (sscanf(line, "%llx-%llx", &a, &b) == 2 && a <= lo && b >= hi) found = 1;
    }
    if (f) fclose(f);
    return found;
}

/* collisions and re-exec are tested in child processes before this process reserves the window */
static int child_status(void) {
    int st = 0;
    wait(&st);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}
static void test_collision(void) {
    SECTION("window collision: EEXIST, fatal message, re-exec with ADDR_NO_RANDOMIZE");
    if (fork() == 0) {
        void *p = mmap((void *)(uintptr_t)0x7300000000ull, 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        _exit(p != MAP_FAILED && vmem_try_init() == -EEXIST ? 0 : 1);
    }
    CHECK(child_status() == 0);
    if (fork() == 0) {                                    /* vmem_init: fatal (exit 70) when re-exec is disabled */
        setenv("SSBU_VMEM_NO_REEXEC", "1", 1);
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, 2);
        mmap((void *)(uintptr_t)0x7500000000ull, 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        vmem_init();
        _exit(0);
    }
    CHECK(child_status() == 70);
    if (fork() == 0) {                                    /* re-exec path: the probe re-runs in a fresh image */
        execl("/proc/self/exe", "test_vmem", "--reexec-probe", (char *)NULL);
        _exit(99);
    }
    CHECK(child_status() == 0);
}
static int reexec_probe(void) {
    if (!getenv("SSBU_VMEM_REEXECED")) {
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, 2);
        mmap((void *)(uintptr_t)0x7200000000ull, 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    }
    vmem_init();                                          /* first run: collides, re-execs; second run: succeeds */
    return getenv("SSBU_VMEM_REEXECED") && maps_has(VMEM_GUEST_LO, VMEM_GUEST_HI) ? 0 : 3;
}

static void test_window(void) {
    SECTION("init: one PROT_NONE mapping over [0x7100000000, 0x7600000000)");
    vmem_init();
    vmem_init();                                          /* idempotent */
    CHECK(maps_has(VMEM_GUEST_LO, VMEM_GUEST_HI));
    CHECK(vmem_in_guest(GUEST_BASE, 0x1000) && !vmem_in_guest(VMEM_GUEST_HI - 0x1000, 0x2000));
    MEMORY_BASIC_INFORMATION mi;
    CHECK(VirtualQuery((void *)(uintptr_t)GUEST_BASE, &mi, sizeof mi) == sizeof mi);
    CHECK(mi.State == MEM_FREE && mi.RegionSize == VMEM_GUEST_HI - GUEST_BASE);
    CHECK(!vmem_readable((void *)(uintptr_t)GUEST_BASE, 1));

    SECTION("image: reserve+commit at a fixed address, read-only split, SIGSEGV on write");
    uint8_t *img = VirtualAlloc((void *)(uintptr_t)GUEST_BASE, 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(img == (uint8_t *)(uintptr_t)GUEST_BASE);
    memset(img, 0xab, 0x100000);
    DWORD old = 0;
    CHECK(VirtualProtect(img, 0x10000, PAGE_READONLY, &old) && old == PAGE_READWRITE);
    CHECK(VirtualQuery(img + 5, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.Protect == PAGE_READONLY && mi.BaseAddress == img &&
          mi.RegionSize == 0x10000 && mi.AllocationBase == img && mi.AllocationProtect == PAGE_READWRITE && mi.Type == MEM_PRIVATE);
    CHECK(VirtualQuery(img + 0x10000, &mi, sizeof mi) && mi.Protect == PAGE_READWRITE && mi.RegionSize == 0x100000 - 0x10000);
    CHECK(VirtualQuery(img + 0x23456, &mi, sizeof mi) && mi.BaseAddress == img + 0x23000 && mi.RegionSize == 0x100000 - 0x23000);
    struct sigaction sa = {0}, osa;
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, &osa);
    g_faults = 0;
    CHECK(write_faults(img + 0x100) == 1 && g_fault_addr == img + 0x100);
    CHECK(img[0x100] == 0xab);
    CHECK(write_faults(img + 0x10000) == 0 && img[0x10000] == 0x5a);
    CHECK(read_faults(img + 0x100) == 0);
    CHECK(read_faults((uint8_t *)(uintptr_t)(GUEST_BASE + 0x200000)) == 1);   /* free window page */
    CHECK(vmem_readable(img, 0x100000) && !vmem_readable(img, 0x100001));
    CHECK(!VirtualAlloc((void *)(uintptr_t)(GUEST_BASE + 0x80000), 0x1000, MEM_RESERVE, PAGE_NOACCESS) && GetLastError() == ERROR_INVALID_ADDRESS);   /* overlap */
    CHECK(!VirtualProtect((void *)(uintptr_t)(GUEST_BASE + 0xff000), 0x2000, PAGE_READONLY, &old));   /* runs past the allocation */

    SECTION("NRO: reserve, commit/decommit/recommit (zero), protect, release without size");
    uint8_t *nro = VirtualAlloc((void *)(uintptr_t)NRO_BASE, 16 << 20, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(nro == (uint8_t *)(uintptr_t)NRO_BASE);
    CHECK(VirtualQuery(nro, &mi, sizeof mi) && mi.State == MEM_RESERVE && mi.Protect == 0 && mi.RegionSize == 16 << 20);
    CHECK(!vmem_readable(nro, 1) && read_faults(nro) == 1);
    uint8_t *c = VirtualAlloc(nro + 0x10123, 0x3000, MEM_COMMIT, PAGE_READWRITE);
    CHECK(c == nro + 0x10000);
    CHECK(VirtualQuery(c, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.RegionSize == 0x4000 && mi.AllocationBase == nro);
    memset(c, 7, 0x4000);
    CHECK(VirtualAlloc(c, 0x4000, MEM_COMMIT, PAGE_READWRITE) == c && c[0x3fff] == 7);   /* recommit keeps contents */
    CHECK(VirtualProtect(c, 0x1000, PAGE_READONLY, &old) && write_faults(c) == 1);
    CHECK(!VirtualProtect(nro, 0x1000, PAGE_READONLY, &old));   /* not committed */
    CHECK(VirtualFree(c, 0x4000, MEM_DECOMMIT));
    CHECK(VirtualQuery(c, &mi, sizeof mi) && mi.State == MEM_RESERVE && mi.RegionSize == (16 << 20) - 0x10000);   /* coalesced again: from c to the end */
    CHECK(read_faults(c) == 1);
    CHECK(VirtualAlloc(c, 0x4000, MEM_COMMIT, PAGE_READWRITE) == c && c[0] == 0 && c[0x3fff] == 0);   /* fresh zero pages */
    CHECK(!VirtualFree(nro, 0x1000, MEM_RELEASE) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(!VirtualFree(nro + 0x10000, 0, MEM_RELEASE));  /* not the base */
    CHECK(VirtualFree(nro, 0, MEM_RELEASE));
    CHECK(VirtualQuery(nro, &mi, sizeof mi) && mi.State == MEM_FREE);
    CHECK(read_faults(c) == 1 && maps_has((uint64_t)(uintptr_t)c, (uint64_t)(uintptr_t)c + 0x4000));   /* back under the window reservation */
    CHECK(!VirtualFree(nro, 0, MEM_RELEASE));
    uint8_t *d = VirtualAlloc((void *)(uintptr_t)NRO_BASE, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(d == nro && d[0] == 0);
    CHECK(VirtualFree(d, 0, MEM_DECOMMIT) && VirtualQuery(d, &mi, sizeof mi) && mi.State == MEM_RESERVE);   /* whole allocation */
    CHECK(VirtualFree(d, 0, MEM_RELEASE));
    sigaction(SIGSEGV, &osa, NULL);
    CHECK(VirtualFree(img, 0, MEM_RELEASE));
}

static void test_outside(void) {
    SECTION("outside the window: 64 KiB aligned allocations, release without size");
    uint8_t *p = VirtualAlloc(NULL, 100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(p && ((uintptr_t)p & 0xffff) == 0 && !vmem_in_guest((uint64_t)(uintptr_t)p, 1) && (uint64_t)(uintptr_t)p < (1ull << 47));
    memset(p, 1, 100000);
    MEMORY_BASIC_INFORMATION mi;
    CHECK(VirtualQuery(p + 4096 * 3, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.AllocationBase == p && mi.RegionSize == 0x19000 - 0x3000);
    CHECK(VirtualFree(p, 0, MEM_RELEASE));
    CHECK(VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_FREE && !vmem_readable(p, 1));
    /* stack reservation with a guard, as boot.c builds guest stacks */
    uint8_t *res = VirtualAlloc(NULL, (1 << 20) + 0x20000, MEM_RESERVE, PAGE_NOACCESS);
    uint8_t *stk = VirtualAlloc(res + 0x10000, 1 << 20, MEM_COMMIT, PAGE_READWRITE);
    CHECK(res && stk == res + 0x10000);
    CHECK(!vmem_readable(res, 0x10000) && vmem_readable(stk, 1 << 20) && !vmem_readable(stk, (1 << 20) + 1));
    CHECK(VirtualQuery(res, &mi, sizeof mi) && mi.State == MEM_RESERVE && mi.RegionSize == 0x10000);
    CHECK(VirtualFree(res, 0, MEM_RELEASE));
    uint8_t *fixed = VirtualAlloc((void *)(uintptr_t)0x6000000000ull, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(fixed == (uint8_t *)(uintptr_t)0x6000000000ull);
    CHECK(!VirtualAlloc((void *)(uintptr_t)0x6000000000ull, 0x10000, MEM_RESERVE, PAGE_READWRITE));   /* taken */
    CHECK(VirtualFree(fixed, 0, MEM_RELEASE));
    CHECK(!VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS) && GetLastError() == ERROR_NOT_SUPPORTED);
    CHECK(!VirtualAlloc(NULL, 0x10000, MEM_RESERVE, 0x12345));
    void *v = vmem_alloc(0x8000, VMEM_RESERVE_ONLY);
    CHECK(v && vmem_commit(v, 0x1000, PROT_READ | PROT_WRITE) == 0 && vmem_readable(v, 0x1000) && !vmem_readable(v, 0x1001));
    int oldp = -1;
    CHECK(vmem_protect(v, 0x1000, PROT_READ, &oldp) == 0 && oldp == (PROT_READ | PROT_WRITE));
    CHECK(vmem_decommit(v, 0x1000) == 0 && vmem_release(v) == 0 && vmem_release(v) == -EINVAL);
}

static int g_global = 5;
static void test_query_host(void) {
    SECTION("host memory: heap, stack, image, gaps, NULL, full walk");
    MEMORY_BASIC_INFORMATION mi;
    char *heap = malloc(64);
    int local = 1;
    CHECK(VirtualQuery(heap, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE && mi.Type == MEM_PRIVATE);
    CHECK((char *)mi.BaseAddress <= heap && (char *)mi.BaseAddress + mi.RegionSize >= heap + 64);
    CHECK(VirtualQuery(&local, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE);
    CHECK(VirtualQuery((void *)(uintptr_t)&test_query_host, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.Protect == PAGE_EXECUTE_READ && mi.Type == MEM_IMAGE);
    CHECK(VirtualQuery(&g_global, &mi, sizeof mi) && mi.State == MEM_COMMIT && (mi.Protect == PAGE_READWRITE));
    CHECK(VirtualQuery((void *)(uintptr_t)strlen, &mi, sizeof mi) && mi.Type == MEM_IMAGE);   /* libc.so */
    CHECK(VirtualQuery(NULL, &mi, sizeof mi) && mi.State == MEM_FREE);
    CHECK(VirtualQuery((void *)(uintptr_t)(1ull << 48), &mi, sizeof mi) == 0);
    CHECK(vmem_readable(heap, 64) && vmem_readable(&local, sizeof local) && vmem_readable((void *)(uintptr_t)&test_query_host, 16));
    CHECK(!vmem_readable(NULL, 1) && !vmem_readable((void *)(uintptr_t)0x1000, 8) && vmem_readable(NULL, 0));
    CHECK(!vmem_readable((void *)(uintptr_t)~0ull, 2));
    uint8_t *m = mmap(NULL, 0x3000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(m + 0x1000, 0x1000, PROT_NONE);
    CHECK(vmem_readable(m, 0x1000) && !vmem_readable(m + 0xfff, 2) && vmem_readable(m + 0x2000, 0x1000) && !vmem_readable(m, 0x3000));
    CHECK(VirtualQuery(m + 0x1000, &mi, sizeof mi) && mi.State == MEM_RESERVE);
    DWORD old = 0;
    CHECK(VirtualProtect(m + 0x1000, 0x1000, PAGE_READONLY, &old) && old == PAGE_NOACCESS && vmem_readable(m, 0x3000));   /* untracked: mprotect */
    munmap(m, 0x3000);
    CHECK(!vmem_readable(m, 1));
    free(heap);
    /* the walk debug_mode.c / nvn_vk_m4.c do */
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int regions = 0, monotonic = 1, saw_window = 0;
    for (uint8_t *p = si.lpMinimumApplicationAddress; p < (uint8_t *)si.lpMaximumApplicationAddress && VirtualQuery(p, &mi, sizeof mi); regions++) {
        uint8_t *next = (uint8_t *)mi.BaseAddress + mi.RegionSize;
        if (next <= p) { monotonic = 0; break; }
        if ((uint64_t)(uintptr_t)p == VMEM_GUEST_LO && mi.State == MEM_FREE) saw_window = 1;
        p = next;
    }
    CHECK(monotonic && regions > 5 && saw_window);
    printf("    walk: %d regions up to %p\n", regions, si.lpMaximumApplicationAddress);
    void *small = malloc(64), *big = malloc(1 << 20);
    printf("    host pointers: malloc(64) %p, malloc(1M) %p, stack %p, vmem_alloc lands below 2^47\n", small, big, (void *)&local);
    free(small); free(big);
}

/* thread safety: 8 threads allocating, committing, protecting, querying and releasing in parallel */
static DWORD WINAPI churn(LPVOID arg) {
    int id = (int)(intptr_t)arg, bad = 0;
    uint64_t slot = 0x7400000000ull + (uint64_t)id * 0x10000000ull;   /* own slice of the window per thread */
    for (int i = 0; i < 1000; i++) {
        uint8_t *a = VirtualAlloc(NULL, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        uint8_t *g = VirtualAlloc((void *)(uintptr_t)(slot + (uint64_t)(i & 7) * 0x100000), 0x40000, MEM_RESERVE, PAGE_NOACCESS);
        if (!a || !g) { bad++; continue; }
        a[0] = (uint8_t)i;
        if (VirtualAlloc(g + 0x10000, 0x10000, MEM_COMMIT, PAGE_READWRITE) != g + 0x10000) bad++;
        g[0x10000] = 1;
        DWORD old;
        if (!VirtualProtect(g + 0x10000, 0x1000, PAGE_READONLY, &old)) bad++;
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(g + 0x10000, &mi, sizeof mi) || mi.Protect != PAGE_READONLY || mi.AllocationBase != g) bad++;
        if (!vmem_readable(g + 0x10000, 0x10000) || vmem_readable(g, 0x10001)) bad++;
        if (!VirtualFree(a, 0, MEM_RELEASE) || !VirtualFree(g, 0, MEM_RELEASE)) bad++;
    }
    return (DWORD)bad;
}
static void test_threads(void) {
    SECTION("8 threads churning the region table");
    size_t before = vmem_region_count();
    HANDLE h[8];
    for (int i = 0; i < 8; i++) h[i] = CreateThread(NULL, 0, churn, (void *)(intptr_t)i, 0, NULL);
    CHECK(WaitForMultipleObjects(8, h, TRUE, 120000) == WAIT_OBJECT_0);
    for (int i = 0; i < 8; i++) { DWORD c; GetExitCodeThread(h[i], &c); CHECKF(c == 0, "thread %d: %u failures", i, c); CloseHandle(h[i]); }
    CHECK(vmem_region_count() == before);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--reexec-probe")) return reexec_probe();
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_collision();
    test_window();
    test_outside();
    test_query_host();
    test_threads();
    if (tp_fail) vmem_dump(stderr);
    return tp_done("test_vmem");
}

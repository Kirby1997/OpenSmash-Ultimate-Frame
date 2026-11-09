/* test_readable.c - the "may I read this" checks agree with the kernel: plat_posix.c's IsBadReadPtr (VirtualQuery over the page
 * runs; nncxxrt.c's cxxrt_readable and the unwinder / RTTI checks ask the same thing) against plat_crash.c's crash_read (a
 * write(2) of the memory into a pipe: the kernel says EFAULT instead of the process faulting). They must give the same
 * answer on mapped, unmapped, PROT_NONE, reserved, decommitted, released, straddling and untracked (raw mmap, stack, heap)
 * memory, and the bytes crash_read gives back must be the memory's. Was tests/port/crash/test_readable.c of linux-arm64
 * (vmem_readable / cxxrt_readable there; plat's VirtualQuery here, cxxrt_readable itself is in tests/port/threads/test_cxxrt.c). */
#include "tp.h"
#include "plat_crash.h"
#include <sys/mman.h>
#include <string.h>

static int both(const void *p, size_t n) {                /* IsBadReadPtr and crash_read agree; returns the answer */
    uint64_t a = (uint64_t)(uintptr_t)p;
    int r1 = !IsBadReadPtr(p, n);
    unsigned char buf[256];
    int r2 = n <= sizeof buf ? crash_read(a, buf, n) : r1;
    CHECKF(r1 == r2, "IsBadReadPtr says %s, crash_read %s at %p + %zu", r1 ? "readable" : "unreadable", r2 ? "readable" : "unreadable", p, n);
    if (r2 && n && n <= sizeof buf) CHECK(memcmp(buf, p, n) == 0);
    return r1;
}

int main(void) {
    crash_install(NULL, NULL, NULL, 4);                    /* makes the pipe crash_read needs; no signal is raised here */
    const size_t PG = 0x4000;                              /* protection is exact to this on every host (macOS arm64: 16 KiB host pages) */

    SECTION("an allocation of VirtualAlloc: committed / PROT_NONE page / straddle / decommitted / released");
    uint8_t *a = VirtualAlloc(NULL, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(a != NULL);
    memset(a, 0x5a, 0x20000);
    DWORD old;
    CHECK(both(a, 0x20000 > 256 ? 256 : 0x20000) == 1);
    CHECK(both(a + 0x1ffff, 1) == 1);
    CHECK(both(a + 0x20000 - 8, 16) == 0);                /* runs past the end */
    CHECK(VirtualProtect(a + 0x8000, PG, PAGE_NOACCESS, &old));
    CHECK(both(a + 0x8000, 8) == 0);
    CHECK(both(a + 0x8000 - 8, 16) == 0);                 /* straddles into the PROT_NONE page */
    CHECK(both(a + 0x4000, 0x100) == 1 && both(a + 0x7f00, 0x100) == 1);
    CHECK(both(a + 0x8000 + PG, 8) == 1);
    CHECK(VirtualProtect(a + 0x8000, PG, PAGE_READONLY, &old) && old == PAGE_NOACCESS);
    CHECK(both(a + 0x8000, 8) == 1);                      /* read-only is readable */
    CHECK(VirtualFree(a + 0x10000, 0x10000, MEM_DECOMMIT));
    CHECK(both(a + 0x10000, 8) == 0);                     /* decommitted = reserved */
    CHECK(both(a + 0xfff8, 8) == 1);
    CHECK(both(a + 0xfff8, 16) == 0);                     /* across the end of the committed part */
    CHECK(VirtualFree(a, 0, MEM_RELEASE));
    CHECK(both(a, 8) == 0);
    CHECK(both(a + 0x8000 + PG, 8) == 0);

    SECTION("a reservation at a fixed address: free, reserved, committed, decommitted, PROT_NONE commit");
    #ifdef __APPLE__
    uint64_t g = 0x7340000000ull;
#else
    uint64_t g = 0x6040000000ull;
#endif
    uint8_t *r = VirtualAlloc((void *)(uintptr_t)g, 0x40000, MEM_RESERVE, PAGE_NOACCESS);
    if (r != (uint8_t *)(uintptr_t)g) printf("    (0x%llx not free: skipped)\n", (unsigned long long)g);
    else {
        CHECK(both(r, 8) == 0);                           /* reserved only */
        CHECK(VirtualAlloc(r + 0x10000, 0x10000, MEM_COMMIT, PAGE_READWRITE) == r + 0x10000);
        memset(r + 0x10000, 3, 0x10000);
        CHECK(both(r + 0x10000, 0x100) == 1);
        CHECK(both(r + 0x10000 - 4, 8) == 0);
        CHECK(both(r + 0x20000 - 4, 8) == 0);
        CHECK(VirtualAlloc(r + 0x20000, PG, MEM_COMMIT, PAGE_NOACCESS) == r + 0x20000);
        CHECK(both(r + 0x20000, 8) == 0);                 /* committed but no access */
        CHECK(VirtualFree(r + 0x10000, 0x10000, MEM_DECOMMIT));
        CHECK(both(r + 0x10000, 8) == 0);
        CHECK(VirtualFree(r, 0, MEM_RELEASE));
        CHECK(both(r + 0x10000, 8) == 0);
    }

    SECTION("memory plat did not map: stack, heap, raw mmap RW / PROT_NONE / unmapped, null, kernel space, wrap-around");
    volatile uint64_t local = 0x1122334455667788ull;
    CHECK(both((const void *)&local, 8) == 1);
    char *h = malloc(1000);
    memset(h, 1, 1000);
    CHECK(both(h, 256) == 1);
    free(h);
    size_t P = (size_t)sysconf(_SC_PAGESIZE);              /* a raw mmap cannot be split below the host page */
    uint8_t *m = mmap(NULL, 3 * P, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    m[0] = 7;
    CHECK(both(m, 256) == 1);
    CHECK(!IsBadReadPtr(m, 3 * P));
    mprotect(m + P, P, PROT_NONE);
    CHECK(both(m + P, 8) == 0);
    CHECK(both(m + P - 4, 8) == 0);
    CHECK(both(m + 2 * P, 8) == 1);
    CHECK(IsBadReadPtr(m, 3 * P));
    munmap(m, 3 * P);
    CHECK(both(m, 8) == 0);
    CHECK(both(m + 2 * P, 8) == 0);
    CHECK(IsBadReadPtr(NULL, 8));
    CHECK(!crash_read(0, (void *)&local, 8));
    CHECK(both((void *)(uintptr_t)0xffff800000001000ull, 8) == 0);
    CHECK(IsBadReadPtr((void *)(uintptr_t)0xfffffffffffffff0ull, 0x20));          /* wraps */
    CHECK(!IsBadReadPtr((const void *)&local, 0));

    return tp_done("test_readable");
}

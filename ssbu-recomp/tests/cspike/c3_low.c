/* C3: every address the guest can be handed stays below 2^47, and write watch still works.
 * Build: like plat_selftest, plus runtime/plat_heap.c on aarch64 (tests/cspike/c3_build.sh). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <malloc.h>
#include <sys/mman.h>
static int g_bad, g_n;
#define LIM (1ULL << 47)
static void chk(const void *p, size_t n, const char *what) {
    g_n++;
    if (!p || (uint64_t)(uintptr_t)p + n > LIM) { g_bad++; if (g_bad < 20) printf("FAIL %s %p + %zx\n", what, p, n); }
}
static void *worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 300; i++) {
        size_t n = (size_t)1 << (4 + i % 28);                  /* 16 B .. 1 GiB */
        void *p = malloc(n); chk(p, n, "malloc"); if (p && n <= (1 << 22)) memset(p, 1, n); free(p);
        p = calloc(1, n > (1 << 24) ? 4096 : n); chk(p, n > (1 << 24) ? 4096 : n, "calloc"); free(p);
        p = NULL; if (posix_memalign(&p, 1 << (4 + i % 12), n > (1 << 24) ? 65536 : n) == 0) chk(p, 1, "memalign"); free(p);
        void *v = VirtualAlloc(NULL, 1 + (size_t)(i % 97) * 4099, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        chk(v, 1, "VirtualAlloc"); if (v) { *(volatile char *)v = 1; VirtualFree(v, 0, MEM_RELEASE); }
    }
    return NULL;
}
int main(void) {
    void *keep[200]; int nk = 0;
    for (int i = 0; i < 200; i++) {                             /* reserve-only, big, small, held live */
        size_t n = i % 5 == 0 ? (size_t)1 << 32 : (size_t)(i + 1) * 4096;
        keep[nk] = VirtualAlloc(NULL, n, i & 1 ? MEM_RESERVE : MEM_RESERVE | MEM_COMMIT, i & 1 ? PAGE_NOACCESS : PAGE_READWRITE);
        chk(keep[nk], n, "VirtualAlloc keep");
        if (keep[nk] && ((uintptr_t)keep[nk] & 0xffff)) { g_bad++; printf("FAIL not 64K aligned %p\n", keep[nk]); }
        if (keep[nk]) nk++;
    }
    void *a = VirtualAlloc(keep[0], 4096, MEM_COMMIT, PAGE_READWRITE); (void)a;
    pthread_t t[8];
    for (int i = 0; i < 8; i++) pthread_create(&t[i], 0, worker, 0);
    worker(0);
    for (int i = 0; i < 8; i++) pthread_join(t[i], 0);
    for (int i = 0; i < nk; i++) VirtualFree(keep[i], 0, MEM_RELEASE);
#if defined(__aarch64__)
    {   /* low_place's two slow paths. EEXIST: a foreign mapping where the cursor points is stepped over, not an error */
        uint8_t *v1 = VirtualAlloc(NULL, 0x10000, MEM_RESERVE, PAGE_NOACCESS); chk(v1, 0x10000, "eexist v1");
        void *blk = v1 ? mmap(v1 + 0x10000, 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) : MAP_FAILED;
        uint8_t *v2 = VirtualAlloc(NULL, 0x10000, MEM_RESERVE, PAGE_NOACCESS); chk(v2, 0x10000, "eexist v2");
        if (blk == MAP_FAILED || !v2 || v2 == (uint8_t *)blk || ((uintptr_t)v2 & 0xffff)) { g_bad++; printf("FAIL eexist skip %p %p %p\n", (void *)v1, blk, (void *)v2); }
        /* wrap: push the cursor to within 2 GiB of 2^47 (one PROT_NONE NORESERVE reservation, not accounted), release it,
         * then an 8 GiB reservation must come back from the low end (reused space), never above 2^47 */
        size_t big = v2 ? (size_t)((LIM - (uintptr_t)v2 - 0x10000 - (2ULL << 30)) & ~0xffffULL) : 0;
        uint8_t *b = big ? VirtualAlloc(NULL, big, MEM_RESERVE, PAGE_NOACCESS) : NULL; chk(b, big, "wrap big");
        if (b) VirtualFree(b, 0, MEM_RELEASE);
        uint8_t *r = VirtualAlloc(NULL, (size_t)8 << 30, MEM_RESERVE, PAGE_NOACCESS); chk(r, (size_t)8 << 30, "wrap after");
        if (!b || !r || r >= b) { g_bad++; printf("FAIL wrap %p (+%zx) -> %p\n", (void *)b, big, (void *)r); }
        if (r) VirtualFree(r, 0, MEM_RELEASE);
        if (blk != MAP_FAILED) munmap(blk, 0x10000);
        if (v1) VirtualFree(v1, 0, MEM_RELEASE);
        if (v2) VirtualFree(v2, 0, MEM_RELEASE);
    }
#endif
    /* reuse: lots of alloc/free cycles do not walk off the end (20 GiB of the ~127 TiB: the wrap is the block above) */
    for (int i = 0; i < 20000; i++) { void *v = VirtualAlloc(NULL, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); chk(v, 1 << 20, "cycle"); VirtualFree(v, 0, MEM_RELEASE); }
    /* write watch: reserve with MEM_WRITE_WATCH, commit, write 3 pages, GetWriteWatch reports them */
    uint8_t *w = VirtualAlloc(NULL, 64 * 4096, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
    chk(w, 64 * 4096, "ww reserve");
    int ww_ok = 0;
    if (w && VirtualAlloc(w, 64 * 4096, MEM_COMMIT, PAGE_READWRITE)) {
        PVOID addrs[64]; ULONG_PTR cnt = 64; ULONG gran;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, w, 64 * 4096, addrs, &cnt, &gran) == 0) {
            w[5 * 4096] = 1; w[9 * 4096 + 7] = 2; w[40 * 4096] = 3;
            cnt = 64;
            if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, w, 64 * 4096, addrs, &cnt, &gran) == 0 && cnt == 3 && addrs[0] == w + 5 * 4096 && addrs[1] == w + 9 * 4096 && addrs[2] == w + 40 * 4096) {
                cnt = 64;
                if (GetWriteWatch(0, w, 64 * 4096, addrs, &cnt, &gran) == 0 && cnt == 0) ww_ok = 1;
            }
        } else { printf("write watch unavailable on this kernel: skipped\n"); ww_ok = -1; }
    }
    if (ww_ok == 0) { g_bad++; printf("FAIL write watch\n"); }
    MEMORY_BASIC_INFORMATION mi; SYSTEM_INFO si; GetSystemInfo(&si);
    if ((uint64_t)(uintptr_t)si.lpMaximumApplicationAddress >= LIM) { g_bad++; printf("FAIL GetSystemInfo max\n"); }
    if (VirtualQuery((void *)LIM, &mi, sizeof mi) != 0) { g_bad++; printf("FAIL VirtualQuery at 2^47 answered\n"); }
    if (!VirtualQuery(w, &mi, sizeof mi) || mi.State != MEM_COMMIT) { g_bad++; printf("FAIL VirtualQuery of ww\n"); }
    printf("c3_low: %d addresses checked, %d bad, writewatch %s -> %s\n", g_n, g_bad, ww_ok > 0 ? "ok" : "skipped", g_bad ? "FAIL" : "ok");
    return g_bad != 0;
}

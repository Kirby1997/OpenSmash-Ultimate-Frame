/* SSBU_VK_STREAMKEEP's table (runtime/stream_keep.inc) against the truth, with a fake guest memory, write watch and stream
 * ring: every offset sk_put answers must hold the bytes guest memory holds NOW. Random CPU writes (the watch sees them),
 * decommits (pages become zero, not "written", the epoch moves), unwatched ranges, ring wraps (also in the middle of a
 * copy, with a copy-back that writes guest memory first), storage-buffer uses that drop an entry and then scribble on its
 * copy (the GPU writing it), batches (back-off). No game, no GPU.
 *   -DBREAK=1 ignores the epoch, 2 the wrap count, 3 the stamps, 4 the drop for a storage-buffer use: each must be caught
 *   (run.sh checks). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 4096
#define NPAGE 512                      /* guest memory: 2 MB, the last 64 pages not write-watched */
#define WATCHED (NPAGE - 64)
#define RING (16u << 20)
static uint8_t guest[NPAGE * PAGE], ring[RING];
static uint8_t written[NPAGE];
static uint64_t pstamp[NPAGE], g_stamp = 1, epoch;
static int64_t wraps;
static uint64_t pos;
static int in_copy_wrap_land;          /* a wrap inside a copy lands a compute buffer: a CPU write between question and copy */

static void cpu_write(uint64_t a, uint64_t n, int seed) { for (uint64_t i = 0; i < n; i++) guest[a + i] = (uint8_t)(seed + i * 7); for (uint64_t p = a / PAGE; p <= (a + n - 1) / PAGE; p++) written[p] = 1; }
static void decommit(uint64_t p0, uint64_t np) { memset(guest + p0 * PAGE, 0, np * PAGE); for (uint64_t p = p0; p < p0 + np; p++) written[p] = 0; epoch++; }
static uint64_t newest(uint64_t a, uint64_t n) {          /* ww_newest: written pages get a new stamp, the answer is the max */
    uint64_t f = a / PAGE, l = (a + n - 1) / PAGE, m = 1, s = 0;
    if (l >= WATCHED) return 0;
    for (uint64_t p = f; p <= l; p++) if (written[p]) { if (!s) s = ++g_stamp; pstamp[p] = s; written[p] = 0; }
    for (uint64_t p = f; p <= l; p++) if (pstamp[p] > m) m = pstamp[p];
    return m;
}
static uint64_t copy(uint64_t a, uint64_t n) {             /* ring_raw */
    pos = (pos + 255) & ~255ull;
    if (pos + n > RING) { pos = 0; wraps++; memset(ring, 0xEE, RING); if (in_copy_wrap_land) cpu_write(a + n / 2, 1, 99); }
    memcpy(ring + pos, guest + a, n);
    uint64_t o = pos; pos += n; return o;
}
#if BREAK == 1
#define SK_EPOCH() 0ull
#else
#define SK_EPOCH() epoch
#endif
#if BREAK == 2
#define SK_WRAPS 0
#else
#define SK_WRAPS wraps
#endif
#if BREAK == 3
#define SK_NEWEST(a, n) (newest(a, n) ? 1 : 0)
#else
#define SK_NEWEST(a, n) newest(a, n)
#endif
#define SK_COPY(a, n) copy(a, n)
#include "stream_keep.inc"
#if BREAK == 4
#define sk_kill(a, n) ((void)0)
#endif

static uint64_t rnd = 88172645463325252ull;
static uint64_t rn(uint64_t m) { rnd ^= rnd << 13; rnd ^= rnd >> 7; rnd ^= rnd << 17; return rnd % m; }

int main(void) {
    g_sk = calloc((size_t)SK_SETS * SK_WAYS, sizeof *g_sk);
    struct { uint64_t a, n; } key[200];
    for (int k = 0; k < 200; k++) {                          /* fixed ranges as a game binds them, some unwatched, some sharing pages */
        uint64_t n = 16384 + rn(8) * 4096 + rn(2) * rn(4000);
        key[k].n = n; key[k].a = rn(NPAGE * PAGE - n) & ~15ull;
    }
    for (uint64_t i = 0; i < sizeof guest; i++) guest[i] = (uint8_t)(i * 13);
    long checks = 0, bad = 0, hits = 0;
    for (int step = 0; step < 2000000; step++) {
        int op = (int)rn(1000), k = (int)rn(200);
        if (op < 15) cpu_write(rn(sizeof guest - 64), 1 + rn(64), step);
        else if (op < 17) { uint64_t p = rn(NPAGE - 8); decommit(p, 1 + rn(8)); }
        else if (op < 20) {                                   /* a storage-buffer use of the range: it shares the acache copy, which may be the kept one, and the GPU writes it */
            SkEnt *w = sk_set(key[k].a, key[k].n);
            uint64_t o = SK_NONE;
            for (int i = 0; i < SK_WAYS; i++) if (w[i].addr == key[k].a && w[i].size == key[k].n && w[i].wrap == wraps) o = w[i].off;      /* the acache shares only a copy of this wrap */
            sk_kill(key[k].a, key[k].n);
            if (o == SK_NONE) o = copy(key[k].a, key[k].n);
            memset(ring + o, 0x55, 64);
        }
        else if (op < 40) g_sk_batch++;
        else {
            in_copy_wrap_land = op < 45;
            uint64_t h0 = (uint64_t)g_sk_hits, o = sk_put(key[k].a, key[k].n);
            hits += (uint64_t)g_sk_hits != h0;
            checks++;
            if (memcmp(ring + o, guest + key[k].a, key[k].n)) { if (bad++ < 5) printf("FAIL step %d: range %llx+%llu at ring %llu does not hold guest memory\n", step, (unsigned long long)key[k].a, (unsigned long long)key[k].n, (unsigned long long)o); }
        }
    }
    printf("%ld checks, %ld kept copies bound again, %lld questions, %lld changed, %lld back-off copies, %lld unwatched, %lld stale, %ld failed\n",
           checks, hits, (long long)g_sk_queries, (long long)g_sk_changed, (long long)g_sk_skips, (long long)g_sk_unwatched, (long long)g_sk_stale, bad);
    if (hits < checks / 10) { printf("FAIL: almost nothing was kept (%ld of %ld): the test does not exercise the table\n", hits, checks); return 1; }
    return bad != 0;
}

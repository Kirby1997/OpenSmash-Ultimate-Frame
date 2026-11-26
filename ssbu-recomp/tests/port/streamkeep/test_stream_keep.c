/* SSBU_VK_STREAMKEEP's table (runtime/stream_keep.inc) against the truth, with a fake guest memory, write watch and stream
 * ring: every offset sk_put answers must hold the bytes guest memory holds NOW. Random CPU writes (the watch sees them),
 * decommits (pages become zero, not "written", the epoch moves), unwatched ranges, ring wraps (also in the middle of a
 * copy, with a copy-back that writes guest memory first), storage-buffer uses that drop an entry and then scribble on its
 * copy (the GPU writing it), batches (back-off). No game, no GPU.
 *   -DBREAK=1 ignores the epoch, 2 the wrap count, 3 the stamps, 4 the drop for a storage-buffer use: each must be caught
 *   (run.sh checks).
 * Second part (review F1 of perf/render-2): the same table, checked when the GPU READS the bytes, not when an offset is
 * answered. Draws as draw_prepare collects them (two c2 blocks reused while unchanged in the wrap, small ranges through a
 * per-batch range cache, a per-draw range, kept ranges), recorded into a command buffer that runs at batch end or at the
 * next wrap (a wrap submits and waits first, then copies from offset 0 again and does not scribble: the old bytes stay
 * until overwritten). Every range of every recorded draw must hold, when its command buffer runs, the bytes it held when
 * it was taken. A wrap in the middle of a draw collects the draw again (draw_prepare's loop); -DF1=0 does not, as before
 * the fix, and must fail (run.sh checks). */
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
/* second part: what the command buffer being recorded reads (offset, size, hash of the bytes when they were taken) */
#ifndef F1
#define F1 1
#endif
static int g_gpu;                      /* second part: a wrap submits what is recorded (checks it) and does not scribble */
static uint64_t gen = 1;               /* the range cache's generation (R.gen): batches and wraps */
static struct { uint64_t off, n, h; } pend[16384];
static int npend;
static long gchecks, gbad;
static uint64_t h64(const uint8_t *p, uint64_t n) {
    uint64_t h = 1469598103934665603ull, i = 0, w;
    for (; i + 8 <= n; i += 8) { memcpy(&w, p + i, 8); h = (h ^ w) * 1099511628211ull; }
    for (; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
static void submit(void) {             /* the command buffer runs now: every recorded draw reads its ranges */
    for (int i = 0; i < npend; i++) {
        gchecks++;
        if (h64(ring + pend[i].off, pend[i].n) != pend[i].h && gbad++ < 5) printf("FAIL draw check: ring %llu+%llu was overwritten before the GPU read it\n", (unsigned long long)pend[i].off, (unsigned long long)pend[i].n);
    }
    npend = 0;
}
static void wrap(void) { if (g_gpu) submit(); pos = 0; wraps++; gen++; if (!g_gpu) memset(ring, 0xEE, RING); }
static uint64_t copy(uint64_t a, uint64_t n) {             /* ring_raw */
    pos = (pos + 255) & ~255ull;
    if (pos + n > RING) { wrap(); if (in_copy_wrap_land) cpu_write(a + n / 2, 1, 99); }
    memcpy(ring + pos, guest + a, n);
    uint64_t o = pos; pos += n; return o;
}
static uint64_t copyh(const uint8_t *p, uint64_t n) {     /* ring_raw of host bytes (c2) */
    pos = (pos + 255) & ~255ull;
    if (pos + n > RING) wrap();
    memcpy(ring + pos, p, n);
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

/* ---------------------------------------------------------------- second part: checked when the GPU reads (review F1) */
static struct { uint64_t a, n, off, gen; } ac[1024];
static uint64_t rput(uint64_t a, uint64_t n, int keep) {   /* ring_put: the batch's range cache, then a kept copy or a copy */
    unsigned i = (unsigned)(((a * 0x9E3779B97F4A7C15ull) ^ n) >> 54);
    if (ac[i].gen == gen && ac[i].a == a && ac[i].n == n) return ac[i].off;
    uint64_t o = keep ? sk_put(a, n) : copy(a, n);
    ac[i].a = a; ac[i].n = n; ac[i].off = o; ac[i].gen = gen;
    return o;
}
static uint8_t c2[2][896], c2last[2][896];
static uint64_t c2off[2];
static int64_t c2wrap[2] = {-1, -1};
static uint64_t c2put(int s) {                             /* c2_put */
    if (c2wrap[s] == wraps && !memcmp(c2last[s], c2[s], sizeof c2[s])) return c2off[s];
    uint64_t o = copyh(c2[s], sizeof c2[s]);
    memcpy(c2last[s], c2[s], sizeof c2[s]); c2off[s] = o; c2wrap[s] = wraps;
    return o;
}
static int gpu_read_test(void) {
    struct { uint64_t a, n; } key[40], small[60];
    for (int k = 0; k < 40; k++) { uint64_t n = 16384 + rn(8) * 4096 + rn(4000); key[k].n = n; key[k].a = rn(NPAGE * PAGE - n) & ~15ull; }
    for (int k = 0; k < 60; k++) { uint64_t n = 256 + rn(1800); small[k].n = n; small[k].a = rn(NPAGE * PAGE - n) & ~15ull; }
    for (uint64_t i = 0; i < sizeof guest; i++) guest[i] = (uint8_t)rn(256);      /* no repeating pattern: a range overwritten by another must differ */
    memset(written, 1, sizeof written); epoch++;
    g_gpu = 1; npend = 0; in_copy_wrap_land = 0;
    long draws = 0, redo = 0, wrapped = 0;
    int64_t wraps0 = wraps;
    long hits0 = (long)g_sk_hits;
    for (int batch = 0; batch < 600; batch++) {
        for (int d = 0, nd = 20 + (int)rn(40); d < nd; d++) {
            struct { uint64_t off, n, h; } rec[16];
            int nr = 0, nk = 1 + (int)rn(3), ks[3], sm[2];
            for (int i = 0; i < nk; i++) ks[i] = (int)rn(40);
            for (int i = 0; i < 2; i++) sm[i] = (int)rn(60);
            uint64_t fa, fn = 8192 + rn(57344);
            fa = rn(NPAGE * PAGE - fn) & ~15ull;
            if (rn(100) < 1) c2[0][rn(896)]++;                 /* the vertex stage's block nearly never changes */
            if (rn(100) < 30) c2[1][rn(896)]++;
            int64_t wd = wraps;
            for (int tries = 0; ; tries++) {                   /* draw_prepare step 2 */
                int64_t w0 = wraps;
                nr = 0;
                for (int s = 0; s < 2; s++) { rec[nr].off = c2put(s); rec[nr].n = sizeof c2[s]; rec[nr].h = h64(c2[s], sizeof c2[s]); nr++; }
                for (int i = 0; i < 2; i++) { rec[nr].off = rput(small[sm[i]].a, small[sm[i]].n, 0); rec[nr].n = small[sm[i]].n; rec[nr].h = h64(guest + small[sm[i]].a, small[sm[i]].n); nr++; }
                rec[nr].off = copy(fa, fn); rec[nr].n = fn; rec[nr].h = h64(guest + fa, fn); nr++;      /* a range that changes every draw */
                for (int i = 0; i < nk; i++) { rec[nr].off = rput(key[ks[i]].a, key[ks[i]].n, 1); rec[nr].n = key[ks[i]].n; rec[nr].h = h64(guest + key[ks[i]].a, key[ks[i]].n); nr++; }
                if (!F1 || wraps == w0) break;
                redo++;
                if (tries == 2) { printf("FAIL: a draw does not fit the ring in one wrap\n"); return 1; }
            }
            wrapped += wraps != wd;
            for (int i = 0; i < nr; i++) { pend[npend].off = rec[i].off; pend[npend].n = rec[i].n; pend[npend].h = rec[i].h; npend++; }
            draws++;
        }
        submit(); gen++; g_sk_batch++;                         /* batch end: the command buffer runs, the range cache is emptied */
        for (int w = 0, nw = (int)rn(4); w < nw; w++) { int k = (int)rn(40); cpu_write(key[k].a + rn(key[k].n), 1 + rn(64), batch + w); }      /* the game, between batches */
        for (int w = 0, nw = (int)rn(4); w < nw; w++) cpu_write(rn(sizeof guest - 64), 1 + rn(64), batch);
        if (rn(20) == 0) { uint64_t p = rn(NPAGE - 8); decommit(p, 1 + rn(4)); }
    }
    long hits = (long)g_sk_hits - hits0;
    printf("GPU-read check (F1=%d): %ld draws, %lld ring wraps, %ld draws with a wrap in them, %ld collected again, %ld kept copies bound again, %ld ranges checked when read, %ld overwritten\n",
           F1, draws, (long long)(wraps - wraps0), wrapped, redo, hits, gchecks, gbad);
    if (wraps - wraps0 < 10 || wrapped < 10) { printf("FAIL: too few wraps in draws: the test does not exercise F1\n"); return 1; }
    if (hits < draws / 4) { printf("FAIL: almost nothing was kept: the test does not exercise the table\n"); return 1; }
    return gbad != 0;
}

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
    int g = gpu_read_test();
    return bad != 0 || g != 0;
}

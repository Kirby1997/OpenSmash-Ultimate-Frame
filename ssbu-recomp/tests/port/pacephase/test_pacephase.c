/* tests/port/pacephase - the phase hold's arithmetic (runtime/pace_phase.h), no clock, no GPU, no game.
 * The 95th percentile of the last 256 samples (nearest rank), the window sliding, negative samples, the margin, the cap
 * at one tick, the fallback to the old hold before 256 samples, and the smallest slack worth a wake. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pace_phase.h"

static int g_checks, g_fail;
#define CHECKF(c, ...) do { g_checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, #c); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); g_fail++; } } while (0)
#define SECTION(s) printf("  - %s\n", s)

#define PER 166666LL            /* one tick of a 10 MHz clock at 60 Hz (QPC on Linux) */
#define MS 10000LL

static int cmp_ll(const void *a, const void *b) { long long x = *(const long long *)a, y = *(const long long *)b; return x < y ? -1 : x > y; }
/* the reference: sort the last 256 (clamped) and take rank ceil(0.95 * 256) = 244 */
static long long ref_p95(const long long *all, long long n) {
    long long a[PP_WIN];
    for (int i = 0; i < PP_WIN; i++) { long long x = all[n - PP_WIN + i]; a[i] = x < 0 ? 0 : x; }
    qsort(a, PP_WIN, sizeof a[0], cmp_ll);
    return a[243];
}

int main(void) {
    static PpWin w;
    static long long all[100000];
    printf("pace_phase.h:\n");

    SECTION("start-up: fewer than 256 samples = one tick (the old hold), slack 0");
    memset(&w, 0, sizeof w);
    CHECKF(pp_p95(&w) == -1, "%lld", pp_p95(&w));
    CHECKF(pp_offset(&w, MS, PER) == PER, "%lld", pp_offset(&w, MS, PER));
    for (int i = 0; i < 255; i++) pp_add(&w, 2 * MS);
    CHECKF(pp_p95(&w) == -1, "255 samples: %lld", pp_p95(&w));
    CHECKF(pp_offset(&w, MS, PER) == PER, "255 samples: %lld", pp_offset(&w, MS, PER));
    CHECKF(pp_slack(pp_offset(&w, MS, PER), PER, 5000) == 0, "slack before the window is full");
    pp_add(&w, 2 * MS);
    CHECKF(pp_p95(&w) == 2 * MS, "256 equal samples: %lld", pp_p95(&w));
    CHECKF(pp_offset(&w, MS, PER) == 3 * MS, "p95 + 1 ms margin: %lld", pp_offset(&w, MS, PER));
    CHECKF(pp_slack(3 * MS, PER, 5000) == PER - 3 * MS, "slack %lld", pp_slack(3 * MS, PER, 5000));

    SECTION("nearest rank: 0..255 gives 243; 12 samples above the percentile do not move it, the 13th does");
    memset(&w, 0, sizeof w);
    for (int i = 0; i < 256; i++) pp_add(&w, i);
    CHECKF(pp_p95(&w) == 243, "%lld", pp_p95(&w));
    memset(&w, 0, sizeof w);
    for (int i = 0; i < 244; i++) pp_add(&w, 10 * MS);
    for (int i = 0; i < 12; i++) pp_add(&w, 40 * MS);
    CHECKF(pp_p95(&w) == 10 * MS, "12 spikes: %lld", pp_p95(&w));
    memset(&w, 0, sizeof w);
    for (int i = 0; i < 243; i++) pp_add(&w, 10 * MS);
    for (int i = 0; i < 13; i++) pp_add(&w, 40 * MS);
    CHECKF(pp_p95(&w) == 40 * MS, "13 spikes: %lld", pp_p95(&w));
    CHECKF(pp_offset(&w, MS, PER) == PER, "capped at one tick: %lld", pp_offset(&w, MS, PER));
    CHECKF(pp_slack(PER, PER, 5000) == 0, "capped: slack 0 (the old hold)");

    SECTION("the cap: p95 + margin just under, at and over one tick");
    memset(&w, 0, sizeof w);
    for (int i = 0; i < 256; i++) pp_add(&w, PER - MS - 1);
    CHECKF(pp_offset(&w, MS, PER) == PER - 1, "%lld", pp_offset(&w, MS, PER));
    CHECKF(pp_slack(PER - 1, PER, 5000) == 0, "1 tick of slack is under the smallest worth a wake");
    memset(&w, 0, sizeof w);
    for (int i = 0; i < 256; i++) pp_add(&w, PER - MS);
    CHECKF(pp_offset(&w, MS, PER) == PER, "%lld", pp_offset(&w, MS, PER));
    memset(&w, 0, sizeof w);
    for (int i = 0; i < 256; i++) pp_add(&w, 3 * PER);
    CHECKF(pp_offset(&w, MS, PER) == PER, "%lld", pp_offset(&w, MS, PER));

    SECTION("negative samples (a frame queued ahead) count as 0");
    memset(&w, 0, sizeof w);
    for (int i = 0; i < 256; i++) pp_add(&w, -5 * MS);
    CHECKF(pp_p95(&w) == 0, "%lld", pp_p95(&w));
    CHECKF(pp_offset(&w, MS, PER) == MS, "only the margin: %lld", pp_offset(&w, MS, PER));

    SECTION("the window slides: only the last 256 count (100000 pseudo-random samples against a sort)");
    memset(&w, 0, sizeof w);
    unsigned long long x = 0x9e3779b97f4a7c15ull;
    int bad = 0;
    for (long long i = 0; i < 100000; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        long long s = (long long)(x % (unsigned long long)(20 * MS)) - 2 * MS;      /* -2 .. 18 ms */
        if ((i / 5000) % 2) s += 6 * MS;                                            /* a heavier scene every other 5000 */
        all[i] = s;
        pp_add(&w, s);
        if (i + 1 >= PP_WIN && (i % 37 == 0 || i % 5000 < 300)) { long long got = pp_p95(&w), want = ref_p95(all, i + 1); if (got != want && bad++ < 5) fprintf(stderr, "  sample %lld: p95 %lld, sorted %lld\n", i, got, want); }
    }
    CHECKF(!bad, "%d windows differ from the sorted reference", bad);
    {   /* after the switch to the heavy scene the percentile follows within 13 samples (5 % of 256) */
        memset(&w, 0, sizeof w);
        for (int i = 0; i < 256; i++) pp_add(&w, 8 * MS);
        int k = 0;
        while (pp_p95(&w) < 14 * MS && k < 256) { pp_add(&w, 14 * MS); k++; }
        CHECKF(k == 13, "the heavy scene took %d samples to reach the percentile", k);
    }

    SECTION("slack: period - P, 0 below the smallest worth a wake");
    CHECKF(pp_slack(12 * MS, PER, 5000) == PER - 12 * MS, "%lld", pp_slack(12 * MS, PER, 5000));
    CHECKF(pp_slack(PER - 4999, PER, 5000) == 0, "%lld", pp_slack(PER - 4999, PER, 5000));
    CHECKF(pp_slack(PER - 5000, PER, 5000) == 5000, "%lld", pp_slack(PER - 5000, PER, 5000));

    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

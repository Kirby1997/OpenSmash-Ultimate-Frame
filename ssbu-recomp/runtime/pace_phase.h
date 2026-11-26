/* pace_phase.h - display.c's phase hold (SSBU_PACE_GPU_PHASE=1, tests/port/pacephase): where after a tick of the refresh
 * clock a finished frame is presented.
 *
 * The hold of session 14 keeps every finished frame on the render thread until the next tick of our 60 Hz clock. On a
 * PC with no vblank source (Linux, the Frame: gamescope into SteamVR) that tick is not tied to the display, so the hold
 * only keeps the presents evenly spaced; it costs ~8 ms of the frame's 15 ms from pad poll to vkQueuePresentKHR. The
 * phase hold keeps the even spacing at a fixed offset P after the tick the game was released at instead:
 *     present at max(ready, tick + P),  P = 95th percentile of (tick -> GPU done) over the last 256 held frames + margin,
 * at most one tick (P = one tick is the old hold). The arithmetic is here, without the clock, the threads or Vulkan.
 *
 * A sample is "GPU done - the tick before the one the frame was held for", in the caller's clock ticks, taken only for
 * frames that were held (on time when the game queued them). A negative one (a frame queued more than a tick ahead)
 * counts as 0. Until 256 samples are in, the offset is one tick: the old hold. */
#ifndef PACE_PHASE_H
#define PACE_PHASE_H

#define PP_WIN 256

typedef struct {
    long long v[PP_WIN];        /* the last PP_WIN samples (a ring) */
    long long n;                /* samples added in all */
} PpWin;

static inline void pp_add(PpWin *w, long long s) {
    w->v[w->n % PP_WIN] = s < 0 ? 0 : s;
    w->n++;
}
/* the nearest-rank 95th percentile of the window: the 244th smallest of 256 (12 are above it); -1 until it is full */
static inline long long pp_p95(const PpWin *w) {
    long long a[PP_WIN];
    if (w->n < PP_WIN) return -1;
    for (int i = 0; i < PP_WIN; i++) {                  /* insertion sort: 256 values, run every few frames off the render thread */
        long long x = w->v[i];
        int k = i;
        while (k > 0 && a[k - 1] > x) { a[k] = a[k - 1]; k--; }
        a[k] = x;
    }
    return a[(PP_WIN * 95 + 99) / 100 - 1];
}
/* P: the present's offset after the tick, p95 + margin, at most period; period until the window is full */
static inline long long pp_offset(const PpWin *w, long long margin, long long period) {
    long long p = pp_p95(w);
    if (p < 0) return period;
    p += margin;
    return p > period ? period : p;
}
/* how long before the NEXT tick the frame may be presented: period - P; 0 (= at the tick, the old hold) when that is less
 * than min_slack (not worth a second wake of the vsync thread so close to its tick) */
static inline long long pp_slack(long long p, long long period, long long min_slack) {
    long long s = period - p;
    return s < min_slack ? 0 : s;
}
#endif

/* vsync_wait.h - Linux: the wait of display.c's precise_wait_until without the busy spin (tests/port/vsyncwait).
 *
 * Before (2026-11, the Frame, attract-mode demo, perf at 199 Hz): the vsync thread slept to 0.5 ms before each tick and
 * spun on clock_gettime for the rest. With the default 50 us timer slack the sleep woke ~70 us after its deadline, so it
 * spun ~0.43 ms of every 16.7 ms tick on a big core: __kernel_clock_gettime was 6.1 % of all the game's samples.
 *
 * Now it sleeps to SSBU_VSYNC_SPIN_US (default 200) before the deadline with clock_nanosleep(TIMER_ABSTIME,
 * CLOCK_MONOTONIC), so there is no drift between reading the clock and sleeping, and spins only that last stretch. The
 * vsync thread's own timer slack is 1 ns (PR_SET_TIMERSLACK, SSBU_VSYNC_SLACK_NS; 0 leaves the system's 50 us).
 * Measured on the Frame (Linux 7.2, 900 sleeps of 16.7 ms each), how late clock_nanosleep woke:
 *                          p50      p90      p99      past 200 us   past 400 us   past 600 us
 *   slack 50 us, idle      71 us    97 us   419 us    2.9 %         1.0 %         0 %
 *   slack 50 us, 4 busy    61 us    66 us   386 us    1.7 %         0.3 %         0 %
 *   slack  1 ns, idle      27 us    42 us   485 us    1.8 %         1.2 %         0.1 %
 *   slack  1 ns, 4 busy    12 us    19 us   262 us    1.1 %         0.1 %         0 %
 * The old 0.5 ms margin was passed by under 1 % of its wakes. With 200 us, 1-2 % of ticks come a little late (that
 * wake's lateness less 200 us: under 0.5 ms). The spin falls from ~430 to ~180 us a tick. SSBU_VSYNC_SPIN_US=500 gives
 * the old margin back. A late tick is counted in VwStats.late, and the vsync thread prints the counts once a minute
 * ("display: vsync wait").
 *
 * Only the vsync thread and display.c's lat_release (latency = low / lowest) call it. Nothing here touches another
 * thread's hot path: no locks, no fences; the statistics are the caller's own (NULL: none). */
#ifndef VSYNC_WAIT_H
#define VSYNC_WAIT_H
#include <errno.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    long long sleeps;           /* waits that slept */
    long long late;             /* ... and woke after the target itself (the spin budget was too short for that wake) */
    long long late_max_ns;      /* the most such a wake was late */
    long long spin_ns;          /* time spun in all, from the wake (or the call) to the target */
    long long waits;            /* every call, also those that only spun or found the target passed */
} VwStats;

#define VW_SPIN_US_DEFAULT 200
#define VW_SPIN_US_MAX 5000

/* SSBU_VSYNC_SPIN_US: the stretch spun before the target, microseconds (default 200, 0..5000; 0 = sleep to the target,
 * no spin). Something that is not a number gives the default. */
static inline long long vw_spin_ns_from(const char *e) {
    if (!e || !*e) return VW_SPIN_US_DEFAULT * 1000LL;
    char *end;
    long v = strtol(e, &end, 10);
    if (end == e || *end) return VW_SPIN_US_DEFAULT * 1000LL;
    if (v < 0) v = 0;
    if (v > VW_SPIN_US_MAX) v = VW_SPIN_US_MAX;
    return (long long)v * 1000LL;
}
/* SSBU_VSYNC_SLACK_NS: the vsync thread's timer slack, ns (default 1; 0 = leave the system's, 50 us). */
static inline long vw_slack_ns_from(const char *e) {
    if (!e || !*e) return 1;
    char *end;
    long v = strtol(e, &end, 10);
    if (end == e || *end || v < 0) return 1;
    return v > 1000000 ? 1000000 : v;
}
/* a QueryPerformanceCounter value (ticks at freq per second) in ns, without overflow (qpc * 1e9 would pass 2^63 after ~10 days at 10 MHz) */
static inline long long vw_qpc_to_ns(long long qpc, long long freq) {
    return qpc / freq * 1000000000LL + qpc % freq * 1000000000LL / freq;
}
/* the absolute time to sleep to: spin_ns before the target, or 0 when the target is that close already (spin only) */
static inline long long vw_sleep_deadline(long long target_ns, long long now_ns, long long spin_ns) {
    return target_ns - now_ns > spin_ns ? target_ns - spin_ns : 0;
}
static inline long long vw_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static inline void vw_relax(void) {
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}
/* Returns at the target (CLOCK_MONOTONIC, ns) or just after it, never before. */
static inline void vw_wait_until_ns(long long target_ns, long long spin_ns, VwStats *st) {
    long long now = vw_now_ns(), wake = vw_sleep_deadline(target_ns, now, spin_ns);
    if (st) st->waits++;
    if (wake) {
        struct timespec ts = {(time_t)(wake / 1000000000LL), (long)(wake % 1000000000LL)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}      /* absolute: a signal changes nothing */
        now = vw_now_ns();
        if (st) {
            st->sleeps++;
            if (now > target_ns) { st->late++; if (now - target_ns > st->late_max_ns) st->late_max_ns = now - target_ns; }
        }
    }
    long long from = now;
    while (now < target_ns) { vw_relax(); now = vw_now_ns(); }
    if (st && target_ns > from) st->spin_ns += now - from;
}
#endif

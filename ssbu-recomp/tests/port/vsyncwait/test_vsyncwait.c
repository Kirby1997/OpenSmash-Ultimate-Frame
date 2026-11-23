/* tests/port/vsyncwait - the vsync thread's wait (runtime/vsync_wait.h), no GPU, no game.
 * The arithmetic (environment knobs, QPC -> ns, the sleep deadline), then real 60 Hz waits on this machine's clock:
 * never before the target, the spin within its budget, the CPU time a small part of the wall time (no busy wait), and
 * signals arriving during the sleep (EINTR) change nothing. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/time.h>
#include "vsync_wait.h"

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECKF(c, ...) do { g_checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, #c); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); g_fail++; } } while (0)
#define SECTION(s) printf("  - %s\n", s)

static double cpu_s(void) {
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
    return (double)u.ru_utime.tv_sec + (double)u.ru_utime.tv_usec / 1e6 + (double)u.ru_stime.tv_sec + (double)u.ru_stime.tv_usec / 1e6;
}
static int cmp_ll(const void *a, const void *b) { long long x = *(const long long *)a, y = *(const long long *)b; return x < y ? -1 : x > y; }
static volatile long g_alarms;
static void on_alarm(int s) { (void)s; g_alarms++; }

/* ticks waits of a 60 Hz clock with this spin budget; returns the CPU share of the wall time */
static double run_ticks(int ticks, long long spin, const char *what) {
    VwStats st; memset(&st, 0, sizeof st);
    long long *late = malloc(sizeof *late * (size_t)ticks), early = 0, late_sum = 0, t0 = vw_now_ns();
    double c0 = cpu_s();
    for (int i = 0; i < ticks; i++) {
        long long target = t0 + (long long)(i + 1) * 16666667LL;
        vw_wait_until_ns(target, spin, &st);
        long long now = vw_now_ns();
        late[i] = now - target;
        late_sum += late[i];
        if (now < target) early++;
    }
    double wall = (double)(vw_now_ns() - t0) / 1e9, cpu = cpu_s() - c0;
    qsort(late, (size_t)ticks, sizeof *late, cmp_ll);
    printf("    %s: budget %lld us: %lld of %lld sleeps woke after the tick (latest %.0f us); return after the tick p50 %.1f us, p99 %.1f us; spin %.0f us a tick; CPU %.2f %% of %.2f s\n",
           what, spin / 1000, st.late, st.sleeps, (double)st.late_max_ns / 1e3, (double)late[ticks / 2] / 1e3, (double)late[ticks * 99 / 100] / 1e3,
           (double)st.spin_ns / 1e3 / ticks, 100.0 * cpu / wall, wall);
    CHECKF(early == 0, "%s: %lld waits returned before their target", what, early);
    CHECKF(st.waits == ticks && st.sleeps == ticks, "%s: waits %lld sleeps %lld of %d (each had ~16 ms left)", what, st.waits, st.sleeps, ticks);
    CHECK(st.late <= st.sleeps);
    /* the spin starts at the wake, which is never before target - budget, and ends at the return: less the time past the
     * target (a busy machine can take the CPU away mid-spin), no more than the budget (+ clock reads) */
    CHECKF((double)(st.spin_ns - late_sum) / ticks <= (double)spin + 30000.0, "%s: spin %.0f us a tick (%.0f us of it past the target) over the %lld us budget",
           what, (double)st.spin_ns / 1e3 / ticks, (double)late_sum / 1e3 / ticks, spin / 1000);
    free(late);
    return cpu / wall;
}

int main(void) {
    printf("vsyncwait: runtime/vsync_wait.h\n");
    SECTION("SSBU_VSYNC_SPIN_US and SSBU_VSYNC_SLACK_NS");
    CHECK(vw_spin_ns_from(NULL) == 200000 && vw_spin_ns_from("") == 200000);
    CHECK(vw_spin_ns_from("0") == 0 && vw_spin_ns_from("500") == 500000 && vw_spin_ns_from("-3") == 0);
    CHECK(vw_spin_ns_from("99999") == VW_SPIN_US_MAX * 1000LL);
    CHECK(vw_spin_ns_from("abc") == 200000 && vw_spin_ns_from("12x") == 200000);
    CHECK(vw_slack_ns_from(NULL) == 1 && vw_slack_ns_from("0") == 0 && vw_slack_ns_from("50000") == 50000);
    CHECK(vw_slack_ns_from("x") == 1 && vw_slack_ns_from("-1") == 1 && vw_slack_ns_from("999999999") == 1000000);

    SECTION("QueryPerformanceCounter ticks -> ns, no overflow");
    CHECK(vw_qpc_to_ns(12345678, 10000000) == 1234567800LL);                 /* plat_posix.c: 10 MHz, 100 ns a tick */
    CHECK(vw_qpc_to_ns(0, 10000000) == 0);
    long long big = 900000000000000000LL;                                      /* ~2,850 years at 10 MHz: qpc * 1e9 would overflow */
    CHECK(vw_qpc_to_ns(big, 10000000) == big * 100);
    for (long long q = 1; q < 4000000000000000LL; q = q * 7 + 3) {           /* a 3 GHz counter (x86 TSC-like) against long double */
        long long f = 3000000000LL, want = (long long)((long double)q * 1e9L / (long double)f);
        long long got = vw_qpc_to_ns(q, f);
        CHECKF(got - want <= 1 && want - got <= 1, "q %lld: %lld vs %lld", q, got, want);
    }

    SECTION("the sleep deadline");
    CHECK(vw_sleep_deadline(1000000, 0, 200000) == 800000);
    CHECK(vw_sleep_deadline(1000000, 800000, 200000) == 0);                   /* exactly the budget left: spin only */
    CHECK(vw_sleep_deadline(1000000, 900000, 200000) == 0);
    CHECK(vw_sleep_deadline(1000000, 2000000, 200000) == 0);                  /* passed */
    CHECK(vw_sleep_deadline(1000000, 0, 0) == 1000000);                       /* budget 0: sleep to the target itself */
    {   VwStats st; memset(&st, 0, sizeof st);
        long long t = vw_now_ns() - 5000000;
        vw_wait_until_ns(t, 200000, &st);                                     /* a target already passed returns at once */
        CHECK(st.waits == 1 && st.sleeps == 0 && st.spin_ns == 0);
    }

    int ticks = getenv("VW_TICKS") ? atoi(getenv("VW_TICKS")) : 240;
    if (ticks < 10) ticks = 10;
    SECTION("real 60 Hz waits on this machine's clock");
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);                                   /* as the vsync thread */
    double share = run_ticks(ticks, vw_spin_ns_from(NULL), "default");
    CHECKF(share < 0.10, "the waits took %.1f %% of a CPU: a busy wait", 100.0 * share);
    run_ticks(ticks / 2, 0, "no spin");
    run_ticks(ticks / 2, 500000, "old margin");

    SECTION("signals during the sleep (EINTR) end no wait early");
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;                                                 /* no SA_RESTART: clock_nanosleep returns EINTR */
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = {{0, 3000}, {0, 3000}};                             /* every 3 ms */
    setitimer(ITIMER_REAL, &it, NULL);
    run_ticks(ticks / 4, vw_spin_ns_from(NULL), "SIGALRM every 3 ms");
    memset(&it, 0, sizeof it);
    setitimer(ITIMER_REAL, &it, NULL);
    CHECKF(g_alarms > ticks, "only %ld signals arrived", g_alarms);

    printf("vsyncwait      %s (%d checks, %d failed)\n", g_fail ? "FAILED" : "PASS", g_checks, g_fail);
    return g_fail != 0;
}

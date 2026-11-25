/* test_cv_stress.c - plat_posix.c's CONDITION_VARIABLE under load: the waiter count that lets a Wake with nobody waiting
 * skip the futex call must never lose a wake-up and must come back to 0. Two parts run at the same time for
 * CV_STRESS_SECS seconds (default 3; 600 for the long run under ASan / TSan):
 *   queue     4 producers and 4 consumers on one bounded queue (a CRITICAL_SECTION, a CV for "not empty" and one for "not
 *             full"); every wait takes a timeout of 0 ms, 1 ms or 1 s at random; every push and pop wakes with Wake or
 *             WakeAll, with the lock held or after leaving it. Every item arrives exactly once, in each producer's order.
 *   pingpong  4 pairs of threads passing a turn back and forth, each pair with its own CV (two pairs under a
 *             CRITICAL_SECTION, two under an SRW lock), the same random timeouts and kinds of Wake. A wait that ends
 *             more than 50 ms after the partner gave the turn is a lost wake-up (a 1 s wait that missed its Wake ends at
 *             its timeout; the 0 / 1 ms waits hide one, so half the waits are 1 s ones).
 * For every wait with a timeout: it ends no later than the timeout + 50 ms, a FALSE return is ERROR_TIMEOUT and does not
 * come early. At the end every CV's waiter count is 0 (a wait that did not count itself down on some way out).
 * Build / run: tests/port/threads/run.sh. With a sanitizer, from ssbu-recomp/ (OUT a scratch folder):
 *   F="-O1 -g -w -D_GNU_SOURCE -fno-omit-frame-pointer -fsanitize=thread -include runtime/plat/plat_pre.h -I runtime/plat -I runtime -I tests/port/threads/stub"
 *   cc $F -c runtime/plat_posix.c -o $OUT/p.o && cc $F tests/port/threads/test_cv_stress.c $OUT/p.o $OUT/libstubs.o -lm -lpthread -ldl -o $OUT/t
 *   (libstubs.o as run.sh makes it), then CV_STRESS_SECS=600 $OUT/t */
#include "tp.h"
#include <stdint.h>
#include <string.h>

#define NP 4                                      /* producers = consumers = pairs */
#define QCAP 8
#define LONG_MS 1000
#define SLACK_NS 50000000LL                       /* 50 ms */

static volatile int g_stop;
static int64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }
static uint32_t rnd(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static DWORD pick_ms(uint32_t *s) { uint32_t r = rnd(s) & 3; return r == 0 ? 0 : r == 1 ? 1 : LONG_MS; }   /* 1/4 0 ms, 1/4 1 ms, 1/2 1 s */

static volatile LONG64 g_waits, g_timeouts, g_over, g_early, g_badcode, g_lost, g_late_items;
static volatile LONG64 g_max_over_ns = -1000000000000LL, g_max_age_ns, g_max_lost_ns;
static void max64(volatile LONG64 *m, LONG64 v) { LONG64 o = LOAD(*m); while (v > o && !__atomic_compare_exchange_n(m, &o, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {} }

/* one wait, checked: kind 0 a CRITICAL_SECTION, 1 an SRW lock (exclusive) */
static BOOL sleep_checked(CONDITION_VARIABLE *cv, void *lock, int kind, DWORD ms) {
    SetLastError(0);
    int64_t t0 = now_ns();
    BOOL r = kind ? SleepConditionVariableSRW(cv, (SRWLOCK *)lock, ms, 0) : SleepConditionVariableCS(cv, (CRITICAL_SECTION *)lock, ms);
    int64_t dt = now_ns() - t0;
    TP_BUMP(g_waits);
    int64_t over = dt - (int64_t)ms * 1000000LL;
    max64(&g_max_over_ns, over);
    if (over > SLACK_NS) {
        if (TP_BUMP(g_over) <= 5) fprintf(stderr, "wait of %u ms took %.3f ms (returned %d)\n", (unsigned)ms, (double)dt / 1e6, (int)r);
    }
    if (!r) {
        TP_BUMP(g_timeouts);
        if (GetLastError() != 1460 && TP_BUMP(g_badcode) <= 5) fprintf(stderr, "a timed-out wait set error %u, not 1460\n", (unsigned)GetLastError());
        if (over < -200000 && TP_BUMP(g_early) <= 5) fprintf(stderr, "wait of %u ms timed out after %.3f ms\n", (unsigned)ms, (double)dt / 1e6);
    }
    return r;
}
static void wake(CONDITION_VARIABLE *cv, int all) { if (all) WakeAllConditionVariable(cv); else WakeConditionVariable(cv); }

/* ---------------------------------------------------------------- queue: 4 producers, 4 consumers */
typedef struct { int p; uint32_t seq; int64_t t; } Item;
static CRITICAL_SECTION q_cs;
static CONDITION_VARIABLE q_not_empty = CONDITION_VARIABLE_INIT, q_not_full = CONDITION_VARIABLE_INIT;
static Item q_buf[QCAP]; static int q_head, q_len, q_done;
static uint64_t q_made[NP], q_got[NP], q_sum_made[NP], q_sum_got[NP];      /* q_got / q_sum_got under q_cs */
static volatile LONG64 q_order_bad;

static DWORD WINAPI producer(LPVOID arg) {
    int p = (int)(intptr_t)arg;
    uint32_t s = 0x9e3779b9u * (uint32_t)(p + 1);
    uint32_t seq = 0;
    while (!LOAD(g_stop)) {
        EnterCriticalSection(&q_cs);
        while (q_len == QCAP && !LOAD(g_stop)) sleep_checked(&q_not_full, &q_cs, 0, pick_ms(&s));
        if (q_len == QCAP) { LeaveCriticalSection(&q_cs); break; }
        Item it = {p, ++seq, now_ns()};
        q_buf[(q_head + q_len) % QCAP] = it; q_len++;
        q_made[p]++; q_sum_made[p] += seq;
        uint32_t m = rnd(&s) & 3;
        if (m < 2) wake(&q_not_empty, m & 1);                 /* with the lock held */
        LeaveCriticalSection(&q_cs);
        if (m >= 2) wake(&q_not_empty, m & 1);                /* after leaving it */
        uint32_t z = rnd(&s) & 63;                            /* now and then a pause, so the consumers go to sleep */
        if (z == 0) Sleep(1 + (rnd(&s) & 1));
        else if (z < 4) Sleep(0);
    }
    return 0;
}
static DWORD WINAPI consumer(LPVOID arg) {
    int me = (int)(intptr_t)arg;
    uint32_t s = 0x85ebca6bu * (uint32_t)(me + 7);
    uint32_t last[NP] = {0};
    EnterCriticalSection(&q_cs);
    for (;;) {
        while (!q_len && !q_done) sleep_checked(&q_not_empty, &q_cs, 0, pick_ms(&s));
        if (!q_len) break;                                    /* done and drained */
        Item it = q_buf[q_head]; q_head = (q_head + 1) % QCAP; q_len--;
        int64_t age = now_ns() - it.t;
        max64(&g_max_age_ns, age);
        if (age > SLACK_NS && TP_BUMP(g_late_items) <= 5) fprintf(stderr, "item %d:%u waited %.3f ms in the queue\n", it.p, it.seq, (double)age / 1e6);
        if (it.seq <= last[it.p]) TP_BUMP(q_order_bad);
        last[it.p] = it.seq;
        q_got[it.p]++; q_sum_got[it.p] += it.seq;
        uint32_t m = rnd(&s) & 3;
        if (m < 2) wake(&q_not_full, m & 1);
        else { LeaveCriticalSection(&q_cs); wake(&q_not_full, m & 1); EnterCriticalSection(&q_cs); }
    }
    LeaveCriticalSection(&q_cs);
    return 0;
}

/* ---------------------------------------------------------------- ping-pong: 4 pairs, one CV each */
typedef struct {
    CONDITION_VARIABLE cv;
    CRITICAL_SECTION cs; SRWLOCK srw; int kind;
    int turn, quit;
    int64_t t_turn;                                           /* when the turn was given (under the lock) */
    uint64_t rounds;
} Pair;
static Pair g_pair[NP];
typedef struct { Pair *pr; int side; } Side;
static Side g_side[NP][2];
static void pr_lock(Pair *pr) { if (pr->kind) AcquireSRWLockExclusive(&pr->srw); else EnterCriticalSection(&pr->cs); }
static void pr_unlock(Pair *pr) { if (pr->kind) ReleaseSRWLockExclusive(&pr->srw); else LeaveCriticalSection(&pr->cs); }
static DWORD WINAPI pinger(LPVOID arg) {
    Side *sd = arg; Pair *pr = sd->pr; int me = sd->side;
    uint32_t s = 0xc2b2ae35u * (uint32_t)((pr - g_pair) * 2 + me + 3);
    void *lk = pr->kind ? (void *)&pr->srw : (void *)&pr->cs;
    pr_lock(pr);
    for (;;) {
        while (pr->turn != me && !pr->quit) {
            DWORD ms = pick_ms(&s);
            BOOL r = sleep_checked(&pr->cv, lk, pr->kind, ms);
            if (pr->turn == me || pr->quit) {
                int64_t late = now_ns() - pr->t_turn;
                if (late > SLACK_NS) {
                    max64(&g_max_lost_ns, late);
                    if (TP_BUMP(g_lost) <= 5) fprintf(stderr, "pair %d side %d: the turn came %.3f ms before the wait (%u ms, returned %d) ended\n",
                                                      (int)(pr - g_pair), me, (double)late / 1e6, (unsigned)ms, (int)r);
                }
            }
        }
        if (pr->quit) break;
        pr->rounds++;
        int stop = LOAD(g_stop);
        if (stop) pr->quit = 1;
        pr->turn = !me; pr->t_turn = now_ns();
        uint32_t m = rnd(&s) & 3;
        if (m < 2) { wake(&pr->cv, m & 1); pr_unlock(pr); }
        else { pr_unlock(pr); wake(&pr->cv, m & 1); }
        if (stop) return 0;
        if ((rnd(&s) & 255) == 0) Sleep(1);                   /* the partner then sleeps in the futex for sure */
        pr_lock(pr);
    }
    pr_unlock(pr);
    return 0;
}

/* ---------------------------------------------------------------- the cost of a Wake with nobody waiting (information) */
static void wake_cost(void) {
    CONDITION_VARIABLE cv; InitializeConditionVariable(&cv);
    enum { N = 1000000 };
    int64_t t0 = now_ns();
    for (int i = 0; i < N; i++) WakeConditionVariable(&cv);
    int64_t t1 = now_ns();
    for (int i = 0; i < N; i++) WakeAllConditionVariable(&cv);
    int64_t t2 = now_ns();
    printf("  Wake with nobody waiting: %.1f ns, WakeAll %.1f ns\n", (double)(t1 - t0) / N, (double)(t2 - t1) / N);
    CHECK(cv.seq == 2u * N && cv.waiters == 0);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *e = getenv("CV_STRESS_SECS");
    int secs = e && atoi(e) > 0 ? atoi(e) : 3;
    SECTION("condition variables under load: 4 producers / 4 consumers on a queue and 4 ping-pong pairs, timeouts 0 / 1 / 1000 ms, Wake / WakeAll with and without the lock");
    printf("  %d s\n", secs);
    CHECK(sizeof(CONDITION_VARIABLE) == 8);
    wake_cost();
    InitializeCriticalSection(&q_cs);
    HANDLE th[4 * NP]; int n = 0;
    for (int i = 0; i < NP; i++) {
        Pair *pr = &g_pair[i];
        InitializeConditionVariable(&pr->cv);
        InitializeCriticalSection(&pr->cs); InitializeSRWLock(&pr->srw);
        pr->kind = i >= NP / 2;
        pr->t_turn = now_ns();
        for (int sd = 0; sd < 2; sd++) { g_side[i][sd].pr = pr; g_side[i][sd].side = sd; th[n++] = CreateThread(NULL, 0, pinger, &g_side[i][sd], 0, NULL); }
    }
    for (int i = 0; i < NP; i++) th[n++] = CreateThread(NULL, 0, consumer, (LPVOID)(intptr_t)i, 0, NULL);
    HANDLE prod[NP];
    for (int i = 0; i < NP; i++) prod[i] = th[n++] = CreateThread(NULL, 0, producer, (LPVOID)(intptr_t)i, 0, NULL);
    int64_t t_end = now_ns() + (int64_t)secs * 1000000000LL, t_say = now_ns() + 60000000000LL;
    while (now_ns() < t_end) {
        Sleep(100);
        if (now_ns() >= t_say) {
            t_say += 60000000000LL;
            printf("  ... %lld waits, %lld timeouts, %lld lost, %lld late items, max over %.3f ms\n", (long long)LOAD(g_waits), (long long)LOAD(g_timeouts),
                   (long long)LOAD(g_lost), (long long)LOAD(g_late_items), (double)LOAD(g_max_over_ns) / 1e6);
        }
    }
    __atomic_store_n(&g_stop, 1, __ATOMIC_SEQ_CST);
    CHECK(WaitForMultipleObjects(NP, prod, TRUE, 10000) == WAIT_OBJECT_0);
    EnterCriticalSection(&q_cs); q_done = 1; WakeAllConditionVariable(&q_not_empty); LeaveCriticalSection(&q_cs);
    for (int i = 0; i < n; i++) { CHECKF(WaitForSingleObject(th[i], 10000) == WAIT_OBJECT_0, "thread %d did not end", i); CloseHandle(th[i]); }

    uint64_t items = 0, rounds = 0;
    for (int p = 0; p < NP; p++) {
        CHECKF(q_got[p] == q_made[p] && q_sum_got[p] == q_sum_made[p], "producer %d: made %llu, got %llu", p, (unsigned long long)q_made[p], (unsigned long long)q_got[p]);
        items += q_got[p];
    }
    for (int i = 0; i < NP; i++) { rounds += g_pair[i].rounds; CHECKF(g_pair[i].rounds > 0, "pair %d", i); }
    printf("  queue: %llu items; ping-pong: %llu turns; %lld waits, %lld timed out; longest past its timeout %.3f ms; oldest item %.3f ms\n",
           (unsigned long long)items, (unsigned long long)rounds, (long long)g_waits, (long long)g_timeouts, (double)g_max_over_ns / 1e6, (double)g_max_age_ns / 1e6);
    CHECK(items > 0);
    CHECK(q_order_bad == 0);
    CHECKF(g_over == 0, "%lld waits ended more than 50 ms after their timeout", (long long)g_over);
    CHECKF(g_early == 0, "%lld waits timed out early", (long long)g_early);
    CHECKF(g_badcode == 0, "%lld timeouts without ERROR_TIMEOUT", (long long)g_badcode);
    CHECKF(g_lost == 0, "%lld lost wake-ups (ping-pong), the latest %.3f ms", (long long)g_lost, (double)g_max_lost_ns / 1e6);
    CHECKF(g_late_items == 0, "%lld items waited more than 50 ms (queue)", (long long)g_late_items);
    CHECKF(q_not_empty.waiters == 0 && q_not_full.waiters == 0, "queue CV waiters left: %u, %u", q_not_empty.waiters, q_not_full.waiters);
    for (int i = 0; i < NP; i++) CHECKF(g_pair[i].cv.waiters == 0, "pair %d: CV waiters left: %u", i, g_pair[i].cv.waiters);
    return tp_done("test_cv_stress");
}

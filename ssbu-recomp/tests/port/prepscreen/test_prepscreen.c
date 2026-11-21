/* tests/port/prepscreen - the first-run preparation screen's numbers and text (runtime/nvn_vk_prepscreen.h), no GPU.
 * Cases from the Frame's first boot (2026-11): a start that went on from an earlier run's stage records said "15 %, 10 s so
 * far, about 54 s left" and then took 36 minutes. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "nvn_vk_prepscreen.h"

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECKF(c, ...) do { g_checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, #c); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); g_fail++; } } while (0)
#define SECTION(s) printf("  - %s\n", s)

static PsIn in_of(int st, int store_pct, int nfiles, int nread, long long done, long long queued) {
    PsIn in; memset(&in, 0, sizeof in);
    in.st = st; in.store_pct = store_pct; in.nfiles = nfiles; in.nread = nread; in.done = done; in.queued = queued;
    return in;
}
/* every character the screen draws is one the dot font has (a space is a gap), the line fits the buffer and a 1080p screen */
static void check_drawable(const PsOut *o) {
    for (int i = 0; i < 4; i++) {
        size_t n = strlen(o->text[i]);
        CHECKF(n > 0 && n < sizeof o->text[i] - 1, "line %d length %zu", i, n);
        for (const char *s = o->text[i]; *s; s++) CHECKF(*s == ' ' || strchr(PS_FONT_CHARS, *s), "line %d \"%s\": '%c' is not in the font", i, o->text[i], *s);
        int H = 1080, W = 1920, dot = H / 180, d = i ? dot : dot * 2;      /* nvn_vk_draw.c overlay_draw */
        CHECKF((int)n * 6 * d <= W, "line %d \"%s\" is %d px wide at 1080p", i, o->text[i], (int)n * 6 * d);
        H = 720; W = 1280; dot = H / 180; d = i ? dot : dot * 2;
        CHECKF((int)n * 6 * d <= W, "line %d \"%s\" is %d px wide at 720p", i, o->text[i], (int)n * 6 * d);
    }
}

int main(void) {
    PsOut o;
    printf("prepscreen: runtime/nvn_vk_prepscreen.h\n");

    SECTION("the store being read: the first tenth, no time left yet");
    {   PsEta e; memset(&e, 0, sizeof e);
        PsIn in = in_of(5, 50, 0, 0, 0, 0);
        ps_compute(&in, &e, 3.0, &o);
        CHECK(fabsf(o.frac - 0.05f) < 1e-4f); CHECK(o.pct == 5); CHECK(o.eta == -1);
        CHECK(!strcmp(o.text[0], "PREPARING SHADERS 5 %"));
        CHECK(!strcmp(o.text[1], "READING THE GAME DATA"));
        CHECK(!strcmp(o.text[2], "TIME LEFT: ..."));
        CHECK(e.n == 0);                            /* no rate samples while the store is read */
        check_drawable(&o);
    }
    SECTION("the Frame's boot 2: a start that goes on from an earlier run (15 % at 10 s) is not '54 s left'");
    {   PsEta e; memset(&e, 0, sizeof e);
        /* 521 files, 125 read; jobs known 3554 -> total 3554 + 396 x 90 = 39194; 2363 done at the screen's start, ~10 a second */
        PsIn in = in_of(1, 100, 521, 125, 2363, 3554);
        ps_compute(&in, &e, 100.0, &o);
        CHECK(o.eta == -1);                         /* one sample: no rate yet */
        CHECK(!strcmp(o.text[2], "TIME LEFT: ..."));
        in.done = 2412; ps_compute(&in, &e, 105.0, &o);
        CHECK(o.eta == -1);                         /* the window spans 5 s: still under 10 */
        in.done = 2462; ps_compute(&in, &e, 110.0, &o);
        CHECK(o.total == 39194); CHECK(o.pct == 15);
        double want = (39194.0 - 2462.0) / ((2462.0 - 2363.0) / 10.0);
        CHECKF(fabs(o.eta - want) < 2.0, "eta %d, want %.0f", o.eta, want);
        CHECKF(o.eta > 3000, "eta %d s: the old formula said 54 s", o.eta);
        CHECKF(!strncmp(o.text[2], "ABOUT 1:", 8), "\"%s\"", o.text[2]);      /* over an hour: h:mm:ss */
        CHECK(!strcmp(o.text[1], "2462 / 39194"));
        check_drawable(&o);
    }
    SECTION("a steady 20 stages a second: the time left is the rest at that rate");
    {   PsEta e; memset(&e, 0, sizeof e);
        PsIn in = in_of(1, 100, 521, 521, 0, 42000);
        for (int t = 0; t <= 120; t++) { in.done = 20LL * t; ps_compute(&in, &e, (double)t, &o); }
        CHECKF(abs(o.eta - (42000 - 2400) / 20) <= 2, "eta %d", o.eta);
        CHECK(e.n == PS_SAMPLES);
        CHECKF(!strcmp(o.text[2], "ABOUT 33:00 LEFT"), "\"%s\"", o.text[2]);
    }
    SECTION("the window is the last minute: a rate that rises (the game's own boot work ends, the hold raises the workers) is followed");
    {   PsEta e; memset(&e, 0, sizeof e);
        PsIn in = in_of(1, 100, 100, 100, 0, 100000);
        long long d = 0;
        for (int t = 0; t <= 300; t++) { d += t < 150 ? 5 : 20; in.done = d; ps_compute(&in, &e, (double)t, &o); }
        double rate = ps_eta_rate(&e, 300.0, d);
        CHECKF(rate > 19.0 && rate < 21.0, "rate %.2f after a minute at 20/s", rate);
        CHECKF(abs(o.eta - (int)((100000 - d) / 20)) <= 60, "eta %d", o.eta);
    }
    SECTION("samples closer than 5 s are not kept; calls 10 times a second cost one sample per 5 s");
    {   PsEta e; memset(&e, 0, sizeof e);
        for (int i = 0; i <= 100; i++) ps_eta_note(&e, i * 0.1, i);
        CHECK(e.n == 3);                            /* t = 0, 5, 10 */
    }
    SECTION("the end: never 100 % before it is over; nothing done = no time left");
    {   PsEta e; memset(&e, 0, sizeof e);
        PsIn in = in_of(1, 100, 521, 521, 42382, 42382);
        ps_compute(&in, &e, 0.0, &o);
        CHECK(o.frac <= 0.999f + 1e-6f); CHECK(o.pct == 99);
        memset(&e, 0, sizeof e);
        in.done = 10;
        for (int t = 0; t <= 60; t += 5) ps_compute(&in, &e, (double)t, &o);
        CHECK(o.eta == -1);                         /* no progress in the window */
    }
    SECTION("a very slow rate: the time left is capped and still drawable");
    {   PsEta e; memset(&e, 0, sizeof e);
        PsIn in = in_of(1, 100, 1000, 0, 0, 1);
        for (int t = 0; t <= 20; t += 5) { in.done = t / 5; ps_compute(&in, &e, (double)t, &o); }
        CHECK(o.eta == 359999);
        check_drawable(&o);
    }
    SECTION("LOADING SHADERS (a prepared start, shader_load = start): no store tenth");
    {   PsEta e; memset(&e, 0, sizeof e);
        PsIn in = in_of(1, 0, 521, 521, 500, 1000);
        in.loadall = 1;
        ps_compute(&in, &e, 0.0, &o);
        CHECK(fabsf(o.frac - 0.5f) < 1e-4f);
        CHECK(!strcmp(o.text[0], "LOADING SHADERS 50 %"));
        CHECK(!strcmp(o.text[3], "NO SHADER LOADING BEFORE A FIGHT"));
        check_drawable(&o);
    }
    SECTION("the screen before the game's first frame: when it is shown");
    {   /* mode, have_prep, marked, limited, state, stopped, game_presented */
        CHECK(ps_early_wanted(0, 1, 0, 0, 0, 0, 0) == 1);      /* first run, store not started */
        CHECK(ps_early_wanted(0, 1, 0, 0, 5, 0, 0) == 1);      /* the store being read */
        CHECK(ps_early_wanted(0, 1, 0, 0, 1, 0, 0) == 1);      /* preparing */
        CHECK(ps_early_wanted(0, 1, 0, 0, 2, 0, 0) == 0);      /* finished */
        CHECK(ps_early_wanted(0, 1, 0, 0, 3, 0, 0) == 0);      /* no container source */
        CHECK(ps_early_wanted(1, 1, 0, 0, 1, 0, 0) == 0);      /* shader_prepare = background */
        CHECK(ps_early_wanted(2, 1, 0, 0, 1, 0, 0) == 0);      /* shader_prepare = off */
        CHECK(ps_early_wanted(0, 0, 0, 0, 1, 0, 0) == 0);      /* no preparation (no stage libraries) */
        CHECK(ps_early_wanted(0, 1, 1, 0, 1, 0, 0) == 0);      /* marker of an earlier run */
        CHECK(ps_early_wanted(0, 1, 0, 1, 1, 0, 0) == 0);      /* SSBU_VK_PREP_LIMIT test */
        CHECK(ps_early_wanted(0, 1, 0, 0, 1, 1, 0) == 0);      /* the game is closing */
        CHECK(ps_early_wanted(0, 1, 0, 0, 1, 0, 1) == 0);      /* the game presented: the hold owns the screen */
    }
    SECTION("the preparation's end: whole and the marker (review_firstrun.md MEDIUM 1)");
    {   /* a model of prep_main + the workers: 521 files, 100 jobs queued; a job left by job_prep after the stop still counts done */
        long long queued = 100, done = 0;
        int read = 521, todo = 521, stopped = 0;
        for (; done < queued; done++) ;             /* a normal finish */
        CHECK(ps_prep_whole(done, queued, read, todo, stopped) == 1);
        CHECK(ps_marker_due(ps_prep_whole(done, queued, read, todo, stopped), 0) == 1);      /* first run: marker written */
        CHECK(ps_marker_due(ps_prep_whole(done, queued, read, todo, stopped), 1) == 0);      /* marked start: the marker is there already */
        done = 0; stopped = 0;
        for (; done < 97; done++) ;                 /* the stop lands while the last 3 jobs run ... */
        stopped = 1;
        for (; done < queued; done++) ;             /* ... job_prep leaves them, pw_main counts them done anyway */
        CHECK(done == queued);
        CHECK(ps_prep_whole(done, queued, read, todo, stopped) == 0);      /* the race: done == queued, yet not whole */
        CHECK(ps_marker_due(ps_prep_whole(done, queued, read, todo, stopped), 0) == 0);      /* no marker: the next start prepares again */
        CHECK(ps_prep_whole(40, queued, 200, todo, 1) == 0);               /* a stop in the middle */
        CHECK(ps_prep_whole(40, queued, 200, todo, 0) == 0);               /* not stopped, not finished either */
        CHECK(ps_prep_whole(queued, queued, 520, todo, 0) == 0);           /* one container not read yet */
    }
    SECTION("a STALE store (a new data.arc): the marker goes, the screen before the first frame is shown (MEDIUM 2)");
    {   int marked = 1;                             /* prep_start found the marker of the old data */
        CHECK(ps_early_wanted(0, 1, marked, 0, 0, 0, 0) == 0);             /* what prep_start alone would have decided */
        marked = ps_marked_after_store_check(marked, 1);                   /* prep_main: the store is stale */
        CHECK(marked == 0);
        CHECK(ps_early_wanted(0, 1, marked, 0, 5, 0, 0) == 1);             /* the store being built again: the screen shows */
        CHECK(ps_marked_after_store_check(1, 0) == 1);                     /* the same data.arc: still prepared, no screen */
        CHECK(ps_early_wanted(0, 1, ps_marked_after_store_check(1, 0), 0, 1, 0, 0) == 0);
        CHECK(ps_marked_after_store_check(0, 0) == 0);
    }
    printf("%-14s %s (%d checks, %d failed)\n", "prepscreen", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

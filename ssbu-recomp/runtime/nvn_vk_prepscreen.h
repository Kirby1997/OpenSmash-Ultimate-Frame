/* nvn_vk_prepscreen.h - the numbers and the text of the first-run preparation screen (nvn_vk_draw.c prep_first_hold and
 * the screen before the game's first frame), kept apart from Vulkan so that tests/port/prepscreen can check them without a GPU.
 *
 * What the screen shows: "PREPARING SHADERS NN %", stages done / about how many, the time left, one line of note, the bar.
 * The fraction: the store (data.arc read into the container store) is the first tenth, then stage jobs done of the jobs known
 * so far plus about 90 for every container file not read yet.
 * The time left (2026-11, Frame first boot: "15 %, 10 s so far, about 54 s left" on a start that went on from an earlier run's
 * stage records, then 36 minutes): from the RATE of the last minute (samples 5 s apart), not from elapsed x (1 - f) / f,
 * which counts the work of an earlier run (and the store's tenth) as if it had been done in this run's seconds. */
#ifndef NVN_VK_PREPSCREEN_H
#define NVN_VK_PREPSCREEN_H
#include <stdio.h>
#include <string.h>

/* the characters the screen's 5x7 dot font has (nvn_vk_draw.c ovl_text, one glyph each, in this order); a space is a gap */
#define PS_FONT_CHARS "0123456789%/:.-ABCDEFGHIKLMNOPRSTUVWXY"
#define PS_PER_FILE 90                              /* about 90 variations a container file not read yet (measured: 42382 / 521 = 81) */
#define PS_SAMPLES 13                               /* the rate window: 12 steps of PS_STEP seconds = the last minute */
#define PS_STEP 5.0

typedef struct { double t[PS_SAMPLES]; long long d[PS_SAMPLES]; int n, head; } PsEta;      /* zero = empty */
typedef struct {
    int loadall;                                    /* the "LOADING SHADERS" screen of a prepared start (shader_load = start) */
    int st;                                         /* g_prep_state: 5 = the store is being built */
    int store_pct, nfiles, nread;
    long long done, queued;                         /* g_pq_done, g_pq_queued */
} PsIn;
typedef struct { float frac; int pct, eta; long long total; char text[4][48]; } PsOut;

/* a sample of (seconds, jobs done), kept when PS_STEP has passed since the newest one */
static inline void ps_eta_note(PsEta *e, double t, long long d) {
    if (e->n) {
        int newest = (e->head + PS_SAMPLES - 1) % PS_SAMPLES;
        if (t - e->t[newest] < PS_STEP) return;
        if (d < e->d[newest]) { e->n = 0; e->head = 0; }      /* the count went back (cannot happen; start again) */
    }
    e->t[e->head] = t; e->d[e->head] = d;
    e->head = (e->head + 1) % PS_SAMPLES;
    if (e->n < PS_SAMPLES) e->n++;
}
/* jobs a second over the window (the oldest sample kept up to now); -1 while it spans under 10 s or nothing was done */
static inline double ps_eta_rate(const PsEta *e, double t, long long d) {
    if (!e->n) return -1.0;
    int oldest = (e->head + PS_SAMPLES - e->n) % PS_SAMPLES;
    double dt = t - e->t[oldest];
    if (dt < 10.0 || d <= e->d[oldest]) return -1.0;
    return (double)(d - e->d[oldest]) / dt;
}
static inline void ps_compute(const PsIn *in, PsEta *e, double now, PsOut *o) {
    long long left_files = in->nfiles > in->nread ? (long long)(in->nfiles - in->nread) : 0;
    int store = in->st == 5 || !in->nfiles;
    double f;
    o->total = in->queued + left_files * PS_PER_FILE;
    if (in->loadall) f = o->total > 0 ? (double)in->done / (double)o->total : 0.0;
    else if (store) f = 0.10 * (double)in->store_pct / 100.0;
    else f = 0.10 + 0.90 * (o->total > 0 ? (double)in->done / (double)o->total : 0.0);
    if (f < 0.0) f = 0.0;
    if (f > 0.999) f = 0.999;
    o->frac = (float)f;
    o->pct = (int)(f * 100.0);
    o->eta = -1;
    if (!store) {
        ps_eta_note(e, now, in->done);
        double r = ps_eta_rate(e, now, in->done);
        long long rest = o->total - in->done;
        if (r > 0.0 && rest >= 0) { double s = (double)rest / r; o->eta = s > 359999.0 ? 359999 : (int)(s + 0.5); }
    }
    snprintf(o->text[0], sizeof o->text[0], in->loadall ? "LOADING SHADERS %d %%" : "PREPARING SHADERS %d %%", o->pct);
    if (store) snprintf(o->text[1], sizeof o->text[1], "READING THE GAME DATA");
    else snprintf(o->text[1], sizeof o->text[1], "%lld / %lld", in->done, o->total);
    if (o->eta >= 3600) snprintf(o->text[2], sizeof o->text[2], "ABOUT %d:%02d:%02d LEFT", o->eta / 3600, o->eta / 60 % 60, o->eta % 60);
    else if (o->eta >= 0) snprintf(o->text[2], sizeof o->text[2], "ABOUT %d:%02d LEFT", o->eta / 60, o->eta % 60);
    else snprintf(o->text[2], sizeof o->text[2], "TIME LEFT: ...");
    snprintf(o->text[3], sizeof o->text[3], in->loadall ? "NO SHADER LOADING BEFORE A FIGHT" : "FIRST START ONLY - PROGRESS IS KEPT");
}
/* The screen BEFORE the game's first frame (nvn_vk_draw.c prep_early_*): the game starts (data.arc read into the store,
 * thousands of programs set, each waiting for its stages) for minutes before it presents anything, and the first-run screen
 * of prep_first_hold begins only at that present. mode: graphics.ini shader_prepare (0 first_run, 1 background, 2 off). */
static inline int ps_early_wanted(int mode, int have_prep, int marked, int limited, int state, int stopped, int game_presented) {
    if (mode != 0 || !have_prep || marked || limited || stopped || game_presented) return 0;
    return state != 2 && state != 3;                /* finished / no container source: nothing to show */
}
/* The preparation's END (nvn_vk_m4.c prep_main): whole = every container read and every queued job done, and the game
 * NOT closing. Once prep_stopped(), job_prep leaves a job between two stages and the worker still counts it done, so a
 * stop during the last jobs gives done == queued although stages were dropped: without `stopped` that run would write
 * the marker and the next start would skip the preparation (review_firstrun.md, MEDIUM 1). `stopped` must be read AFTER
 * done / queued (under g_sh_lock): an abandoned job's done++ comes after its own read of the stop. */
static inline int ps_prep_whole(long long done, long long queued, int read, int todo, int stopped) {
    return !stopped && done == queued && read == todo;
}
/* the marker (with the pipeline cache) is due only for a whole run that was not already prepared by an earlier one */
static inline int ps_marker_due(int whole, int marked) { return whole && !marked; }
/* prep_main's store check: a store made from another data.arc (stale) removes the marker, so that start is a first run
 * again (store + whole preparation) and gets the screen before the first frame (review_firstrun.md, MEDIUM 2) */
static inline int ps_marked_after_store_check(int marked, int stale) { return marked && !stale; }
#endif

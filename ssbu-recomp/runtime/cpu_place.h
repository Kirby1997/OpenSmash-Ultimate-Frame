/* cpu_place.h - Linux on a hybrid aarch64 CPU (the Steam Frame): which processors are the slow tier and which the fastest,
 * from the kernel's cpu_capacity, and the SSBU_CPU_PLACE switch (tests/port/cpuplace). os_threads.c applies it
 * (os_cpu_place_self); nothing here touches a thread.
 *
 * Why (2026-11, the Frame, 8 fighters on Mario Galaxy, report_perf_bench 7.4): the game's main thread, the render thread
 * and the game's second "render dispatch" thread spent 4-9 %, 4-9 % and 17 % of their time on the two Cortex-A520s,
 * where the game thread runs at IPC 0.45 against 1.03 on an A720 (2.27 GHz against 2.96-3.15). A frame is late when the
 * serial chain game (7.4 ms) -> render replay (7.1 ms) runs past the tick, so every slice on a slow core lands on it.
 *
 * Core types come from /sys/devices/system/cpu/cpuN/cpu_capacity (the kernel's own scale, 1024 = the fastest CPU), no CPU
 * numbers are assumed. The slow tier is the lowest group of capacities that ends at the first step of more than 25 %
 * (Frame: the A520s; the A720s at 2.96 and 3.15 GHz and the X4 are one group). "Biggest" is the CPUs at the highest
 * capacity (Frame: the X4). Nothing is done when the capacities are all equal, missing, or there are more than 64 CPUs. */
#ifndef CPU_PLACE_H
#define CPU_PLACE_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CP_MAX_CPU 64

/* SSBU_CPU_PLACE: unset / "1" / "a" = the default: the critical threads off the slow tier (option A);
 * "b" / "2" = A, and the render thread also prefers the biggest CPU (uclamp_min, a hint to the scheduler, not a pin);
 * "bmain" = A, and the game's main thread prefers the biggest CPU instead; "0" / "off" = nothing is changed. */
enum { CP_OFF, CP_A, CP_B_RENDER, CP_B_MAIN };

/* *known = 0: a value that is none of the above (A is used) */
static inline int cp_mode_from(const char *e, int *known) {
    *known = 1;
    if (!e || !e[0] || !strcmp(e, "1") || !strcmp(e, "a") || !strcmp(e, "A")) return CP_A;
    if (!strcmp(e, "0") || !strcmp(e, "off")) return CP_OFF;
    if (!strcmp(e, "2") || !strcmp(e, "b") || !strcmp(e, "B")) return CP_B_RENDER;
    if (!strcmp(e, "bmain")) return CP_B_MAIN;
    *known = 0;
    return CP_A;
}
static inline const char *cp_mode_name(int m) { return m == CP_OFF ? "0" : m == CP_B_RENDER ? "b" : m == CP_B_MAIN ? "bmain" : "a"; }

typedef struct {
    unsigned cap[CP_MAX_CPU];   /* cpu_capacity of cpu0..cpu63; 0 = no file or not a number */
    int n;                      /* how many were read */
    int over;                   /* <root>/cpu64 exists: more CPUs than these masks hold (nothing is done) */
} CpCaps;

/* root: /sys/devices/system/cpu, or a test's fake tree */
static inline int cp_read_caps(const char *root, CpCaps *t) {
    char path[512];
    memset(t, 0, sizeof *t);
    for (int i = 0; i < CP_MAX_CPU; i++) {
        snprintf(path, sizeof path, "%s/cpu%d/cpu_capacity", root, i);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        unsigned v = 0;
        if (fscanf(f, "%u", &v) == 1 && v > 0 && v <= 1u << 20) { t->cap[i] = v; t->n++; }
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/cpu%d", root, CP_MAX_CPU);
    t->over = access(path, F_OK) == 0;
    return t->n;
}

typedef struct {
    uint64_t known;             /* CPUs with a capacity */
    uint64_t slow;              /* the slow tier (0: no step of more than 25 % between the capacities) */
    uint64_t biggest;           /* the CPUs at the highest capacity */
    unsigned cap_min, cap_max;
} CpClass;

/* 1 = hybrid (the capacities differ): the masks are set; 0 = not (homogeneous, nothing read, or over 64 CPUs) */
static inline int cp_classify(const CpCaps *t, CpClass *c) {
    unsigned v[CP_MAX_CPU];
    int k = 0;
    memset(c, 0, sizeof *c);
    if (t->over || t->n < 2) return 0;
    for (int i = 0; i < CP_MAX_CPU; i++) {
        unsigned x = t->cap[i];
        if (!x) continue;
        c->known |= 1ull << i;
        if (!c->cap_min || x < c->cap_min) c->cap_min = x;
        if (x > c->cap_max) c->cap_max = x;
        int j = k;                                      /* the distinct values, ascending */
        while (j > 0 && v[j - 1] > x) j--;
        if (j > 0 && v[j - 1] == x) continue;
        memmove(v + j + 1, v + j, (size_t)(k - j) * sizeof *v);
        v[j] = x; k++;
    }
    if (c->cap_min == c->cap_max) { c->known = 0; c->cap_min = c->cap_max = 0; return 0; }
    unsigned slow_top = 0;
    for (int i = 0; i + 1 < k; i++)
        if ((uint64_t)v[i + 1] * 4 > (uint64_t)v[i] * 5) { slow_top = v[i]; break; }
    for (int i = 0; i < CP_MAX_CPU; i++) {
        if (!t->cap[i]) continue;
        if (slow_top && t->cap[i] <= slow_top) c->slow |= 1ull << i;
        if (t->cap[i] == c->cap_max) c->biggest |= 1ull << i;
    }
    return 1;
}

/* "0:325,1:325,2:1003,...", for the start-up line */
static inline void cp_caps_text(const CpCaps *t, char *out, size_t cap) {
    size_t at = 0;
    out[0] = 0;
    for (int i = 0; i < CP_MAX_CPU && at + 1 < cap; i++)
        if (t->cap[i]) {
            int w = snprintf(out + at, cap - at, "%s%d:%u", at ? "," : "", i, t->cap[i]);
            if (w < 0) break;
            at += (size_t)w;
        }
}
#endif

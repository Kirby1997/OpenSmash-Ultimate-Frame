/* tests/port/cpuplace - the core-type detection of SSBU_CPU_PLACE (runtime/cpu_place.h) against fake sysfs trees: no game,
 * no thread is moved. Homogeneous, the Frame's 2 + 5 + 1 (two A720 clocks), files missing, more than 64 CPUs, and the
 * SSBU_CPU_PLACE values. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "cpu_place.h"

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define SECTION(s) printf("  - %s\n", s)

static char g_base[256];
static int g_tree;

/* a fresh fake /sys/devices/system/cpu with cpu0..cpu<n-1>; caps[i] < 0: the directory without cpu_capacity;
 * text (optional): what cpu<i>/cpu_capacity holds instead of the number */
static const char *tree(int n, const int *caps, const char *const *text) {
    static char root[512];
    char p[600];
    snprintf(root, sizeof root, "%s/t%d", g_base, g_tree++);
    if (mkdir(root, 0700)) { perror(root); exit(2); }
    for (int i = 0; i < n; i++) {
        snprintf(p, sizeof p, "%s/cpu%d", root, i);
        if (mkdir(p, 0700)) { perror(p); exit(2); }
        if (caps && caps[i] < 0 && !(text && text[i])) continue;
        snprintf(p, sizeof p, "%s/cpu%d/cpu_capacity", root, i);
        FILE *f = fopen(p, "w");
        if (!f) { perror(p); exit(2); }
        if (text && text[i]) fputs(text[i], f); else fprintf(f, "%d\n", caps ? caps[i] : 1024);
        fclose(f);
    }
    return root;
}

int main(void) {
    CpCaps t; CpClass c;
    char s[1024];
    snprintf(g_base, sizeof g_base, "%s/ssbu-cpuplace-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(g_base)) { perror("mkdtemp"); return 2; }
    printf("cpu_place.h, fake trees in %s\n", g_base);

    SECTION("homogeneous: 8 CPUs of 1024 -> not hybrid, no masks");
    {   int caps[8] = {1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024};
        CHECK(cp_read_caps(tree(8, caps, NULL), &t) == 8);
        CHECK(!t.over);
        CHECK(cp_classify(&t, &c) == 0);
        CHECK(c.slow == 0 && c.biggest == 0 && c.known == 0); }

    SECTION("Frame, 2 + 5 + 1: A520 x2, A720 x3 at 3.15 GHz + x2 at 2.96, X4 -> slow 0x3, biggest 0x80");
    {   int caps[8] = {325, 325, 975, 975, 975, 916, 916, 1024};
        CHECK(cp_read_caps(tree(8, caps, NULL), &t) == 8);
        CHECK(cp_classify(&t, &c) == 1);
        CHECK(c.known == 0xff);
        CHECK(c.slow == 0x3);
        CHECK(c.biggest == 0x80);
        CHECK(c.cap_min == 325 && c.cap_max == 1024);
        cp_caps_text(&t, s, sizeof s);
        CHECK(!strcmp(s, "0:325,1:325,2:975,3:975,4:975,5:916,6:916,7:1024"));
        CHECK((0xffull & ~c.slow) == 0xfc); }                    /* option A's mask with every CPU allowed */

    SECTION("same shape, the cores listed in another order (no CPU numbers assumed)");
    {   int caps[8] = {1024, 916, 325, 975, 325, 975, 916, 975};
        cp_read_caps(tree(8, caps, NULL), &t);
        CHECK(cp_classify(&t, &c) == 1);
        CHECK(c.slow == 0x14 && c.biggest == 0x1); }

    SECTION("two tiers within 25 % (no slow tier): hybrid for B only");
    {   int caps[4] = {870, 870, 1024, 1024};
        cp_read_caps(tree(4, caps, NULL), &t);
        CHECK(cp_classify(&t, &c) == 1);
        CHECK(c.slow == 0 && c.biggest == 0xc); }

    SECTION("three tiers 300 / 600 / 1024: only the lowest is slow");
    {   int caps[6] = {300, 300, 600, 600, 1024, 1024};
        cp_read_caps(tree(6, caps, NULL), &t);
        CHECK(cp_classify(&t, &c) == 1);
        CHECK(c.slow == 0x3 && c.biggest == 0x30); }

    SECTION("files missing: no cpu_capacity anywhere (x86, an old kernel) -> nothing read, not hybrid");
    {   int caps[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
        CHECK(cp_read_caps(tree(8, caps, NULL), &t) == 0);
        CHECK(cp_classify(&t, &c) == 0 && c.slow == 0 && c.biggest == 0);
        cp_caps_text(&t, s, sizeof s);
        CHECK(s[0] == 0); }

    SECTION("files missing: no tree at all");
    {   char p[600];
        snprintf(p, sizeof p, "%s/does-not-exist", g_base);
        CHECK(cp_read_caps(p, &t) == 0);
        CHECK(cp_classify(&t, &c) == 0); }

    SECTION("files missing: one CPU without the file, one unreadable -> those CPUs are unknown, never slow");
    {   int caps[8] = {325, -1, 975, -1, 975, 916, 916, 1024};       /* cpu1: "garbage", cpu3: no file */
        const char *text[8] = {0, "garbage\n", 0, 0, 0, 0, 0, 0};
        CHECK(cp_read_caps(tree(8, caps, text), &t) == 6);
        CHECK(t.cap[1] == 0 && t.cap[3] == 0);
        CHECK(cp_classify(&t, &c) == 1);
        CHECK(c.known == 0xf5 && c.slow == 0x1 && c.biggest == 0x80); }
    {   int caps[8] = {-1, -1, -1, -1, -1, -1, -1, 1024};             /* one capacity only: nothing to compare */
        CHECK(cp_read_caps(tree(8, caps, NULL), &t) == 1);
        CHECK(cp_classify(&t, &c) == 0); }

    SECTION("more than 64 CPUs: not acted on");
    {   int caps[65];
        for (int i = 0; i < 65; i++) caps[i] = i < 2 ? 325 : 1024;
        cp_read_caps(tree(65, caps, NULL), &t);
        CHECK(t.over && t.n == 64);
        CHECK(cp_classify(&t, &c) == 0); }

    SECTION("SSBU_CPU_PLACE values");
    {   int k;
        CHECK(cp_mode_from(NULL, &k) == CP_A && k);
        CHECK(cp_mode_from("", &k) == CP_A && k);
        CHECK(cp_mode_from("1", &k) == CP_A && k);
        CHECK(cp_mode_from("a", &k) == CP_A && k);
        CHECK(cp_mode_from("0", &k) == CP_OFF && k);
        CHECK(cp_mode_from("off", &k) == CP_OFF && k);
        CHECK(cp_mode_from("b", &k) == CP_B_RENDER && k);
        CHECK(cp_mode_from("2", &k) == CP_B_RENDER && k);
        CHECK(cp_mode_from("bmain", &k) == CP_B_MAIN && k);
        CHECK(cp_mode_from("x4", &k) == CP_A && !k);
        CHECK(!strcmp(cp_mode_name(CP_B_MAIN), "bmain") && !strcmp(cp_mode_name(CP_OFF), "0")); }

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_base);
    if (system(cmd)) fprintf(stderr, "note: %s left behind\n", g_base);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}

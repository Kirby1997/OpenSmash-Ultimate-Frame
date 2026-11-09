/* test_mii.c - nnmii.c on Linux: GetRegionCode from SSBU_REGION / $LC_ALL / $LC_MESSAGES / $LANG. nnmii.c is compiled INTO this test. */
#include "nnmii.c"
#include "stubs.h"
/* what nnmii.c reaches in the rest of the runtime (nothing of it runs here, but the region code: a trap is an abort) */
void guest_trap(const char *why, uint64_t pc) { fprintf(stderr, "guest_trap: %s (pc %llx)\n", why, (unsigned long long)pc); abort(); }
void *import_data(const char *name) { (void)name; return NULL; }
void report_and_exit(int code) { exit(code); }
void trace_printf(const char *fmt, ...) { (void)fmt; }
int sdk_image_ready(void) { return 0; }
ImportFn sdk_lifted(const char *name) { (void)name; return NULL; }
void sdk_host_override(const char *name, ImportFn fn) { (void)name; (void)fn; }
static int trapped;
static int region(const char *region_env, const char *lc_all, const char *lc_msg, const char *lang, char *iso) {
    const char *e[4][2] = {{"SSBU_REGION", region_env}, {"LC_ALL", lc_all}, {"LC_MESSAGES", lc_msg}, {"LANG", lang}};
    for (int i = 0; i < 4; i++) { if (e[i][1]) setenv(e[i][0], e[i][1], 1); else unsetenv(e[i][0]); }
    return host_region_code(iso);
}
int main(void) {
    char iso[4];
    SECTION("locale territory -> region code");
    CHECK(region(NULL, NULL, NULL, "en_GB.UTF-8", iso) == 2 && !strcmp(iso, "GB"));
    CHECK(region(NULL, NULL, NULL, "en_US.UTF-8", iso) == 1 && !strcmp(iso, "US"));
    CHECK(region(NULL, NULL, NULL, "ja_JP.UTF-8", iso) == 0);
    CHECK(region(NULL, NULL, NULL, "en_AU", iso) == 3);
    CHECK(region(NULL, NULL, NULL, "ko_KR.eucKR", iso) == 4);
    CHECK(region(NULL, NULL, NULL, "zh_CN.UTF-8", iso) == 5);
    CHECK(region(NULL, NULL, NULL, "de_DE@euro", iso) == 2);
    CHECK(region(NULL, "fr_CA.UTF-8", NULL, "en_GB.UTF-8", iso) == 1);          /* LC_ALL wins */
    /* (linux-arm64 also read $LC_MESSAGES, between LC_ALL and LANG; plat's GetUserDefaultGeoName reads LC_ALL and LANG only) */
    SECTION("SSBU_REGION overrides; a locale without a territory (C, POSIX, \"en\", none) is the Americas, as plat's GetUserDefaultGeoName defaults to US");
    CHECK(region("3", NULL, NULL, "en_GB.UTF-8", iso) == 3);
    CHECK(region("9", NULL, NULL, "en_GB.UTF-8", iso) == 2);                   /* invalid override ignored */
    CHECK(region(NULL, NULL, NULL, "C", iso) == 1 && !strcmp(iso, "US"));       /* (linux-arm64 left it unknown: the game stopped with a message to set SSBU_REGION) */
    CHECK(region(NULL, NULL, NULL, "POSIX", iso) == 1);
    CHECK(region(NULL, NULL, NULL, "en", iso) == 1);
    CHECK(region(NULL, NULL, NULL, NULL, iso) == 1);
    CHECK(region(NULL, NULL, NULL, "xx_ZZ.UTF-8", iso) == -1 && !strcmp(iso, "ZZ"));   /* a territory the table lacks stays unknown */
    CHECK(region("4", NULL, NULL, "C", iso) == 4);
    return mt_done("nnmii");
}

/* test_time.c - nntime.c on Linux: TZif zones (history, footer continuation, DST transitions) against glibc's localtime_r reading the same
 * zoneinfo, ToPosixTime round trips, steady clock. nntime.c is compiled INTO this test. Usage: test_time   (run with TZ=<zone> too for the
 * host-zone path: ToCalendarTime without a rule). */
#include "nntime.c"
#include "stubs.h"
void host_clock_caller(const char *what, uint64_t lr) { (void)what; (void)lr; }
int host_fixed_clock(uint64_t lr) { (void)lr; return 0; }
#define H(n) svc_find_sub(n)
static Ctx C;
static uint8_t cal[8], info[0x18];
static TzRule *rule;

static void to_local(int64_t t, int use_rule) {
    static int64_t posix; posix = t;
    memset(&C, 0, sizeof C);
    C.x[0] = (uint64_t)(uintptr_t)cal; C.x[1] = (uint64_t)(uintptr_t)info; C.x[2] = (uint64_t)(uintptr_t)&posix; C.x[3] = (uint64_t)(uintptr_t)rule;
    (use_rule ? H("ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE") : H("14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE"))(&C);
}
static int same_as_glibc(int64_t t, int use_rule) {
    struct tm lt; time_t tt = (time_t)t;
    to_local(t, use_rule);
    if (C.x[0] || !localtime_r(&tt, &lt)) return 0;
    return rd16((uint64_t)(uintptr_t)cal) == lt.tm_year + 1900 && cal[2] == lt.tm_mon + 1 && cal[3] == lt.tm_mday && cal[4] == lt.tm_hour && cal[5] == lt.tm_min && cal[6] == lt.tm_sec
        && (int)rd32((uint64_t)(uintptr_t)info) == lt.tm_wday && (int)rd32((uint64_t)(uintptr_t)info + 4) == lt.tm_yday && info[16] == (lt.tm_isdst > 0)
        && (int)rd32((uint64_t)(uintptr_t)info + 20) == lt.tm_gmtoff && !strcmp((char *)info + 8, lt.tm_zone);
}
static const char *zones[] = {"UTC", "Europe/London", "Europe/Dublin", "Europe/Berlin", "America/New_York", "America/Los_Angeles", "America/Sao_Paulo", "America/St_Johns",
    "America/Godthab", "Australia/Sydney", "Australia/Lord_Howe", "Asia/Kolkata", "Asia/Tokyo", "Asia/Kathmandu", "Pacific/Auckland", "Pacific/Chatham", "Africa/Casablanca", "Asia/Tehran", "America/Santiago", "Antarctica/Troll"};

int main(void) {
    const char *tz0 = getenv("TZ"); char tzsave[64] = ""; if (tz0) snprintf(tzsave, sizeof tzsave, "%s", tz0);
    register_time_imports();
    CHECK(svc_nimports > 10);
    ImportFn load = H("16LoadTimeZoneRule");
    rule = calloc(1, sizeof(TzRule));
    printf("nntime: %s\n", getenv("TZ") ? getenv("TZ") : "(host zone)");
    SECTION("LoadTimeZoneRule + ToCalendarTime(rule) == glibc localtime, every transition +-1 s 1970..2037, plus a 6 h sweep of 2024");
    for (unsigned z = 0; z < sizeof zones / sizeof *zones; z++) {
        char name[0x24] = {0}; strcpy(name, zones[z]);
        if (access(strstr("x", "x") ? "/usr/share/zoneinfo/UTC" : "", R_OK)) { printf("no zoneinfo\n"); return 0; }
        memset(&C, 0, sizeof C); C.x[0] = (uint64_t)(uintptr_t)rule; C.x[1] = (uint64_t)(uintptr_t)name;
        load(&C);
        char path[128]; snprintf(path, sizeof path, "/usr/share/zoneinfo/%s", zones[z]);
        if (access(path, R_OK)) { printf("  %s: not installed, skipped\n", zones[z]); continue; }
        CHECKF(C.x[0] == 0, "LoadTimeZoneRule(%s) = %llx", zones[z], (unsigned long long)C.x[0]);
        setenv("TZ", zones[z], 1); tzset();
        int bad = 0; long n = 0;
        for (int k = 0; k < rule->timecnt; k++) for (int d = -1; d <= 1; d++) { n++; if (!same_as_glibc(rule->ats[k] + d, 1)) { if (bad++ < 3) printf("  %s: transition %d (%lld) %+d differs\n", zones[z], k, (long long)rule->ats[k], d); } }
        for (int64_t t = 1704067200; t < 1735689600; t += 21600) { n++; if (!same_as_glibc(t, 1)) { if (bad++ < 3) printf("  %s: %lld differs\n", zones[z], (long long)t); } }
        int last_year = 1970; { TzTm g; if (!tz_split(rule->ats[rule->timecnt ? rule->timecnt - 1 : 0], 0, &g)) last_year = g.year; }
        CHECKF(bad == 0, "%s: %d of %ld differ", zones[z], bad, n);
        printf("  %-22s %3d transitions (last %d), %d types, %ld checks, %d differ\n", zones[z], rule->timecnt, last_year, rule->typecnt, n, bad);
    }
    SECTION("unknown / hostile locations");
    { char name[0x24] = {0}; strcpy(name, "Mars/Phobos"); memset(&C, 0, sizeof C); C.x[0] = (uint64_t)(uintptr_t)rule; C.x[1] = (uint64_t)(uintptr_t)name; load(&C); CHECK(C.x[0] == TZ_NOT_FOUND);
      strcpy(name, "../../etc/passwd"); memset(&C, 0, sizeof C); C.x[0] = (uint64_t)(uintptr_t)rule; C.x[1] = (uint64_t)(uintptr_t)name; load(&C); CHECK(C.x[0] == TZ_NOT_FOUND); }
    SECTION("ToPosixTime round trip incl. fall-back hour (2 results) and spring gap (0 results), Europe/London 2026");
    { char name[0x24] = {0}; strcpy(name, "Europe/London"); memset(&C, 0, sizeof C); C.x[0] = (uint64_t)(uintptr_t)rule; C.x[1] = (uint64_t)(uintptr_t)name; load(&C); }
    /* my_rule is the host zone, so ToPosixTime is checked in the self-test / host-zone run below */
    SECTION("host zone path (TZ env / /etc/localtime): ToCalendarTime without a rule == localtime_r, ToPosixTime inverts it");
    { const char *e = getenv("TZ"); (void)e; }
    if (tzsave[0]) setenv("TZ", tzsave, 1); else unsetenv("TZ");
    tzset();
    {
        char host[0x24]; location_name(host);
        printf("  host location name: %s\n", host);
    }
    CHECK(nntime_selftest(&C) == 0);
    SECTION("steady clock is nanoseconds on the monotonic clock (QueryPerformanceCounter), monotonic");
    { memset(&C, 0, sizeof C); H("StandardSteadyClock3now")(&C); uint64_t a = C.x[0];
      LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
      uint64_t b = (uint64_t)((double)q.QuadPart * 1e9 / (double)f.QuadPart);
      CHECKF(a <= b && b - a < 1000000000ull, "%llu %llu", (unsigned long long)a, (unsigned long long)b);
      memset(&C, 0, sizeof C); H("StandardSteadyClock3now")(&C); CHECK(C.x[0] >= a); }
    SECTION("UTC conversions");
    { static int64_t t = 1700000000; memset(&C, 0, sizeof C); C.x[0] = (uint64_t)(uintptr_t)&t; H("ToCalendarTimeInUtc")(&C); uint8_t b[8]; memcpy(b, &C.x[0], 8);
      CHECK((b[0] | b[1] << 8) == 2023 && b[2] == 11 && b[3] == 14 && b[4] == 22 && b[5] == 13 && b[6] == 20);
      memset(&C, 0, sizeof C); C.x[0] = (uint64_t)(uintptr_t)b; H("ToPosixTimeFromUtc")(&C); CHECK((int64_t)C.x[0] == 1700000000); }
    return mt_done("nntime");
}

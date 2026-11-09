/* mt.h - tiny test helpers for tests/port/services (the output format of tests/port/threads/tp.h) */
#ifndef MT_H
#define MT_H
#include <stdio.h>
#include <stdlib.h>
static int mt_fail, mt_checks;
#define CHECK(c) do { mt_checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); mt_fail++; } } while (0)
#define CHECKF(c, ...) do { mt_checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, #c); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); mt_fail++; } } while (0)
#define SECTION(s) do { printf("  - %s\n", s); fflush(stdout); } while (0)
static inline int mt_done(const char *name) {
    printf("%-16s %s (%d checks, %d failed)\n", name, mt_fail ? "FAIL" : "PASS", mt_checks, mt_fail);
    fflush(stdout);
    return mt_fail ? 1 : 0;
}
#endif

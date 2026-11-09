/* tp.h - tiny test helpers for tests/plat */
#ifndef TP_H
#define TP_H
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__SANITIZE_THREAD__)
#define TP_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define TP_TSAN 1
#endif
#endif

static int tp_fail, tp_checks;
#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define TP_BUMP(x) __atomic_add_fetch(&(x), 1, __ATOMIC_RELAXED)
#define CHECK(c) do { TP_BUMP(tp_checks); if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); TP_BUMP(tp_fail); } } while (0)
#define CHECKF(c, ...) do { TP_BUMP(tp_checks); if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, #c); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); TP_BUMP(tp_fail); } } while (0)
static inline int64_t tp_now_us(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart / 1000; }
static inline int tp_done(const char *name) {
    printf("%-14s %s (%d checks, %d failed)\n", name, tp_fail ? "FAIL" : "PASS", tp_checks, tp_fail);
    fflush(stdout);
    return tp_fail ? 1 : 0;
}
#define SECTION(s) do { printf("  - %s\n", s); fflush(stdout); } while (0)
#endif

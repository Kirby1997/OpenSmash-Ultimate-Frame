/* test_threads.c (C3b) - plat_posix.c CreateThread on aarch64 Linux: each thread's stack and TLS block (its __thread
 * variables, errno) below 2^47, the size asked for, a guard page under the stack, stacks of ended threads unmapped
 * (3000 threads one after the other, 64 at once), suspended start, exit codes. Build / run: tests/port/threads/run.sh. */
#include "tp.h"
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#if defined(__linux__) && defined(__aarch64__)
#define LOW_CHECKS 1                               /* the Frame: glibc's own placement would be above 2^47 */
#endif
static int low(uintptr_t a) { return a < (1ull << 47) && a >= 0x7600000000ull; }
static int maps_lines(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    int n = 0, ch;
    while ((ch = fgetc(f)) != EOF) n += ch == '\n';
    fclose(f);
    return n;
}
static __thread int t_var = 5;
typedef struct { uintptr_t stack, tls, err; size_t lo, hi; int tv; } Seen;
static DWORD WINAPI look(LPVOID p) {
    Seen *s = p;
    int v;
    s->stack = (uintptr_t)&v; s->tls = (uintptr_t)&t_var; s->err = (uintptr_t)&errno; s->tv = t_var;
#ifdef __linux__
    pthread_attr_t a; void *base; size_t sz;
    if (!pthread_getattr_np(pthread_self(), &a) && !pthread_attr_getstack(&a, &base, &sz)) { s->lo = (uintptr_t)base; s->hi = (uintptr_t)base + sz; }
    pthread_attr_destroy(&a);
#endif
    return 0x1234;
}
static void test_placement(void) {
    SECTION("stack and TLS block (a __thread variable, errno) of CreateThread threads below 2^47; the size asked for");
    SIZE_T sizes[] = {0, 64 << 10, 1 << 20, (SIZE_T)256 << 20, (SIZE_T)1 << 30};
    for (int i = 0; i < 5; i++) {
        Seen s = {0};
        HANDLE h = CreateThread(NULL, sizes[i], look, &s, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        CHECK(h && WaitForSingleObject(h, 10000) == WAIT_OBJECT_0);
        DWORD code = 0;
        CHECK(GetExitCodeThread(h, &code) && code == 0x1234);
        CloseHandle(h);
        printf("    asked %10llu: stack %016llx, __thread %016llx, errno %016llx, stack range %llx..%llx\n", (unsigned long long)sizes[i],
               (unsigned long long)s.stack, (unsigned long long)s.tls, (unsigned long long)s.err, (unsigned long long)s.lo, (unsigned long long)s.hi);
        CHECK(s.tv == 5);                                     /* the TLS block was initialised in the given stack */
#ifdef LOW_CHECKS
        CHECK(low(s.stack) && low(s.tls) && low(s.err));
        size_t want = sizes[i] ? (sizes[i] < (1 << 20) ? (1 << 20) : sizes[i]) : (SIZE_T)8 << 20;
        CHECKF(s.hi - s.lo == want && s.stack > s.lo && s.stack < s.hi && s.tls > s.lo && s.tls < s.hi, "%zx", (size_t)(s.hi - s.lo));
#endif
    }
}
static DWORD WINAPI nothing(LPVOID p) { return (DWORD)(uintptr_t)p; }
static void test_reaping(void) {
    SECTION("3000 threads one after the other, then 64 at once: ended threads' stacks are unmapped");
    HANDLE h = CreateThread(NULL, 0, nothing, NULL, 0, NULL); WaitForSingleObject(h, INFINITE); CloseHandle(h);
    int m0 = maps_lines(), bad = 0;
    for (int i = 0; i < 3000; i++) {
        h = CreateThread(NULL, (SIZE_T)256 << 20, nothing, (void *)(uintptr_t)i, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        DWORD code = 0;
        if (!h || WaitForSingleObject(h, 10000) != WAIT_OBJECT_0 || !GetExitCodeThread(h, &code) || code != (DWORD)i) bad++;
        CloseHandle(h);
    }
    HANDLE hs[64];
    for (int i = 0; i < 64; i++) hs[i] = CreateThread(NULL, 0, nothing, NULL, 0, NULL);
    CHECK(WaitForMultipleObjects(64, hs, TRUE, 20000) == WAIT_OBJECT_0);
    for (int i = 0; i < 64; i++) CloseHandle(hs[i]);
    h = CreateThread(NULL, 0, nothing, NULL, 0, NULL); WaitForSingleObject(h, INFINITE); CloseHandle(h);   /* reaps the 64 */
    Sleep(50);
    h = CreateThread(NULL, 0, nothing, NULL, 0, NULL); WaitForSingleObject(h, INFINITE); CloseHandle(h);
    int m1 = maps_lines();
    CHECK(bad == 0);
    CHECKF(m1 <= m0 + 8, "mappings %d -> %d", m0, m1);
}
static volatile LONG g_ran;
static DWORD WINAPI mark(LPVOID p) { (void)p; InterlockedExchange(&g_ran, 1); return 7; }
static void test_suspended(void) {
    SECTION("CREATE_SUSPENDED, ResumeThread, the thread id");
    DWORD tid = 0;
    HANDLE h = CreateThread(NULL, 0, mark, NULL, CREATE_SUSPENDED, &tid);
    Sleep(20);
    CHECK(h && tid && g_ran == 0 && GetThreadId(h) == tid);
    CHECK(ResumeThread(h) == 1);
    CHECK(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0 && g_ran == 1);
    DWORD code = 0;
    CHECK(GetExitCodeThread(h, &code) && code == 7);
    CloseHandle(h);
}
static int depth(int n) { volatile char buf[1024]; buf[0] = (char)n; return n ? depth(n - 1) + buf[0] : 0; }
static DWORD WINAPI deep(LPVOID p) { (void)p; return (DWORD)depth(1 << 20); }
static void test_guard(void) {
    SECTION("a thread that runs off its stack hits the guard page (SIGSEGV; SIGBUS on macOS)");
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY); dup2(dn, 2);
        HANDLE h = CreateThread(NULL, 1 << 20, deep, NULL, 0, NULL);
        WaitForSingleObject(h, INFINITE);
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECKF(WIFSIGNALED(st) && (WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGBUS), "status %x", st);
}
int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_placement();
    test_reaping();
    test_suspended();
    test_guard();
    return tp_done("test_threads");
}

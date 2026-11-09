/* test_headers.c - runtime/plat/windows.h (and the other Win32 stand-in headers) together with every system header the runtime
 * includes, in both orders: compiled twice, with -DPLAT_FIRST and without, with -Wall -Wextra -Werror (tests/port/winshim/run.sh).
 * The runtime's files include <windows.h> at different points among <stdio.h>, <pthread.h>, <sys/mman.h>, <SDL3/SDL.h>,
 * <vulkan/vulkan.h> ...; a macro or a typedef of the stand-in (min, max, TRUE, INFINITE, WAIT_TIMEOUT, MAX_PATH, LONG, DWORD ...)
 * must not break a system header that comes after it, nor be broken by one that comes before. The same functions are then
 * called, so that the declarations are checked against the shapes the runtime relies on.
 * Was tests/plat/test_headers.c of linux-arm64 (there plat.h; here windows.h with plat_pre.h forced in). */
#ifdef PLAT_FIRST
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdint.h>
#include <math.h>
#include <stdarg.h>
#include <setjmp.h>
#include <time.h>
#include <stddef.h>
#include <ctype.h>
#include <stdbool.h>
#include <inttypes.h>
#include <limits.h>
#include <errno.h>
#include <float.h>
#include <wchar.h>
#include <locale.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <dirent.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#endif
#if __has_include(<SDL3/SDL.h>)
#include <SDL3/SDL.h>
#endif
#if __has_include(<ft2build.h>)
#include <ft2build.h>
#include FT_FREETYPE_H
#endif
#ifndef PLAT_FIRST
#include <windows.h>
#endif
/* the other stand-ins of runtime/plat */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h>
#include <timeapi.h>
#include <wincrypt.h>
#include <shlwapi.h>
#include <psapi.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <intrin.h>

/* the shapes the runtime relies on */
_Static_assert(sizeof(LONG) == 4 && sizeof(DWORD) == 4 && sizeof(LONG64) == 8 && sizeof(SIZE_T) == sizeof(void *), "types");
_Static_assert(sizeof(LARGE_INTEGER) == 8 && sizeof(FILETIME) == 8 && sizeof(SYSTEMTIME) == 16 && sizeof(WCHAR) == sizeof(wchar_t), "structs");   /* WCHAR is wchar_t here: 4 bytes */
_Static_assert(INFINITE == 0xFFFFFFFFu && WAIT_TIMEOUT == 258 && MEM_COMMIT == 0x1000 && PAGE_READWRITE == 4, "constants");
static __declspec(thread) HANDLE t_h;
static SRWLOCK s_l = SRWLOCK_INIT;
static CONDITION_VARIABLE s_cv = CONDITION_VARIABLE_INIT;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static volatile LONG s_n;
static volatile LONG64 s_n64;
static DWORD WINAPI thread_fn(LPVOID arg) { return (DWORD)(uintptr_t)arg; }
static BOOL CALLBACK once_fn(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; return TRUE; }
static void WINAPI fiber_fn(LPVOID arg) { (void)arg; }
#ifdef PLAT_FIRST
#define ENTRY headers_plat_first
#else
#define ENTRY headers_plat_last
#endif
int ENTRY(void);
int ENTRY(void) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    MEMORY_BASIC_INFORMATION mi = {0};
    CRITICAL_SECTION cs;
    InitializeCriticalSection(&cs);
    EnterCriticalSection(&cs);
    LeaveCriticalSection(&cs);
    AcquireSRWLockExclusive(&s_l);
    ReleaseSRWLockExclusive(&s_l);
    WakeAllConditionVariable(&s_cv);
    InitOnceExecuteOnce(&s_once, once_fn, NULL, NULL);
    InterlockedIncrement(&s_n);
    InterlockedAdd64(&s_n64, 2);
    t_h = CreateThread(NULL, 0, thread_fn, NULL, 0, NULL);
    WaitForSingleObject(t_h, INFINITE);
    CloseHandle(t_h);
    (void)CreateFiberEx;
    (void)fiber_fn;
    DeleteCriticalSection(&cs);
    return (int)VirtualQuery(&mi, &mi, sizeof mi) + _stricmp("a", "A") + (int)(q.QuadPart & 0);
}
#ifdef PLAT_FIRST
int headers_plat_last(void);
int main(void) {
    int r = headers_plat_first() + headers_plat_last();
    printf("%-14s %s\n", "test_headers", r > 0 ? "PASS" : "FAIL");
    return r > 0 ? 0 : 1;
}
#endif

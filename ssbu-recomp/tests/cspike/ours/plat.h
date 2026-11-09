/* C-spike shim: our tests/plat include "plat.h"; here it is upstream's layer */
#include <windows.h>
#include <stdint.h>
/* Win32 error codes our tests compare against that upstream's windows.h does not define (the Windows values) */
#ifndef ERROR_TIMEOUT
#define ERROR_TIMEOUT 1460
#endif
#ifndef ERROR_INSUFFICIENT_BUFFER
#define ERROR_INSUFFICIENT_BUFFER 122
#endif
#ifndef ERROR_TOO_MANY_POSTS
#define ERROR_TOO_MANY_POSTS 298
#endif
#ifndef ERROR_NOT_SUPPORTED
#define ERROR_NOT_SUPPORTED 50
#endif
#ifndef ERROR_NOT_OWNER
#define ERROR_NOT_OWNER 288
#endif
#ifndef ERROR_INVALID_HANDLE
#define ERROR_INVALID_HANDLE 6
#endif
#ifndef ERROR_ALREADY_EXISTS
#define ERROR_ALREADY_EXISTS 183
#endif
#ifndef ERROR_INVALID_PARAMETER
#define ERROR_INVALID_PARAMETER 87
#endif
#ifndef ERROR_MOD_NOT_FOUND
#define ERROR_MOD_NOT_FOUND 126
#endif
#ifdef PLAT_LINUX_SYNC
/* since C3b plat's own Linux CRITICAL_SECTION, with Windows' fields */
#define CS_OWNER(cs) ((uintptr_t)(cs).OwningThread)
#define CS_RECURSION(cs) ((cs).RecursionCount)
#else
/* upstream's CRITICAL_SECTION is a recursive glibc pthread_mutex_t: the owner / recursion fields are glibc's */
#define CS_OWNER(cs) ((uintptr_t)(cs).__data.__owner)
#define CS_RECURSION(cs) ((cs).__data.__count)
#endif

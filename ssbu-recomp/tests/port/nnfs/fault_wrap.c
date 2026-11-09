/* fault_wrap.c - the real calls behind fault.h's fi_* names (this file does not see the renaming), after the hook. */
#include <windows.h>
#include "hostpath.h"

void (*fi_hook)(const char *op, const char *path);        /* NULL: straight through */

BOOL fi_DeleteFileA(LPCSTR p) { if (fi_hook) fi_hook("unlink", p); return DeleteFileA(p); }
BOOL fi_RemoveDirectoryA(LPCSTR p) { if (fi_hook) fi_hook("rmdir", p); return RemoveDirectoryA(p); }
BOOL fi_CreateDirectoryA(LPCSTR p, SECURITY_ATTRIBUTES *sa) { if (fi_hook) fi_hook("mkdir", p); return CreateDirectoryA(p, sa); }
BOOL fi_MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) { if (fi_hook) fi_hook("move", to); return MoveFileExA(from, to, flags); }
BOOL fi_CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) { if (fi_hook) fi_hook("copy", to); return CopyFileA(from, to, fail_if_exists); }
BOOL fi_FlushFileBuffers(HANDLE h) { if (fi_hook) fi_hook("fsync", "-"); return FlushFileBuffers(h); }
int fi_host_fsync_parent(const char *path) { if (fi_hook) fi_hook("fsync-dir", path); return host_fsync_parent(path); }

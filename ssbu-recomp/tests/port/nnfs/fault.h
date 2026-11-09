/* fault.h - forced into the build of runtime/nnfs.c only (-include): the calls that change the disk are renamed fi_*, so that
 * fault_wrap.c can tell a hook (a log, a SIGKILL before the n-th call, a folder made read-only) and then make the real call.
 * The names are declared by windows.h / hostpath.h under their new spelling; nothing else in the unit is touched. */
#ifndef NNFS_FAULT_H
#define NNFS_FAULT_H
#define DeleteFileA fi_DeleteFileA
#define RemoveDirectoryA fi_RemoveDirectoryA
#define CreateDirectoryA fi_CreateDirectoryA
#define MoveFileExA fi_MoveFileExA
#define CopyFileA fi_CopyFileA
#define FlushFileBuffers fi_FlushFileBuffers
#define host_fsync_parent fi_host_fsync_parent
#endif

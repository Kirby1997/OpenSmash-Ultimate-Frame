/* hostpath.h - host path helpers of plat_posix.c for the runtime's own files (Linux, macOS). */
#ifndef SSBU_HOSTPATH_H
#define SSBU_HOSTPATH_H
#include <stddef.h>
int host_path_resolve(const char *in, char *out, size_t n);   /* '\\' -> '/'; 1 = exists */
int host_fsync_parent(const char *path);      /* fsync of the folder holding path; 0 or -errno */
const char *host_exe_dir(void);       /* folder of the executable, trailing '/' (datapaths.c) */
int host_make_parents(const char *path);      /* creates the missing folders above path; 0 or -errno (datapaths.c) */
#endif

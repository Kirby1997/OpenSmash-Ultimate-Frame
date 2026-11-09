/* hostpath.h - the two path helpers datapaths.c needs (Linux; plat_posix.c). C10: the cspike branch has no hostpath.c. */
#ifndef SSBU_HOSTPATH_H
#define SSBU_HOSTPATH_H
#include <stddef.h>
const char *host_exe_dir(void);       /* folder of the executable, trailing '/' */
int host_path_resolve(const char *in, char *out, size_t n);   /* '\\' -> '/', case-insensitive lookup; 1 = exists */
int host_make_parents(const char *path);      /* creates the missing folders above path; 0 or -errno */
int host_fsync_parent(const char *path);      /* fsync of the folder holding path; 0 or -errno */
#endif

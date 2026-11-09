/* nn::fs on host directories, plus the minimal nn::account and nn::nifm
 * answers the game needs to start: one local user, no network.
 *
 * MOUNT TABLE. A path is "name:/dir/file"; the name must have been mounted, as in the SDK's own mount table
 * ([sdk 0x1f4960] MountTable::Find: an unknown name is fs NotMounted 0x35f202; mounting a name twice is
 * MountNameAlreadyExists 0x7802). Mount kinds and their host folders:
 *   MountRom           -> %SSBU_ROMFS% or ..\romfs                  (the game's extracted romfs, update applied; read-only)
 *   MountSaveData      -> <userdata>\save                           (journaled, see below)
 *   MountCacheStorage  -> <userdata>\cache                          (journaled like save data)
 * <userdata> is %SSBU_USERDATA% or ..\userdata (tests point SSBU_USERDATA at a copy of the user's save).
 *
 * SAVE DATA JOURNAL. On the console, writes to save data are not permanent until nn::fs::Commit: they go to a journal,
 * Commit applies all of them together, and uncommitted writes are gone at the next mount. Here:
 *   - <userdata>\save is the COMMITTED state and is never written outside Commit.
 *   - Mounting creates the working folder <userdata>\save.work.<pid>-<run>-<n> (a name never used before). A file is copied into it the first time it is
 *     opened for writing (copy-on-write) or created there; deletions are recorded (tombstones); every read, existence
 *     test and directory listing sees the working folder over the committed one, minus the tombstones.
 *   - Commit ([sdk 0x1f4490] FileSystemAccessor::Commit: fails with 0x327202 while a file of the mount is open with the
 *     write mode) is two-phase: (1) the working folder is copied to a new <userdata>\save.stage.<pid>-<run>-<n> and the list
 *     of changes is written to a journal file, flushed, and renamed to <userdata>\save.commit -- from this rename on the
 *     commit is decided; (2) the changes are applied (deletions done, files moved from the stage folder over the committed
 *     ones) and the marker is removed. A process killed in phase 1 leaves the committed state untouched; one killed in
 *     phase 2 leaves the marker, and the next mount finishes the apply (roll forward) before anything is read. An apply that
 *     fails (a full disk, a read-only file) leaves the commit pending; it is finished by the next Commit, Unmount or mount,
 *     and until then every mount sees the decided state. The protocol, its invariants and each crash point: "THE COMMIT
 *     PROTOCOL" below.
 *   - Unmount, or a later mount by any process, discards the working folder of a process that no longer runs.
 * Not reproduced: the console's limit on the journal SIZE (a Commit is needed before the journal fills; the game's own
 * save code already commits within it). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"
#ifndef _WIN32
#include "hostpath.h"                                    /* host_fsync_parent: a deletion, a new folder, the removed marker reach the disk */
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>                                    /* flock: the commit lock between processes */
#include <unistd.h>
#endif

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define STR(n) ((const char *)(uintptr_t)c->x[n])

/* nn::fs results (module 2), value = description << 9 | 2. Descriptions as published (switchbrew "Error codes",
 * the same numbers the suyu reference uses in file_sys/errors.h): 1 PathNotFound, 2 PathAlreadyExists,
 * 1002 TargetNotFound (no such save data), 3005 OutOfRange (read past the end of the file),
 * 6201 FileExtensionWithoutOpenModeAllowAppend (write past the end of a file opened without the append mode),
 * 6202 ReadNotPermitted, 6203 WriteNotPermitted (the handle was opened without that mode).
 * Read in sdk.elf: 60 MountNameAlreadyExists, 6905 NotMounted [0x1f498c], 6457 "a file is still open for writing" [0x1f44f4].
 * 7 DirectoryNotEmpty, 6400 PermissionDenied (suyu file_sys/errors.h; write to the read-only rom). */
#define RESULT_PATH_NOT_FOUND 0x202u
#define RESULT_PATH_ALREADY_EXISTS 0x402u
#define RESULT_DIRECTORY_NOT_EMPTY 0xe02u
#define RESULT_MOUNT_NAME_EXISTS 0x7802u
#define RESULT_TARGET_NOT_FOUND 0x7d402u
#define RESULT_OUT_OF_RANGE 0x177a02u
#define RESULT_NO_APPEND 0x307202u
#define RESULT_READ_NOT_PERMITTED 0x307402u
#define RESULT_WRITE_NOT_PERMITTED 0x307602u
#define RESULT_PERMISSION_DENIED 0x320002u
#define RESULT_WRITE_FILE_OPEN 0x327202u
#define RESULT_NOT_MOUNTED 0x35f202u
#define RESULT_GENERIC_FAILURE 0x3e802u      /* a Windows failure with no console counterpart (disk error, sharing violation) */
/* Read in sdk.elf: [0x1f4cd0] CheckMountName: 0x2f5e02 null name (NullptrArgument), 0x2f6202 InvalidMountName.
 * [doc] switchbrew / nn-fuse: 6003 TooLongPath, 6364 UnsupportedOperation (what nn-fuse returns for unsupported calls). */
#define RESULT_NULLPTR_ARGUMENT 0x2f5e02u
#define RESULT_INVALID_MOUNT_NAME 0x2f6202u
#define RESULT_TOO_LONG_PATH 0x2ee602u
#define RESULT_UNSUPPORTED 0x31b802u

enum { MK_ROM = 1, MK_SAVE, MK_CACHE, MK_PLAIN /* self-tests: a writable host folder without a journal */,
       MK_DIRECT /* a host folder written directly, no working folder (Skyline's sd:) */,
       MK_FSA /* a file system implemented by guest code (nn::fs::fsa::Register, see below) */ };
typedef struct {
    char name[16];
    int kind;
    char root[MAX_PATH];                     /* committed state (or the rom folder) */
    char work[MAX_PATH];                     /* journal working folder ("" for the rom) */
    char **tomb;                             /* relative paths deleted since the last commit, '\\' separators */
    int tomb_n;
    int pend;                                /* a decided commit is pending (see the commit protocol): every deletion is recorded */
    uint64_t fsa;                            /* MK_FSA: the guest nn::fs::fsa::IFileSystem object */
} Mount;
#define MOUNT_MAX 16
static Mount g_mounts[MOUNT_MAX];
static char g_rom_root[MAX_PATH], g_user_root[MAX_PATH];
static volatile LONG g_open_log;
static CRITICAL_SECTION g_fs_lock;
static volatile LONG g_fs_lock_ready;

static void fs_lock(void) {
    if (InterlockedCompareExchange(&g_fs_lock_ready, 1, 0) == 0) { InitializeCriticalSection(&g_fs_lock); g_fs_lock_ready = 2; }
    while (g_fs_lock_ready != 2) SwitchToThread();
    EnterCriticalSection(&g_fs_lock);
}
static void fs_unlock(void) { LeaveCriticalSection(&g_fs_lock); }

static void fs_roots(void) {
    if (g_rom_root[0]) return;
    const char *env = getenv("SSBU_ROMFS"), *user = getenv("SSBU_USERDATA");
    strncpy(g_rom_root, env && env[0] ? env : "..\\romfs", MAX_PATH - 1);
    strncpy(g_user_root, user && user[0] ? user : "..\\userdata", MAX_PATH - 1);
    CreateDirectoryA(g_user_root, NULL);
}
const char *fs_user_root(void) { fs_roots(); return g_user_root; }

static int is_dir(const char *p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static void make_parents(const char *path) {             /* creates every missing folder above `path` */
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 3; *p; p++) if (*p == '\\') { *p = 0; CreateDirectoryA(tmp, NULL); *p = '\\'; }
}
/* Windows: an antivirus or the indexer can hold a file for a moment (sharing violation, or access denied while it is open):
 * tried again for up to about half a second. Elsewhere a failure is final at once. */
static int transient_failure(int attempt) {
#ifdef _WIN32
    DWORD e = GetLastError();
    if (attempt < 15 && (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED)) { Sleep(30); return 1; }
#endif
    (void)attempt;
    return 0;
}
/* The folder and everything in it; read-only attributes cleared, transient failures retried. 1 = gone afterwards. */
static int remove_tree(const char *dir) {
    char pat[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(sub);
            else for (int i = 0; exists(sub); i++) {
                SetFileAttributesA(sub, FILE_ATTRIBUTE_NORMAL);
                if (DeleteFileA(sub) || !transient_failure(i)) break;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    SetFileAttributesA(dir, FILE_ATTRIBUTE_NORMAL);       /* Windows: a read-only folder is not removed */
    for (int i = 0; exists(dir); i++) if (RemoveDirectoryA(dir) || !transient_failure(i)) break;
    return !exists(dir);
}

/* ---- tombstones */
static int tomb_find(Mount *m, const char *rel) {
    for (int i = 0; i < m->tomb_n; i++) if (_stricmp(m->tomb[i], rel) == 0) return i;
    return -1;
}
static int tomb_hides(Mount *m, const char *rel) {       /* rel itself or one of its parent folders was deleted */
    for (int i = 0; i < m->tomb_n; i++) {
        size_t n = strlen(m->tomb[i]);
        if (_strnicmp(m->tomb[i], rel, n) == 0 && (rel[n] == 0 || rel[n] == '\\')) return 1;
    }
    return 0;
}
static void tomb_add(Mount *m, const char *rel) {
    if (tomb_find(m, rel) >= 0) return;
    m->tomb = realloc(m->tomb, sizeof(char *) * (size_t)(m->tomb_n + 1));
    m->tomb[m->tomb_n++] = _strdup(rel);
}
static void tomb_remove(Mount *m, const char *rel) {
    int i = tomb_find(m, rel);
    if (i >= 0) { free(m->tomb[i]); m->tomb[i] = m->tomb[--m->tomb_n]; }
}
static void tomb_clear(Mount *m) { for (int i = 0; i < m->tomb_n; i++) free(m->tomb[i]); free(m->tomb); m->tomb = NULL; m->tomb_n = 0; }

/* ---- path resolution */
static Mount *mount_find(const char *name, size_t n) {
    for (int i = 0; i < MOUNT_MAX; i++) if (g_mounts[i].kind && strlen(g_mounts[i].name) == n && strncmp(g_mounts[i].name, name, n) == 0) return &g_mounts[i];
    return NULL;
}
/* "mount:/dir/file" -> mount and relative path with '\\' separators (no leading or trailing separator). Result code. */
static uint32_t split_path(const char *guest, Mount **mount, char *rel) {
    const char *colon = guest ? strchr(guest, ':') : NULL;
    if (!colon) return RESULT_NOT_MOUNTED;
    fs_roots();
    Mount *m = mount_find(guest, (size_t)(colon - guest));
    if (!m) return RESULT_NOT_MOUNTED;
    if (m->kind == MK_FSA) return RESULT_UNSUPPORTED;      /* a guest file system: every entry point sends those to the fsa_* functions first */
    const char *rest = colon + 1;
    while (*rest == '/' || *rest == '\\') rest++;
    snprintf(rel, MAX_PATH, "%s", rest);
    for (char *p = rel; *p; p++) if (*p == '/') *p = '\\';
    size_t len = strlen(rel);
    while (len > 0 && rel[len - 1] == '\\') rel[--len] = 0;
    *mount = m;
    return 0;
}
static void join(char *out, const char *root, const char *rel) { if (rel[0]) snprintf(out, MAX_PATH, "%s\\%s", root, rel); else snprintf(out, MAX_PATH, "%s", root); }
/* The host path that currently holds `rel` in the mount's view; returns 0 when it does not exist in the view. */
static int view_path(Mount *m, const char *rel, char *out) {
    if (m->work[0]) {
        if (tomb_hides(m, rel)) { join(out, m->work, rel); return exists(out); }      /* re-created after a deletion: only the working copy counts */
        join(out, m->work, rel);
        if (exists(out)) return 1;
    }
    join(out, m->root, rel);
    return exists(out);
}
static int view_parent_is_dir(Mount *m, const char *rel) {
    char parent[MAX_PATH], host[MAX_PATH];
    snprintf(parent, sizeof parent, "%s", rel);
    char *s = strrchr(parent, '\\');
    if (!s) return 1;                                     /* directly under the root */
    *s = 0;
    return view_path(m, parent, host) && is_dir(host);
}
/* Used by nnfsdir.c: the host folders to list for a guest directory (working folder first), 0 = no such directory. */
int fs_dir_roots(const char *guest, char roots[2][MAX_PATH], uint32_t *result) {
    Mount *m; char rel[MAX_PATH], host[MAX_PATH];
    int n = 0;
    fs_lock();
    *result = split_path(guest, &m, rel);
    if (!*result) {
        if (!view_path(m, rel, host) || !is_dir(host)) *result = RESULT_PATH_NOT_FOUND;
        else {
            if (m->work[0]) { join(roots[n], m->work, rel); if (is_dir(roots[n])) n++; }
            if (!m->work[0] || !tomb_hides(m, rel)) { join(roots[n], m->root, rel); if (is_dir(roots[n])) n++; }
        }
    }
    fs_unlock();
    return n;
}
int fs_dir_hidden(const char *guest_dir, const char *name) {      /* 1 = this entry of the committed folder was deleted in the working state */
    Mount *m; char rel[MAX_PATH], full[MAX_PATH];
    int hidden = 0;
    fs_lock();
    if (!split_path(guest_dir, &m, rel) && m->work[0]) { join(full, rel, name); hidden = tomb_hides(m, rel[0] ? full : name); }
    fs_unlock();
    return hidden;
}
uint32_t fs_delete_directory(const char *guest) {
    Mount *m; char rel[MAX_PATH], host[MAX_PATH], roots[2][MAX_PATH];
    uint32_t r;
    fs_lock();
    r = split_path(guest, &m, rel);
    if (!r && m->kind == MK_ROM) r = RESULT_PERMISSION_DENIED;
    if (!r && (!view_path(m, rel, host) || !is_dir(host))) r = RESULT_PATH_NOT_FOUND;
    fs_unlock();
    if (r) return r;
    uint32_t r2;                                          /* empty in the view? */
    int n = fs_dir_roots(guest, roots, &r2);
    for (int i = 0; i < n; i++) {
        char pat[MAX_PATH + 4];
        WIN32_FIND_DATAA fd;
        snprintf(pat, sizeof pat, "%s\\*", roots[i]);
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            int from_committed = !m->work[0] || _strnicmp(roots[i], m->work, strlen(m->work)) != 0;
            if (from_committed && m->work[0] && fs_dir_hidden(guest, fd.cFileName)) continue;       /* deleted in the working state */
            FindClose(h);
            return RESULT_DIRECTORY_NOT_EMPTY;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    fs_lock();
    if (m->work[0]) {
        char w[MAX_PATH], cm[MAX_PATH];
        join(w, m->work, rel); join(cm, m->root, rel);
        if (is_dir(w)) RemoveDirectoryA(w);
        if (m->pend || is_dir(cm)) tomb_add(m, rel);       /* (pending: it may exist only in the pending commit) */
    } else r = RemoveDirectoryA(host) ? 0 : RESULT_GENERIC_FAILURE;
    fs_unlock();
    return r;
}

/* Open files: nn::fs::FileHandle is the Windows handle itself; the open mode (1 read, 2 write, 4 allow append) and the
 * mount are kept here so reads, writes, growth and Commit behave exactly as the mode says. */
#define OPEN_MAX 1024
static struct { HANDLE h; int mode; Mount *mount; } g_open[OPEN_MAX];
static void open_remember(HANDLE h, int mode, Mount *m) {
    fs_lock();
    for (int i = 0; i < OPEN_MAX; i++) if (!g_open[i].h) { g_open[i].h = h; g_open[i].mode = mode; g_open[i].mount = m; break; }
    fs_unlock();
}
static int open_mode(HANDLE h, int forget) {             /* mode of an open handle, 7 (everything) if unknown */
    int mode = 7;
    fs_lock();
    for (int i = 0; i < OPEN_MAX; i++) if (g_open[i].h == h) { mode = g_open[i].mode; if (forget) g_open[i].h = NULL; break; }
    fs_unlock();
    return mode;
}

/* ---- guest file systems: nn::fs::fsa::Register
 * ARCropolis mounts "arc:" and "mods:" as file systems implemented in its own (lifted) code, through the nn-fuse crate
 * (github.com/Raytwo/nn-fuse f167b47); HDR, smashline and the game's hooked loaders then read "mods:/..." with the
 * ordinary nn::fs calls. The object given to Register is an nn::fs::fsa::IFileSystem, word 0 = its vtable. Slots, read
 * in sdk.elf (FileSystemAccessor 0x1f37a0 .. 0x1f4490: the offset each accessor method calls through) and identical to
 * nn-fuse's FsAccessorVtable:
 *   0 ~IFileSystem, 1 deleting destructor, 2 DoCreateFile(path, s64 size, int option), 3 DoDeleteFile(path),
 *   4 DoCreateDirectory(path), 5 DoDeleteDirectory(path), 6 DoDeleteDirectoryRecursively(path),
 *   7 DoCleanDirectoryRecursively(path), 8 DoRenameFile(old, new), 9 DoRenameDirectory(old, new),
 *   10 DoGetEntryType(DirectoryEntryType *out, path), 11 DoGetFreeSpaceSize, 12 DoGetTotalSpaceSize,
 *   13 DoOpenFile(unique_ptr<IFile> *out, path, OpenMode), 14 DoOpenDirectory(unique_ptr<IDirectory> *out, path, mode),
 *   15 DoCommit.
 * IFile ([sdk] FileAccessor 0x1f2d00 Read, 0x1f2fb0 Write, 0x1f3150 Flush, 0x1f31d0 SetSize, 0x1f3270 GetSize): 1 deleting
 *   destructor, 2 DoRead(size_t *out, s64 offset, void *buf, size_t size, const ReadOption &), 3 DoWrite(s64 offset,
 *   const void *buf, size_t size, const WriteOption &), 4 DoFlush(), 5 DoSetSize(s64), 6 DoGetSize(s64 *out).
 * IDirectory ([sdk] DirectoryAccessor 0x1f2540 Read, 0x1f25a0 GetEntryCount): 1 deleting destructor, 2 DoRead(s64 *out,
 *   DirectoryEntry *buf, s64 count), 3 DoGetEntryCount(s64 *out).
 * Every method returns an nn::Result; it runs on the calling guest thread (call_guest, x0..x30 saved around it) and no
 * lock of this file is held meanwhile (the guest code may itself call nn::fs). Out-parameters live in host memory.
 * The path handed down is what follows "name:", with a leading '/' (nn-fuse strips it with unwrap()).
 * Handles given to the guest: FSA_FILE_TAG | slot and FSA_DIR_TAG | slot, never a Windows HANDLE (small numbers) nor a
 * HostDir pointer (nnfsdir.c), so every nn::fs entry point can tell them apart.
 * nn-fuse's IDirectory::DoRead always fills from the FIRST entry and writes exactly `count` entries (one past the end
 * panics): a directory read asks for exactly DoGetEntryCount entries into host memory and gives the caller those after its
 * position, so ReadDirectory consumes the listing as the SDK's own directories do. */
#define FSA_TAG_MASK 0xffffff0000000000ull
#define FSA_FILE_TAG 0x4653460000000000ull                 /* "FSF" */
#define FSA_DIR_TAG 0x4653440000000000ull                  /* "FSD" */
#define FSA_FILES 1024
#define FSA_DIRS 256
#define FSA_PATH_MAX 0x302                                 /* EntryNameLengthMax 0x300 + '/' + NUL */
static struct { uint64_t obj; Mount *mount; int mode; } g_fsa_file[FSA_FILES];
static struct { uint64_t obj; Mount *mount; int64_t pos; } g_fsa_dir[FSA_DIRS];

static uint32_t fsa_call(Ctx *c, uint64_t obj, int slot, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t saved[31], fn = rd64(rd64(obj) + (uint64_t)slot * 8);
    memcpy(saved, c->x, sizeof saved);
    c->x[0] = obj; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[4] = a4; c->x[5] = a5;
    call_guest(c, fn);
    uint32_t r = (uint32_t)c->x[0];
    memcpy(c->x, saved, sizeof saved);
    return r;
}
#define HP(p) ((uint64_t)(uintptr_t)(p))
/* The guest file system that `guest` names ("name:/rest"), or NULL when the name is not such a mount (then the host
 * code handles the path, including "not mounted"). `sub` receives "/rest"; *r a path error. */
static Mount *fsa_mount_of(const char *guest, char *sub, uint32_t *r) {
    const char *colon = guest ? strchr(guest, ':') : NULL;
    if (!colon) return NULL;
    fs_lock();
    Mount *m = mount_find(guest, (size_t)(colon - guest));
    int is_fsa = m && m->kind == MK_FSA;
    fs_unlock();
    if (!is_fsa) return NULL;
    const char *rest = colon + 1;
    size_t n = strlen(rest);
    *r = 0;
    sub[0] = 0;
    if (n + 2 > FSA_PATH_MAX) { *r = RESULT_TOO_LONG_PATH; return m; }
    if (rest[0] != '/') { sub[0] = '/'; memcpy(sub + 1, rest, n + 1); }
    else memcpy(sub, rest, n + 1);
    return m;
}
/* slot of a guest-file-system file handle: > 0 valid, 0 = not such a handle (a host one), -1 = stale */
static int fsa_file_slot(uint64_t h) {
    if ((h & FSA_TAG_MASK) != FSA_FILE_TAG) return 0;
    uint64_t i = h & 0xffffffffu;
    return i > 0 && i < FSA_FILES && g_fsa_file[i].obj ? (int)i : -1;
}
static int fsa_dir_slot(uint64_t h) {
    if ((h & FSA_TAG_MASK) != FSA_DIR_TAG) return 0;
    uint64_t i = h & 0xffffffffu;
    return i > 0 && i < FSA_DIRS && g_fsa_dir[i].obj ? (int)i : -1;
}
static volatile LONG g_fsa_log;
static void fsa_log(const char *what, const char *path, uint32_t r) {
    if (InterlockedIncrement(&g_fsa_log) <= 400 || (r && r != RESULT_PATH_NOT_FOUND))
        trace_printf("     fs (guest file system) %s \"%s\" -> %x\n", what, path ? path : "", r);
    static int on = -1; if (on < 0) on = getenv("SSBU_FSLOG") != NULL;
    if (on) printf("fslog: guest fs %s \"%s\" -> %x\n", what, path ? path : "", r);
}

static uint32_t fsa_open_file(Ctx *c, Mount *m, const char *guest, const char *sub, int mode, uint64_t out) {
    uint64_t file = 0;
    uint32_t r = fsa_call(c, m->fsa, 13, HP(&file), HP(sub), (uint64_t)(uint32_t)mode, 0, 0);
    if (!r && !file) r = RESULT_GENERIC_FAILURE;
    if (!r) {
        int slot = 0;
        fs_lock();
        for (int i = 1; i < FSA_FILES && !slot; i++) if (!g_fsa_file[i].obj) { g_fsa_file[i].obj = file; g_fsa_file[i].mount = m; g_fsa_file[i].mode = mode & 7; slot = i; }
        fs_unlock();
        if (!slot) { fsa_call(c, file, 1, 0, 0, 0, 0, 0); r = RESULT_GENERIC_FAILURE; }
        else if (out) wr64(out, FSA_FILE_TAG | (uint64_t)slot);
    }
    if (r && out) wr64(out, 0);
    fsa_log("open", guest, r);
    return r;
}
static void fsa_close_file(Ctx *c, int slot) {             /* [sdk] the FileAccessor deletes its unique_ptr<IFile> */
    fs_lock();
    uint64_t file = g_fsa_file[slot].obj;
    g_fsa_file[slot].obj = 0;
    fs_unlock();
    if (file) fsa_call(c, file, 1, 0, 0, 0, 0, 0);
}
/* DoRead; *got = bytes read. ReadNotPermitted without the read mode ([sdk] FileAccessor::Read). */
static uint32_t fsa_read(Ctx *c, int slot, int64_t offset, uint64_t buf, uint64_t size, uint64_t *got) {
    uint64_t option = 0;
    *got = 0;
    if (!(g_fsa_file[slot].mode & 1)) return RESULT_READ_NOT_PERMITTED;
    if (offset < 0) return RESULT_OUT_OF_RANGE;
    if (!size) return 0;
    return fsa_call(c, g_fsa_file[slot].obj, 2, HP(got), (uint64_t)offset, buf, size, HP(&option));
}
static uint32_t fsa_get_size(Ctx *c, int slot, int64_t *size) {
    *size = 0;
    return fsa_call(c, g_fsa_file[slot].obj, 6, HP(size), 0, 0, 0, 0);
}
static uint32_t fsa_write(Ctx *c, int slot, int64_t offset, uint64_t buf, uint64_t size, uint64_t option) {
    uint64_t zero = 0;
    int64_t cur;
    int mode = g_fsa_file[slot].mode;
    if (!(mode & 2)) return RESULT_WRITE_NOT_PERMITTED;
    if (!(mode & 4)) {                                      /* a file only grows in append mode */
        uint32_t r = fsa_get_size(c, slot, &cur);
        if (r) return r;
        if (offset + (int64_t)size > cur) return RESULT_NO_APPEND;
    }
    return fsa_call(c, g_fsa_file[slot].obj, 3, (uint64_t)offset, buf, size, option ? option : HP(&zero), 0);
}
/* path operations: slot 2 create file, 3 delete file, 4 create directory, 5 delete directory, 6 delete recursively,
 * 7 clean recursively, 10 get entry type */
static uint32_t fsa_path_op(Ctx *c, Mount *m, const char *what, const char *guest, const char *sub, uint32_t perr, int slot, uint64_t a2, uint64_t a3) {
    uint32_t r = perr ? perr : fsa_call(c, m->fsa, slot, HP(sub), a2, a3, 0, 0);
    fsa_log(what, guest, r);
    return r;
}
static uint32_t fsa_get_entry_type(Ctx *c, Mount *m, const char *guest, const char *sub, uint32_t perr, uint64_t out) {
    uint32_t type = 0;
    uint32_t r = perr ? perr : fsa_call(c, m->fsa, 10, HP(&type), HP(sub), 0, 0, 0);
    if (!r && out) wr32(out, type);
    fsa_log("entry type", guest, r);
    return r;
}
static uint32_t fsa_rename(Ctx *c, int slot, const char *from, const char *to) {     /* slot 8 file, 9 directory */
    char a[FSA_PATH_MAX], b[FSA_PATH_MAX];
    uint32_t ra = 0, rb = 0;
    Mount *ma = fsa_mount_of(from, a, &ra), *mb = fsa_mount_of(to, b, &rb);
    uint32_t r = ma != mb ? RESULT_UNSUPPORTED : ra ? ra : rb ? rb : fsa_call(c, ma->fsa, slot, HP(a), HP(b), 0, 0, 0);   /* both on one guest file system */
    fsa_log(slot == 8 ? "rename file" : "rename directory", from, r);
    return r;
}
/* nnfsdir.c: directories of guest file systems */
int fs_fsa_dir_handle(uint64_t h) { return fsa_dir_slot(h) != 0; }
uint32_t fs_fsa_open_directory(Ctx *c, const char *guest, int mode, uint64_t out, int *handled) {
    char sub[FSA_PATH_MAX];
    uint32_t r = 0;
    Mount *m = fsa_mount_of(guest, sub, &r);
    *handled = m != NULL;
    if (!m) return 0;
    uint64_t dir = 0;
    if (!r) r = fsa_call(c, m->fsa, 14, HP(&dir), HP(sub), (uint64_t)(uint32_t)mode, 0, 0);
    if (!r && !dir) r = RESULT_GENERIC_FAILURE;
    if (!r) {
        int slot = 0;
        fs_lock();
        for (int i = 1; i < FSA_DIRS && !slot; i++) if (!g_fsa_dir[i].obj) { g_fsa_dir[i].obj = dir; g_fsa_dir[i].mount = m; g_fsa_dir[i].pos = 0; slot = i; }
        fs_unlock();
        if (!slot) { fsa_call(c, dir, 1, 0, 0, 0, 0, 0); r = RESULT_GENERIC_FAILURE; }
        else if (out) wr64(out, FSA_DIR_TAG | (uint64_t)slot);
    }
    if (r && out) wr64(out, 0);
    fsa_log("open directory", guest, r);
    return r;
}
uint32_t fs_fsa_dir_count(Ctx *c, uint64_t h, int64_t *count) {      /* entries not read yet */
    int slot = fsa_dir_slot(h);
    *count = 0;
    if (slot <= 0) return RESULT_GENERIC_FAILURE;
    int64_t n = 0;
    uint32_t r = fsa_call(c, g_fsa_dir[slot].obj, 3, HP(&n), 0, 0, 0, 0);
    if (!r) *count = n > g_fsa_dir[slot].pos ? n - g_fsa_dir[slot].pos : 0;
    return r;
}
uint32_t fs_fsa_dir_read(Ctx *c, uint64_t h, uint64_t entries, int64_t max, int64_t *got) {
    int slot = fsa_dir_slot(h);
    *got = 0;
    if (slot <= 0) return RESULT_GENERIC_FAILURE;
    if (max <= 0) return 0;
    int64_t total = 0, read = 0, pos = g_fsa_dir[slot].pos;
    uint32_t r = fsa_call(c, g_fsa_dir[slot].obj, 3, HP(&total), 0, 0, 0, 0);
    if (r || total <= pos) return r;
    uint8_t *all = calloc((size_t)total, 0x310);           /* sizeof(nn::fs::DirectoryEntry) */
    if (!all) return RESULT_GENERIC_FAILURE;
    r = fsa_call(c, g_fsa_dir[slot].obj, 2, HP(&read), HP(all), (uint64_t)total, 0, 0);
    if (!r) {
        if (read > total) read = total;
        int64_t n = read - pos < max ? read - pos : max;
        if (n > 0) { memcpy((void *)(uintptr_t)entries, all + pos * 0x310, (size_t)n * 0x310); g_fsa_dir[slot].pos = pos + n; *got = n; }
    }
    free(all);
    return r;
}
void fs_fsa_dir_close(Ctx *c, uint64_t h) {
    int slot = fsa_dir_slot(h);
    if (slot <= 0) return;
    fs_lock();
    uint64_t dir = g_fsa_dir[slot].obj;
    g_fsa_dir[slot].obj = 0;
    fs_unlock();
    if (dir) fsa_call(c, dir, 1, 0, 0, 0, 0, 0);
}

/* nn::fs::detail::CheckMountName(const char *) [sdk 0x1f4cd0]: a name is valid unless it is null, starts with '@'
 * (reserved), is empty or a single drive letter, holds ':' or '/', or is longer than 15 characters. It does not look at
 * the mount table (Register answers MountNameAlreadyExists). */
static uint32_t check_mount_name(const char *name) {
    if (!name) return RESULT_NULLPTR_ARGUMENT;
    size_t n = strlen(name);
    if (name[0] == '@' || n == 0 || n > 15 || (n == 1 && ((name[0] | 0x20) >= 'a' && (name[0] | 0x20) <= 'z')) || strpbrk(name, ":/")) return RESULT_INVALID_MOUNT_NAME;
    return 0;
}
static void fs_CheckMountName(Ctx *c) { RET(check_mount_name(STR(0))); }
/* nn::fs::fsa::Register(const char *name, std::unique_ptr<IFileSystem> &&fs): the mount takes the object over (the
 * caller's unique_ptr is left empty, as after a move); Unmount deletes it. */
static void fs_fsa_Register(Ctx *c) {
    const char *name = STR(0);
    uint64_t holder = A1, fsobj = A1 ? rd64(A1) : 0;
    uint32_t r = check_mount_name(name);
    if (!r && !fsobj) r = RESULT_NULLPTR_ARGUMENT;
    if (!r) {
        Mount *m = NULL;
        fs_lock();
        if (mount_find(name, strlen(name))) r = RESULT_MOUNT_NAME_EXISTS;
        for (int i = 0; i < MOUNT_MAX && !m && !r; i++) if (!g_mounts[i].kind) m = &g_mounts[i];
        if (!r && !m) r = RESULT_GENERIC_FAILURE;
        if (!r) { memset(m, 0, sizeof *m); strcpy(m->name, name); m->fsa = fsobj; m->kind = MK_FSA; wr64(holder, 0); }
        fs_unlock();
    }
    printf("nn::fs::fsa::Register(\"%s\", guest file system %llx) -> %x\n", name ? name : "(null)", (unsigned long long)fsobj, r);
    trace_printf("     fs fsa::Register \"%s\" object %llx vtable %llx -> %x\n", name ? name : "(null)", (unsigned long long)fsobj, fsobj ? (unsigned long long)rd64(fsobj) : 0ull, r);
    RET(r);
}
/* nn::fs::detail::Allocate(size_t) / Deallocate(void *, size_t): the SDK's file-system allocator; nn-fuse makes its
 * accessor objects with it. The SDK's default allocator is malloc / free, which the game defines itself (13.0.4: [game]
 * 0x710353be80 / 0x710353bf00; found by name in main's symbol table): the same heap the plugins' objects come from anyway. */
static uint64_t game_call1(Ctx *c, uint64_t *fn_cache, const char *name, uint64_t a0) {
    uint64_t saved[31], fn = *fn_cache ? *fn_cache : (*fn_cache = main_export(name));
    if (!fn) guest_trap("nn::fs::detail::Allocate / Deallocate: main does not define the allocator", c->x[30] - 4);
    memcpy(saved, c->x, sizeof saved);
    c->x[0] = a0;
    call_guest(c, fn);
    uint64_t r = c->x[0];
    memcpy(c->x, saved, sizeof saved);
    return r;
}
static uint64_t g_game_malloc, g_game_free;
static void fs_detail_Allocate(Ctx *c) { RET(game_call1(c, &g_game_malloc, "malloc", A0)); }
static void fs_detail_Deallocate(Ctx *c) { if (A0) game_call1(c, &g_game_free, "free", A0); }

/* ---- journal: commit and recovery
 *
 * THE COMMIT PROTOCOL. Folders and files next to a journaled root R (<userdata>\save, <userdata>\cache); <id> below is
 * "<pid>-<run>-<n>": the pid, this run's start time and a counter, so a name is never used twice, not even by a later start
 * that gets the same pid (a Steam runtime container can give the game the same pid every time). Earlier builds used "<pid>".
 *   R                  the committed state. Changed only by APPLY of a decided marker.
 *   R.work.<id>   (E)  a mount's working folder: the uncommitted changes of that mount (copy-on-write files, new files),
 *                      with the mount's tombstones (in memory) for its deletions. Never named by a marker of this build, so
 *                      an uncommitted write can never be applied.
 *   R.stage.<id>  (S)  a frozen copy of E taken by one Commit: what that decided commit moves into R. A new name for every
 *                      Commit (a folder whose removal failed is never mistaken for an empty one); written only before its
 *                      marker exists; once a marker names it, only APPLY's moves take files out of it; removed only after
 *                      its marker is gone.
 *   R.journal.<id>     the marker being written.
 *   R.commit      (K)  the marker. Lines: "W <folder the files come from>", "R <root>", then "D rel" (delete), then "M rel"
 *                      (make folder) and "F rel" (move file) in the order of a walk of W. All D lines come before the first
 *                      F line. Earlier builds wrote the same lines with W = their E; those are applied the same way.
 * A commit is DECIDED once K is renamed into place (on POSIX: and its folder is synced). Every step below runs under the
 * commit lock: a named mutex on Windows, flock of R's parent folder elsewhere (plat_posix.c's CreateMutexA has no names).
 * When the lock cannot be taken, Commit is refused, recovery does nothing, and a mount over a pending commit is refused.
 *
 * Commit (fs_Commit), states and what a crash or a failure leaves:
 *   C1 K exists (an earlier decided commit, of any process, is unfinished, or its marker cannot be read): APPLY it. If that
 *      fails the new Commit is refused and nothing else changes -- a commit is never decided over an unfinished one.
 *   C2 E is copied into a new S; S's files are flushed and its folders synced.
 *   C3 the marker is written to a new R.journal.<id> and flushed.  Crash or failure in C2..C3: K absent, R = OLD state;
 *                                                                    S and the journal are dropped (now, or by the sweep).
 *   C4 R.journal.<id> is renamed to K WITHOUT replacing: if a K exists after all (a committer without the lock: an older
 *      build), the rename fails and this commit is not decided. POSIX: K's folder is then synced (3 tries): if that fails,
 *      K stays but is not applied now -- the moves of C5 must not reach the disk before K does; the commit is pending.
 *   C5 APPLY K.                                                      Crash in C4..C5: K present, the next mount APPLIES it:
 *                                                                    R = NEW state. Failure in C5: K stays, see "pending".
 *   C6 success: E is emptied (a new E if that fails) and the tombstones dropped (R now holds all of it).
 * APPLY(K), repeatable from any point:
 *   A1 if every F source still exists in W, the D phase may be unfinished: each D target is deleted (read-only cleared,
 *      sharing violations retried on Windows) and must be GONE afterwards, else APPLY stops before the first F (-3). If some
 *      F source is gone, an F ran, so every D was done: no D runs again (a "D a" + "F a" pair would delete the new a).
 *   A2 M and F lines in order; an F whose source is gone was moved already. A folder that cannot be made, or a move that
 *      fails, stops APPLY (-2): the D phase is complete, the remaining sources are still in W.
 *   A3 K is deleted and must be GONE (an antivirus can hold it open on Windows), else -4; then R's folder is synced.
 *   A4 then (only then) a stage folder W is removed. (An older build's ".work." W is left to the sweep.)
 *   A K that exists but cannot be read is -1: nothing is done, and it counts as pending.
 * Pending: K exists and APPLY failed (a full disk, a read-only file or folder, a sharing violation, an unreadable K).
 *   - The committing mount keeps E and its tombstones: they still describe the decided state, which is what the game sees.
 *     New writes go to E only; S is untouched, so they are never applied by the pending commit.
 *   - A mount that finds K pending (after its own APPLY attempt failed) starts E as a copy of W, plus tombstones for the D
 *     lines when the D phase is unfinished: it sees the decided state too. If K cannot be read, or that copy fails, or the
 *     lock is not held, the mount fails (the game sees an error, not a wrong state; everything stays for the next attempt).
 *   - While a commit is pending, every deletion records a tombstone, since a file may exist only in the pending commit,
 *     which C1 moves into R before this mount's own changes.
 *   - K is APPLIED again by the next Commit (C1), by Unmount, and by every mount.
 * The sweep (journal_recover, at every mount and Unmount) drops the working folders, stage folders and
 * journals of runs that are over (another pid that no longer runs; this pid with another <run>; an older build's name with
 * this pid). While any K exists -- readable or not -- it drops no working or stage folder: one of them is K's W.
 * Unmount: APPLY a pending K (best effort) and sweep, then remove E: uncommitted changes are dropped.
 * Every crash point thus converges to the complete old state (K never written) or the complete state of the last decided
 * commit (K written; APPLY finishes it), and the game's view of a mounted save is the last decided state plus its own
 * uncommitted changes. Not covered: two processes changing the same save at once (each commit is whole; the later one
 * wins), and an older build running at the same time as this one (it does not know stage folders or the POSIX lock; it
 * can still replace a marker this build decided). */
typedef struct { intptr_t h; int ok; } CommitLock;       /* ok 0: the lock could not be taken (see "the commit lock") */
#ifdef _WIN32
static CommitLock commit_mutex(const char *root) {
    (void)root;
    CommitLock l = {0, 0};
    HANDLE mx = CreateMutexA(NULL, FALSE, "ssbu_native_save_commit");
    if (mx) { DWORD w = WaitForSingleObject(mx, INFINITE); l.ok = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED; }   /* abandoned: its owner died; ours now */
    if (mx && !l.ok) { CloseHandle(mx); mx = NULL; }
    l.h = (intptr_t)mx;
    return l;
}
static void commit_done(CommitLock l) { if (l.h) { ReleaseMutex((HANDLE)l.h); CloseHandle((HANDLE)l.h); } }
#else
static CommitLock commit_mutex(const char *root) {       /* flock of the folder that holds R: other processes and other threads (own descriptions) */
    CommitLock l = {-1, 0};
    char dir[MAX_PATH];
    host_path_resolve(root, dir, sizeof dir);
    char *sl = strrchr(dir, '/');
    if (!sl) strcpy(dir, "."); else if (sl == dir) dir[1] = 0; else *sl = 0;
    int fd = open(dir, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return l;
    int r;
    while ((r = flock(fd, LOCK_EX)) != 0 && errno == EINTR) {}
    if (r) { close(fd); return l; }                       /* (ENOLCK: a network file system without locks) */
    l.h = fd; l.ok = 1;
    return l;
}
static void commit_done(CommitLock l) { if (l.ok) close((int)l.h); }
#endif
/* Names of working, stage and journal files that no other process and no earlier run with the same pid ever uses:
 * "<root>.<kind>.<pid>-<run>-<n>" (<run> = this process's start time, hex). A Steam runtime container can give the game the
 * same pid at every start, so a pid alone is not enough; a name is never reused, so a folder left by a failed removal is
 * never mistaken for a fresh one. Earlier builds named them "<root>.<kind>.<pid>". */
static unsigned long long g_run_id;
static volatile LONG g_name_seq;
static unsigned long long run_id(void) {
    if (!g_run_id) { FILETIME ft; GetSystemTimeAsFileTime(&ft); g_run_id = ((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) | 1; }
    return g_run_id;
}
static void journal_name(char *out, const char *root, const char *kind) {
    snprintf(out, MAX_PATH, "%s.%s.%lu-%llx-%ld", root, kind, (unsigned long)GetCurrentProcessId(), run_id(), (long)InterlockedIncrement(&g_name_seq));
}
/* "<root leaf>.<kind>.<pid>[-<run>-<n>]" next to the root: its kind ("work", "stage", "journal"), pid and run (0: an earlier
 * build's name); NULL for anything else. */
static const char *journal_entry(const char *root, const char *name, DWORD *pid, unsigned long long *run) {
    static const char *const kinds[] = {"work", "stage", "journal"};
    const char *leaf = root + strlen(root);
    while (leaf > root && leaf[-1] != '\\' && leaf[-1] != '/') leaf--;
    size_t n = strlen(leaf);
    if (_strnicmp(name, leaf, n) || name[n] != '.') return NULL;
    const char *k = name + n + 1;
    for (int i = 0; i < 3; i++) {
        size_t kl = strlen(kinds[i]);
        if (strncmp(k, kinds[i], kl) || k[kl] != '.' || k[kl + 1] < '0' || k[kl + 1] > '9') continue;
        char *e;
        *pid = (DWORD)strtoul(k + kl + 1, &e, 10);
        *run = *e == '-' ? strtoull(e + 1, NULL, 16) : 0;
        return kinds[i];
    }
    return NULL;
}
static int delete_entry(const char *path) {               /* 1 = gone afterwards */
    if (is_dir(path)) remove_tree(path);
    else for (int i = 0; exists(path); i++) {
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);  /* Windows: DeleteFileA refuses a read-only file */
        if (DeleteFileA(path) || !transient_failure(i)) break;
    }
    return !exists(path);
}
static int move_over(const char *from, const char *to) {
    if (exists(to)) SetFileAttributesA(to, FILE_ATTRIBUTE_NORMAL);   /* Windows: MoveFileEx does not replace a read-only file */
    for (int i = 0;; i++) {
        if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 1;
        if (!transient_failure(i)) return 0;
    }
}
static void sync_folder(const char *dir) {                /* the folder's own entries on disk (POSIX; NTFS journals its metadata) */
#ifndef _WIN32
    char child[MAX_PATH];
    join(child, dir, "-");
    host_fsync_parent(child);
#else
    (void)dir;
#endif
}
/* A copy of the tree `from` as `to` (folders and files). durable: every folder synced after its entries. 1 = complete. */
static int copy_tree(const char *from, const char *to, int durable) {
    char pat[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    WIN32_FIND_DATAA fd;
    int ok = CreateDirectoryA(to, NULL) || is_dir(to);
    snprintf(pat, sizeof pat, "%s\\*", from);
    HANDLE h = ok ? FindFirstFileA(pat, &fd) : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE) return ok && !is_dir(from);      /* nothing to copy; a folder that cannot be listed fails (it lists "." and "..") */
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        join(a, from, fd.cFileName); join(b, to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!copy_tree(a, b, durable)) ok = 0; }
        else if (!CopyFileA(a, b, FALSE)) ok = 0;
        else SetFileAttributesA(b, FILE_ATTRIBUTE_NORMAL);
    } while (ok && FindNextFileA(h, &fd));
    FindClose(h);
    if (ok && durable) sync_folder(to);
    return ok;
}
/* The M and F lines of every entry of the folder, depth first; each file is flushed first. 1 = every file reached the disk. */
static int journal_list(FILE *j, const char *work, const char *rel) {
    char dir[MAX_PATH], pat[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAA fd;
    int ok = 1;
    join(dir, work, rel);
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (rel[0]) snprintf(sub, sizeof sub, "%s\\%s", rel, fd.cFileName); else snprintf(sub, sizeof sub, "%s", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { fprintf(j, "M\t%s\n", sub); if (!journal_list(j, work, sub)) ok = 0; }
        else {
            char full[MAX_PATH];
            join(full, work, sub);
            HANDLE f = CreateFileA(full, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
            if (f == INVALID_HANDLE_VALUE || !FlushFileBuffers(f)) ok = 0;
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            fprintf(j, "F\t%s\n", sub);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}
static int marker_line(FILE *j, char *line, size_t cap) {  /* next line without its end; 0 at the end of the file */
    if (!fgets(line, (int)cap, j)) return 0;
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
    return 1;
}
/* Opens a marker and reads its W and R (either may be "") and whether its D phase is over (some F source is gone, see A1).
 * The file is positioned at its start again. */
static FILE *marker_open(const char *marker, char *work, char *root, int *deletes_done) {
    char line[MAX_PATH + 8], a[MAX_PATH];
    FILE *j = fopen(marker, "r");
    work[0] = root[0] = 0; *deletes_done = 0;
    if (!j) return NULL;
    while (marker_line(j, line, sizeof line)) {
        if (strlen(line) < 3) continue;
        if (line[0] == 'W') snprintf(work, MAX_PATH, "%s", line + 2);
        else if (line[0] == 'R') snprintf(root, MAX_PATH, "%s", line + 2);
        else if (line[0] == 'F' && work[0] && !*deletes_done) { join(a, work, line + 2); *deletes_done = !exists(a); }
    }
    rewind(j);
    return j;
}
static int is_stage(const char *folder) { const char *leaf = strrchr(folder, '\\'); return strstr(leaf ? leaf : folder, ".stage.") != NULL; }
/* APPLY (A1..A4 above). >= 0: done (the number of changes); -1 no marker, -2 a folder or a move failed, -3 a deletion failed,
 * -4 the marker could not be removed. */
static int journal_apply(const char *marker) {
    char line[MAX_PATH + 8], work[MAX_PATH], root[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    int n = 0, deletes_done;
    FILE *j = marker_open(marker, work, root, &deletes_done);
    if (!j) return -1;
    while (root[0] && work[0] && marker_line(j, line, sizeof line)) {
        if (strlen(line) < 3) continue;
        const char *arg = line + 2;
        if (line[0] == 'D') {
            n++;
            if (deletes_done) continue;
            join(a, root, arg);
            if (!delete_entry(a)) { fclose(j); return -3; }           /* before any F: the next APPLY does the D phase again */
#ifndef _WIN32
            host_fsync_parent(a);                         /* on disk before the marker goes: a deleted file must not come back */
#endif
        } else if (line[0] == 'M') {
            join(a, root, arg); make_parents(a); CreateDirectoryA(a, NULL);
            if (!is_dir(a)) { fclose(j); return -2; }
#ifndef _WIN32
            host_fsync_parent(a);
#endif
        } else if (line[0] == 'F') {
            join(a, work, arg); join(b, root, arg);
            if (exists(a)) { make_parents(b); if (!move_over(a, b)) { fclose(j); return -2; } }
            n++;
        }
    }
    fclose(j);
    if (!delete_entry(marker)) return -4;                 /* the commit stays pending: replaying it is safe, W is never reused while it exists */
#ifndef _WIN32
    host_fsync_parent(marker);                            /* else a crash could replay this marker later */
#endif
    if (work[0] && is_stage(work)) remove_tree(work);
    return n;
}
/* A decided commit of `root` that is not finished: 1, and `work` = its W (the folder that holds the rest of it); -1 when its
 * marker exists but cannot be read now (Windows: an antivirus or a sync client holding it; POSIX: EMFILE, EIO); 0 none. */
static int commit_pending(const char *root, char *work) {
    char marker[MAX_PATH], r[MAX_PATH];
    int done;
    snprintf(marker, sizeof marker, "%s.commit", root);
    FILE *j = marker_open(marker, work, r, &done);
    if (!j) return exists(marker) ? -1 : 0;
    fclose(j);
    return 1;
}
static int pid_alive(DWORD pid) {
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p) return GetLastError() == ERROR_ACCESS_DENIED;
    int alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
    CloseHandle(p);
    return alive;
}
/* Before a save-type folder is mounted, and at Unmount: APPLY a decided commit that is not finished, and drop the working
 * folders, stage folders and half-written journals of runs that are over. While any marker exists -- readable or not --
 * no working or stage folder is dropped: one of them is the rest of the decided commit. Without the lock nothing is done. */
static void journal_recover(const char *root) {
    char marker[MAX_PATH], pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    CommitLock mx = commit_mutex(root);
    if (!mx.ok) { printf("nn::fs: the commit lock of %s could not be taken: recovery skipped\n", root); return; }
    snprintf(marker, sizeof marker, "%s.commit", root);
    if (exists(marker)) {
        int n = journal_apply(marker);
        if (n >= 0) printf("nn::fs: a commit of %s was interrupted after it was decided: finished now (%d changes)\n", root, n);
        else printf("nn::fs: a decided commit of %s could not be finished yet (%d); it is kept and tried again\n", root, n);
        trace_printf("     fs journal: interrupted commit of %s rolled forward (%d)\n", root, n);
    }
    int marker_left = exists(marker);
    snprintf(pat, sizeof pat, "%s.*", root);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            DWORD pid;
            unsigned long long run;
            const char *kind = journal_entry(root, fd.cFileName, &pid, &run);
            if (!kind || !pid || (marker_left && strcmp(kind, "journal"))) continue;
            if (pid == GetCurrentProcessId() ? run == run_id() : pid_alive(pid)) continue;    /* (our pid, another run: an earlier start) */
            snprintf(path, sizeof path, "%s\\..\\%s", root, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(path); else delete_entry(path);
            trace_printf("     fs journal: uncommitted %s of process %lu discarded (%s)\n", kind, (unsigned long)pid, fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    commit_done(mx);
}
/* A mount that finds a decided commit still pending starts from it (see "Pending"): E = a copy of its W, plus tombstones
 * for its D lines while its D phase is unfinished. Commit lock held. 1 = done (or nothing pending). */
static int seed_from_pending(Mount *m) {
    char marker[MAX_PATH], line[MAX_PATH + 8], work[MAX_PATH], root[MAX_PATH];
    int deletes_done;
    snprintf(marker, sizeof marker, "%s.commit", m->root);
    FILE *j = marker_open(marker, work, root, &deletes_done);
    if (!j) return 1;
    int ok = !work[0] || !is_dir(work) || copy_tree(work, m->work, 0);       /* (no W left: every move was done) */
    while (ok && !deletes_done && marker_line(j, line, sizeof line))
        if (strlen(line) >= 3 && line[0] == 'D') tomb_add(m, line + 2);
    fclose(j);
    m->pend = 1;
    trace_printf("     fs journal: %s mounted over a pending commit (from %s): %s\n", m->root, work, ok ? "its state is the working state" : "COPY FAILED");
    return ok;
}
static uint32_t mount_add(const char *name, int kind, const char *root) {
    Mount *m = NULL;
    size_t n = name ? strlen(name) : 0;
    if (!n || n >= sizeof m->name) return RESULT_GENERIC_FAILURE;
    fs_lock();
    if (mount_find(name, n)) { fs_unlock(); return RESULT_MOUNT_NAME_EXISTS; }
    for (int i = 0; i < MOUNT_MAX && !m; i++) if (!g_mounts[i].kind) m = &g_mounts[i];
    if (!m) { fs_unlock(); return RESULT_GENERIC_FAILURE; }
    memset(m, 0, sizeof *m);
    strcpy(m->name, name);
    snprintf(m->root, MAX_PATH, "%s", root);
    if (kind != MK_ROM && kind != MK_DIRECT) {             /* a direct folder (sd:) is written at once: no working folder */
        char pending[MAX_PATH];
        CommitLock mx = commit_mutex(root);
        journal_name(m->work, root, "work");               /* a new name: nothing of an earlier mount or run is in it */
        int pend = commit_pending(root, pending);
        const char *why = pend < 0 ? "its marker cannot be read now" : pend && !mx.ok ? "the commit lock could not be taken" : NULL;
        if (!why && !((CreateDirectoryA(m->work, NULL) || is_dir(m->work)))) why = "the working folder cannot be made";
        if (!why && pend && !seed_from_pending(m)) why = "it could not be copied (disk full?)";
        commit_done(mx);
        if (why) {                                         /* the game sees an error, never a state that is not the decided one */
            if (pend) printf("nn::fs: %s has a decided commit that is not finished and %s: mount refused\n", root, why);
            else printf("nn::fs: %s: %s: mount refused\n", root, why);
            remove_tree(m->work);                          /* (a name of this run only) */
            tomb_clear(m);
            memset(m, 0, sizeof *m);
            fs_unlock();
            return RESULT_GENERIC_FAILURE;
        }
    }
    m->kind = kind;
    fs_unlock();
    return 0;
}

static void save_dir(char *dir, const char *leaf) { fs_roots(); snprintf(dir, MAX_PATH, "%s\\%s", g_user_root, leaf); }
/* Save data of the one local user lives in <userdata>\save. It exists once EnsureSaveData has created it;
 * MountSaveData of save data that does not exist fails as on the console. */
static int save_exists(void) { char dir[MAX_PATH]; save_dir(dir, "save"); return is_dir(dir); }
static void fs_MountRom(Ctx *c) {                        /* (name, cache buffer, cache size): the buffer holds the RomFS metadata cache of the SDK's reader; the host reads the extracted tree */
    fs_roots();
    uint32_t r = mount_add(STR(0), MK_ROM, g_rom_root);
    trace_printf("     fs mount rom \"%s\" -> %s (%x)\n", STR(0), g_rom_root, r);
    RET(r);
}
static void fs_MountSaveData(Ctx *c) {                   /* (const char *name, const Uid &) */
    char dir[MAX_PATH];
    save_dir(dir, "save");
    if (!save_exists()) { trace_printf("     fs mount save data \"%s\": no save data (EnsureSaveData was not called)\n", STR(0)); RET(RESULT_TARGET_NOT_FOUND); return; }
    journal_recover(dir);
    uint32_t r = mount_add(STR(0), MK_SAVE, dir);
    trace_printf("     fs mount save data \"%s\" -> %s, journal working folder %s.work.%lu (%x)\n", STR(0), dir, dir, (unsigned long)GetCurrentProcessId(), r);
    RET(r);
}
static void fs_MountCacheStorage(Ctx *c) {               /* (const char *name): the application's cache storage, created on first use */
    char dir[MAX_PATH];
    save_dir(dir, "cache");
    CreateDirectoryA(dir, NULL);
    journal_recover(dir);
    uint32_t r = mount_add(STR(0), MK_CACHE, dir);
    trace_printf("     fs mount cache storage \"%s\" -> %s (%x)\n", STR(0), dir, r);
    RET(r);
}
static void fs_Unmount(Ctx *c) {                         /* (const char *name): uncommitted changes are dropped */
    const char *name = STR(0);
    fs_lock();
    Mount *m = name ? mount_find(name, strlen(name)) : NULL;
    if (m && m->kind == MK_FSA) {                         /* guest file system: what is still open, then the object itself, is deleted */
        uint64_t objs[FSA_FILES + FSA_DIRS + 1];
        int n = 0;
        for (int i = 1; i < FSA_FILES; i++) if (g_fsa_file[i].obj && g_fsa_file[i].mount == m) { objs[n++] = g_fsa_file[i].obj; g_fsa_file[i].obj = 0; }
        for (int i = 1; i < FSA_DIRS; i++) if (g_fsa_dir[i].obj && g_fsa_dir[i].mount == m) { objs[n++] = g_fsa_dir[i].obj; g_fsa_dir[i].obj = 0; }
        objs[n++] = m->fsa;
        m->kind = 0; m->fsa = 0;
        fs_unlock();
        trace_printf("     fs unmount \"%s\" (guest file system, %d objects deleted)\n", name, n);
        for (int i = 0; i < n; i++) fsa_call(c, objs[i], 1, 0, 0, 0, 0, 0);
        return;
    }
    if (m) {
        for (int i = 0; i < OPEN_MAX; i++) if (g_open[i].h && g_open[i].mount == m) { CloseHandle(g_open[i].h); g_open[i].h = NULL; }   /* [sdk 0x1f4870] the accessor closes what is left open */
        if (m->work[0]) {
            journal_recover(m->root);                      /* a decided commit not finished: try now (else the next Commit or mount does); the sweep */
            remove_tree(m->work);                          /* the uncommitted changes: never part of a decided commit */
        }
        tomb_clear(m);
        m->pend = 0;
        trace_printf("     fs unmount \"%s\"\n", name);
        m->kind = 0;
    } else trace_printf("     fs unmount \"%s\": not mounted\n", name ? name : "(null)");
    fs_unlock();
}
static void fs_EnsureSaveData(Ctx *c) {                  /* (const Uid &): creates the user's save data if there is none */
    char dir[MAX_PATH];
    save_dir(dir, "save");
    RET(CreateDirectoryA(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : RESULT_GENERIC_FAILURE);
}
static void fs_IsSaveDataExisting(Ctx *c) { RET(save_exists()); }
static void fs_Commit(Ctx *c) {                          /* (const char *mount name): C1..C6 of the commit protocol above */
    const char *name = STR(0);
    char journal[MAX_PATH], marker[MAX_PATH], stage[MAX_PATH];
    fs_lock();
    Mount *m = name ? mount_find(name, strlen(name)) : NULL;
    if (!m) { fs_unlock(); RET(RESULT_NOT_MOUNTED); return; }
    if (m->kind == MK_ROM) { fs_unlock(); RET(0); return; }                 /* nothing to commit on a read-only file system */
    if (m->kind == MK_FSA || m->kind == MK_DIRECT) { fs_unlock(); RET(0); return; }  /* no journal (nn-fuse's DoCommit would panic: not called) */
    for (int i = 0; i < OPEN_MAX; i++)
        if (g_open[i].h && g_open[i].mount == m && (g_open[i].mode & 2)) { fs_unlock(); trace_printf("     fs commit \"%s\": refused, a file is open for writing\n", name); RET(RESULT_WRITE_FILE_OPEN); return; }
    CommitLock mx = commit_mutex(m->root);
    journal_name(journal, m->root, "journal");
    journal_name(stage, m->root, "stage");                /* never used before: no leftover of a failed removal can be in it */
    snprintf(marker, sizeof marker, "%s.commit", m->root);
    const char *step = "lock";
    int changes = -1, ok = mx.ok, decided = 0;
    if (ok) {                                                                   /* C1 */
        step = "finish the pending commit";
        changes = exists(marker) ? journal_apply(marker) : 0;                  /* (-1: a marker that cannot be read) */
        ok = changes >= 0;
    }
    if (ok) {                                                                   /* C2 */
        step = "stage";
        ok = !exists(stage) && copy_tree(m->work, stage, 1);
#ifndef _WIN32
        if (ok) host_fsync_parent(stage);
#endif
    }
    if (ok) {                                                                   /* C3 */
        step = "write the journal";
        FILE *j = exists(journal) ? NULL : fopen(journal, "w");
        ok = j != NULL;
        if (ok) {
            fprintf(j, "W\t%s\nR\t%s\n", stage, m->root);
            for (int i = 0; i < m->tomb_n; i++) fprintf(j, "D\t%s\n", m->tomb[i]);
            ok = journal_list(j, stage, "");                 /* (the stage's files flushed: on disk before the decision) */
            ok = fflush(j) == 0 && ok && !ferror(j) && FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(j)));
            if (fclose(j)) ok = 0;
        }
    }
    if (ok) {                                                                   /* C4 */
        step = "decide";
        /* never over an existing marker (an older build, or a process without the lock, may have decided one since C1): the
         * rename fails instead, and this commit is not decided */
#ifdef _WIN32
        decided = ok = MoveFileExA(journal, marker, MOVEFILE_WRITE_THROUGH);
#else
        decided = ok = MoveFileExA(journal, marker, 0);   /* (the journal's data is on disk already; its folder is synced next) */
        if (decided) {
            step = "sync the decision";
            ok = 0;
            for (int i = 0; i < 3 && !ok; i++) ok = host_fsync_parent(marker) == 0;
        }                                                 /* not synced: K stays and is not applied now (C5 must not outrun it to the disk) */
#endif
    }
    if (decided && ok) { step = "apply"; changes = journal_apply(marker); ok = changes >= 0; }     /* C5 */
    DWORD err = ok ? 0 : GetLastError();
    if (ok) {                                                                   /* C6 */
        tomb_clear(m);
        if (!remove_tree(m->work)) journal_name(m->work, m->root, "work");     /* (not all gone: a new folder, the rest is swept later) */
        CreateDirectoryA(m->work, NULL);
        m->pend = 0;
    }
    else if (decided) m->pend = 1;                         /* E and the tombstones stay: they show the decided state */
    else if (mx.ok && changes >= 0) { delete_entry(journal); remove_tree(stage); }   /* not decided: nothing happened (leftovers are swept later) */
    commit_done(mx);
    fs_unlock();
    trace_printf("     fs commit \"%s\": %s (%d changes)\n", name, ok ? "ok" : step, changes);
    if (!ok) printf("nn::fs::Commit(\"%s\") failed on the host at \"%s\" (%d, error %lu): %s\n", name, step, changes, (unsigned long)err,
                    decided ? "it is decided and is finished by the next Commit, Unmount or mount"
                    : !strcmp(step, "finish the pending commit") ? "an earlier decided commit is still unfinished, so this one was refused"
                    : !strcmp(step, "lock") ? "the commit lock could not be taken, so nothing was changed"
                    : "the committed save data is unchanged");
    RET(ok ? 0 : RESULT_GENERIC_FAILURE);
}
static void fs_ok(Ctx *c) { RET(0); }

/* [sdk 0x2175e0] QueryMountRomCacheSize asks the RomFS reader for the working memory its metadata needs: the sizes of the
 * four tables of the RomFS image (directory hash table, directory entries, file hash table, file entries). The image
 * itself is not on this machine (only the extracted tree is), so the tables are rebuilt from the tree with the format's
 * rules: a directory entry is 0x18 bytes + its name padded to 4, a file entry 0x20 + name padded to 4, a hash table has
 * 4 bytes per bucket and the bucket count the RomFS builder derives from the entry count. NOT VERIFIED against the
 * image's own header (it would need the original romfs image). */
static uint32_t romfs_buckets(uint32_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    while (n % 2 == 0 || n % 3 == 0 || n % 5 == 0 || n % 7 == 0 || n % 11 == 0 || n % 13 == 0 || n % 17 == 0) n++;
    return n;
}
static void romfs_walk(const char *dir, uint32_t *dirs, uint32_t *files, uint64_t *dir_bytes, uint64_t *file_bytes) {
    wchar_t wpat[MAX_PATH + 4];
    char pat[MAX_PATH + 4], sub[MAX_PATH], utf8[1024];
    WIN32_FIND_DATAW fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    MultiByteToWideChar(CP_ACP, 0, pat, -1, wpat, MAX_PATH + 4);
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        int len = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, utf8, sizeof utf8, NULL, NULL) - 1;
        if (len < 0) len = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            (*dirs)++; *dir_bytes += 0x18 + (uint64_t)((len + 3) & ~3);
            WideCharToMultiByte(CP_ACP, 0, fd.cFileName, -1, utf8, sizeof utf8, NULL, NULL);
            snprintf(sub, sizeof sub, "%s\\%s", dir, utf8);
            romfs_walk(sub, dirs, files, dir_bytes, file_bytes);
        } else { (*files)++; *file_bytes += 0x20 + (uint64_t)((len + 3) & ~3); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static void fs_QueryMountRomCacheSize(Ctx *c) {          /* (size_t *out) */
    uint32_t dirs = 1, files = 0;
    uint64_t dir_bytes = 0x18, file_bytes = 0;
    fs_roots();
    romfs_walk(g_rom_root, &dirs, &files, &dir_bytes, &file_bytes);
    uint64_t size = 4ull * romfs_buckets(dirs) + dir_bytes + 4ull * romfs_buckets(files) + file_bytes;
    trace_printf("     fs QueryMountRomCacheSize: %u directories, %u files -> %llu bytes\n", dirs, files, (unsigned long long)size);
    wr64(A0, size);
    RET(0);
}

static void fs_OpenFile(Ctx *c) {                     /* (FileHandle *out, const char *path, int mode) */
    char rel[MAX_PATH], path[MAX_PATH], sub[FSA_PATH_MAX];
    Mount *m = NULL;
    HANDLE h = INVALID_HANDLE_VALUE;
    int write = (A2 & 2) != 0;
    uint32_t pr = 0;
    Mount *fm = fsa_mount_of(STR(1), sub, &pr);
    if (fm) { uint64_t out = A0; if (pr) { wr64(out, 0); fsa_log("open", STR(1), pr); RET(pr); } else RET(fsa_open_file(c, fm, STR(1), sub, (int)A2, out)); return; }
    fs_lock();
    uint32_t r = split_path(STR(1), &m, rel);
    if (!r && write && m->kind == MK_ROM) r = RESULT_PERMISSION_DENIED;
    if (!r) {
        int found = view_path(m, rel, path) && !is_dir(path);
        if (!found) r = RESULT_PATH_NOT_FOUND;
        else if (write && m->work[0]) {                    /* copy-on-write: the committed file is never opened for writing */
            char w[MAX_PATH];
            join(w, m->work, rel);
            if (_stricmp(w, path) != 0) { make_parents(w); if (!CopyFileA(path, w, TRUE)) r = RESULT_GENERIC_FAILURE; }
            strcpy(path, w);
        }
    }
    fs_unlock();
    if (!r) {
        h = CreateFileA(path, GENERIC_READ | (write ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) r = RESULT_PATH_NOT_FOUND;
    }
    if (InterlockedIncrement(&g_open_log) <= 200 || r)     /* D2: every failed open is logged, not only the first 200 */
        trace_printf("     fs open \"%s\" mode %d -> %s\n", STR(1), (int)A2, r == RESULT_NOT_MOUNTED ? "NOT MOUNTED" : r ? "NOT FOUND" : "ok");
    {   static int on = -1; if (on < 0) on = getenv("SSBU_FSLOG") != NULL;       /* D2 debug: every open on stdout */
        if (on) printf("fslog: open \"%s\" mode %d -> %s\n", STR(1), (int)A2, r ? "NOT FOUND" : "ok"); }
    if (r) { wr64(A0, 0); RET(r); return; }
    open_remember(h, (int)(A2 & 7), m);
    wr64(A0, (uint64_t)h);
    RET(0);
}
static void fs_CloseFile(Ctx *c) {
    int slot = fsa_file_slot(A0);
    if (slot) { if (slot > 0) fsa_close_file(c, slot); return; }
    if (A0) { open_mode((HANDLE)A0, 1); CloseHandle((HANDLE)A0); }
}
extern int g_hitch_ms; void hitch_note(const char *s);      /* hitch.c: SSBU_HITCH */
static uint64_t read_at(HANDLE h, int64_t offset, void *buf, uint64_t size) {
    uint64_t total = 0;
    if (g_hitch_ms) { char note[80]; snprintf(note, sizeof note, "read of %llu KB", (unsigned long long)(size >> 10)); hitch_note(note); }
    { void r1_wrote(uint64_t, uint64_t); r1_wrote((uint64_t)(uintptr_t)buf, size); }   /* R1 (r1_state.c): the kernel's write into guest memory is marked (nothing without SSBU_R1_MARK) */
    while (total < size) {
        OVERLAPPED ov = {0};
        uint64_t pos = (uint64_t)offset + total;
        ov.Offset = (DWORD)pos;
        ov.OffsetHigh = (DWORD)(pos >> 32);
        DWORD chunk = size - total > 0x40000000 ? 0x40000000 : (DWORD)(size - total), got = 0;
        if (!ReadFile(h, (char *)buf + total, chunk, &got, &ov) || got == 0) {
            /* (R4) the learned-set save (r1_state.c, rb_learned.c) keeps the fight's guest pages read-only until their first
             * write; r1_wrote above opened them, but a kernel write into a page protected after that fails with
             * ERROR_NOACCESS instead of faulting: open the pages again and read once more (counted in the r1 log) */
            if (GetLastError() == ERROR_NOACCESS) {
                void r1_wrote(uint64_t, uint64_t); void r1_read_retry_note(void);
                r1_wrote((uint64_t)(uintptr_t)buf + total, chunk); r1_read_retry_note();
                memset(&ov, 0, sizeof ov); ov.Offset = (DWORD)pos; ov.OffsetHigh = (DWORD)(pos >> 32);
                if (ReadFile(h, (char *)buf + total, chunk, &got, &ov) && got) { total += got; continue; }
            }
            break;
        }
        total += got;
    }
    if (g_hitch_ms) hitch_note(NULL);
    return total;
}
static volatile LONG g_read_log;
static void fs_ReadFile(Ctx *c) {                     /* (handle, offset, buffer, size) */
    int slot = fsa_file_slot(A0);
    if (slot) {
        uint64_t n = 0;
        uint32_t r = slot < 0 ? RESULT_GENERIC_FAILURE : fsa_read(c, slot, (int64_t)A1, A2, A3, &n);
        RET(r ? r : n == A3 ? 0 : RESULT_OUT_OF_RANGE);
        return;
    }
    uint64_t got = read_at((HANDLE)A0, (int64_t)A1, (void *)(uintptr_t)A2, A3);
    if (InterlockedIncrement(&g_read_log) <= 60)
        trace_printf("     fs read offset %llx size %llx -> %llx bytes  from=%llx\n", (unsigned long long)A1,
                     (unsigned long long)A3, (unsigned long long)got, (unsigned long long)c->x[30] - 4);
    if (got != A3 && getenv("SSBU_FSLOG")) printf("fslog: ReadFile SHORT handle %llx offset %llx size %llx got %llx from=%llx\n", (unsigned long long)A0, (unsigned long long)A1, (unsigned long long)A3, (unsigned long long)got, (unsigned long long)c->x[30] - 4);   /* D2 debug */
    RET(got == A3 ? 0 : RESULT_OUT_OF_RANGE);            /* this overload must read exactly `size` bytes */
}
static void fs_ReadFileCount(Ctx *c) {                /* (size_t *outRead, handle, offset, buffer, size) */
    int slot = fsa_file_slot(A1);
    if (slot) {
        uint64_t n = 0, out = A0;
        uint32_t r = slot < 0 ? RESULT_GENERIC_FAILURE : fsa_read(c, slot, (int64_t)A2, A3, A4, &n);
        if (out) wr64(out, r ? 0 : n);
        RET(r);
        return;
    }
    uint64_t got = read_at((HANDLE)A1, (int64_t)A2, (void *)(uintptr_t)A3, A4);
    wr64(A0, got);
    if (got != A4 && getenv("SSBU_FSLOG")) printf("fslog: ReadFile(count) SHORT handle %llx offset %llx size %llx got %llx from=%llx\n", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A4, (unsigned long long)got, (unsigned long long)c->x[30] - 4);   /* D2 debug */
    RET(0);
}
static void fs_WriteFile(Ctx *c) {                    /* (handle, offset, buffer, size, option) */
    int slot = fsa_file_slot(A0);
    if (slot) { RET(slot < 0 ? RESULT_GENERIC_FAILURE : fsa_write(c, slot, (int64_t)A1, A2, A3, A4)); return; }
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)A1;
    ov.OffsetHigh = (DWORD)(A1 >> 32);
    DWORD put = 0;
    int mode = open_mode((HANDLE)A0, 0);
    LARGE_INTEGER size;
    if (!(mode & 2)) { RET(RESULT_WRITE_NOT_PERMITTED); return; }
    if (!(mode & 4) && GetFileSizeEx((HANDLE)A0, &size) && (int64_t)(A1 + A3) > size.QuadPart) { RET(RESULT_NO_APPEND); return; }   /* a file only grows in append mode */
    if (A3 && !WriteFile((HANDLE)A0, (void *)(uintptr_t)A2, (DWORD)A3, &put, &ov)) { RET(RESULT_GENERIC_FAILURE); return; }
    if (A4 && (rd32(A4) & 1)) FlushFileBuffers((HANDLE)A0);                  /* WriteOption::Flush */
    RET(0);
}
static void fs_GetFileSize(Ctx *c) {                  /* (s64 *out, handle) */
    LARGE_INTEGER size;
    int slot = fsa_file_slot(A1);
    if (slot) {
        int64_t n = 0;
        uint64_t out = A0;
        uint32_t r = slot < 0 ? RESULT_GENERIC_FAILURE : fsa_get_size(c, slot, &n);
        if (!r) wr64(out, (uint64_t)n);
        RET(r);
        return;
    }
    if (!GetFileSizeEx((HANDLE)A1, &size)) { RET(RESULT_GENERIC_FAILURE); return; }
    wr64(A0, (uint64_t)size.QuadPart);
    RET(0);
}
/* (handle, s64 size): WriteNotPermitted without the write mode; the file is cut or zero-extended */
static void fs_SetFileSize(Ctx *c) {
    int slot = fsa_file_slot(A0);
    if (slot) {
        uint32_t r = slot < 0 ? RESULT_GENERIC_FAILURE : !(g_fsa_file[slot].mode & 2) ? RESULT_WRITE_NOT_PERMITTED
                   : fsa_call(c, g_fsa_file[slot].obj, 5, A1, 0, 0, 0, 0);
        RET(r);
        return;
    }
    if ((int64_t)A1 < 0) { RET(RESULT_OUT_OF_RANGE); return; }
    if (!(open_mode((HANDLE)A0, 0) & 2)) { RET(RESULT_WRITE_NOT_PERMITTED); return; }
#ifdef _WIN32
    FILE_END_OF_FILE_INFO eof;
    eof.EndOfFile.QuadPart = (LONGLONG)A1;
    RET(SetFileInformationByHandle((HANDLE)A0, FileEndOfFileInfo, &eof, sizeof eof) ? 0 : RESULT_GENERIC_FAILURE);
#else   /* POSIX builds (plat/windows.h): move to the size, set the end there, put the file position back */
    LARGE_INTEGER zero = {0}, at = {0}, size;
    size.QuadPart = (LONGLONG)A1;
    BOOL ok = SetFilePointerEx((HANDLE)A0, zero, &at, FILE_CURRENT) && SetFilePointerEx((HANDLE)A0, size, NULL, FILE_BEGIN) && SetEndOfFile((HANDLE)A0);
    SetFilePointerEx((HANDLE)A0, at, NULL, FILE_BEGIN);
    RET(ok ? 0 : RESULT_GENERIC_FAILURE);
#endif
}
/* (handle): [sdk 0x1f3150] FileAccessor::Flush does nothing for a file opened without the write mode */
static void fs_FlushFile(Ctx *c) {
    int slot = fsa_file_slot(A0);
    if (slot) { RET(slot < 0 ? RESULT_GENERIC_FAILURE : !(g_fsa_file[slot].mode & 2) ? 0 : fsa_call(c, g_fsa_file[slot].obj, 4, 0, 0, 0, 0, 0)); return; }
    if (!(open_mode((HANDLE)A0, 0) & 2)) { RET(0); return; }
    RET(FlushFileBuffers((HANDLE)A0) ? 0 : RESULT_GENERIC_FAILURE);
}
/* Where a new entry goes: the working folder of a journaled mount. Result code; `path` = host path to create. */
static uint32_t create_target(const char *guest, Mount **mount, char *rel, char *path) {
    char host[MAX_PATH];
    uint32_t r = split_path(guest, mount, rel);
    if (r) return r;
    if ((*mount)->kind == MK_ROM) return RESULT_PERMISSION_DENIED;
    if (!rel[0] || view_path(*mount, rel, host)) return RESULT_PATH_ALREADY_EXISTS;
    if (!view_parent_is_dir(*mount, rel)) return RESULT_PATH_NOT_FOUND;
    join(path, (*mount)->work[0] ? (*mount)->work : (*mount)->root, rel);      /* a plain folder has no working copy */
    make_parents(path);
    return 0;
}
/* the path names a guest file system: its method `slot` answers (see fsa_path_op) */
#define FSA_PATH_CALL(argpath, what, slot, a2, a3) do { char sub_[FSA_PATH_MAX]; uint32_t pr_ = 0; Mount *fm_ = fsa_mount_of(argpath, sub_, &pr_); \
        if (fm_) { RET(fsa_path_op(c, fm_, what, argpath, sub_, pr_, slot, a2, a3)); return; } } while (0)
static void fs_CreateFile(Ctx *c) {                   /* (path, size) */
    char rel[MAX_PATH], path[MAX_PATH];
    Mount *m;
    FSA_PATH_CALL(STR(0), "create file", 2, A1, 0);
    fs_lock();
    uint32_t r = create_target(STR(0), &m, rel, path);
    if (!r) {
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) r = RESULT_GENERIC_FAILURE;
        else {
            LARGE_INTEGER size;
            size.QuadPart = (LONGLONG)A1;
            SetFilePointerEx(h, size, NULL, FILE_BEGIN);
            SetEndOfFile(h);
            CloseHandle(h);
            /* the deletion of an older file of this name stays recorded: the commit deletes it, then moves the new one in */
        }
    }
    fs_unlock();
    RET(r);
}
static void fs_DeleteFile(Ctx *c) {
    char rel[MAX_PATH], path[MAX_PATH], w[MAX_PATH], cm[MAX_PATH];
    Mount *m;
    FSA_PATH_CALL(STR(0), "delete file", 3, 0, 0);
    fs_lock();
    uint32_t r = split_path(STR(0), &m, rel);
    if (!r && m->kind == MK_ROM) r = RESULT_PERMISSION_DENIED;
    if (!r && (!view_path(m, rel, path) || is_dir(path))) r = RESULT_PATH_NOT_FOUND;
    if (!r && !m->work[0]) r = DeleteFileA(path) ? 0 : RESULT_GENERIC_FAILURE;   /* a plain folder: at once */
    else if (!r) {
        join(w, m->work, rel); join(cm, m->root, rel);
        if (exists(w)) DeleteFileA(w);
        if (m->pend || exists(cm)) tomb_add(m, rel);       /* (pending: it may exist only in the pending commit) */
    }
    fs_unlock();
    RET(r);
}
static void fs_CreateDirectory(Ctx *c) {
    char rel[MAX_PATH], path[MAX_PATH];
    Mount *m;
    FSA_PATH_CALL(STR(0), "create directory", 4, 0, 0);
    fs_lock();
    uint32_t r = create_target(STR(0), &m, rel, path);
    if (!r && !CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) r = RESULT_GENERIC_FAILURE;
    fs_unlock();
    RET(r);
}
static void fs_GetEntryType(Ctx *c) {                 /* (DirectoryEntryType *out, path): 0 directory, 1 file */
    char rel[MAX_PATH], path[MAX_PATH], sub[FSA_PATH_MAX];
    Mount *m;
    uint32_t pr = 0;
    Mount *fm = fsa_mount_of(STR(1), sub, &pr);
    if (fm) { RET(fsa_get_entry_type(c, fm, STR(1), sub, pr, A0)); return; }
    fs_lock();
    uint32_t r = split_path(STR(1), &m, rel);
    if (!r && !view_path(m, rel, path)) r = RESULT_PATH_NOT_FOUND;
    fs_unlock();
    if (r) { RET(r); return; }
    wr32(A0, is_dir(path) ? 0 : 1);
    RET(0);
}
static void fs_OpenDirectory(Ctx *c) { wr64(A0, 0); RET(RESULT_PATH_NOT_FOUND); }   /* replaced by nnfsdir.c */
/* nnfsdir.c DeleteDirectory: a guest file system's DoDeleteDirectory; *handled = 0 for any other path */
uint32_t fs_fsa_delete_directory(Ctx *c, const char *guest, int *handled) {
    char sub[FSA_PATH_MAX];
    uint32_t pr = 0;
    Mount *fm = fsa_mount_of(guest, sub, &pr);
    *handled = fm != NULL;
    return fm ? fsa_path_op(c, fm, "delete directory", guest, sub, pr, 5, 0, 0) : 0;
}
/* RenameFile / RenameDirectory (old, new): both names on one mount; the new name must not exist and its folder must.
 * Journaled save data: a file is renamed in the working state (the working copy moves, or the committed file is copied
 * under the new name) and the old name is deleted at the next Commit; a folder of journaled save data is not renamed
 * (UnsupportedOperation: nothing asks for it). */
static uint32_t host_rename(const char *from, const char *to, int want_dir) {
    Mount *ma = NULL, *mb = NULL;
    char ra[MAX_PATH], rb[MAX_PATH], pa[MAX_PATH], pb[MAX_PATH], host[MAX_PATH];
    fs_lock();
    uint32_t r = split_path(from, &ma, ra);
    if (!r) r = split_path(to, &mb, rb);
    if (!r && ma != mb) r = RESULT_UNSUPPORTED;
    if (!r && ma->kind == MK_ROM) r = RESULT_PERMISSION_DENIED;
    if (!r && (!ra[0] || !view_path(ma, ra, pa) || is_dir(pa) != want_dir)) r = RESULT_PATH_NOT_FOUND;
    if (!r && (!rb[0] || view_path(ma, rb, host))) r = RESULT_PATH_ALREADY_EXISTS;
    if (!r && !view_parent_is_dir(ma, rb)) r = RESULT_PATH_NOT_FOUND;
    if (!r && !ma->work[0]) { join(pb, ma->root, rb); r = MoveFileExA(pa, pb, 0) ? 0 : RESULT_GENERIC_FAILURE; }
    else if (!r && want_dir) r = RESULT_UNSUPPORTED;
    else if (!r) {
        char w[MAX_PATH], cm[MAX_PATH];
        join(pb, ma->work, rb); make_parents(pb);
        join(w, ma->work, ra); join(cm, ma->root, ra);
        r = (_stricmp(pa, w) == 0 ? MoveFileExA(w, pb, 0) : CopyFileA(pa, pb, TRUE)) ? 0 : RESULT_GENERIC_FAILURE;
        if (!r && (ma->pend || exists(cm))) tomb_add(ma, ra);
    }
    fs_unlock();
    trace_printf("     fs rename \"%s\" -> \"%s\": %x\n", from ? from : "", to ? to : "", r);
    return r;
}
static void fs_RenameFile(Ctx *c) {
    char tmp[FSA_PATH_MAX]; uint32_t pr = 0;
    if (fsa_mount_of(STR(0), tmp, &pr) || fsa_mount_of(STR(1), tmp, &pr)) { RET(fsa_rename(c, 8, STR(0), STR(1))); return; }
    RET(host_rename(STR(0), STR(1), 0));
}
static void fs_RenameDirectory(Ctx *c) {
    char tmp[FSA_PATH_MAX]; uint32_t pr = 0;
    if (fsa_mount_of(STR(0), tmp, &pr) || fsa_mount_of(STR(1), tmp, &pr)) { RET(fsa_rename(c, 9, STR(0), STR(1))); return; }
    RET(host_rename(STR(0), STR(1), 1));
}
/* DeleteDirectoryRecursively(path): the folder and everything below it. Journaled save data: the working copy goes at
 * once, the committed folder is deleted at the next Commit (its tombstone hides everything below it meanwhile). The root
 * of a mount is not deleted (UnsupportedOperation). */
static void fs_DeleteDirectoryRecursively(Ctx *c) {
    char rel[MAX_PATH], path[MAX_PATH];
    Mount *m;
    FSA_PATH_CALL(STR(0), "delete directory recursively", 6, 0, 0);
    fs_lock();
    uint32_t r = split_path(STR(0), &m, rel);
    if (!r && m->kind == MK_ROM) r = RESULT_PERMISSION_DENIED;
    if (!r && !rel[0]) r = RESULT_UNSUPPORTED;
    if (!r && (!view_path(m, rel, path) || !is_dir(path))) r = RESULT_PATH_NOT_FOUND;
    if (!r && !m->work[0]) { remove_tree(path); r = exists(path) ? RESULT_GENERIC_FAILURE : 0; }
    else if (!r) {
        char w[MAX_PATH], cm[MAX_PATH];
        join(w, m->work, rel); join(cm, m->root, rel);
        if (is_dir(w)) remove_tree(w);
        if (m->pend || is_dir(cm)) tomb_add(m, rel);
    }
    fs_unlock();
    trace_printf("     fs DeleteDirectoryRecursively \"%s\" -> %x\n", STR(0), r);
    RET(r);
}
static void fs_false(Ctx *c) { RET(0); }

/* ---- self-test of the mount table and of the save data journal, in a scratch folder under %TEMP% (SSBU_SDK_SELFTEST) */
uint32_t fs_mount_plain(const char *name, const char *host_dir) { fs_roots(); return mount_add(name, MK_PLAIN, host_dir); }
const char *fs_mount_work(const char *name) {          /* self-tests: the working folder of a mount ("" if none) */
    static char w[MAX_PATH];
    fs_lock();
    Mount *m = name ? mount_find(name, strlen(name)) : NULL;
    snprintf(w, sizeof w, "%s", m ? m->work : "");
    fs_unlock();
    return w;
}
uint32_t fs_mount_direct(const char *name, const char *host_dir) { fs_roots(); return mount_add(name, MK_DIRECT, host_dir); }   /* skyline.c: sd: */
static int st_host_is(const char *path, const char *text) {
    char buf[64] = "";
    FILE *f = fopen(path, "rb");
    if (!f) return text == NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return text && strcmp(buf, text) == 0;
}
static uint32_t st_write(Ctx *c, const char *guest, const char *text, uint64_t *keep_open) {
    uint64_t h = 0;
    uint32_t option = 1;
    c->x[0] = (uint64_t)(uintptr_t)&h; c->x[1] = (uint64_t)(uintptr_t)guest; c->x[2] = 7; fs_OpenFile(c);
    if (c->x[0]) return (uint32_t)c->x[0];
    c->x[0] = h; c->x[1] = 0; c->x[2] = (uint64_t)(uintptr_t)text; c->x[3] = strlen(text); c->x[4] = (uint64_t)(uintptr_t)&option; fs_WriteFile(c);
    uint32_t r = (uint32_t)c->x[0];
    if (keep_open) *keep_open = h; else { c->x[0] = h; fs_CloseFile(c); }
    return r;
}
static int st_read_is(Ctx *c, const char *guest, const char *text) {
    uint64_t h = 0, got = 0;
    char buf[64] = "";
    c->x[0] = (uint64_t)(uintptr_t)&h; c->x[1] = (uint64_t)(uintptr_t)guest; c->x[2] = 1; fs_OpenFile(c);
    if (c->x[0]) return text == NULL;
    c->x[0] = (uint64_t)(uintptr_t)&got; c->x[1] = h; c->x[2] = 0; c->x[3] = (uint64_t)(uintptr_t)buf; c->x[4] = sizeof buf - 1; fs_ReadFileCount(c);
    c->x[0] = h; fs_CloseFile(c);
    buf[got] = 0;
    return text && strcmp(buf, text) == 0;
}
int nnfs_selftest(Ctx *c) {
    char saved_root[MAX_PATH], t[MAX_PATH], save[MAX_PATH], a[MAX_PATH], n[MAX_PATH], work[MAX_PATH], marker[MAX_PATH], journal[MAX_PATH];
    int bad = 0;
    uint64_t uid[2] = {1, 0}, h = 0;
    #define FSCHECK(cond, text) do { if (!(cond)) { printf("  FAIL fs journal: %s\n", text); bad++; } } while (0)
    fs_roots();
    strcpy(saved_root, g_user_root);
    GetTempPathA(MAX_PATH - 64, t);
    snprintf(t + strlen(t), 64, "ssbu_fs_selftest_%lu", (unsigned long)GetCurrentProcessId());
    remove_tree(t);
    CreateDirectoryA(t, NULL);
    strcpy(g_user_root, t);
    snprintf(save, sizeof save, "%s\\save", t); CreateDirectoryA(save, NULL);
    snprintf(a, sizeof a, "%s\\a.bin", save); snprintf(n, sizeof n, "%s\\sub\\new.bin", save);
    snprintf(marker, sizeof marker, "%s.commit", save); snprintf(journal, sizeof journal, "%s.journal.%lu", save, (unsigned long)GetCurrentProcessId());
    FILE *f = fopen(a, "wb"); if (f) { fputs("committed", f); fclose(f); }

    c->x[0] = (uint64_t)(uintptr_t)"nosuch:/a.bin"; c->x[1] = 0; fs_CreateFile(c);
    FSCHECK(c->x[0] == RESULT_NOT_MOUNTED, "a path with an unmounted name is not NotMounted");
    c->x[0] = (uint64_t)(uintptr_t)"st"; c->x[1] = (uint64_t)(uintptr_t)uid; fs_MountSaveData(c);
    FSCHECK(c->x[0] == 0, "MountSaveData failed");
    c->x[0] = (uint64_t)(uintptr_t)"st"; c->x[1] = (uint64_t)(uintptr_t)uid; fs_MountSaveData(c);
    FSCHECK(c->x[0] == RESULT_MOUNT_NAME_EXISTS, "mounting the same name twice is not MountNameAlreadyExists");
    snprintf(work, sizeof work, "%s", fs_mount_work("st"));
    /* 1. a write is visible to the game and NOT in the committed folder */
    FSCHECK(st_write(c, "st:/a.bin", "CHANGED!!", NULL) == 0, "write failed");
    FSCHECK(st_read_is(c, "st:/a.bin", "CHANGED!!"), "the game does not read back what it wrote");
    FSCHECK(st_host_is(a, "committed"), "an uncommitted write reached the committed folder");
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub"; fs_CreateDirectory(c);
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub/new.bin"; c->x[1] = 0; fs_CreateFile(c);
    FSCHECK(c->x[0] == 0 && !exists(n), "a new file went straight to the committed folder");
    /* 2. unmount without commit: everything is rolled back */
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Unmount(c);
    FSCHECK(!exists(work), "the working folder survived Unmount");
    c->x[0] = (uint64_t)(uintptr_t)"st"; c->x[1] = (uint64_t)(uintptr_t)uid; fs_MountSaveData(c);
    snprintf(work, sizeof work, "%s", fs_mount_work("st"));
    FSCHECK(st_read_is(c, "st:/a.bin", "committed") && st_read_is(c, "st:/sub/new.bin", NULL), "uncommitted changes survived a remount");
    /* 3. commit is refused while a file is open for writing, then applies everything */
    FSCHECK(st_write(c, "st:/a.bin", "SECOND!!!", &h) == 0, "write failed");
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Commit(c);
    FSCHECK(c->x[0] == RESULT_WRITE_FILE_OPEN && st_host_is(a, "committed"), "Commit with a file open for writing was not refused");
    c->x[0] = h; fs_CloseFile(c);
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub"; fs_CreateDirectory(c);
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub/new.bin"; c->x[1] = 0; fs_CreateFile(c);
    FSCHECK(st_write(c, "st:/sub/new.bin", "", NULL) == 0, "open of a created file failed");
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Commit(c);
    FSCHECK(c->x[0] == 0 && st_host_is(a, "SECOND!!!") && exists(n) && !exists(marker), "Commit did not apply the changes");
    FSCHECK(st_read_is(c, "st:/a.bin", "SECOND!!!"), "the committed file is not read back after Commit");
    /* 4. deletion: hidden at once, done on the host at Commit */
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub/new.bin"; fs_DeleteFile(c);
    FSCHECK(c->x[0] == 0 && exists(n) && st_read_is(c, "st:/sub/new.bin", NULL), "DeleteFile is not journaled");
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub/new.bin"; fs_DeleteFile(c);
    FSCHECK(c->x[0] == RESULT_PATH_NOT_FOUND, "deleting a deleted file is not PathNotFound");
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Commit(c);
    FSCHECK(c->x[0] == 0 && !exists(n), "a committed deletion did not reach the host");
    /* 5. a process killed after the commit was decided: the next mount finishes it */
    FSCHECK(st_write(c, "st:/a.bin", "THIRD!!!!", NULL) == 0, "write failed");
    f = fopen(journal, "w");
    if (f) { fprintf(f, "W\t%s\nR\t%s\n", work, save); journal_list(f, work, ""); fclose(f); }
    MoveFileExA(journal, marker, MOVEFILE_REPLACE_EXISTING);
    FSCHECK(st_host_is(a, "SECOND!!!") && exists(marker), "test setup: decided commit");
    journal_recover(save);
    FSCHECK(st_host_is(a, "THIRD!!!!") && !exists(marker), "a decided commit was not rolled forward at the next mount");
    /* 6. a process killed before the commit was decided: its working folder is dropped (pid 4 is the System process: use a dead pid) */
    {
        char dead[MAX_PATH];
        snprintf(dead, sizeof dead, "%s.work.4294967294", save);
        CreateDirectoryA(dead, NULL);
        snprintf(dead + strlen(dead), 32, "\\a.bin");
        f = fopen(dead, "wb"); if (f) { fputs("LOST", f); fclose(f); }
        journal_recover(save);
        dead[strlen(dead) - 6] = 0;
        FSCHECK(!exists(dead) && st_host_is(a, "THIRD!!!!"), "the working folder of a dead process was not discarded");
    }
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Unmount(c);
    remove_tree(t);
    strcpy(g_user_root, saved_root);
    printf("  fs: mount table (NotMounted, MountNameAlreadyExists) and save data journal (copy-on-write, rollback at Unmount, Commit refused with a\n"
           "      writer open, two-phase Commit, journaled deletion, roll-forward of a decided commit, discard of a dead process's work): %s\n", bad ? "FAILED" : "ok");
    #undef FSCHECK
    return bad;
}

/* ---- nn::account: one local user with a fixed id */
static void acc_ListAllUsers(Ctx *c) { wr32(A0, 1); if ((int)A2 > 0) { wr64(A1, 1); wr64(A1 + 8, 0); } RET(0); }
static volatile LONG g_acc_user_open;                    /* the one local user {1, 0} has been opened (there is no CloseUser import) */
int account_user_is_open(uint64_t lo, uint64_t hi) { return g_acc_user_open && lo == 1 && hi == 0; }
static void acc_TryOpenPreselectedUser(Ctx *c) { wr64(A0, 1); wr64(A0 + 8, 0); wr64(A0 + 16, 0); InterlockedExchange(&g_acc_user_open, 1); RET(1); }
static void acc_GetUserId(Ctx *c) { wr64(A0, 1); wr64(A0 + 8, 0); RET(0); }
/* CloseUser(const UserHandle &): only ARCropolis imports it, for the handle of its own TryOpenPreselectedUser; the game
 * keeps its own open, so the user stays open here (the console counts opens per handle). */
static void acc_CloseUser(Ctx *c) { (void)c; }
#include "gfx_options.h"
static int ini_player_name(char *out, size_t n) {          /* `player_name = <name>` in graphics.ini (the menu writes it) */
    const GfxOptions *o = gfx_options();
    FILE *f = o && o->path[0] ? fopen(o->path, "r") : NULL;
    char line[512];
    int found = 0;
    if (!f) return 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (_strnicmp(s, "player_name", 11) || (s[11] != ' ' && s[11] != '\t' && s[11] != '=')) continue;
        s += 11;
        while (*s == ' ' || *s == '\t') s++;
        if (*s++ != '=') continue;
        while (*s == ' ' || *s == '\t') s++;
        size_t l = strcspn(s, "\r\n#;");
        while (l && (s[l - 1] == ' ' || s[l - 1] == '\t')) l--;
        if (l && l < n) { memcpy(out, s, l); out[l] = 0; found = 1; }
    }
    fclose(f);
    return found;
}
/* The nickname of the console user: SSBU_NICKNAME, else the name chosen in the settings menu (graphics.ini player_name),
 * else the name of the Windows user: UTF-8, at most 32 bytes, NUL-terminated, in the 33-byte Nickname. */
static void acc_GetNickname(Ctx *c) {
    static char name[33];
    static volatile LONG ready;
    if (InterlockedCompareExchange(&ready, 1, 0) == 0) {
        const char *env = getenv("SSBU_NICKNAME");
        char utf8[512] = "";
        if (env && env[0]) snprintf(utf8, sizeof utf8, "%s", env);
        else if (ini_player_name(utf8, sizeof utf8)) {}                  /* F1 > Experimental > Your name (graphics.ini player_name) */
        else {
            wchar_t wname[257] = L"";
            DWORD n = 257;
            if (GetUserNameW(wname, &n)) WideCharToMultiByte(CP_UTF8, 0, wname, -1, utf8, sizeof utf8, NULL, NULL);
        }
        if (!utf8[0]) strcpy(utf8, "Player");
        size_t len = strlen(utf8);
        if (len > 32) { len = 32; while (len > 0 && ((unsigned char)utf8[len] & 0xc0) == 0x80) len--; }   /* cut between characters */
        memcpy(name, utf8, len);
        name[len] = 0;
        ready = 2;
    }
    while (ready != 2) SwitchToThread();
    memset((void *)(uintptr_t)A0, 0, 33);
    strcpy((char *)(uintptr_t)A0, name);
    RET(0);
}
static void acc_out_true(Ctx *c) { wr8(A0, 1); RET(0); }
static void acc_out_false(Ctx *c) { wr8(A0, 0); RET(0); }
/* [sdk] 0x4b3a8 (EnsureNetworkServiceAccountAvailable): the SDK treats the account results 124-0200 .. 124-0269 from
 * the service as "no usable network service account" (it then offers the account-linking applet and returns the
 * result). An offline machine has no linked account: 124-0200 = 0x1907c, the first code of that range, is returned
 * (NOT VERIFIED: which code of the range a console gives; the linking applet is not shown, it needs the network). */
#define RESULT_ACCOUNT_NSA_UNAVAILABLE 0x1907cull
static void acc_unavailable(Ctx *c) { RET(RESULT_ACCOUNT_NSA_UNAVAILABLE); }
static void keep(Ctx *c) { (void)c; }

/* ---- nn::nifm: no network */
static void nifm_false(Ctx *c) { RET(0); }

#define FS(n) "_ZN2nn2fs" n
#define AC(n) "_ZN2nn7account" n
#define NI(n) "_ZN2nn4nifm" n
static const ImportDef fs_imports[] = {
    {FS("8MountRomEPKcPvm"), fs_MountRom}, {FS("13MountSaveDataEPKcRKNS_7account3UidE"), fs_MountSaveData}, {FS("17MountCacheStorageEPKc"), fs_MountCacheStorage},
    {FS("7UnmountEPKc"), fs_Unmount}, {FS("22QueryMountRomCacheSizeEPm"), fs_QueryMountRomCacheSize}, {FS("6CommitEPKc"), fs_Commit}, {FS("14EnsureSaveDataERKNS_7account3UidE"), fs_EnsureSaveData},
    {FS("18IsSaveDataExistingERKNS_7account3UidE"), fs_IsSaveDataExisting},
    {FS("8OpenFileEPNS0_10FileHandleEPKci"), fs_OpenFile}, {FS("9CloseFileENS0_10FileHandleE"), fs_CloseFile},
    {FS("8ReadFileENS0_10FileHandleElPvm"), fs_ReadFile}, {FS("8ReadFileEPmNS0_10FileHandleElPvm"), fs_ReadFileCount},
    {FS("9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE"), fs_WriteFile}, {FS("11GetFileSizeEPlNS0_10FileHandleE"), fs_GetFileSize},
    {FS("10CreateFileEPKcl"), fs_CreateFile}, {FS("10DeleteFileEPKc"), fs_DeleteFile}, {FS("15CreateDirectoryEPKc"), fs_CreateDirectory},
    {FS("12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc"), fs_GetEntryType}, {FS("13OpenDirectoryEPNS0_15DirectoryHandleEPKci"), fs_OpenDirectory},
    /* names only the Skyline plugins import (hostrt.c keeps them for nnro.c's binding) */
    {FS("11SetFileSizeENS0_10FileHandleEl"), fs_SetFileSize}, {FS("9FlushFileENS0_10FileHandleE"), fs_FlushFile},
    {FS("10RenameFileEPKcS2_"), fs_RenameFile}, {FS("15RenameDirectoryEPKcS2_"), fs_RenameDirectory},
    {FS("26DeleteDirectoryRecursivelyEPKc"), fs_DeleteDirectoryRecursively},
    {FS("3fsa8RegisterEPKcONSt3__110unique_ptrINS1_11IFileSystemENS4_14default_deleteIS6_EEEE"), fs_fsa_Register},
    {FS("6detail14CheckMountNameEPKc"), fs_CheckMountName}, {FS("6detail8AllocateEm"), fs_detail_Allocate},
    {FS("6detail10DeallocateEPvm"), fs_detail_Deallocate}, {AC("9CloseUserERKNS0_10UserHandleE"), acc_CloseUser},
    {AC("10InitializeEv"), fs_ok}, {AC("12ListAllUsersEPiPNS0_3UidEi"), acc_ListAllUsers},
    {AC("22TryOpenPreselectedUserEPNS0_10UserHandleE"), acc_TryOpenPreselectedUser},
    {AC("9GetUserIdEPNS0_3UidERKNS0_10UserHandleE"), acc_GetUserId}, {AC("11GetNicknameEPNS0_8NicknameERKNS0_3UidE"), acc_GetNickname},
    {AC("16GetUserExistenceEPbRKNS0_3UidE"), acc_out_true},
    {AC("32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE"), acc_out_false},
    {AC("36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE"), acc_unavailable},
    {AC("25CachedNintendoAccountInfoC1Ev"), keep}, {AC("12AsyncContextC1Ev"), keep}, {AC("12AsyncContextD1Ev"), keep},
    {AC("12AsyncContext7HasDoneEPb"), acc_out_true}, {AC("12AsyncContext9GetResultEv"), acc_unavailable},
    {NI("10InitializeEv"), fs_ok}, {NI("18IsNetworkAvailableEv"), nifm_false}, {NI("22IsNetworkRequestOnHoldEv"), nifm_false},
    {NI("20SubmitNetworkRequestEv"), keep}, {NI("27SubmitNetworkRequestAndWaitEv"), keep}, {NI("20CancelNetworkRequestEv"), keep},
};

void register_fs_imports(void) {
    register_imports(fs_imports, (int)(sizeof fs_imports / sizeof fs_imports[0]));
}

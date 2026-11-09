/* C9: plat_posix.c's file layer: case-insensitive fallback (open/fopen/FindFirstFile/GetFileAttributes/Delete/Move),
 * cache change detection, write-through rename, Windows error codes. Build/run: tests/port/fs/run.sh [x86|arm64]. argv[1] = scratch dir parent. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
FILE *plat_fopen(const char *path, const char *mode);
static int g_n, g_bad;
#define CHK(c) do { g_n++; if (!(c)) { g_bad++; printf("FAIL line %d: %s (err %lu errno %d)\n", __LINE__, #c, (unsigned long)GetLastError(), errno); } } while (0)
static void put(const char *p, const char *s) { FILE *f = fopen(p, "w"); if (f) { fputs(s, f); fclose(f); } }
static int rd(HANDLE h, char *b, int n) { DWORD got = 0; if (!ReadFile(h, b, n - 1, &got, NULL)) return -1; b[got] = 0; return (int)got; }
int main(int argc, char **argv) {
    char root[512], p[640], b[64];
    snprintf(root, sizeof root, "%s/c9fs.XXXXXX", argc > 1 ? argv[1] : "/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
    char t[640];
    snprintf(t, sizeof t, "%s/RomFS", root); mkdir(t, 0777);
    snprintf(t, sizeof t, "%s/RomFS/Data", root); mkdir(t, 0777);
    snprintf(t, sizeof t, "%s/RomFS/Data/Fighter.BIN", root); put(t, "hello");
    /* CreateFileA open, mixed case, Windows separators */
    snprintf(p, sizeof p, "%s\\romfs\\DATA\\fighter.bin", root);
    HANDLE h = CreateFileA(p, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHK(h != INVALID_HANDLE_VALUE);
    CHK(rd(h, b, sizeof b) == 5 && !strcmp(b, "hello"));
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    /* exact case still works */
    snprintf(p, sizeof p, "%s/RomFS/Data/Fighter.BIN", root);
    h = CreateFileA(p, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL); CHK(h != INVALID_HANDLE_VALUE); CloseHandle(h);
    /* creating disposition on an existing file in another case opens it, no second file appears */
    snprintf(p, sizeof p, "%s/ROMFS/data/FIGHTER.bin", root);
    h = CreateFileA(p, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, 0, NULL); CHK(h != INVALID_HANDLE_VALUE); CloseHandle(h);
    h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL); CHK(h == INVALID_HANDLE_VALUE); CHK(GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS);
    snprintf(t, sizeof t, "%s/RomFS/Data", root);
    { int cnt = 0; DIR *d = opendir(t); struct dirent *e; while ((e = readdir(d))) if (e->d_name[0] != '.') cnt++; closedir(d); CHK(cnt == 1); }
    /* new file in a folder given in another case lands in the real folder */
    snprintf(p, sizeof p, "%s/romfs/data/New.txt", root);
    h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); CHK(h != INVALID_HANDLE_VALUE); CloseHandle(h);
    snprintf(t, sizeof t, "%s/RomFS/Data/New.txt", root); CHK(access(t, F_OK) == 0);
    /* fopen read and write */
    snprintf(p, sizeof p, "%s/ROMFS/DATA/fighter.BIN", root);
    FILE *f = plat_fopen(p, "rb"); CHK(f != NULL); if (f) fclose(f);
    snprintf(p, sizeof p, "%s/romfs/data/new.TXT", root);
    f = plat_fopen(p, "w"); CHK(f != NULL); if (f) { fputs("x", f); fclose(f); }
    snprintf(t, sizeof t, "%s/RomFS/Data/New.txt", root); { struct stat st; CHK(stat(t, &st) == 0 && st.st_size == 1); }
    /* GetFileAttributes(Ex), directories too */
    snprintf(p, sizeof p, "%s\\ROMFS\\data", root);
    CHK((GetFileAttributesA(p) & FILE_ATTRIBUTE_DIRECTORY) != 0);
    snprintf(p, sizeof p, "%s\\romfs\\DATA\\NEW.TXT", root);
    WIN32_FILE_ATTRIBUTE_DATA ad; CHK(GetFileAttributesExA(p, GetFileExInfoStandard, &ad) && ad.nFileSizeLow == 1);
    /* FindFirstFile in a folder of another case; the listing shows stored names */
    snprintf(p, sizeof p, "%s/romfs/DATA/*", root);
    WIN32_FIND_DATAA fd; h = FindFirstFileA(p, &fd); int seen = 0;
    if (h != INVALID_HANDLE_VALUE) { do { if (!strcmp(fd.cFileName, "Fighter.BIN") || !strcmp(fd.cFileName, "New.txt")) seen++; } while (FindNextFileA(h, &fd)); FindClose(h); }
    CHK(seen == 2);
    /* cache change detection: create / rename / delete are seen on the very next lookup */
    snprintf(t, sizeof t, "%s/RomFS/Data/Late.dat", root); put(t, "1");
    snprintf(p, sizeof p, "%s/romfs/data/LATE.DAT", root); CHK(GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES);
    snprintf(t, sizeof t, "%s/RomFS/Data/Moved.dat", root);
    CHK(MoveFileExA(p, t, 0));
    snprintf(p, sizeof p, "%s/romfs/data/late.dat", root); CHK(GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES);
    snprintf(p, sizeof p, "%s/romfs/data/MOVED.DAT", root); CHK(GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES);
    CHK(DeleteFileA(p));
    CHK(GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES);
    /* write-through rename over an existing target (case-mixed names) */
    snprintf(t, sizeof t, "%s/RomFS/Data/Save.tmp", root); put(t, "NEW");
    snprintf(t, sizeof t, "%s/RomFS/Data/Save.dat", root); put(t, "OLD");
    char a2[640], b2[640];
    snprintf(a2, sizeof a2, "%s/romfs/data/SAVE.TMP", root); snprintf(b2, sizeof b2, "%s/romfs/data/save.DAT", root);
    CHK(!MoveFileExA(a2, b2, MOVEFILE_WRITE_THROUGH) && GetLastError() == ERROR_ALREADY_EXISTS);
    CHK(MoveFileExA(a2, b2, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    h = CreateFileA(t, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL); CHK(h != INVALID_HANDLE_VALUE && rd(h, b, sizeof b) == 3 && !strcmp(b, "NEW")); if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    snprintf(t, sizeof t, "%s/RomFS/Data/Save.tmp", root); CHK(access(t, F_OK) != 0);
    /* cross-folder write-through rename */
    snprintf(t, sizeof t, "%s/RomFS/Other", root); mkdir(t, 0777);
    snprintf(a2, sizeof a2, "%s/romfs/data/save.dat", root); snprintf(b2, sizeof b2, "%s/ROMFS/other/s2.dat", root);
    CHK(MoveFileExA(a2, b2, MOVEFILE_WRITE_THROUGH));
    snprintf(t, sizeof t, "%s/RomFS/Other/s2.dat", root); CHK(access(t, F_OK) == 0);
    /* negative: no match anywhere */
    snprintf(p, sizeof p, "%s/romfs/data/nothere.bin", root);
    h = CreateFileA(p, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL); CHK(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND);
    CHK(GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND);
    CHK(plat_fopen(p, "r") == NULL && errno == ENOENT);
    CHK(!DeleteFileA(p) && GetLastError() == ERROR_FILE_NOT_FOUND);
    snprintf(p, sizeof p, "%s/romfs/nodir/x.bin", root);
    h = CreateFileA(p, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL); CHK(h == INVALID_HANDLE_VALUE && (GetLastError() == ERROR_PATH_NOT_FOUND || GetLastError() == ERROR_FILE_NOT_FOUND));
    snprintf(p, sizeof p, "%s/romfs/nodir/*", root);
    h = FindFirstFileA(p, &fd); CHK(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND);
    snprintf(p, sizeof p, "%s/romfs/data/*.zzz", root);
    h = FindFirstFileA(p, &fd); CHK(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND);
    /* cleanup */
    char cmd[700]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", root); if (g_bad == 0) system(cmd);
    printf("fs: %d checks, %d failed\n", g_n, g_bad);
    return g_bad != 0;
}

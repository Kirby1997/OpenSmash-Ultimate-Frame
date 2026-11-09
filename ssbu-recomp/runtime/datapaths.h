/* datapaths.h - where the player's own game data are on Linux (WP-U5a), and whether they are usable.
 *
 * Nothing of the game ships with the port: the player points it at the dump folder tools/extract/dump_folder.py makes
 * from his own cartridge / eShop copy and the 13.0.4 or 13.0.5 update:
 *     <dump>/exefs/main ...        optional at run time (the game code is compiled in); when present it names the dump's build
 *     <dump>/romfs/data.arc        required, with romfs/.nrr/<name>.nrr (the script module lists)
 *     <dump>/sysdata/fonts/<7 .ttf>  required: the 7 shared system fonts (nnpl.c stops with exit code 7 without them)
 *     <dump>/sysdata/zoneinfo      optional
 * A RomFS folder itself (the one holding data.arc) is accepted too; its fonts are then looked for in ../sysdata/fonts.
 * prod.keys and the .xci/.nsp/.nsz packages are never read here: a folder holding only packages gets a message naming
 * tools/extract/dump_folder.py.
 *
 * Order of the sources, first found wins (dp_resolve):
 *   DP_DATA      --data <dir> > SSBU_DATA > SSBU_ROMFS > config "data" > [only when its data.arc exists:] <exe>/../state.json
 *                dump.romfs (scripts/setup.py) > <exe>/../romfs > not configured
 *   DP_FONTS     --fonts > SSBU_FONTS > config "fonts" > <dump>/sysdata/fonts (or <romfs>/../sysdata/fonts when the RomFS
 *                folder itself was chosen) > <exe>/../sysdata/fonts (setup.py's convert step copies the dump's fonts there)
 *   DP_DLC       --dlc > SSBU_DLC_LIST > config "dlc" > <exe>/../config/dlc.txt (exists) > $XDG_CONFIG_HOME/ssbu-recomp/dlc.txt
 *                (exists) > none owned.  A file is a dlc.txt list (nnmisc.c aoc_load), a folder is scanned for the
 *                add-on content ids in the file names of the player's DLC dumps (as tools/audit/dlc_list.py does).
 *   DP_USERDATA  --userdata > SSBU_USERDATA > config "userdata" > <exe>/../userdata (exists: older builds keep their
 *                saves) > $XDG_DATA_HOME/ssbu-recomp/userdata
 * Explicit sources (CLI, environment, config) are returned even when the folder is unusable, so that the error names
 * what the player chose; the build-layout fallbacks are only taken when they hold the data, as on Windows.
 *
 * The saved choice is $XDG_CONFIG_HOME/ssbu-recomp/config (default ~/.config/ssbu-recomp/config): `key=value` lines
 * (data, fonts, dlc, userdata; other keys and `#` comments are kept), rewritten atomically (temporary file, fsync,
 * rename, fsync of the folder). A line that is not key=value, an over-long line or a file over 64 KiB is ignored.
 *
 * Linux only (the Windows build keeps runtime/boot.c's own ..\state.json reader and is unchanged). Not thread-safe (static
 * buffers): boot.c calls it before any game thread exists, nnmisc.c once at the first nn::aoc call, U5b from its picker. */
#ifndef SSBU_DATAPATHS_H
#define SSBU_DATAPATHS_H
#include <stddef.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DP_PATH 4096
#define DP_EXIT_DATA 9              /* exit code of a start whose game data are missing or unusable (unattended, or no picker) */

typedef enum { DP_DATA, DP_FONTS, DP_DLC, DP_USERDATA, DP_KINDS } dp_kind;
typedef enum {
    DP_SRC_NONE,                    /* nothing found */
    DP_SRC_CLI,                     /* --data / --fonts / --dlc / --userdata */
    DP_SRC_ENV,                     /* SSBU_DATA, SSBU_ROMFS, SSBU_FONTS, SSBU_DLC_LIST, SSBU_USERDATA */
    DP_SRC_CONFIG,                  /* the saved choice */
    DP_SRC_STATE,                   /* <exe>/../state.json written by scripts/setup.py */
    DP_SRC_LAYOUT,                  /* the build folder's own layout (<exe>/../romfs, ../sysdata/fonts, ../config/dlc.txt, ../userdata) */
    DP_SRC_DUMP,                    /* fonts: beside the chosen dump / RomFS */
    DP_SRC_DEFAULT                  /* the XDG default */
} dp_source;
typedef enum {
    DP_OK,
    DP_NOT_CONFIGURED,              /* no data folder from any source */
    DP_NOT_FOUND,                   /* the folder chosen does not exist or is not a folder */
    DP_PACKAGES_ONLY,               /* only .xci / .nsp / .nsz packages in it: they must be extracted first (dump_folder.py) */
    DP_MISSING_ROMFS,               /* no romfs/data.arc (nor data.arc itself) */
    DP_MISSING_NRR,                 /* data.arc but no .nrr/<name>.nrr beside it */
    DP_UNKNOWN_VERSION,             /* exefs/main is not one of the builds this port handles */
    DP_WRONG_VERSION,               /* exefs/main is the other known build than the one this exe was built from */
    DP_MISSING_FONTS                /* one of the 7 system font files is missing or empty */
} dp_status;
#define DP_WARN_NO_EXEFS 1u         /* no exefs/main beside the RomFS: the dump's build was not checked */

typedef struct {
    dp_status status;
    unsigned warnings;              /* DP_WARN_* */
    dp_source source, fonts_source;
    char dir[DP_PATH];              /* the folder that was checked, as chosen */
    char romfs[DP_PATH];            /* absolute RomFS folder (holds data.arc), once found */
    char fonts[DP_PATH];            /* absolute fonts folder, once found */
    char missing[DP_PATH];          /* the file or folder whose absence is the error */
    char found[32], expected[24];   /* DP_UNKNOWN_VERSION / DP_WRONG_VERSION: e.g. "13.0.5" or "code size 0x1234", "13.0.4" */
    char message[3072];             /* for the player: what is wrong and what to do (several lines) */
} dp_result;

/* ---- for runtime/boot.c */
int dp_take_args(int *argc, char **argv);         /* removes --data/--fonts/--dlc/--userdata <v> (or =v), --check-data and --pick-data; -1 = a value is missing */
int dp_check_only(void);                          /* --check-data was given: print the result and exit */
int dp_pick_requested(void);                      /* --pick-data, or SSBU_PICK_DATA set and not "0": open the picker even when the data are usable */
int dp_check(const char *expect, dp_result *r);   /* resolve DP_DATA, then dp_validate; returns r->status == DP_OK */
void dp_apply(const dp_result *r);                /* exports SSBU_ROMFS / SSBU_FONTS (when found), SSBU_USERDATA (always, folder created), SSBU_TZDIR (dump's zoneinfo, if unset) */
void dp_report(FILE *out, const dp_result *r);    /* --check-data: the result, every source and the message */
int dp_unattended(void);                          /* scripted run (SSBU_SEQ/PLAY/PRESS/SCRIPT) or SSBU_ERROR_DIALOG=0: never ask, just exit */
int dp_default_elf(char *out, size_t n);          /* <exe>/../binaries/<state.json game_version, else the one present>/main.elf; 1 = it exists */
const char *dp_elf_build(const char *elf);        /* "13.0.4" / "13.0.5" from main.elf's first PT_LOAD, or NULL */
const char *dp_nso_build(const char *nso, unsigned *text_size);   /* the same from an ExeFS `main` (NSO header +0x18) */

/* ---- the C API for the picker / error screen (WP-U5b) */
int dp_validate(const char *dir, const char *expect, dp_result *r);   /* expect "13.0.4", "13.0.5" or NULL (either); returns status == DP_OK */
int dp_save_choice(const char *dir);              /* config "data" = the absolute folder (it is not validated here); 0 or -errno */
int dp_current_choice(char *out, size_t n, dp_source *src);           /* = dp_resolve(DP_DATA, ...) */
/* Set by U5b (datapicker_sdl.c). Called by boot.c when the run is attended and the data are not usable, or are usable but
 * dp_pick_requested(); `r` says why. Return 1 when the player chose a folder and it was saved (boot.c then calls dp_picked
 * and checks again: the saved choice now wins over --data / SSBU_DATA / SSBU_ROMFS); 0 to give up (unusable data: boot.c
 * prints r->message and exits 9) or, with usable data, to start with them; -1 when the player quit (exit 9 with unusable
 * data, 0 with usable ones). */
extern int (*dp_picker_hook)(const dp_result *r);
void dp_picked(const dp_result *r);               /* after the hook returned 1: drop the overrides that would hide the saved choice */
int dp_validate_picked(const char *dir, const char *expect, const dp_result *was, dp_result *r);   /* dp_validate as boot.c's check after dp_picked(was) will see it */
int dp_fonts_check(const char *dir, char *missing, size_t n);         /* the 7 system fonts are in `dir` (not empty); 0: *missing = the first one absent */

/* ---- lower level */
int dp_resolve(dp_kind k, char *out, size_t n, dp_source *src);       /* 1 = found (out set), 0 = none */
void dp_set_cli(dp_kind k, const char *value);    /* what --data etc. set (NULL clears) */
int dp_config_path(char *out, size_t n);          /* $XDG_CONFIG_HOME/ssbu-recomp/config */
int dp_config_get(const char *key, char *out, size_t n);              /* 1 = present (out set, ~/ expanded) */
int dp_config_set(const char *key, const char *value);                /* NULL value removes the key; atomic; 0 or -errno */
int dp_xdg_dir(int which, char *out, size_t n);   /* 'c' config, 'd' data, 's' state: $XDG_*_HOME or ~/.config, ~/.local/share, ~/.local/state, + /ssbu-recomp */
int dp_state_string(const char *key, char *out, size_t n);            /* a string of <exe>/../state.json (first "key": "..."), UTF-8 */
int dp_exe_path(const char *rel, char *out, size_t n);                /* <exe dir> + rel ('\\' or '/'), case-insensitively resolved; 1 = exists */
int dp_dlc_scan(const char *dir, int *idx, int max);                  /* add-on content indices in the names in `dir`, sorted, unique */
int dp_is_dir(const char *path);
const char *dp_source_name(dp_source s);
const char *dp_status_name(dp_status s);
void dp_set_base(const char *exe_dir);            /* tests: use this folder instead of the exe's (NULL = the exe's) */

#ifdef __cplusplus
}
#endif
#endif

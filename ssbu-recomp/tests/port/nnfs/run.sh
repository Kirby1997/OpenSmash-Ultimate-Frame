#!/bin/sh
# The game's file services on the rebuilt Linux layer (test_nnfs.c): nnfs.c (nn::fs: romfs-like tree, journaled save data, Commit
# and its recovery after kill -9, failed moves / deletions / marker removals, two processes), nnfsdir.c, nnpl.c (shared fonts) and gfx_options.c on plat_posix.c's Win32 file
# layer. No game data. SDL3 and OpenSSL are stand-ins (no test reaches them).
#   tests/port/nnfs/run.sh [mode]
#     native  (default) build and run here with $CC (cc): on the Frame (aarch64 Linux), or on any Linux
#     mac     macOS arm64: the Linux code paths (PLAT_LINUX_PATHS_ON_MAC); the file layer is POSIX either way
#   OUT (default ${TMPDIR:-/tmp}/ssbu-nnfs-<mode>), CC, CFLAGS_EXTRA; the scratch folder is made under $TMPDIR (argument 2 of test_nnfs).
#   FS_NO_KILL=1 leaves out the kill -9 runs (about 1 minute on a Mac, more on a slow disk).
set -eu
cd "$(dirname "$0")/../../.."
MODE=${1:-native}
OUT=${OUT:-${TMPDIR:-/tmp}/ssbu-nnfs-$MODE}; mkdir -p "$OUT"
T=tests/port/nnfs
. tests/port/native_env.sh
GT="-include tests/port/threads/game_tables.h -I tests/cspike/stubs"
BASE="-O1 -g -w -fvisibility=hidden -fno-omit-frame-pointer -include runtime/plat/plat_pre.h -I runtime/plat -I runtime ${CFLAGS_EXTRA:-}"
case "$MODE" in
native)
    CC=${CC:-cc}
    F="-fno-pie $BASE $NAT_INC"
    L="-no-pie $NAT_SDL_LIBS $NAT_SSL_LIBS -lm -lpthread -ldl" ;;
mac)
    CC=${CC:-clang}
    SDK=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk          # (the default SDK of some installs does not link)
    [ -d "$SDK" ] || SDK=$(xcrun --show-sdk-path)
    NAT_SSL_LIBS=""; NAT_SDL_LIBS=""; NAT_SYM_PREFIX=_
    F="-isysroot $SDK -mmacosx-version-min=14.4 -DPLAT_LINUX_PATHS_ON_MAC $BASE -idirafter tests/port/threads/stub"
    L="" ;;
*) echo "mode: native | mac"; exit 2 ;;
esac
SCRATCH=${TMPDIR:-/tmp}
if [ "$MODE" = mac ] && [ -z "${NNFS_NO_CSVOL:-}" ] && command -v hdiutil >/dev/null 2>&1; then
    # macOS volumes are case-insensitive: the case-insensitive lookup of plat_posix.c is tested on a case-sensitive APFS image
    rm -f "$OUT/cs.sparseimage"
    mkdir -p "$OUT/cs"
    if hdiutil create -size 300m -fs "Case-sensitive APFS" -volname ssbucs -type SPARSE "$OUT/cs" >/dev/null 2>&1 &&
       hdiutil attach "$OUT/cs.sparseimage" -nobrowse -mountpoint "$OUT/cs" >/dev/null 2>&1; then
        trap 'hdiutil detach "$OUT/cs" -force >/dev/null 2>&1; rm -f "$OUT/cs.sparseimage"' EXIT
        SCRATCH=$OUT/cs
    else echo "(no case-sensitive volume could be made: the case-insensitive lookup section is skipped)"; fi
fi
$CC $F -c runtime/plat_posix.c -o "$OUT/plat_posix.o"
$CC $(echo "$F" | sed 's#-include runtime/plat/plat_pre.h##; s#-I runtime/plat ##') -std=gnu11 -c runtime/plat_crash.c -o "$OUT/plat_crash.o"
OBJS="$OUT/plat_posix.o $OUT/plat_crash.o"
$CC $F -include $T/fault.h -c "${NNFS_SRC:-runtime/nnfs.c}" -o "$OUT/nnfs.o"             # the one unit whose disk calls the test can stop (NNFS_SRC: another version of it)
for u in nnfsdir nnpl gfx_options; do $CC $F $GT -c runtime/$u.c -o "$OUT/$u.o"; done
$CC $F -c $T/fault_wrap.c -o "$OUT/fault_wrap.o"
OBJS="$OBJS $OUT/nnfs.o $OUT/nnfsdir.o $OUT/nnpl.o $OUT/gfx_options.o $OUT/fault_wrap.o"
$CC $F $GT -c $T/test_nnfs.c -o "$OUT/test_nnfs.o"
nat_stubs "$OUT/stubs.c" $OBJS "$OUT/test_nnfs.o"
$CC $F -c "$OUT/stubs.c" -o "$OUT/stubs.o"
$CC $F "$OUT/test_nnfs.o" $OBJS "$OUT/stubs.o" $L -o "$OUT/test_nnfs"
timeout 900 "$OUT/test_nnfs" "$SCRATCH"

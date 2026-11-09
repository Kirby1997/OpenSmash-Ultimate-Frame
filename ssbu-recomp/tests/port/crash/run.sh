#!/bin/sh
# C4 crash-report test (runtime/plat_crash.c). Native: builds with the host's cc and runs; works on aarch64 Linux (the Steam
# Frame: tests/port/crash/run_frame.sh) and on macOS arm64 (a development Mac). Standalone: no sysroot, no game files.
#   run.sh [scratch-dir]    CC=gcc|clang   CFG=pac (-mbranch-protection=standard) | fortify (-O2 -D_FORTIFY_SOURCE=2)
set -eu
cd "$(dirname "$0")/../../.."
OUT=${OUT:-${TMPDIR:-/tmp}/ssbu-c4}; mkdir -p "$OUT"
CC=${CC:-cc}
OPT="-O1"; EXTRA=""
case "${CFG:-}" in
    fortify) OPT="-O2 -D_FORTIFY_SOURCE=2" ;;
    pac) EXTRA="-mbranch-protection=standard" ;;
esac
case "$(uname -s)" in Linux) LIBS="-pthread -rdynamic" ;; *) LIBS="" ;; esac
# plat_crash.c is held to -Wall -Wextra -Werror
$CC $OPT $EXTRA -g -fno-omit-frame-pointer -Wall -Wextra -Werror -std=gnu11 -Iruntime -c runtime/plat_crash.c -o "$OUT/plat_crash.o"
$CC $OPT $EXTRA -g -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-infinite-recursion -Wno-unused-result -std=gnu11 -Iruntime \
    tests/port/crash/test_crash.c "$OUT/plat_crash.o" $LIBS -o "$OUT/test_crash"
mkdir -p "$OUT/scratch"
timeout 300 "$OUT/test_crash" "${1:-$OUT/scratch}"

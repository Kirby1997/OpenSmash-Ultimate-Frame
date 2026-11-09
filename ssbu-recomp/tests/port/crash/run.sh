#!/bin/sh
# C4 crash-report test (runtime/plat_crash.c) and the A64 audit decoder (runtime/a64_trivial.h, tests/port/crash/test_a64.c). Native: builds with the host's cc and runs; works on aarch64 Linux (the Steam
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
# runtime/a64_trivial.h (hostrt.c audit_trivial on Linux aarch64) against hand-assembled A64 and the handler shapes this compiler makes
$CC $OPT $EXTRA -g -fno-omit-frame-pointer -c tests/port/crash/a64_funcs.c -o "$OUT/a64_funcs.o"
$CC $OPT $EXTRA -g -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -std=gnu11 -Iruntime tests/port/crash/test_a64.c "$OUT/a64_funcs.o" -o "$OUT/test_a64"
mkdir -p "$OUT/scratch"
timeout 60 "$OUT/test_a64"
timeout 300 "$OUT/test_crash" "${1:-$OUT/scratch}"

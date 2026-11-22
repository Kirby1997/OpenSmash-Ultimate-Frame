#!/bin/sh
# The vsync thread's wait (runtime/vsync_wait.h, Linux): the arithmetic, then real 60 Hz waits on this machine's clock.
# No GPU, no game. $1 = x86 (default: the PC's cc) | native (ON the Frame: $CC, default cc) | arm64 (cross-build only,
# tests/cspike/xenv.sh). VW_TICKS=<n> waits (default 240, 4 s).
set -eu
cd "$(dirname "$0")/../../.."
OUT=${OUT:-/tmp/ssbu-vsyncwait}; mkdir -p "$OUT"
MODE=${1:-x86}
F="-O2 -g -std=gnu11 -Wall -Wextra -Werror -Iruntime"
if [ "$MODE" = arm64 ]; then
    . tests/cspike/xenv.sh
    $CC $X_ARCH $X_CODEGEN $X_LD $F tests/port/vsyncwait/test_vsyncwait.c -o "$OUT/test_vsyncwait"
    echo "built $OUT/test_vsyncwait"; exit 0
fi
${CC:-cc} $F tests/port/vsyncwait/test_vsyncwait.c -o "$OUT/test_vsyncwait"
timeout 120 "$OUT/test_vsyncwait"

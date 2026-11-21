#!/bin/sh
# First-run preparation screen: its numbers and text (runtime/nvn_vk_prepscreen.h), no GPU, no game.
# $1 = x86 (default: the PC's cc) | native (ON the Frame: $CC, default cc) | arm64 (cross-build only, tests/cspike/xenv.sh).
set -eu
cd "$(dirname "$0")/../../.."
OUT=${OUT:-/tmp/ssbu-prepscreen}; mkdir -p "$OUT"
MODE=${1:-x86}
F="-O1 -g -std=gnu11 -Wall -Wextra -Werror -Iruntime"
if [ "$MODE" = arm64 ]; then
    . tests/cspike/xenv.sh
    $CC $X_ARCH $X_CODEGEN $X_LD $F tests/port/prepscreen/test_prepscreen.c -lm -o "$OUT/test_prepscreen"
    echo "built $OUT/test_prepscreen"; exit 0
fi
${CC:-cc} $F tests/port/prepscreen/test_prepscreen.c -lm -o "$OUT/test_prepscreen"
timeout 60 "$OUT/test_prepscreen"

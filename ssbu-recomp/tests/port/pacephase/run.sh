#!/bin/sh
# The phase hold's arithmetic (runtime/pace_phase.h, display.c SSBU_PACE_PHASE=1). No clock, no GPU, no game.
# $1 = x86 (default: the PC's cc) | native (ON the Frame: $CC, default cc) | arm64 (cross-build only, tests/cspike/xenv.sh).
set -eu
cd "$(dirname "$0")/../../.."
OUT=${OUT:-/tmp/ssbu-pacephase}; mkdir -p "$OUT"
MODE=${1:-x86}
F="-O2 -g -std=gnu11 -Wall -Wextra -Werror -Iruntime"
if [ "$MODE" = arm64 ]; then
    . tests/cspike/xenv.sh
    $CC $X_ARCH $X_CODEGEN $X_LD $F tests/port/pacephase/test_pacephase.c -o "$OUT/test_pacephase"
    echo "built $OUT/test_pacephase"; exit 0
fi
${CC:-cc} $F tests/port/pacephase/test_pacephase.c -o "$OUT/test_pacephase"
"$OUT/test_pacephase"

#!/bin/sh
# SSBU_CPU_PLACE's core-type detection (runtime/cpu_place.h) against fake sysfs trees. No game, no thread is moved.
# $1 = x86 (default: the PC's cc) | native (ON the Frame: $CC, default cc) | arm64 (cross-build only, tests/cspike/xenv.sh).
set -eu
cd "$(dirname "$0")/../../.."
OUT=${OUT:-/tmp/ssbu-cpuplace}; mkdir -p "$OUT"
MODE=${1:-x86}
F="-O2 -g -std=gnu11 -Wall -Wextra -Werror -Iruntime"
if [ "$MODE" = arm64 ]; then
    . tests/cspike/xenv.sh
    $CC $X_ARCH $X_CODEGEN $X_LD $F tests/port/cpuplace/test_cpuplace.c -o "$OUT/test_cpuplace"
    echo "built $OUT/test_cpuplace"; exit 0
fi
${CC:-cc} $F tests/port/cpuplace/test_cpuplace.c -o "$OUT/test_cpuplace"
"$OUT/test_cpuplace"

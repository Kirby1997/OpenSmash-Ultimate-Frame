#!/bin/sh
# C-spike: cross-build for the Frame (aarch64) what needs no game data, into $OUT (default /tmp/ssbu-cspike-bin):
#   plat_selftest            tests/plat_selftest.c + plat_posix.c (upstream's command line, cross)
#   ssbu_native              every host object of xcheck.sh, linked as build.py links it on Linux
#   ssbu_game_eh_o1.so       tests/cspike/nogame.c: a game library with no game (the exe's no-dump modes, e.g. SSBU_PAD_SELFTEST)
# lld honours the objects' `#pragma comment(lib, ...)` (.deplibs; GNU ld ignores them): --no-dependent-libraries.
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
cd "$REPO"
. tests/cspike/xenv.sh
OUT=${OUT:-/tmp/ssbu-cspike-bin}
OBJ=${OBJ:-/tmp/ssbu-cspike-check}
mkdir -p "$OUT"
$CC $X_ARCH $X_CODEGEN $X_LD -O1 -g -w $X_PLAT -I$SDL3_ROOT/include tests/plat_selftest.c runtime/plat_posix.c runtime/plat_crash.c \
    -L$SDL3_ROOT/lib -lSDL3 -lssl -lcrypto -lm -lpthread -ldl -o "$OUT/plat_selftest"
OUT=$OBJ tests/cspike/xcheck.sh >/dev/null || { echo "xcheck failed (logs in $OBJ)"; exit 1; }
$CC $X_ARCH $X_LD $X_CODEGEN -O1 -w -fPIC -shared -I runtime $X_PLAT tests/cspike/nogame.c -Wl,-soname,ssbu_game_eh_o1.so \
    -o "$OUT/ssbu_game_eh_o1.so"
$CC $X_ARCH $X_LD -Wl,--no-dependent-libraries "$OBJ"/*.o -L"$OUT" -l:ssbu_game_eh_o1.so '-Wl,-rpath,$ORIGIN' $X_PKG_LIBS \
    -lGL -lm -ldl -lpthread -o "$OUT/ssbu_native"
ls -la "$OUT"

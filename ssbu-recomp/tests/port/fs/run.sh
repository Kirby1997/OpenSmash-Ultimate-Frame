#!/bin/sh
# C9 fs test. $1 = x86 (default) | arm64 (cross-build only; run on the Frame) | native (ON the Frame: build with $CC (cc), run;
# SDL3 from $SDL3_ROOT or abort() stubs, see tests/port/native_env.sh). Reuses tests/cspike/c3_build.sh's flags via OUT.
set -eu
cd "$(dirname "$0")/../../.."
OUT=${OUT:-/tmp/ssbu-c9}; mkdir -p "$OUT"
MODE=${1:-x86}
if [ "$MODE" = native ]; then
    . tests/port/native_env.sh
    CCN=${CC:-cc}
    F="-fno-pie -no-pie -O1 -g -w -fvisibility=hidden -include runtime/plat/plat_pre.h -I runtime/plat $NAT_INC"
    $CCN $F -c runtime/plat_posix.c -o "$OUT/pp.o"
    nat_stubs "$OUT/stubs.c" "$OUT/pp.o"
    $CCN $F tests/port/fs/test_fs.c runtime/plat_posix.c runtime/plat_heap.c runtime/plat_crash.c "$OUT/stubs.c" $NAT_SDL_LIBS $NAT_SSL_LIBS -lm -lpthread -ldl -o "$OUT/test_fs"
    timeout 60 "$OUT/test_fs" "${TMPDIR:-/tmp}"
elif [ "$MODE" = arm64 ]; then
    . tests/cspike/xenv.sh
    F="$X_ARCH $X_CODEGEN $X_LD -fno-pie -no-pie -O1 -g -w -fvisibility=hidden -include runtime/plat/plat_pre.h -I runtime/plat -I$SDL3_ROOT/include"
    L="-L$SDL3_ROOT/lib -Wl,-rpath-link,$SDL3_ROOT/lib -lSDL3 -lssl -lcrypto -lm -lpthread -ldl"
    $CC $F tests/port/fs/test_fs.c runtime/plat_posix.c runtime/plat_heap.c runtime/plat_crash.c $L -o "$OUT/test_fs"      # plat_crash.c: plat_posix.c's thread start (C4)
else
    S=${SYSROOT:-$HOME/sysroots/steam-frame}; SDL3_ROOT=${SDL3_ROOT:-$S/opt/ssbu-deps/sdl3}
    mkdir -p "$OUT/inc/linux"; cp "$S/usr/include/linux/userfaultfd.h" "$S/usr/include/linux/fs.h" "$OUT/inc/linux/"
    ${CC:-clang} -O1 -w -fPIC -include runtime/plat/plat_pre.h -I runtime/plat -I"$OUT/inc" -I"$SDL3_ROOT/include" -idirafter "$S/usr/include" -c runtime/plat_posix.c -o "$OUT/pp.o"
    nm -u "$OUT/pp.o" | sed -n 's/^ *U \(SDL_[A-Za-z0-9_]*\)$/void \1(void){__builtin_abort();}/p' > "$OUT/sdlstub.c"
    F="-O1 -g -w -fPIC -fvisibility=hidden -include runtime/plat/plat_pre.h -I runtime/plat -I$OUT/inc -I$SDL3_ROOT/include -idirafter $S/usr/include"
    ${CC:-clang} $F tests/port/fs/test_fs.c runtime/plat_posix.c "$OUT/sdlstub.c" /usr/lib/x86_64-linux-gnu/libcrypto.so.3 -lm -lpthread -ldl -o "$OUT/test_fs"
    timeout 60 "$OUT/test_fs" "${TMPDIR:-/tmp}"
fi

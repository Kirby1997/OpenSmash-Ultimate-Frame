#!/bin/sh
# C3: build tests/cspike/c3_low.c + plat_selftest. $1 = arm64 (cross, with plat_heap.c, -no-pie), x86 (native on x86 WSL) or
# native (ON the Steam Frame, $CC (cc), same flags as arm64; SDL3 from $SDL3_ROOT or abort() stubs: tests/port/native_env.sh). OUT default /tmp/ssbu-c3.
set -eu
cd "$(dirname "$0")/../.."
OUT=${OUT:-/tmp/ssbu-c3}; mkdir -p "$OUT"
if [ "${1:-x86}" = native ]; then
    . tests/port/native_env.sh
    CCN=${CC:-cc}
    F="-fno-pie -no-pie -O1 -g -w -fvisibility=hidden -include runtime/plat/plat_pre.h -I runtime/plat $NAT_INC"
    $CCN $F -c runtime/plat_posix.c -o "$OUT/pp.o"
    nat_stubs "$OUT/stubs.c" "$OUT/pp.o"
    # libcrypto is real (plat_selftest checks SHA-256 through it); without it the EVP_* stubs abort and that check fails
    $CCN $F tests/cspike/c3_low.c runtime/plat_posix.c runtime/plat_heap.c runtime/plat_crash.c "$OUT/stubs.c" $NAT_SDL_LIBS $NAT_SSL_LIBS -lm -lpthread -ldl -o "$OUT/c3_low"
    $CCN $F tests/plat_selftest.c runtime/plat_posix.c runtime/plat_heap.c runtime/plat_crash.c "$OUT/stubs.c" $NAT_SDL_LIBS $NAT_SSL_LIBS -lm -lpthread -ldl -o "$OUT/plat_selftest"
elif [ "${1:-x86}" = arm64 ]; then
    . tests/cspike/xenv.sh
    F="$X_ARCH $X_CODEGEN $X_LD -fno-pie -no-pie -O1 -g -w -fvisibility=hidden -include runtime/plat/plat_pre.h -I runtime/plat -I$SDL3_ROOT/include"
    L="-L$SDL3_ROOT/lib -Wl,-rpath-link,$SDL3_ROOT/lib -lSDL3 -lssl -lcrypto -lm -lpthread -ldl"
    # plat_crash.c: plat_posix.c's thread start calls its crash_thread_init on Linux aarch64 (C4)
    $CC $F tests/cspike/c3_low.c runtime/plat_posix.c runtime/plat_heap.c runtime/plat_crash.c $L -o "$OUT/c3_low"
    $CC $F tests/plat_selftest.c runtime/plat_posix.c runtime/plat_heap.c runtime/plat_crash.c $L -o "$OUT/plat_selftest"
else
    # x86 WSL has no SDL3 / OpenSSL / new-kernel dev packages. Headers: SDL3, OpenSSL and linux/{userfaultfd,fs}.h come from the Frame sysroot
    # (arch-neutral); libcrypto is the system one by path; SDL_* symbols get abort() stubs (no test here reaches them).
    # With pkg-config and real dev packages, PKG=1 uses those instead.
    S=${SYSROOT:-$HOME/sysroots/steam-frame}; SDL3_ROOT=${SDL3_ROOT:-$S/opt/ssbu-deps/sdl3}
    if [ -n "${PKG:-}" ]; then P="$(pkg-config --cflags sdl3 openssl)"; L="$(pkg-config --libs sdl3 openssl) -lm -lpthread -ldl"
    else
        mkdir -p "$OUT/inc/linux"; cp "$S/usr/include/linux/userfaultfd.h" "$S/usr/include/linux/fs.h" "$OUT/inc/linux/"
        ${CC:-clang} -O1 -w -fPIC -include runtime/plat/plat_pre.h -I runtime/plat -I"$OUT/inc" -I"$SDL3_ROOT/include" -idirafter "$S/usr/include" -c runtime/plat_posix.c -o "$OUT/pp.o"
        nm -u "$OUT/pp.o" | sed -n 's/^ *U \(SDL_[A-Za-z0-9_]*\)$/void \1(void){__builtin_abort();}/p' > "$OUT/sdlstub.c"
        P="-I$OUT/inc -I$SDL3_ROOT/include -idirafter $S/usr/include"; L="$OUT/sdlstub.c /usr/lib/x86_64-linux-gnu/libcrypto.so.3 -lm -lpthread -ldl"
    fi
    F="-O1 -g -w -fPIC -fvisibility=hidden -include runtime/plat/plat_pre.h -I runtime/plat $P"
    ${CC:-clang} $F tests/cspike/c3_low.c runtime/plat_posix.c $L -o "$OUT/c3_low"
    ${CC:-clang} $F tests/plat_selftest.c runtime/plat_posix.c $L -o "$OUT/plat_selftest"
fi
ls "$OUT"

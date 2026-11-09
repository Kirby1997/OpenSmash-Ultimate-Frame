#!/bin/sh
# The Win32 stand-in layer (runtime/plat/windows.h + plat_posix.c) as the runtime uses it, beyond tests/plat_selftest.c:
#   test_headers   windows.h and the other stand-in headers with every system header, in both orders, -Wall -Wextra -Werror
#   test_virtual   VirtualAlloc / Free / Protect / Query: fixed addresses, collisions, commit cycles, host memory, a walk, 8 threads
#   test_readable  IsBadReadPtr (VirtualQuery) against plat_crash.c's crash_read (the kernel's EFAULT) on every kind of memory
#   test_modules   LoadLibraryA / GetModuleHandleExA / GetProcAddress / GetModuleFileNameA: how nncxxrt.c binds the game library
#   test_crypto    BCryptHash SHA-256 (built in; nnro.c's NRR check) and the CryptoAPI SHA-1 / MD5 / SHA-256 (OpenSSL) against vectors
# No game data. SDL3 is a stand-in (no test reaches it); libcrypto is the system's when there is one (else test_crypto skips
# its OpenSSL half).
#   tests/port/winshim/run.sh [mode]
#     native  (default) build and run here with $CC (cc): on the Frame (aarch64 Linux), or on any Linux
#     mac     macOS arm64: the Linux code paths (PLAT_LINUX_PATHS_ON_MAC) built and run natively, as the threads suite does; libcrypto from
#             Homebrew's openssl@3 when installed. Stack / heap placement below 2^47 is not checked there.
#   OUT (default ${TMPDIR:-/tmp}/ssbu-winshim-<mode>) for the binaries, CC, CFLAGS_EXTRA.
set -eu
cd "$(dirname "$0")/../../.."
MODE=${1:-native}
OUT=${OUT:-${TMPDIR:-/tmp}/ssbu-winshim-$MODE}; mkdir -p "$OUT"
T=tests/port/winshim
. tests/port/native_env.sh
BASE="-O1 -g -w -fvisibility=hidden -fno-omit-frame-pointer -include runtime/plat/plat_pre.h -I runtime/plat -I runtime -I tests/port/threads ${CFLAGS_EXTRA:-}"
case "$MODE" in
native)
    CC=${CC:-cc}
    F="-fno-pie $BASE $NAT_INC"
    L="-no-pie -rdynamic $NAT_SDL_LIBS $NAT_SSL_LIBS -lm -lpthread -ldl"
    SO=.so; SHARED="-fPIC -shared -Wl,-soname,dlbind_lib.so"; RPATH="-Wl,-rpath,$OUT"
    HEAP=""
    if [ "$(uname -m)" = aarch64 ]; then HEAP="runtime/plat_heap.c"; fi ;;           # the exe's low malloc heap, as in the game
mac)
    CC=${CC:-clang}
    SDK=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk          # (the default SDK of some installs does not link)
    [ -d "$SDK" ] || SDK=$(xcrun --show-sdk-path)
    SSL=""
    for d in /opt/homebrew/opt/openssl@3/lib /usr/local/opt/openssl@3/lib; do [ -z "$SSL" ] && [ -e "$d/libcrypto.dylib" ] && SSL="$d/libcrypto.dylib"; done
    NAT_SSL_LIBS=$SSL; NAT_SDL_LIBS=""; NAT_SYM_PREFIX=_
    F="-isysroot $SDK -mmacosx-version-min=14.4 -DPLAT_LINUX_PATHS_ON_MAC $BASE -idirafter tests/port/threads/stub"
    L="$NAT_SSL_LIBS"
    SO=.dylib; SHARED="-dynamiclib -fPIC -install_name @rpath/dlbind_lib.dylib"; RPATH="-Wl,-rpath,$OUT"
    HEAP="" ;;
*) echo "mode: native | mac"; exit 2 ;;
esac
$CC $F -c runtime/plat_posix.c -o "$OUT/plat_posix.o"
$CC $(echo "$F" | sed 's#-include runtime/plat/plat_pre.h##; s#-I runtime/plat ##') -std=gnu11 -c runtime/plat_crash.c -o "$OUT/plat_crash.o"
OBJS="$OUT/plat_posix.o $OUT/plat_crash.o"
if [ -n "$HEAP" ]; then $CC $F -c $HEAP -o "$OUT/plat_heap.o"; OBJS="$OBJS $OUT/plat_heap.o"; fi
nat_stubs "$OUT/stubs.c" "$OUT/plat_posix.o"                       # abort() stubs for the SDL_* / EVP_* symbols no real library gives
$CC $F -c "$OUT/stubs.c" -o "$OUT/stubs.o"
OBJS="$OBJS $OUT/stubs.o"
for t in test_virtual test_readable test_crypto; do
    $CC $F $T/$t.c $OBJS $L -o "$OUT/$t"
done
# the headers, both orders, held to -Wall -Wextra -Werror
WF="-Wall -Wextra ${HDR_WERROR:--Werror}"                    # HDR_WERROR=-Wno-error: report the warnings of a compiler this was not tried with, do not fail
$CC $F $WF -DPLAT_FIRST -c $T/test_headers.c -o "$OUT/hdr_first.o"
$CC $F $WF -c $T/test_headers.c -o "$OUT/hdr_last.o"
$CC "$OUT/hdr_first.o" "$OUT/hdr_last.o" $OBJS $L -o "$OUT/test_headers"
# the game library stand-in, linked as the game library is (needed by name, found through the rpath)
$CC $F $SHARED $T/dlbind_lib.c -o "$OUT/dlbind_lib$SO"
$CC $F $T/test_modules.c $OBJS "$OUT/dlbind_lib$SO" $RPATH $L -o "$OUT/test_modules"
fail=0
for t in test_headers test_virtual test_readable test_modules test_crypto; do
    echo "== $OUT/$t"
    timeout 300 "$OUT/$t" || { echo "$t: exit $?"; fail=1; }
done
exit $fail

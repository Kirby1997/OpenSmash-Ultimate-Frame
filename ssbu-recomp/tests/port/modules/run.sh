#!/bin/sh
# nn::ro on the rebuilt Linux layer (test_nnro.c): nnro.c compiled INTO the test, plat_posix.c under it, and fake script modules
# (fake_mod.c + the real runtime/nro_shim.c) built the way tools/lifter/build.py builds a Linux module (-fPIC -fvisibility=hidden -shared
# -Wl,-Bsymbolic): NRR hash check, LoadModule through LoadLibraryA ("x.dll" -> x.so), relocation, protection, unload, reload,
# two modules with the same internal names next to an executable that exports them too, dlclose + dlopen. No game data.
#   tests/port/modules/run.sh [mode]      native (default) | mac      OUT (default ${TMPDIR:-/tmp}/ssbu-modules-<mode>), CC, CFLAGS_EXTRA
set -eu
cd "$(dirname "$0")/../../.."
MODE=${1:-native}
OUT=${OUT:-${TMPDIR:-/tmp}/ssbu-modules-$MODE}; mkdir -p "$OUT"
T=tests/port/modules
. tests/port/native_env.sh
GT="-include tests/port/threads/game_tables.h -I tests/cspike/stubs -I ."
BASE="-O1 -g -w -fno-omit-frame-pointer -include runtime/plat/plat_pre.h -I runtime/plat -I runtime ${CFLAGS_EXTRA:-}"
case "$MODE" in
native)
    CC=${CC:-cc}
    F="-fno-pie -fvisibility=hidden $BASE $NAT_INC"
    L="-no-pie -rdynamic $NAT_SDL_LIBS $NAT_SSL_LIBS -lm -lpthread -ldl"      # -rdynamic: the exe exports the names the modules have too (stricter than the real exe)
    EXT=.so; MODLINK="-shared -Wl,-Bsymbolic"; MODLINK_DEF="-shared -Wl,-Bsymbolic"; MODLINK_INT="-shared" ;;
mac)
    CC=${CC:-clang}
    SDK=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk          # (the default SDK of some installs does not link)
    [ -d "$SDK" ] || SDK=$(xcrun --show-sdk-path)
    NAT_SSL_LIBS=""; NAT_SDL_LIBS=""; NAT_SYM_PREFIX=_
    F="-isysroot $SDK -mmacosx-version-min=14.4 -DPLAT_LINUX_PATHS_ON_MAC -fvisibility=hidden $BASE -idirafter tests/port/threads/stub"
    L=""
    MODX="-isysroot $SDK -mmacosx-version-min=14.4 -Wno-unused-command-line-argument"
    EXT=.dylib; MODLINK="-dynamiclib"; MODLINK_DEF="-dynamiclib"; MODLINK_INT="-dynamiclib" ;;
*) echo "mode: native | mac"; exit 2 ;;
esac
$CC $F -c runtime/plat_posix.c -o "$OUT/plat_posix.o"
$CC $(echo "$F" | sed 's#-include runtime/plat/plat_pre.h##; s#-I runtime/plat ##') -std=gnu11 -c runtime/plat_crash.c -o "$OUT/plat_crash.o"
$CC $F $GT -DTEST_SO_EXT="\"$EXT\"" -I $T -c $T/test_nnro.c -o "$OUT/test_nnro.o"
nat_stubs "$OUT/stubs.c" "$OUT/plat_posix.o" "$OUT/test_nnro.o"
$CC $F -c "$OUT/stubs.c" -o "$OUT/stubs.o"
$CC $F "$OUT/test_nnro.o" "$OUT/plat_posix.o" "$OUT/plat_crash.o" "$OUT/stubs.o" $L -o "$OUT/test_nnro"
# a module: tools/lifter/build.py PLAT_FLAGS + the link flags of a Linux module (module=True)
MODC="-O1 -std=gnu11 -ffp-contract=off -fno-math-errno -fno-omit-frame-pointer -fno-strict-aliasing -fPIC -fno-semantic-interposition -include runtime/plat/plat_pre.h -I runtime/plat -I runtime ${MODX:-}"
MODSRC="$T/fake_mod.c $T/fake_who.c runtime/nro_shim.c"
mkdir -p "$OUT/nro_bin_test" "$OUT/extra"
mod() { # <out> <visibility> <link flags> <name> <who> <base>
    $CC $MODC $2 -I $T -DNRO_MODULE=$4 -DWHO="'$5'" -DMOD_BASE=$6 $3 $MODSRC -o "$1"
}
mod "$OUT/nro_bin_test/fake_a$EXT" -fvisibility=hidden "$MODLINK" fake_a A 0x7200000000ULL
mod "$OUT/nro_bin_test/fake_b$EXT" -fvisibility=hidden "$MODLINK" fake_b B 0x7200200000ULL
mod "$OUT/extra/fake_c$EXT" -fvisibility=hidden "$MODLINK" fake_c C 0x7200400000ULL
mod "$OUT/extra/fake_def$EXT" "" "$MODLINK_DEF" fake_def D 0x7200600000ULL          # control: default visibility, bound to itself (-Bsymbolic)
mod "$OUT/extra/fake_int$EXT" "" "$MODLINK_INT" fake_int I 0x7200800000ULL          # control: default visibility alone, interposed by the exe
fail=0
if [ "$MODE" = native ] && command -v nm >/dev/null 2>&1; then
    n=$(nm -D --defined-only "$OUT/nro_bin_test/fake_a$EXT" | grep -vc ' nro_bind$' || true)
    if [ "$n" = 0 ]; then echo "exports          PASS (only nro_bind)"; else echo "exports          FAIL ($n other symbols)"; fail=1; fi
    if command -v readelf >/dev/null 2>&1; then
        if readelf -d "$OUT/nro_bin_test/fake_a$EXT" | grep -q SYMBOLIC; then echo "DT_SYMBOLIC      PASS (-Bsymbolic)"; else echo "DT_SYMBOLIC      FAIL"; fail=1; fi
    fi
fi
SSBU_NRO_BIN=nro_bin_test timeout 300 "$OUT/test_nnro" "$OUT/extra" || fail=1
exit $fail

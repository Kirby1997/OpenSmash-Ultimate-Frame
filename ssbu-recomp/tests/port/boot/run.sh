#!/bin/sh
# C10a boot/datapaths test. $1 = x86 (default, builds and runs) | arm64 (cross-build only; run $OUT/test_boot <scratch> on the Frame)
# | native (ON the Frame: builds with $CC (cc) and runs; SDL3 from $SDL3_ROOT or stand-ins, see tests/port/native_env.sh).
# Synthetic fixtures only. NEVER builds with ASan (a vmem re-exec under ASan once fork-bombed the Frame).
set -eu
cd "$(dirname "$0")/../../.."
OUT=${OUT:-/tmp/ssbu-c10}; mkdir -p "$OUT"
MODE=${1:-x86}
[ "$MODE" = native ] || . tests/cspike/xenv.sh
DEFS="-Dfn_table=(*game_fn_table_p) -Dfn_table_count=(*game_fn_table_count_p) -D_GNU_SOURCE"
if [ "$MODE" = native ]; then
    . tests/port/native_env.sh
    F="-fPIC -O1 -g -w -include runtime/plat/plat_pre.h -I runtime/plat -Iruntime -Itests/port/boot $NAT_INC $DEFS"
    L="$NAT_SDL_LIBS $NAT_SSL_LIBS -lm -lpthread -ldl -rdynamic -Wl,-z,lazy -Wl,--unresolved-symbols=ignore-all"
    CCX=${CC:-cc}
elif [ "$MODE" = arm64 ]; then
    F="$X_ARCH $X_CODEGEN $X_LD -fPIC -O1 -g -w -include runtime/plat/plat_pre.h -I runtime/plat -Iruntime -Itests/port/boot -I$SDL3_ROOT/include $DEFS"
    L="-L$SDL3_ROOT/lib -Wl,-rpath-link,$SDL3_ROOT/lib -lSDL3 -lssl -lcrypto -lm -lpthread -ldl -rdynamic -Wl,-z,lazy -Wl,--no-dependent-libraries -Wl,--unresolved-symbols=ignore-all"
    CCX=$CC
else
    S=$SYSROOT
    mkdir -p "$OUT/inc/linux"; cp "$S/usr/include/linux/userfaultfd.h" "$S/usr/include/linux/fs.h" "$OUT/inc/linux/" 2>/dev/null || true
    F="-O1 -g -w -fno-pie -include runtime/plat/plat_pre.h -I runtime/plat -Iruntime -Itests/port/boot -I$OUT/inc -I$SDL3_ROOT/include -idirafter $S/usr/include $DEFS"
    L="-no-pie /usr/lib/x86_64-linux-gnu/libcrypto.so.3 -lm -lpthread -ldl -rdynamic -Wl,--unresolved-symbols=ignore-all"
    CCX=${CC_X86:-$CC}
fi
# datapaths.c is held to -Wall -Wextra -Werror
$CCX $F -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare -Werror -c runtime/datapaths.c -o "$OUT/datapaths.o"
$CCX $F -c runtime/plat_posix.c -o "$OUT/plat_posix.o"
CRASH=""                                           # arm64: plat_posix.c's thread start calls plat_crash.c's crash_thread_init (C4)
[ "$MODE" != x86 ] && { $CCX $F -c runtime/plat_crash.c -o "$OUT/plat_crash.o"; CRASH="$OUT/plat_crash.o"; }
$CCX $F -c runtime/nnmisc.c -o "$OUT/nnmisc.o"
$CCX $F -Dmain=ssbu_boot_main -c runtime/boot.c -o "$OUT/boot.o"
$CCX $F -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare tests/port/boot/test_boot.c "$OUT"/datapaths.o "$OUT"/plat_posix.o ${CRASH:+"$CRASH"} "$OUT"/nnmisc.o "$OUT"/boot.o $L -o "$OUT/test_boot"
[ "$MODE" = arm64 ] && { echo "built $OUT/test_boot"; exit 0; }
timeout 300 "$OUT/test_boot" "${TMPDIR:-/tmp}/c10_scratch"

#!/bin/sh
# The host services of the game on the rebuilt Linux layer, each unit compiled INTO its test (so its statics are reachable):
#   test_time    nntime.c: LoadTimeZoneRule + ToCalendarTime against glibc's localtime_r on 20 zones at every transition +-1 s
#                1970..2037 (TZ= each zone), the host zone, ToPosixTime round trips (nntime_selftest), the steady clock, UTC conversions;
#                then with SSBU_TZDIR on slim zone files (zic -b slim) when zic and tzdata.zi are there
#   test_mii     nnmii.c: GetRegionCode from the locale (GetUserDefaultGeoName: $LC_ALL / $LANG) and SSBU_REGION
#   test_net     nnnet.c: the nn::socket service over BSD sockets: loopback TCP + UDP, errors, option numbers, SSBU_NET=0
#   test_applet  nnapplet.c: nn::swkbd answers (SSBU_SWKBD), the UTF-16 text of the guest
#   test_misc    nnmisc.c: nn::oe::GetDesiredLanguage from $LANGUAGE / $LC_ALL / $LC_MESSAGES / $LANG and SSBU_LANGUAGE
# No game data. SDL3 and OpenSSL are stand-ins (no test reaches them).
#   tests/port/services/run.sh [mode]
#     native  (default) build and run here with $CC (cc): on the Frame (aarch64 Linux), or on any Linux
#     mac     macOS arm64: the Linux code paths (PLAT_LINUX_PATHS_ON_MAC)
#   OUT (default ${TMPDIR:-/tmp}/ssbu-services-<mode>), CC, CFLAGS_EXTRA.
set -eu
cd "$(dirname "$0")/../../.."
MODE=${1:-native}
OUT=${OUT:-${TMPDIR:-/tmp}/ssbu-services-$MODE}; mkdir -p "$OUT"
T=tests/port/services
. tests/port/native_env.sh
GT="-include tests/port/threads/game_tables.h -I tests/cspike/stubs"
BASE="-O1 -g -w -fvisibility=hidden -fno-omit-frame-pointer -include runtime/plat/plat_pre.h -I runtime/plat -I runtime ${CFLAGS_EXTRA:-}"
case "$MODE" in
native)
    CC=${CC:-cc}
    F="-fno-pie $BASE $NAT_INC"
    L="-no-pie -rdynamic $NAT_SDL_LIBS $NAT_SSL_LIBS -lm -lpthread -ldl" ;;
mac)
    CC=${CC:-clang}
    SDK=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk          # (the default SDK of some installs does not link)
    [ -d "$SDK" ] || SDK=$(xcrun --show-sdk-path)
    NAT_SSL_LIBS=""; NAT_SDL_LIBS=""; NAT_SYM_PREFIX=_
    F="-isysroot $SDK -mmacosx-version-min=14.4 -DPLAT_LINUX_PATHS_ON_MAC $BASE -idirafter tests/port/threads/stub"
    L="" ;;
*) echo "mode: native | mac"; exit 2 ;;
esac
$CC $F -c runtime/plat_posix.c -o "$OUT/plat_posix.o"
$CC $(echo "$F" | sed 's#-include runtime/plat/plat_pre.h##; s#-I runtime/plat ##') -std=gnu11 -c runtime/plat_crash.c -o "$OUT/plat_crash.o"
for t in test_time test_mii test_net test_applet test_misc; do
    $CC $F $GT -I $T -c $T/$t.c -o "$OUT/$t.o"
    nat_stubs "$OUT/$t-stubs.c" "$OUT/plat_posix.o" "$OUT/$t.o"
    $CC $F -c "$OUT/$t-stubs.c" -o "$OUT/$t-stubs.o"
    $CC $F "$OUT/$t.o" "$OUT/plat_posix.o" "$OUT/plat_crash.o" "$OUT/$t-stubs.o" $L -o "$OUT/$t"
done
fail=0
run() { echo "== $*"; timeout 300 "$@" || { echo "$1: exit $?"; fail=1; }; }
run "$OUT/test_mii"
run "$OUT/test_applet"
run "$OUT/test_misc"
run "$OUT/test_net"
run "$OUT/test_time"
if [ -d /usr/share/zoneinfo ]; then
    for z in America/New_York Europe/London Australia/Sydney Asia/Kolkata; do
        echo "== TZ=$z test_time (the host zone)"
        TZ=$z "$OUT/test_time" > "$OUT/tz.log" 2>&1 || { cat "$OUT/tz.log"; fail=1; }
        tail -3 "$OUT/tz.log"
    done
    # slim zone files stop at the last rule change and continue with the POSIX string of the footer: only these exercise that
    if command -v zic >/dev/null 2>&1 && [ -r /usr/share/zoneinfo/tzdata.zi ]; then
        rm -rf "$OUT/slim" && zic -b slim -d "$OUT/slim" /usr/share/zoneinfo/tzdata.zi
        echo "== slim zone files (footer continuation) test_time"
        SSBU_TZDIR="$OUT/slim" TZDIR="$OUT/slim" "$OUT/test_time" > "$OUT/tz.log" 2>&1 || { cat "$OUT/tz.log"; fail=1; }
        tail -3 "$OUT/tz.log"
    else echo "== slim zone files: no zic or /usr/share/zoneinfo/tzdata.zi, skipped"; fi
fi
exit $fail

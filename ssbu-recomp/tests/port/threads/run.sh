#!/bin/sh
# C3b tests: plat_posix.c's thread and fiber stacks, fibers, fiber-local storage and critical sections, and os_threads.c's
# guest fibers on top of them. No game data. SDL3 and OpenSSL are stand-ins (stub/; no test reaches them).
#   tests/port/threads/run.sh [mode]
#     native  (default) build and run here with $CC (cc): on the Frame (aarch64 Linux), or on x86-64 Linux (the CS / FLS
#             paths; fibers there are upstream's ucontext ones)
#     bti     native aarch64 with -mbranch-protection=standard, test_fiber with PROT_BTI on the switch's page
#     cross   build only, for the Frame, from x86 WSL (tests/cspike/xenv.sh); run_frame.sh copies and runs them
#     mac     macOS arm64: the Linux code paths (PLAT_LINUX_PATHS_ON_MAC) built and run natively, as a check of the
#             portable parts (switch, FLS, critical sections); stack placement below 2^47 is not checked there
#   OUT (default /tmp/ssbu-c3b-<mode>) for the binaries, OPT (default -O2), CFLAGS_EXTRA for more flags (PLAT_CFLAGS_EXTRA:
#   plat_posix.c only). mac with CFLAGS_EXTRA="-arch x86_64" PLAT_CFLAGS_EXTRA="-D_XOPEN_SOURCE=700 -Wno-implicit-function-declaration"
#   builds the x86-64 configuration (ucontext fibers + the Linux CS / FLS); it runs where Rosetta is installed.
set -eu
cd "$(dirname "$0")/../../.."
MODE=${1:-native}
OUT=${OUT:-/tmp/ssbu-c3b-$MODE}; mkdir -p "$OUT"
OPT=${OPT:--O2}
T=tests/port/threads
F="$OPT -g -w -fno-strict-aliasing -fno-omit-frame-pointer -fvisibility=hidden -include runtime/plat/plat_pre.h -I runtime/plat -I runtime -I $T/stub ${CFLAGS_EXTRA:-}"
L="-lm -lpthread -ldl"
P=""                                               # symbol prefix of nm output
case "$MODE" in
native) CC=${CC:-cc} ;;
bti) CC=${CC:-cc}; F="$F -mbranch-protection=standard -DTP_BTI" ;;
cross) . tests/cspike/xenv.sh; F="$X_ARCH $X_CODEGEN $X_LD $F" ;;
mac)
    CC=${CC:-clang}
    SDK=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk          # (the default SDK of some installs does not link)
    [ -d "$SDK" ] || SDK=$(xcrun --show-sdk-path)
    F="-isysroot $SDK -mmacosx-version-min=14.4 -DPLAT_LINUX_PATHS_ON_MAC $F"; L=""; P="_" ;;
*) echo "mode: native | bti | cross | mac"; exit 2 ;;
esac
$CC $F ${PLAT_CFLAGS_EXTRA:-} -c runtime/plat_posix.c -o "$OUT/plat_posix.o"
$CC $F -c runtime/os_threads.c -o "$OUT/os_threads.o"
{ echo 'int dprintf(int, const char *, ...); void abort(void);'
  nm -u "$OUT/plat_posix.o" | sed -E -n "s/^ *(U +)?$P((SDL|EVP)_[A-Za-z0-9_]+)\$/void \\2(void) { dprintf(2, \"stub called: \\2\\\\n\"); abort(); }/p"
  echo 'void SDL_GetVersion(void) { abort(); }'                # gaps.c takes its address
} > "$OUT/libstubs.c"
$CC $F -c "$OUT/libstubs.c" -o "$OUT/libstubs.o"
$CC $F -c $T/stubs.c -o "$OUT/stubs.o"
HAVE_CRASH=""
TESTS="test_threads test_sync test_fls test_os_fibers"
link() {                                           # link <name> <sources/objects...>: + the platform layer (paths may hold spaces)
    _n=$1; shift
    if [ -n "$HAVE_CRASH" ]; then $CC $F "$@" "$OUT/plat_posix.o" "$OUT/libstubs.o" "$OUT/plat_crash.o" $L -o "$OUT/$_n"
    else $CC $F "$@" "$OUT/plat_posix.o" "$OUT/libstubs.o" $L -o "$OUT/$_n"; fi
}
if $CC $F -dM -E -x c /dev/null | grep -q __aarch64__; then              # test_fiber: the aarch64 switch
    # plat_crash.c too (C4; Linux aarch64 thread starts call its crash_thread_init), so these runs set up the real alternate
    # signal stack on every thread they make. Built as tests/port/crash builds it (without the plat headers).
    $CC $(echo "$F" | sed 's#-include runtime/plat/plat_pre.h##; s#-I runtime/plat ##') -std=gnu11 -c runtime/plat_crash.c -o "$OUT/plat_crash.o"
    HAVE_CRASH=1
    TESTS="test_fiber $TESTS"
    link test_fiber $T/test_fiber.c $T/regcheck_a64.S
fi
link test_threads $T/test_threads.c
link test_sync $T/test_sync.c
link test_fls $T/test_fls.c
link test_os_fibers $T/test_os_fibers.c "$OUT/os_threads.o" "$OUT/stubs.o"
link gaps tests/cspike/gaps.c                                # the C-spike's gap probe, now against this layer
[ "$MODE" = cross ] && { ls "$OUT"; exit 0; }
fail=0
for t in $TESTS; do
    [ "$MODE" = bti ] && [ "$t" != test_fiber ] && continue
    echo "== $OUT/$t"
    timeout 300 "$OUT/$t" || { echo "$t: exit $?"; fail=1; }
done
if [ "$MODE" = native ]; then                                # information only: G4 (a test function's own TLS use) stays a gap
    echo "== $OUT/gaps"
    timeout 60 "$OUT/gaps" | grep -E '^(GAP|same)|G5' || true
fi
exit $fail

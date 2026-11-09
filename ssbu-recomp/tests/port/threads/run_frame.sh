#!/bin/sh
# C3b on the Steam Frame:
#   - run.sh native with gcc and clang at -O1 and -O2 (thread / fiber stacks below 2^47, the switch, FLS, critical
#     sections, os_threads.c's guest fibers, then tests/cspike/gaps.c for comparison with the C-spike's run)
#   - run.sh bti with clang and gcc (-mbranch-protection=standard, PROT_BTI on the switch's page)
#   - the TLS hazard check: in os_threads.o (fiber_main, fib_switch) and hostrt.o (call_guest) at -O2, no direct read of
#     the thread pointer (tpidr_el0): their thread variables are reached through noinline accessors only
# No game files are used.
# Usage: tests/port/threads/run_frame.sh              on the Frame (aarch64 Linux): runs here, in this checkout
#        tests/port/threads/run_frame.sh --local      the same, on any host
#        tests/port/threads/run_frame.sh [ssh host]   from a PC: copies the sources to ~/ssbu-port/threads on the host
#                                                     (default steamos@frame) and runs this script there with --local
# Environment: FRAME_WORK (build outputs; default ${TMPDIR:-/tmp}/ssbu-c3b), FRAME_QUICK=1 (the first compiler at -O2 only).
# A compiler that is not installed is left out; a missing objdump skips the TLS check ("SKIP:" line). Exit status 0 = all passed.
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
MODE=remote
if [ "${1:-}" = --local ]; then MODE=local
elif [ -z "${1:-}" ] && [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = aarch64 ]; then MODE=local; fi

if [ "$MODE" = remote ]; then
    HOST=${1:-steamos@frame}
    cd "$ROOT"
    tar -cf - runtime/plat_posix.c runtime/plat runtime/os_threads.c runtime/hostrt.c runtime/*.h tests/cspike/gaps.c \
        tests/cspike/stubs/fake_whitelist.h tests/cspike/stubs/fake_openlist.h tests/port/threads |
        ssh -o BatchMode=yes "$HOST" 'rm -rf ~/ssbu-port/threads && mkdir -p ~/ssbu-port/threads && cd ~/ssbu-port/threads && tar -xf -'
    exec ssh -o BatchMode=yes "$HOST" 'cd ~/ssbu-port/threads && sh tests/port/threads/run_frame.sh --local'
fi

set +e
cd "$ROOT" || exit 2
W=${FRAME_WORK:-${TMPDIR:-/tmp}/ssbu-c3b}
mkdir -p "$W" || exit 2
have() { command -v "$1" >/dev/null 2>&1; }
CCS=""
for cc in gcc clang; do have $cc && CCS="$CCS $cc"; done
if [ -z "$CCS" ]; then echo "no gcc or clang on PATH"; exit 2; fi
OPTS="-O1 -O2"
BTICCS="clang gcc"
if [ -n "${FRAME_QUICK:-}" ]; then
    first=${CCS# }; first=${first%% *}
    CCS=" $first"; OPTS="-O2"; BTICCS=$first
fi
rc=0
uname -m
grep -m1 -i "^Features" /proc/cpuinfo 2>/dev/null || true
for cc in $CCS; do for opt in $OPTS; do
    echo "######## run.sh native CC=$cc OPT=$opt"
    CC=$cc OPT=$opt OUT="$W/$cc$opt" sh tests/port/threads/run.sh native 2>&1 || rc=1
done; done
for cc in $BTICCS; do
    have $cc || continue
    echo "######## run.sh bti CC=$cc"
    CC=$cc OUT="$W/$cc-bti" sh tests/port/threads/run.sh bti 2>&1 || rc=1
done
echo "######## TLS hazard: direct tpidr_el0 reads at -O2 (want 0 in each)"
if ! have objdump; then
    echo "SKIP: TLS hazard check needs objdump (binutils)"
else
    for cc in $CCS; do
        F="-O2 -w -fno-strict-aliasing -fPIC -fvisibility=hidden -include runtime/plat/plat_pre.h -I runtime/plat -I runtime -I tests/port/threads/stub -I tests/cspike/stubs"
        $cc $F -c runtime/os_threads.c -o "$W/tls-os_threads-$cc.o" && $cc $F -c runtime/hostrt.c -o "$W/tls-hostrt-$cc.o" || { echo "FAIL compile for the TLS check ($cc)"; rc=1; continue; }
        for f in os_threads:fiber_main os_threads:fib_switch hostrt:call_guest; do
            o="$W/tls-${f%%:*}-$cc.o"; s=${f#*:}
            n=$(objdump -d --no-show-raw-insn --disassemble=$s "$o" | grep -c -i "tpidr_el0")
            if [ "$n" = 0 ]; then echo "ok   TLS $cc $s: $n"; else echo "FAIL TLS $cc $s: $n"; rc=1; fi
        done
    done
fi
exit $rc

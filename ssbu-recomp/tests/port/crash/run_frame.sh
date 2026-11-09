#!/bin/sh
# The C4 crash test on the Steam Frame, in every configuration: gcc, clang, clang with -O2 _FORTIFY_SOURCE=2 (longjmp ->
# __longjmp_chk out of the handler), and PAC (-mbranch-protection=standard) with both.
# Usage: tests/port/crash/run_frame.sh              on the Frame (aarch64 Linux): runs here, in this checkout
#        tests/port/crash/run_frame.sh --local      the same, on any host
#        tests/port/crash/run_frame.sh [ssh host]   from a PC: copies the test to ~/ssbu-port/crash on the host (default
#                                                   steamos@frame) and runs this script there with --local
# Environment: FRAME_WORK (outputs; default ${TMPDIR:-/tmp}/ssbu-c4), FRAME_QUICK=1 (the first compiler: plain and PAC only).
# A compiler that is not installed is left out. Exit status 0 = all passed.
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
MODE=remote
if [ "${1:-}" = --local ]; then MODE=local
elif [ -z "${1:-}" ] && [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = aarch64 ]; then MODE=local; fi

if [ "$MODE" = remote ]; then
    HOST=${1:-steamos@frame}
    cd "$ROOT"
    tar -cf - runtime/plat_crash.h runtime/plat_crash.c tests/port/crash/run.sh tests/port/crash/run_frame.sh tests/port/crash/test_crash.c |
        ssh -o BatchMode=yes "$HOST" 'rm -rf ~/ssbu-port/crash && mkdir -p ~/ssbu-port/crash && cd ~/ssbu-port/crash && tar -xf -'
    exec ssh -o BatchMode=yes "$HOST" 'cd ~/ssbu-port/crash && sh tests/port/crash/run_frame.sh --local'
fi

set +e
cd "$ROOT" || exit 2
W=${FRAME_WORK:-${TMPDIR:-/tmp}/ssbu-c4}
mkdir -p "$W" || exit 2
have() { command -v "$1" >/dev/null 2>&1; }
CCS=""
for cc in gcc clang; do have $cc && CCS="$CCS $cc"; done
if [ -z "$CCS" ]; then echo "no gcc or clang on PATH"; exit 2; fi
CFGS=""
if [ -n "${FRAME_QUICK:-}" ]; then
    first=${CCS# }; first=${first%% *}
    CFGS="CC=$first|CC=$first CFG=pac"
else
    for cc in $CCS; do CFGS="$CFGS${CFGS:+|}CC=$cc"; done
    have clang && CFGS="$CFGS|CC=clang CFG=fortify|CC=clang CFG=pac"
    have gcc && CFGS="$CFGS|CC=gcc CFG=pac"
fi
rc=0
i=0
OLDIFS=$IFS
IFS='|'
for cfg in $CFGS; do
    IFS=$OLDIFS
    i=$((i + 1))
    echo "######## $cfg"
    env $cfg OUT="$W/cfg$i" sh tests/port/crash/run.sh 2>&1 || rc=1
    IFS='|'
done
IFS=$OLDIFS
exit $rc

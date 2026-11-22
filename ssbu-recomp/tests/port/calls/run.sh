#!/bin/sh
# call_indirect's fast table (runtime/ci_direct.inc, used by hostrt.c): test_ci_direct.c at -O1 and -O2, and on aarch64 the
# code of hostrt.c's call_indirect at -O2 as the build makes it (no stack frame, no barrier, atomic read-modify-write,
# system call or thread-pointer read; one indirect branch; with -mbranch-protection=standard: bti c and the tail call
# through x16 / x17). No game data.
#   tests/port/calls/run.sh [native]      OUT (default ${TMPDIR:-/tmp}/ssbu-calls), CC (default cc), CFLAGS_EXTRA
set -u
cd "$(dirname "$0")/../../.."
OUT=${OUT:-${TMPDIR:-/tmp}/ssbu-calls}; mkdir -p "$OUT" || exit 2
CC=${CC:-cc}
rc=0
for opt in -O1 -O2; do
    if $CC $opt -g -Wall -Wno-unused-function -I runtime ${CFLAGS_EXTRA:-} -pthread tests/port/calls/test_ci_direct.c -o "$OUT/test_ci_direct$opt"; then
        echo "== test_ci_direct $CC $opt"
        "$OUT/test_ci_direct$opt" || { echo "FAIL test_ci_direct ($CC $opt)"; rc=1; }
    else echo "FAIL compile test_ci_direct ($CC $opt)"; rc=1; fi
done

# the hot path itself, on the Frame's architecture
if [ "$(uname -m)" = aarch64 ] || [ -n "${CALLS_CODEGEN:-}" ]; then
    if ! command -v objdump >/dev/null 2>&1; then echo "SKIP: call_indirect code check needs objdump (binutils)"
    else
        # tools/lifter/build.py's host flags on linux-arm64 (CODEGEN_FLAGS + PLAT_FLAGS), the Frame's core where the compiler knows it
        CPU=""; $CC -mcpu=cortex-x4 -fsyntax-only -x c /dev/null 2>/dev/null && CPU=-mcpu=cortex-x4
        F="-O2 -w -fno-strict-aliasing -ffp-contract=off -fno-math-errno -fno-omit-frame-pointer $CPU -fPIC -fvisibility=hidden -include runtime/plat/plat_pre.h -I runtime/plat -I runtime -I tests/port/threads/stub -I tests/cspike/stubs ${CFLAGS_EXTRA:-}"
        for bp in none standard; do
            o="$OUT/hostrt-$bp.o"
            if ! $CC $F -mbranch-protection=$bp -c runtime/hostrt.c -o "$o"; then echo "FAIL compile hostrt.c ($CC, branch protection $bp)"; rc=1; continue; fi
            d=$(objdump -d --no-show-raw-insn --disassemble=call_indirect "$o" | sed -n '/<call_indirect>:/,/^$/p' | tail -n +2)
            # the game is built with clang (build.py): there the fast path has no stack frame at all. gcc spills c to the stack
            # (sub sp / str / ldr on the miss path), harmless, so for gcc only the barriers, atomics, syscalls and TLS count
            frame='|stp|str'; $CC --version 2>/dev/null | grep -q -i clang || frame=''
            n_bad=$(printf '%s\n' "$d" | grep -c -i -E "\\b(dmb|dsb|isb|svc|hvc|ldx|ldax|stx|stlx|cas[a-z]*|ldadd[a-z]*|swp[a-z]*$frame)\\b|tpidr")
            n_br=$(printf '%s\n' "$d" | grep -c -E '\bbr\b|\bblr\b')
            n_acq=$(printf '%s\n' "$d" | grep -c -E '\bldapr|\bldar')
            what="$CC, branch protection $bp"
            if [ "$n_bad" = 0 ] && [ "$n_br" = 1 ] && [ "$n_acq" -ge 2 ]; then echo "ok   call_indirect fast path ($what): no ${frame:+frame / }barrier / atomic RMW / syscall / TLS, 1 indirect branch, $n_acq acquire loads"
            else echo "FAIL call_indirect fast path ($what): $n_bad forbidden instructions, $n_br indirect branches, $n_acq acquire loads"; printf '%s\n' "$d"; rc=1; fi
            if [ $bp = standard ]; then
                first=$(printf '%s\n' "$d" | grep -m1 -E '^ *[0-9a-f]+:' | awk '{print $2, $3}')
                tail=$(printf '%s\n' "$d" | grep -E '\bbr\b' | awk '{print $3}')
                case "$first" in "bti c"*) ok1=1 ;; *) ok1=0 ;; esac
                case "$tail" in x16 | x17) ok2=1 ;; *) ok2=0 ;; esac
                if [ $ok1 = 1 ] && [ $ok2 = 1 ]; then echo "ok   call_indirect under BTI: starts with bti c, tail call through $tail"
                else echo "FAIL call_indirect under BTI: first instruction '$first', tail call through '$tail'"; rc=1; fi
            fi
        done
    fi
fi
[ $rc = 0 ] && echo "calls: all passed" || echo "calls: FAILED"
exit $rc

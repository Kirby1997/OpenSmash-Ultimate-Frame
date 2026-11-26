#!/bin/sh
# SSBU_VK_STREAMKEEP's table (runtime/stream_keep.inc, used by nvn_vk_draw.c) against a fake write watch and ring. No GPU,
# no game. Also builds four broken variants (epoch, wrap count, stamps, storage-buffer drop ignored) that must fail,
# and a variant without the redo of a draw after a ring wrap in it (F1=0, review F1) whose GPU-read check must fail.
#   tests/port/streamkeep/run.sh       OUT (default ${TMPDIR:-/tmp}/ssbu-streamkeep), CC (default cc)
set -u
cd "$(dirname "$0")/../../.."
OUT=${OUT:-${TMPDIR:-/tmp}/ssbu-streamkeep}; mkdir -p "$OUT" || exit 2
CC=${CC:-cc}
rc=0
for opt in -O1 -O2; do
    $CC $opt -g -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -Iruntime -DBREAK=0 tests/port/streamkeep/test_stream_keep.c -o "$OUT/t$opt" || { echo "FAIL compile ($CC $opt)"; rc=1; continue; }
    echo "== test_stream_keep $CC $opt"; "$OUT/t$opt" || { echo "FAIL test_stream_keep ($CC $opt)"; rc=1; }
done
for b in 1 2 3 4; do
    $CC -O1 -std=gnu11 -w -Iruntime -DBREAK=$b tests/port/streamkeep/test_stream_keep.c -o "$OUT/b$b" || { echo "FAIL compile BREAK=$b"; rc=1; continue; }
    if "$OUT/b$b" > "$OUT/b$b.out"; then echo "FAIL: the broken variant $b was not caught"; rc=1; else echo "ok: broken variant $b caught ($(grep -c '^FAIL step' "$OUT/b$b.out") reported)"; fi
done
# review F1: without the redo after a wrap in the middle of a draw, a range is overwritten before the GPU reads it
$CC -O1 -std=gnu11 -w -Iruntime -DBREAK=0 -DF1=0 tests/port/streamkeep/test_stream_keep.c -o "$OUT/f1off" || { echo "FAIL compile F1=0"; rc=1; }
if [ -x "$OUT/f1off" ]; then
    if "$OUT/f1off" > "$OUT/f1off.out"; then echo "FAIL: without the redo (F1=0) the GPU-read check passed"; rc=1; else echo "ok: without the redo (F1=0) the GPU-read check fails ($(grep -c '^FAIL draw check' "$OUT/f1off.out") reported)"; fi
fi
[ $rc = 0 ] && echo "PASS streamkeep"
exit $rc

#!/bin/sh
# SSBU_VK_STREAMKEEP's table (runtime/stream_keep.inc, used by nvn_vk_draw.c) against a fake write watch and ring. No GPU,
# no game. Also builds four broken variants (epoch, wrap count, stamps, storage-buffer drop ignored) that must fail.
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
[ $rc = 0 ] && echo "PASS streamkeep"
exit $rc

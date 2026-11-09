#!/bin/sh
# C-spike: compile upstream's Linux host list (build.py with RECOMP_TARGET=linux) for aarch64 with upstream's flags plus our
# cross/codegen flags. Reports ok/FAIL per file.   tests/cspike/xcheck.sh [file.c ...]
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
cd "$REPO"
. tests/cspike/xenv.sh
OUT=${OUT:-/tmp/ssbu-cspike-check}
mkdir -p "$OUT"
FLAGS="$X_ARCH $X_CODEGEN ${X_OPT:--O2} -g -w -fno-strict-aliasing -ferror-limit=0 -Werror=implicit-function-declaration \
 -Werror=int-conversion -Werror=incompatible-pointer-types -I runtime -I . -I tests/cspike/stubs \
 -Dfn_table=(*game_fn_table_p) -Dfn_table_count=(*game_fn_table_count_p) -Ddyn_syms=(*game_dyn_syms_p) \
 -Ddyn_sym_count=(*game_dyn_sym_count_p) -I$SYSROOT/usr/include $X_PLAT $X_PKG_CFLAGS"
if [ $# -gt 0 ]; then FILES="$*"; else
FILES=$(RECOMP_TARGET=linux python3 - "$REPO" <<'PY'
import sys, os, subprocess
sys.argv_repo = sys.argv[1]
src = open(os.path.join(sys.argv[1], 'tools', 'lifter', 'build.py')).read()
ns = {'__file__': os.path.join(sys.argv[1], 'runtime', 'build.py')}
# take the module prologue up to the link helpers, with pkg-config stubbed (no pkg-config on WSL)
start = src.index('HOST_SOURCES = ['); end = src.index('def debug_link_flags')
pro = src[:src.index('HOST_SOURCES = [')]
code = 'import os, sys, subprocess\nCLANG="clang"\nHERE=%r\nGEN=HERE\nTARGET_LINUX=True\nPLAT_FLAGS=[]\nOPT_BASE="-O1"\nCODEGEN_FLAGS=[]\nTARGET_ARM64=False\nTARGET_FLAGS=[]\n' % os.path.join(sys.argv[1], 'runtime')
code += src[start:end].replace("subprocess.run(['pkg-config']", "(lambda *a, **k: type('R', (), {'stdout': ''})())(['pkg-config']")
exec(code, ns)
gen = {'sdkdata.c', 'nro_registry.c', 'sdklift_gen.c', 'game_data.c'}
extra = ['menu_stub.c', 'gamedll.c']
print(' '.join([s for s in ns['HOST_SOURCES'] if s not in gen] + [e for e in extra if os.path.exists(os.path.join(sys.argv[1], 'runtime', e))]))
PY
)
fi
ok=0; bad=0; summary=""
for f in $FILES; do
  log="$OUT/${f%.*}.log"
  if $CC -c $FLAGS "runtime/$f" -o "$OUT/${f%.*}.o" >"$log" 2>&1; then ok=$((ok+1)); summary="$summary\n  ok    $f"
  else n=$(grep -c "error:" "$log"); bad=$((bad+1)); summary="$summary\n  FAIL  $f ($n errors)"; fi
done
printf "$summary\n"
echo "compiled: $ok ok, $bad failed (logs in $OUT)"
[ $bad -eq 0 ]

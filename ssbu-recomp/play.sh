#!/usr/bin/env bash
# Starts the native Linux build (BUILDING_LINUX.md). Run it from anywhere:
#
#   ./play.sh                          play; saves in ~/.local/share/ssbu-recomp/userdata
#   ./play.sh --userdata <folder>      another save folder
#   ./play.sh --help                   every option (they are scripts/run.py's)
#
# The game's output is shown here and also kept in ~/.local/share/ssbu-recomp/last_run.log (attach it to a bug report).
set -uo pipefail

REPO="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/ssbu-recomp"
PY="$REPO/deps/venv/bin/python"

# Steam Frame (aarch64): the build of scripts/setup.py --target linux-arm64 lives in build-linux-arm64
BUILD=build-linux; TARGET=linux; RUNARG=--linux
if [ ! -x "$REPO/build-linux/work/ssbu_native" ] && [ -x "$REPO/build-linux-arm64/work/ssbu_native" ] && [ "$(uname -m)" = aarch64 ]; then
    BUILD=build-linux-arm64; TARGET=linux-arm64; RUNARG=--linux-arm64
elif [ ! -x "$REPO/build-linux/work/ssbu_native" ] && [ "$(uname -m)" = aarch64 ]; then
    TARGET=linux-arm64
fi

[ -x "$PY" ] || { echo "deps/venv is missing: run scripts/linux/install_deps.sh first" >&2; exit 1; }
[ -x "$REPO/$BUILD/work/ssbu_native" ] || {
    echo "no Linux build yet: run" >&2
    echo "  $PY scripts/setup.py --target $TARGET --dump <your dump folder>" >&2
    exit 1; }

mkdir -p "$DATA"
"$PY" "$REPO/scripts/run.py" $RUNARG "$@" 2>&1 | tee "$DATA/last_run.log"
exit "${PIPESTATUS[0]}"

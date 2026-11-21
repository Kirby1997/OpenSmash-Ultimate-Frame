#!/usr/bin/env bash
# Adds "Super Smash Bros. Ultimate (recomp)" to your application menu: a desktop entry in ~/.local/share/applications
# that starts the native Linux build with scripts/run.py (saves in ~/.local/share/ssbu-recomp/userdata, the game's
# output in ~/.local/share/ssbu-recomp/last_run.log). The x86-64 build (build-linux) or the Steam Frame's
# (build-linux-arm64, on an aarch64 host).
#
# Steam Frame: Steam shows the Frame's controls to a game only as its virtual pad, and SDL uses that pad only in a game
# Steam started (SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD). A start from a terminal or over ssh gets no controller.
# So start it from Steam: in Desktop Mode, right-click the entry in the application menu (Games) and choose "Add to
# Steam"; it is then in the library's Non-Steam section, also in Game Mode.
#
#   scripts/linux/install_desktop.sh             install (again, after moving the repository)
#   scripts/linux/install_desktop.sh --remove    remove the entry (saves are kept)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/ssbu-recomp"
ENTRY="$APPS/ssbu-recomp.desktop"

if [ "${1:-}" = "--remove" ]; then
    rm -f "$ENTRY" "$DATA/launch.sh"
    echo "removed $ENTRY (saves in $DATA/userdata are kept)"
    exit 0
fi
BUILD=build-linux; RUNARG=--linux; TARGET=linux
if [ "$(uname -m)" = aarch64 ] && [ -x "$REPO/build-linux-arm64/work/ssbu_native" ] && [ ! -x "$REPO/build-linux/work/ssbu_native" ]; then BUILD=build-linux-arm64; RUNARG=--linux-arm64; TARGET=linux-arm64; fi
[ -x "$REPO/$BUILD/work/ssbu_native" ] || { echo "no Linux build in $REPO/$BUILD: run scripts/setup.py --target $TARGET first" >&2; exit 1; }
[ -x "$REPO/deps/venv/bin/python" ] || { echo "deps/venv is missing: run scripts/linux/install_deps.sh first" >&2; exit 1; }

mkdir -p "$APPS" "$DATA"
# a small launcher script, so that the desktop entry holds no quoting of paths with spaces
cat > "$DATA/launch.sh" <<EOF
#!/bin/sh
exec "$REPO/deps/venv/bin/python" "$REPO/scripts/run.py" $RUNARG --log "$DATA/last_run.log" "\$@"
EOF
chmod +x "$DATA/launch.sh"
cat > "$ENTRY" <<EOF
[Desktop Entry]
Type=Application
Name=Super Smash Bros. Ultimate (recomp)
Comment=Native Linux build of ssbu-recomp
Exec="$DATA/launch.sh"
Icon=applications-games
Terminal=false
Categories=Game;
EOF
command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" 2>/dev/null || true
echo "installed $ENTRY"
echo "the game's output of the last start: $DATA/last_run.log"

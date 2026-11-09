#!/bin/sh
# The picker sources + tests on the Steam Frame:
#   test_picker (no display): gcc, clang, clang ASan+UBSan
#   test_picker_sdl: SDL's offscreen driver (gcc, clang), then on the running gamescope session's display - its Wayland socket,
#   and Xwayland :0 with the gamescope environment Steam gives a game in Game Mode - with each pictured screen held on screen
#   and photographed by the compositor (gamescopectl screenshot).
# Usage: tests/port/picker/run_frame.sh              on the Frame (aarch64 Linux): runs here, in this checkout
#        tests/port/picker/run_frame.sh --local      the same, on any host
#        tests/port/picker/run_frame.sh [host] [shots folder]
#                                                    from a PC: copies the sources to ~/ssbu-port/picker on the host (default
#                                                    steamos@frame), runs this script there with --local, copies the pictures
#                                                    back to <shots> (default tests/port/picker/build/frame_shots)
# Environment: SDL3_ROOT (default ~/ssbu-port/deps/sdl3; without it the SDL parts print "SKIP:" lines), FRAME_WORK (the pictures
# go to $FRAME_WORK/picker-shots; default tests/port/picker/build/shots), FRAME_QUICK=1 (the first compiler, offscreen only, no
# photographs). Build outputs stay in tests/port/picker/build (relative paths: the Makefile does not take spaces in them).
# Guarded like tests/port/boot: at most 300 tasks above what the user already has, a timeout on every run, no re-exec.
# Exit status 0 = everything that ran passed.
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
MODE=remote
if [ "${1:-}" = --local ]; then MODE=local
elif [ -z "${1:-}" ] && [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = aarch64 ]; then MODE=local; fi

if [ "$MODE" = remote ]; then
    HOST=${1:-steamos@frame}
    SHOTS=${2:-$ROOT/tests/port/picker/build/frame_shots}
    cd "$ROOT"
    tar -cf - runtime/*.h runtime/plat runtime/plat_posix.c runtime/plat_crash.c runtime/vmem.c runtime/fiber_aarch64.S runtime/hostpath.c runtime/datapaths.c \
        runtime/datapicker.c runtime/datapicker_sdl.c tests/port/boot/tp.h tests/port/picker/Makefile tests/port/picker/run_frame.sh tests/port/picker/*.c |
        ssh -o BatchMode=yes "$HOST" 'mkdir -p ~/ssbu-port/picker && cd ~/ssbu-port/picker && rm -rf tests/port/picker/build && tar -xf -'
    rc=0
    ssh -o BatchMode=yes "$HOST" 'cd ~/ssbu-port/picker && sh tests/port/picker/run_frame.sh --local' || rc=$?
    mkdir -p "$SHOTS"
    ssh -o BatchMode=yes "$HOST" 'cd ~/ssbu-port/picker/tests/port/picker/build/shots && tar -cf - .' | tar -xf - -C "$SHOTS"
    echo "pictures copied to $SHOTS"
    exit $rc
fi

set +e
cd "$ROOT/tests/port/picker" || exit 2
have() { command -v "$1" >/dev/null 2>&1; }
if ! have make; then echo "make not found"; exit 2; fi
CCS=""
for cc in gcc clang; do have $cc && CCS="$CCS $cc"; done
if [ -z "$CCS" ]; then echo "no gcc or clang on PATH"; exit 2; fi
SDL=${SDL3_ROOT:-$HOME/ssbu-port/deps/sdl3}
HAVE_SDL=""
if [ -f "$SDL/include/SDL3/SDL.h" ] && ls "$SDL"/lib/libSDL3.so* >/dev/null 2>&1; then HAVE_SDL=1; fi
SANCC=clang
have clang || SANCC=""
if [ -n "${FRAME_QUICK:-}" ]; then first=${CCS# }; first=${first%% *}; CCS=" $first"; SANCC=""; fi
rc=0
# bound the user's process count (the picker forks); some systems have no 'ps -L'
N=0; if [ "$(uname -s)" = Linux ]; then N=$(ps -eLf --no-headers -u "$(id -un)" 2>/dev/null | wc -l); fi
if [ "${N:-0}" -gt 0 ] 2>/dev/null; then ulimit -u $((N + 300)) 2>/dev/null; fi
export SSBU_VMEM_NO_REEXEC=1
T=$(mktemp -d "${TMPDIR:-/tmp}/picker.XXXXXX") || exit 2
trap 'rm -rf "$T"' EXIT
runq() {                    # runq <command...>: the output without the "  - " progress lines; the command's status is returned
    "$@" > "$T/out" 2>&1; _st=$?
    grep -v "^  - " "$T/out"
    return $_st
}
for cc in $CCS; do echo "######## make CC=$cc"; runq timeout 600 make -s CC=$cc || rc=1; done
if [ -n "$SANCC" ]; then echo "######## make CC=$SANCC SAN=1"; runq timeout 600 make -s CC=$SANCC SAN=1 || rc=1; fi
if [ -z "$HAVE_SDL" ]; then
    echo "SKIP: test_picker_sdl needs SDL3 (headers and libSDL3.so in $SDL; toolchain/build_deps_on_device.sh builds it, or set SDL3_ROOT)"
    exit $rc
fi
for cc in $CCS; do echo "######## make CC=$cc sdl"; runq timeout 600 make -s CC=$cc SDL="$SDL" sdl || rc=1; done
if [ -n "${FRAME_WORK:-}" ]; then S="$FRAME_WORK/picker-shots"; else S="$PWD/build/shots"; fi
rm -rf "$S"; mkdir -p "$S/offscreen" "$S/wayland" "$S/xwayland"
run() { echo "######## $*"; runq env "$@" || rc=1; }
for cc in $CCS; do
    run SDL_VIDEODRIVER=offscreen timeout -s KILL 120 "build/$cc/test_picker_sdl" "build/$cc/sdl" "$S/offscreen"
done
if [ -n "${FRAME_QUICK:-}" ]; then echo "SKIP: gamescope display runs (--quick)"; echo "######## pictures in $S"; exit $rc; fi
WLCC=clang; case "$CCS" in *clang*) ;; *) WLCC=${CCS# }; WLCC=${WLCC%% *} ;; esac
XCC=gcc; case "$CCS" in *gcc*) ;; *) XCC=${CCS# }; XCC=${XCC%% *} ;; esac
find_wl() {                 # the gamescope session's Wayland socket (not its -ei socket or a .lock) in the current folder
    for s in gamescope-[0-9]*; do
        case $s in
        *-ei|*.lock) ;;
        *) if [ -S "$s" ]; then echo "$s"; return 0; fi ;;
        esac
    done
}
XDG_RUNTIME_DIR=/run/user/$(id -u); export XDG_RUNTIME_DIR
WL=""; X=""
if [ -d "$XDG_RUNTIME_DIR" ]; then
    WL=$(cd "$XDG_RUNTIME_DIR" && find_wl)
fi
[ -S /tmp/.X11-unix/X0 ] && X=:0
echo "######## display: WAYLAND_DISPLAY=${WL:-none} DISPLAY=${X:-none}"
if [ -z "$WL" ] && [ -z "$X" ]; then
    echo "SKIP: no gamescope Wayland socket or X display: run the kit from Game Mode / a Desktop session to photograph the windows"
fi
SHOT="env XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR WAYLAND_DISPLAY=${WL:-gamescope-0} gamescopectl screenshot"
if [ -n "$WL" ]; then
    have gamescopectl || echo "SKIP: gamescopectl not found: the Wayland run takes no photographs"
    run WAYLAND_DISPLAY=$WL SDL_VIDEODRIVER=wayland PICKER_SHOT_CMD="$SHOT" timeout -s KILL 180 "build/$WLCC/test_picker_sdl" "build/$WLCC/sdl" "$S/wayland" 1500
fi
if [ -n "$X" ]; then      # as Steam starts a game in Game Mode: Xwayland, gamescope environment
    run DISPLAY=$X GAMESCOPE_WAYLAND_DISPLAY=${WL:-gamescope-0} XDG_CURRENT_DESKTOP=gamescope SDL_VIDEODRIVER=x11 PICKER_SHOT_CMD="$SHOT" \
        timeout -s KILL 180 "build/$XCC/test_picker_sdl" "build/$XCC/sdl" "$S/xwayland" 1500
fi
echo "######## left running: $(pgrep -c test_picker || true)"
echo "######## pictures in $S"
ls -R "$S" | head -60
exit $rc

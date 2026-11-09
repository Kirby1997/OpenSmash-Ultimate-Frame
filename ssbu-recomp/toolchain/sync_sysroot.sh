#!/usr/bin/env bash
# Build a cross-compile sysroot for the Steam Frame (aarch64 SteamOS) from the device itself, without rsync/sudo.
#
#   toolchain/sync_sysroot.sh [-d <sysroot dir>] [-H <ssh host>] [-s <sdl3 dir on the device>]
#
# defaults: sysroot ~/sysroots/steam-frame, host steamos@frame, SDL3 at ~/ssbu-port/deps/sdl3 (built there by
# toolchain/build_deps_on_device.sh or by hand).
# What is copied (streamed as tar over ssh, symlinks preserved):
#   /usr/include                      everything (about 280 MB: libc, Vulkan, FreeType, Opus, FFmpeg, glslang, X11, Wayland ...)
#   /usr/lib/*.so* *.a crt*.o         the top level only (not Python/Qt/gcc plugin trees) minus a few huge unrelated libraries
#   /usr/lib/gcc/<triple>/<ver>       crtbegin/crtend/libgcc for clang's GCC-install detection
#   /usr/lib/pkgconfig, ffmpeg4.4/    pkg-config files
#   SDL3 install tree                 -> <sysroot>/opt/ssbu-deps/sdl3   (SDL3_ROOT)
# The device is Arch-style: /lib -> usr/lib, flat /usr/lib (no multiarch dir). Absolute symlinks are made relative afterwards.
set -euo pipefail
SYSROOT="${SYSROOT:-$HOME/sysroots/steam-frame}"
HOST="${SSBU_DEVICE:-steamos@frame}"
SDL3_REMOTE='$HOME/ssbu-port/deps/sdl3'
while getopts "d:H:s:h" o; do case $o in d) SYSROOT=$OPTARG;; H) HOST=$OPTARG;; s) SDL3_REMOTE=$OPTARG;; *) sed -n 2,3p "$0"; exit 1;; esac; done
SSH=(ssh -o BatchMode=yes "$HOST")
mkdir -p "$SYSROOT/usr" "$SYSROOT/opt/ssbu-deps"
echo "sysroot: $SYSROOT   device: $HOST"

echo "[1/4] /usr/include"
"${SSH[@]}" 'cd / && tar -c usr/include' | tar -x -C "$SYSROOT"

echo "[2/4] /usr/lib (top-level libraries, crt files, gcc runtime, pkgconfig)"
"${SSH[@]}" 'cd /usr/lib && find . -maxdepth 1 \( -name "*.so" -o -name "*.so.*" -o -name "*.a" -o -name "crt*.o" -o -name "*.o" \) \
    ! -name "libLLVM*" ! -name "libclang*" ! -name "libLTO*" ! -name "libQt*" ! -name "libicu*" ! -name "libgtk*" ! -name "libmozjs*" \
    ! -name "libgallium*" ! -name "libwebkit*" ! -name "libjavascriptcore*" ! -name "libpython*" ! -name "libkf*" ! -name "libKF*" \
    -printf "usr/lib/%P\n"; find gcc pkgconfig ffmpeg4.4 -maxdepth 3 \( -type f -o -type l \) -printf "usr/lib/%p\n" 2>/dev/null' > "$SYSROOT/.filelist"
"${SSH[@]}" "cd / && tar -c -T -" < "$SYSROOT/.filelist" | tar -x -C "$SYSROOT"
[ -e "$SYSROOT/lib" ] || ln -s usr/lib "$SYSROOT/lib"
[ -e "$SYSROOT/usr/lib64" ] || ln -s lib "$SYSROOT/usr/lib64"
[ -e "$SYSROOT/lib64" ] || ln -s usr/lib "$SYSROOT/lib64"
mkdir -p "$SYSROOT/usr/share"; "${SSH[@]}" 'cd / && tar -c usr/share/pkgconfig usr/share/wayland-protocols 2>/dev/null' | tar -x -C "$SYSROOT" 2>/dev/null || true

echo "[3/4] SDL3 from the device ($SDL3_REMOTE)"
"${SSH[@]}" "cd $SDL3_REMOTE && tar -c ." | { mkdir -p "$SYSROOT/opt/ssbu-deps/sdl3" && tar -x -C "$SYSROOT/opt/ssbu-deps/sdl3"; }
# the .pc files of that tree name its prefix on the device (/home/steamos/ssbu-port/deps/sdl3): mirror that path in the sysroot
# (a relative symlink), so pkg-config with PKG_CONFIG_SYSROOT_DIR finds the headers and libraries without editing the .pc files
SDL3_ABS=$("${SSH[@]}" "cd $SDL3_REMOTE && pwd -P")
mkdir -p "$SYSROOT$(dirname "$SDL3_ABS")" && ln -sfn "$(python3 -c 'import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' "$SYSROOT/opt/ssbu-deps/sdl3" "$SYSROOT$(dirname "$SDL3_ABS")")" "$SYSROOT$SDL3_ABS"

echo "[4/4] absolute symlinks -> relative; check"
python3 - "$SYSROOT" <<'PY'
import os, sys
root = os.path.abspath(sys.argv[1]); fixed = broken = 0
for d, dirs, files in os.walk(root):
    for n in dirs + files:
        p = os.path.join(d, n)
        if not os.path.islink(p): continue
        t = os.readlink(p)
        if os.path.isabs(t):
            new = os.path.join(root, t.lstrip('/'))
            os.unlink(p); os.symlink(os.path.relpath(new, d), p); fixed += 1
            if not os.path.exists(new): broken += 1
print('absolute symlinks made relative: %d (dangling: %d)' % (fixed, broken))
PY
for f in usr/lib/crt1.o usr/lib/libc.so.6 usr/lib/libm.so.6 usr/lib/libvulkan.so.1 usr/lib/libfreetype.so usr/lib/libopus.so usr/lib/libavcodec.so usr/lib/libvpx.so \
         usr/lib/libglslang.a usr/lib/libSPIRV-Tools-opt.so opt/ssbu-deps/sdl3/lib/libSDL3.so usr/include/vulkan/vulkan.h usr/include/glslang/Include/glslang_c_interface.h; do
  [ -e "$SYSROOT/$f" ] && echo "  ok      $f" || echo "  MISSING $f"
done
echo "done: $(du -sh "$SYSROOT" | cut -f1)"

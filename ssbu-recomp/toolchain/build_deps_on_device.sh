#!/usr/bin/env bash
# Run ON the Steam Frame (aarch64 SteamOS): builds SDL3 from the GitHub release tag into ~/ssbu-port/deps/sdl3.
# Everything stays under ~/ssbu-port (nothing else on the device is touched). Needs cmake, ninja, clang or gcc, curl, tar,
# and the Wayland/X11/PipeWire/Pulse/ALSA/udev/dbus/libdecor/libusb headers that SteamOS ships.
set -euo pipefail
VER="${SDL3_VERSION:-3.4.18}"
PREFIX="${PREFIX:-$HOME/ssbu-port/deps/sdl3}"
SRC="$HOME/ssbu-port/deps/src"
mkdir -p "$SRC"; cd "$SRC"
[ -d "SDL-release-$VER" ] || curl -sL "https://github.com/libsdl-org/SDL/archive/refs/tags/release-$VER.tar.gz" | tar xz
rm -rf sdl-build
cmake -S "SDL-release-$VER" -B sdl-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
  -DSDL_WAYLAND=ON -DSDL_X11=ON -DSDL_X11_XSCRNSAVER=OFF -DSDL_PIPEWIRE=ON -DSDL_PULSEAUDIO=ON -DSDL_ALSA=ON \
  -DSDL_VULKAN=ON -DSDL_KMSDRM=ON -DSDL_LIBUDEV=ON -DSDL_DBUS=ON | tee sdl-configure.log | tail -20
cmake --build sdl-build -j"$(nproc)"
cmake --install sdl-build
ls "$PREFIX/lib"

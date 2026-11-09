# Source this to cross-compile for the Steam Frame (aarch64 Linux) from x86-64 WSL/Linux.
#   . toolchain/env.sh
#   python scripts/setup.py --target linux-arm64 --dump <folder>          (the build flags below are what it computes itself)
#   $CC hello.c -o hello $SSBU_CFLAGS                                      (a single file)
# Override before sourcing: LLVM_ROOT (default ~/.local/llvm-19.1.7), SYSROOT (default ~/sysroots/steam-frame; made by
# toolchain/sync_sysroot.sh). Not needed on the Frame itself: there scripts/setup.py --target linux-arm64 builds natively.
export LLVM_ROOT="${LLVM_ROOT:-$HOME/.local/llvm-19.1.7}"
export SYSROOT="${SYSROOT:-$HOME/sysroots/steam-frame}"
export SDL3_ROOT="${SDL3_ROOT:-$SYSROOT/opt/ssbu-deps/sdl3}"
export VULKAN_HEADERS="${VULKAN_HEADERS:-$SYSROOT/usr}"      # include/vulkan/vulkan.h
export GLSLANG_ROOT="${GLSLANG_ROOT:-$SYSROOT/usr}"          # include/glslang, lib/libglslang.a
export PATH="$LLVM_ROOT/bin:$PATH"
export SSBU_TARGET=linux-arm64
export SSBU_TRIPLE=aarch64-linux-gnu
# The Frame is a Snapdragon 8 Gen 3 (1 Cortex-X4 + 5 A720 + 2 A520; every core has SVE2): -mcpu=cortex-x4 (armv9.2-a, LSE: casal inline,
# no outline-atomics helpers) where clang knows it (LLVM 18+); otherwise the armv8.2-a+lse set PORT_AUDIT Phase 1 validated.
# Never fast-math / -ffinite-math-only / -fno-signed-zeros / -mstrict-align; -ffp-contract=off even when native.
if "$LLVM_ROOT/bin/clang" --target=aarch64-linux-gnu -mcpu=cortex-x4 -fsyntax-only -x c /dev/null 2>/dev/null; then
    export SSBU_CPU_FLAGS="-mcpu=cortex-x4"
else
    echo "toolchain/env.sh: $LLVM_ROOT/bin/clang has no -mcpu=cortex-x4: using -march=armv8.2-a+lse" >&2
    export SSBU_CPU_FLAGS="-march=armv8.2-a+lse"
fi
export SSBU_ARCH_FLAGS="--target=aarch64-linux-gnu --sysroot=$SYSROOT $SSBU_CPU_FLAGS"
export SSBU_CFLAGS="$SSBU_ARCH_FLAGS -fuse-ld=lld -O2 -w -fno-strict-aliasing -ffp-contract=off -fno-math-errno -fno-omit-frame-pointer"
export CC="$LLVM_ROOT/bin/clang" CXX="$LLVM_ROOT/bin/clang++"
export RECOMP_CLANG="$CC"
# pkg-config against the sysroot's .pc files (tools/lifter/build.py and scripts/setup.py set these themselves)
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig:$SDL3_ROOT/lib/pkgconfig"

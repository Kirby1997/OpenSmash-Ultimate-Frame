# C-spike: cross flags for upstream's Linux layer on aarch64 (Steam Frame), from x86-64 WSL. Source it.
# Uses the linux-arm64 branch's toolchain layout: LLVM 19.1.7 in ~/.local/llvm-19.1.7, sysroot ~/sysroots/steam-frame
# (toolchain/sync_sysroot.sh there). No pkg-config on WSL: the pkg-config output of upstream's build.py is spelled out.
export LLVM_ROOT="${LLVM_ROOT:-$HOME/.local/llvm-19.1.7}"
export SYSROOT="${SYSROOT:-$HOME/sysroots/steam-frame}"
export SDL3_ROOT="${SDL3_ROOT:-$SYSROOT/opt/ssbu-deps/sdl3}"
export CC="$LLVM_ROOT/bin/clang"
# our Phase-1 codegen flags (PORT_AUDIT): LSE atomics, no fp contraction (bit-exact FP), frame pointers for backtraces
export X_ARCH="--target=aarch64-linux-gnu --sysroot=$SYSROOT ${X_CPU:--mcpu=cortex-x4}"      # the C2 flags (tools/lifter/build.py arm_target_flags); X_CPU=-march=armv8.2-a+lse for an LLVM before 18
export X_CODEGEN="-ffp-contract=off -fno-math-errno -fno-omit-frame-pointer"
# upstream's PLAT_FLAGS + the pkg-config --cflags of build.py's list
export X_PLAT="-fPIC -fvisibility=hidden -include runtime/plat/plat_pre.h -I runtime/plat"
export X_PKG_CFLAGS="-I$SDL3_ROOT/include -I$SYSROOT/usr/include/freetype2 -I$SYSROOT/usr/include/opus"
export X_PKG_LIBS="-L$SDL3_ROOT/lib -Wl,-rpath-link,$SDL3_ROOT/lib -lSDL3 -lfreetype -lssl -lcrypto -lavformat -lavcodec -lswscale -lswresample -lavutil"
export X_LD="-fuse-ld=lld"

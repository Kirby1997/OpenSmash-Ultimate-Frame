#!/bin/sh
# ssbu-recomp/tests/frame/run_all.sh - every device check owed by the Steam Frame, one command, one report.
#
#   sh ssbu-recomp/tests/frame/run_all.sh              all suites (about 10 to 30 minutes on the Frame)
#   sh ssbu-recomp/tests/frame/run_all.sh --quick      a short subset of each suite (a few minutes)
#   sh ssbu-recomp/tests/frame/run_all.sh --suite NAME one suite (repeat the option or use NAME,NAME); --list names them
#   sh ssbu-recomp/tests/frame/run_all.sh --dry-run    preflight and the plan only: nothing is built or run
# (from the repository root; inside ssbu-recomp/ it is sh tests/frame/run_all.sh, the same run: the kit always works in
# ssbu-recomp/, which is what <checkout> means below)
#
# Run it ON the Frame, in a checkout of branch frame of Kirby1997/OpenSmash-Ultimate-Frame (OpenSmash-Ultimate Frame,
# docs/STEAM_FRAME.md). It needs no PC, no ssh, no root: it builds the tests with
# the compilers on PATH and runs them natively. It writes ONLY inside the checkout and a work folder (default
# <checkout>/build-frame-tests, see --work); it never uses sudo or pacman and changes no system setting.
#
# Result: <work>/ssbu-frame-<stamp>.txt (the report) and <work>/ssbu-frame-<stamp>.tar.gz (the report and every log) - bring
# both back. Both are scrubbed: $HOME becomes ~, the checkout <repo>, the work folder <work>; user and host names are replaced;
# no environment dump, no serial numbers. Screenshots of the picker windows (they may show other things on screen) stay in
# the work folder unless --with-shots. Exit status: 0 = every suite that ran passed, 1 = a failure or timeout, 2 = bad usage,
# 3 = nothing could run.
#
# Options: --quick  --suite NAME  --list  --dry-run  --work DIR  --sdl3 DIR (default ~/ssbu-port/deps/sdl3)
#          --kheaders DIR (a folder holding linux/userfaultfd.h when the system has no kernel headers)
#          --ref-hash FILE (the "soft-hash" line of another host, for the lifter suite)  --with-shots  --force (run the
#          suites even when this host is not aarch64 Linux)  -h
# POSIX sh only (also bash 3.2), no extra tools beyond what a Linux install has; paths with spaces are fine.

set -u
SELF=$0
case $SELF in /*) ;; *) SELF=$(pwd)/$SELF ;; esac
ROOT=$(cd "$(dirname "$SELF")/../.." && pwd)
SELF=$(cd "$(dirname "$SELF")" && pwd)/$(basename "$SELF")
KIT_VERSION="frame-kit 1"

# ---------------------------------------------------------------------------------------------------- the suites
# name | what it checks | timeout seconds (full) | (quick)
SUITE_NAMES="crash fs nnfs boot plat winshim modules services io threads picker lifter"
suite_desc() {
    case $1 in
    crash)   echo "crash reports: signal handler, alternate stack, report file (gcc, clang, FORTIFY, PAC); the A64 decoder of the fake audit" ;;
    fs)      echo "file layer of plat_posix.c: case-insensitive paths, write-through rename, Windows error codes" ;;
    nnfs)    echo "nn::fs on the file layer: romfs tree, journaled save data, Commit under kill -9 and failed moves, folder listing order (nnfs.c, nnfsdir.c)" ;;
    boot)    echo "game-data lookup (datapaths.c) and boot.c's launch defaults, with synthetic fixtures" ;;
    plat)    echo "platform layer: VirtualAlloc below 2^47, write watch, plat_selftest (interlocked, threads, fibers, Vulkan loader, SHA-256)" ;;
    winshim) echo "Win32 stand-in layer: header order, VirtualAlloc / Query / Protect, IsBadReadPtr vs the kernel, game library binding, hashes (SHA-256 NRR check, SHA-1, MD5)" ;;
    modules) echo "nn::ro: NRR hash check, LoadModule through LoadLibraryA, relocation, unload, two modules with the same internal names (fake modules built as build.py does)" ;;
    services) echo "host services: nn::time zones against the C library (also slim tzdata), region code, nn::socket over BSD sockets, swkbd text" ;;
    io)      echo "nnfontll.c's FreeType outlines against FreeType (needs FreeType's headers: says so and skips without them)" ;;
    threads) echo "thread / fiber stacks and __thread below 2^47, fibers, FLS, critical sections, nn::os threads, setjmp on fibers, C++ runtime exception state, BTI, TLS hazard check" ;;
    picker)  echo "data-folder picker: logic, ASan/UBSan, its SDL window offscreen and (in a gamescope session) on the display" ;;
    lifter)  echo "lifter: memory-ordering codegen, litmus tests on lifted code, FP vs the instructions, NEON, software-FP hash" ;;
    esac
}
suite_timeout() {
    if [ -n "$QUICK" ]; then
        case $1 in crash) echo 300 ;; fs) echo 180 ;; nnfs) echo 300 ;; boot) echo 300 ;; plat) echo 300 ;; winshim) echo 300 ;; modules) echo 300 ;; services) echo 300 ;; io) echo 300 ;; threads) echo 600 ;; picker) echo 600 ;; lifter) echo 900 ;; esac
    else
        case $1 in crash) echo 900 ;; fs) echo 300 ;; nnfs) echo 900 ;; boot) echo 600 ;; plat) echo 600 ;; winshim) echo 600 ;; modules) echo 600 ;; services) echo 600 ;; io) echo 600 ;; threads) echo 1800 ;; picker) echo 1500 ;; lifter) echo 3600 ;; esac
    fi
}
suite_cmd() {   # what the child runs (for the plan and the report)
    case $1 in
    crash)   echo "sh tests/port/crash/run_frame.sh --local" ;;
    fs)      echo "sh tests/port/fs/run.sh native  (per compiler)" ;;
    nnfs)    echo "sh tests/port/nnfs/run.sh native  (per compiler)" ;;
    boot)    echo "sh tests/port/boot/run.sh native  (per compiler)" ;;
    plat)    echo "sh tests/cspike/c3_build.sh native; c3_low; plat_selftest  (per compiler)" ;;
    winshim) echo "sh tests/port/winshim/run.sh native  (per compiler)" ;;
    modules) echo "sh tests/port/modules/run.sh native  (per compiler)" ;;
    services) echo "sh tests/port/services/run.sh native  (per compiler)" ;;
    io)      echo "sh tests/port/io/run.sh native  (per compiler)" ;;
    threads) echo "sh tests/port/threads/run_frame.sh --local" ;;
    picker)  echo "sh tests/port/picker/run_frame.sh --local" ;;
    lifter)  echo "sh tests/arm_host/run_frame.sh --local $PAIRS $ITERS" ;;
    esac
}

# ---------------------------------------------------------------------------------------------------- options
QUICK=""; DRY=""; FORCE=""; WITH_SHOTS=""; WORK=""; WANT=""; REFHASH=""; KHEADERS=""
SDL3_ROOT=${SDL3_ROOT:-$HOME/ssbu-port/deps/sdl3}
CHILD=""
usage() { sed -n '2,/^# POSIX sh only/p' "$SELF" | sed 's/^# \{0,1\}//'; }
while [ $# -gt 0 ]; do
    case $1 in
    --quick) QUICK=1 ;;
    --dry-run) DRY=1 ;;
    --force) FORCE=1 ;;
    --with-shots) WITH_SHOTS=1 ;;
    --suite) [ $# -ge 2 ] || { echo "--suite needs a name"; exit 2; }; WANT="$WANT $(echo "$2" | tr ',' ' ')"; shift ;;
    --work) [ $# -ge 2 ] || { echo "--work needs a folder"; exit 2; }; WORK=$2; shift ;;
    --sdl3) [ $# -ge 2 ] || { echo "--sdl3 needs a folder"; exit 2; }; SDL3_ROOT=$2; shift ;;
    --kheaders) [ $# -ge 2 ] || { echo "--kheaders needs a folder"; exit 2; }; KHEADERS=$2; shift ;;
    --ref-hash) [ $# -ge 2 ] || { echo "--ref-hash needs a file"; exit 2; }; REFHASH=$2; shift ;;
    --list) for s in $SUITE_NAMES; do printf '%-8s %s\n' "$s" "$(suite_desc "$s")"; done; exit 0 ;;
    --child) CHILD=$2; shift ;;
    -h | --help) usage; exit 0 ;;
    *) echo "unknown option: $1 (try -h)"; exit 2 ;;
    esac
    shift
done
for s in $WANT; do
    case " $SUITE_NAMES " in *" $s "*) ;; *) echo "no suite named '$s' (try --list)"; exit 2 ;; esac
done
[ -n "$WANT" ] || WANT=$SUITE_NAMES
if [ -n "$QUICK" ]; then PAIRS=200000; ITERS=20000; MODE_NAME=quick; else PAIRS=20000000; ITERS=1000000; MODE_NAME=full; fi

[ -n "$WORK" ] || WORK=$ROOT/build-frame-tests
case $WORK in /*) ;; *) WORK=$(pwd)/$WORK ;; esac
case $WORK in "" | / ) echo "refusing to use '$WORK' as the work folder"; exit 2 ;; esac

have() { command -v "$1" >/dev/null 2>&1; }
now() { date +%s; }

# ---------------------------------------------------------------------------------------------------- one suite, in a child
# (run under `timeout` by the parent: the environment is exported by the parent)
child_main() {
    cd "$ROOT" || exit 2
    NAME=$1
    # bound the user's process count (the tests fork and make threads; one runaway once filled the Frame)
    N=0
    if [ "$(uname -s)" = Linux ]; then N=$(ps -eLf --no-headers -u "$(id -un)" 2>/dev/null | wc -l); fi
    if [ "${N:-0}" -gt 0 ] 2>/dev/null; then ulimit -u $((N + 300)) 2>/dev/null; fi
    export SSBU_VMEM_NO_REEXEC=1
    rc=0
    CCS=$FK_CCS
    if [ -n "$FK_QUICK" ]; then CCS=${CCS%% *}; fi
    case $NAME in
    crash) sh tests/port/crash/run_frame.sh --local || rc=$? ;;
    threads) sh tests/port/threads/run_frame.sh --local || rc=$? ;;
    picker) sh tests/port/picker/run_frame.sh --local || rc=$? ;;
    lifter) sh tests/arm_host/run_frame.sh --local "$FK_PAIRS" "$FK_ITERS" || rc=$? ;;
    fs | nnfs | boot | winshim | modules | services | io)
        for cc in $CCS; do
            echo "######## $NAME CC=$cc"
            CC=$cc OUT="$FRAME_WORK/$cc" sh "tests/port/$NAME/run.sh" native 2>&1
            st=$?
            if [ $st -ne 0 ]; then echo "FAIL $NAME CC=$cc: exit $st"; rc=1; fi
        done ;;
    plat)
        for cc in $CCS; do
            echo "######## plat CC=$cc"
            O=$FRAME_WORK/$cc
            if CC=$cc OUT="$O" sh tests/cspike/c3_build.sh native 2>&1; then
                for b in c3_low plat_selftest; do
                    echo "== $b"
                    timeout 300 "$O/$b" 2>&1
                    st=$?
                    if [ $st -eq 0 ]; then echo "ok   $b ($cc)"; else echo "FAIL $b ($cc): exit $st"; rc=1; fi
                done
            else echo "FAIL build of c3_low / plat_selftest ($cc)"; rc=1; fi
        done ;;
    esac
    exit $rc
}
if [ -n "$CHILD" ]; then child_main "$CHILD"; fi

# ---------------------------------------------------------------------------------------------------- work folder
mkdir -p "$WORK/tmp" "$WORK/bin" || { echo "cannot create $WORK"; exit 2; }
STAMP=$(date +%Y%m%d-%H%M%S)
RES=$WORK/results/ssbu-frame-$STAMP
if [ -n "$DRY" ]; then RES=$WORK/tmp/dry-run; fi
rm -rf "$RES"
mkdir -p "$RES" || exit 2
START=$(now)

# a stand-in for `timeout` where there is none (the Frame has GNU coreutils; a development Mac does not)
if ! have timeout; then
    cat > "$WORK/bin/timeout" <<'SHIM'
#!/bin/sh
# stand-in for GNU timeout: timeout [-s SIG] [-k DUR] DURATION command...   (kills the command, not its children)
while [ $# -gt 0 ]; do
    case "$1" in
    -s | -k) shift 2 ;;
    --) shift; break ;;
    -*) shift ;;
    *) break ;;
    esac
done
d=$1; shift
case "$d" in
*m) d=$(( ${d%m} * 60 )) ;;
*h) d=$(( ${d%h} * 3600 )) ;;
*d) d=$(( ${d%d} * 86400 )) ;;
*s) d=${d%s} ;;
esac
mark="${TMPDIR:-/tmp}/.timeout.$$"
"$@" &
pid=$!
( sleep "$d"; : > "$mark"; kill -TERM "$pid"; sleep 3; kill -KILL "$pid" ) >/dev/null 2>&1 &
w=$!
wait "$pid"; rc=$?
kill "$w" >/dev/null 2>&1
if [ -f "$mark" ]; then rm -f "$mark"; rc=124; fi
exit $rc
SHIM
    chmod +x "$WORK/bin/timeout"
    PATH=$WORK/bin:$PATH; export PATH
    FAKE_TIMEOUT=1
else
    FAKE_TIMEOUT=""
fi

# ---------------------------------------------------------------------------------------------------- facts + preflight
HOST_OS=$(uname -s); HOST_ARCH=$(uname -m)
ON_DEVICE=""
if [ "$HOST_OS" = Linux ] && [ "$HOST_ARCH" = aarch64 ]; then ON_DEVICE=1; fi
ARM_HOST=""
case $HOST_ARCH in aarch64 | arm64) ARM_HOST=1 ;; esac
if [ -n "$KHEADERS" ]; then
    C_INCLUDE_PATH=$KHEADERS${C_INCLUDE_PATH:+:$C_INCLUDE_PATH}; export C_INCLUDE_PATH
fi
export SDL3_ROOT

first_line() { "$@" 2>&1 | head -1; }
FK_CCS=""
for cc in gcc clang; do
    if have $cc && $cc --version >/dev/null 2>&1; then FK_CCS="$FK_CCS $cc"; fi
done
FK_CCS=${FK_CCS# }
CC1=${FK_CCS%% *}

HAVE_CC=""; [ -n "$CC1" ] && HAVE_CC=1
HAVE_KH=""; HAVE_MAKE=""; HAVE_OBJDUMP=""; HAVE_AS=""; HAVE_PY=""; HAVE_SDL=""; HAVE_SSL=""; HAVE_VK=""
have make && HAVE_MAKE=1
have objdump && HAVE_OBJDUMP=1
have as && HAVE_AS=1
have python3 && HAVE_PY=1
[ -f "$SDL3_ROOT/include/SDL3/SDL.h" ] && ls "$SDL3_ROOT"/lib/libSDL3.so* >/dev/null 2>&1 && HAVE_SDL=1

ldc() {   # the dynamic linker's library list (ldconfig is in /sbin, not on a user's PATH, on some systems)
    if have ldconfig; then ldconfig -p 2>/dev/null; elif [ -x /sbin/ldconfig ]; then /sbin/ldconfig -p 2>/dev/null; fi
}
find_lib() {  # find_lib <file name prefix>: a path, or nothing
    p=$(ldc | grep -F "$1" | head -1 | sed 's/.*=> *//')
    if [ -z "$p" ]; then
        for d in /usr/lib /usr/lib64 /lib /lib64 /usr/lib/aarch64-linux-gnu; do
            for f in "$d/$1"*; do if [ -e "$f" ]; then p=$f; break 2; fi; done
        done
    fi
    echo "$p"
}
SSL_LIB=$(find_lib libcrypto.so.3); [ -n "$SSL_LIB" ] && HAVE_SSL=1
VK_LIB=$(find_lib libvulkan.so.1); [ -n "$VK_LIB" ] && HAVE_VK=1
if [ -n "$HAVE_CC" ]; then
    printf '#include <linux/userfaultfd.h>\n#include <linux/fs.h>\n#include <linux/futex.h>\nint main(void) { return 0; }\n' > "$WORK/tmp/kh.c"
    $CC1 -fsyntax-only "$WORK/tmp/kh.c" >/dev/null 2>&1 && HAVE_KH=1
fi

preflight() {
    echo "Preflight (host: $HOST_OS $HOST_ARCH)"
    if [ -n "$ON_DEVICE" ]; then echo "  [ ok      ] host is aarch64 Linux"
    elif [ -n "$FORCE" ]; then echo "  [WARN     ] host is $HOST_OS $HOST_ARCH, not aarch64 Linux: --force given, suites run anyway and may fail to build"
    else echo "  [MISSING  ] host is $HOST_OS $HOST_ARCH, not aarch64 Linux: device suites are skipped (crash still runs on an arm64 Mac). Run this on the Frame, or --force."; fi
    if [ -n "$FK_CCS" ]; then
        for cc in $FK_CCS; do echo "  [ ok      ] $cc: $(first_line $cc --version)"; done
        case " $FK_CCS " in *" gcc "*) ;; *) echo "  [optional ] gcc not found: every suite runs with clang only (less coverage)" ;; esac
        case " $FK_CCS " in *" clang "*) ;; *) echo "  [optional ] clang not found: every suite runs with gcc only (less coverage; the BTI and FORTIFY configurations that use clang are left out)" ;; esac
    else
        echo "  [MISSING  ] gcc / clang: no compiler on PATH. Fix: the root filesystem is read-only, so either unpack an aarch64 clang release under ~/.local and put its bin on PATH, or (a system change this kit never makes) 'sudo steamos-readonly disable; sudo pacman -S --needed gcc binutils make linux-api-headers'. Without it NO suite can run."
    fi
    if [ -n "$HAVE_KH" ]; then echo "  [ ok      ] kernel headers (linux/userfaultfd.h, linux/fs.h, linux/futex.h)"
    elif [ -n "$HAVE_CC" ]; then echo "  [MISSING  ] kernel headers: Fix: copy a kernel's exported headers to a folder (make headers_install INSTALL_HDR_PATH=~/ssbu-port/kheaders) and pass --kheaders ~/ssbu-port/kheaders/include, or install linux-api-headers. Skips: fs, nnfs, boot, winshim, modules, services, io, threads, plat."; fi
    if have make; then echo "  [ ok      ] make: $(first_line make --version)"
    else echo "  [MISSING  ] make: Fix: build GNU make into ~/.local (./configure --prefix=\$HOME/.local && make install) or install it with pacman. Skips: picker."; fi
    if have timeout; then echo "  [ ok      ] timeout (coreutils)"
    else echo "  [optional ] timeout not found: using the kit's own sh stand-in (it kills the command but not its children). Fix: coreutils."; fi
    if [ -n "$HAVE_OBJDUMP" ]; then echo "  [ ok      ] objdump: $(first_line objdump --version)"
    else echo "  [MISSING  ] objdump (binutils): Fix: binutils via pacman, or put llvm-objdump on PATH as 'objdump' (ln -s \$(command -v llvm-objdump) ~/.local/bin/objdump). Skips: lifter; threads loses its TLS hazard check."; fi
    if [ -n "$HAVE_AS" ]; then echo "  [ ok      ] as (assembler): $(first_line as --version)"
    else echo "  [MISSING  ] as (binutils): Fix: binutils via pacman, or ln -s \$(command -v llvm-mc) is NOT enough; use a toolchain with GNU as. Skips: lifter."; fi
    if [ -n "$HAVE_PY" ]; then echo "  [ ok      ] python3: $(first_line python3 --version)"
    else echo "  [MISSING  ] python3: Fix: SteamOS ships it; otherwise install python or put any python3 on PATH. Skips: lifter (the lifter's decoder test and litmus generator are Python)."; fi
    if [ -n "$HAVE_SSL" ]; then echo "  [ ok      ] libcrypto.so.3: $SSL_LIB"
    else echo "  [MISSING  ] libcrypto.so.3 (OpenSSL 3 runtime): Fix: it is part of SteamOS (/usr/lib); check the install. Skips: plat (SHA-256 check); fs and boot use abort() stubs for it."; fi
    if [ -n "$HAVE_SDL" ]; then echo "  [ ok      ] SDL3 in $SDL3_ROOT ($(sdl_version))"
    else echo "  [optional ] SDL3 not found in $SDL3_ROOT: Fix: sh $ROOT/toolchain/build_deps_on_device.sh (needs cmake, ninja, curl; installs only under ~/ssbu-port) or --sdl3 DIR. Without it: picker runs its no-display test only; fs, boot, plat use stand-in headers."; fi
    if [ -n "$HAVE_VK" ]; then echo "  [ ok      ] Vulkan loader: $VK_LIB"
    else echo "  [optional ] Vulkan loader (libvulkan.so.1) not found: Fix: it comes with Mesa on SteamOS (/usr/lib). Without it plat_selftest's loader check fails, so the plat suite reports FAIL."; fi
    if have vulkaninfo; then echo "  [ ok      ] vulkaninfo"; else echo "  [optional ] vulkaninfo not found: the report has no GPU summary (vulkan-tools)"; fi
    if have gamescope; then echo "  [ ok      ] gamescope present"; else echo "  [optional ] gamescope not found: only noted in the report"; fi
    if have gamescopectl && [ -n "$(gamescope_socket)" ]; then echo "  [ ok      ] a gamescope session is running: the picker suite photographs its windows"
    else echo "  [optional ] no running gamescope session (or no gamescopectl): the picker suite does its offscreen runs only. Run the kit from Game Mode (Steam's terminal/ssh into the running session) for the display runs."; fi
}
sdl_version() {
    h=$SDL3_ROOT/include/SDL3/SDL_version.h
    if [ -f "$h" ]; then
        awk '/#define SDL_(MAJOR|MINOR|MICRO)_VERSION[ \t]/ { v[$2] = $3 } END { printf "%s.%s.%s", v["SDL_MAJOR_VERSION"], v["SDL_MINOR_VERSION"], v["SDL_MICRO_VERSION"] }' "$h"
    else echo "version unknown"; fi
}
gamescope_socket() {
    d=/run/user/$(id -u)
    if [ -d "$d" ]; then ( cd "$d" && for s in gamescope-[0-9]*; do case $s in *-ei | *.lock) ;; *) if [ -S "$s" ]; then echo "$s"; break; fi ;; esac; done ); fi
}

blocked_reason() {   # a reason this suite cannot run here, or nothing
    s=$1
    if [ -z "$ON_DEVICE" ] && [ -z "$FORCE" ]; then
        if [ "$s" = crash ] && [ -n "$ARM_HOST" ] && [ -n "$HAVE_CC" ]; then return 0; fi
        echo "host is $HOST_OS $HOST_ARCH, not aarch64 Linux (--force to try)"; return 0
    fi
    if [ -z "$HAVE_CC" ]; then echo "no gcc or clang on PATH"; return 0; fi
    case $s in
    fs | nnfs | boot | winshim | modules | services | io | threads) [ -n "$HAVE_KH" ] || echo "no kernel headers (linux/userfaultfd.h); see --kheaders" ;;
    plat) if [ -z "$HAVE_KH" ]; then echo "no kernel headers (linux/userfaultfd.h); see --kheaders"; elif [ -z "$HAVE_SSL" ]; then echo "no libcrypto.so.3"; fi ;;
    picker) [ -n "$HAVE_MAKE" ] || echo "no make" ;;
    lifter)
        if [ -z "$HAVE_PY" ]; then echo "no python3"; elif [ -z "$HAVE_OBJDUMP" ]; then echo "no objdump"; elif [ -z "$HAVE_AS" ]; then echo "no as"; fi ;;
    esac
}

# ---------------------------------------------------------------------------------------------------- device info
info_section() { printf '\n== %s ==\n' "$1"; }
collect_info() {
    info_section "kit"
    echo "$KIT_VERSION, mode: $MODE_NAME, suites: $WANT"
    echo "date (UTC): $(date -u '+%Y-%m-%d %H:%M:%S')"
    if have git && git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
        echo "checkout: branch $(git -C "$ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null) at $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null)," \
             "$(git -C "$ROOT" status --porcelain 2>/dev/null | grep -v '^??' | wc -l | tr -d ' ') modified tracked file(s)"
    else echo "checkout: not a git work tree"; fi
    info_section "system"
    echo "uname: $(uname -srvm)"
    if [ -r /etc/os-release ]; then grep -E '^(NAME|PRETTY_NAME|VERSION_ID|BUILD_ID|VARIANT_ID)=' /etc/os-release; fi
    echo "glibc: $(getconf GNU_LIBC_VERSION 2>/dev/null || echo unknown)"
    echo "cpus online: $(getconf _NPROCESSORS_ONLN 2>/dev/null || echo unknown), page size: $(getconf PAGESIZE 2>/dev/null || echo unknown)"
    if [ -r /proc/meminfo ]; then grep -E '^(MemTotal|SwapTotal|CommitLimit|Hugepagesize):' /proc/meminfo; fi
    info_section "cpu (/proc/cpuinfo, one line per distinct value)"
    if [ -r /proc/cpuinfo ]; then
        grep -E '^(Features|CPU implementer|CPU architecture|CPU variant|CPU part|CPU revision|model name|Hardware|flags)[[:space:]]*:' /proc/cpuinfo | sort | uniq -c
    else echo "no /proc/cpuinfo"; fi
    info_section "BTI / PAC / other extensions (from the Features line)"
    feat=$(grep -m1 -E '^Features[[:space:]]*:' /proc/cpuinfo 2>/dev/null | sed 's/^[^:]*://')
    if [ -z "$feat" ]; then echo "no Features line (not an aarch64 Linux kernel)"; else
        for f in bti paca pacg mte sve sve2 atomics asimdrdm fphp asimdhp lrcpc ilrcpc dcpop; do
            case " $feat " in *" $f "*) echo "$f: yes" ;; *) echo "$f: no" ;; esac
        done
    fi
    info_section "compilers and tools"
    for t in gcc clang cc make as ld objdump python3 cmake ninja pkg-config vulkaninfo gamescope gamescopectl; do
        if have $t; then
            case $t in
            gamescope | gamescopectl) echo "$t: present" ;;
            *) echo "$t: $(first_line $t --version)" ;;
            esac
        else echo "$t: not found"; fi
    done
    if have ldd; then echo "ldd: $(first_line ldd --version)"; fi
    info_section "kernel limits and address space"
    for f in /proc/sys/vm/mmap_rnd_bits /proc/sys/vm/mmap_rnd_compat_bits /proc/sys/vm/overcommit_memory /proc/sys/vm/max_map_count \
             /proc/sys/vm/mmap_min_addr /sys/kernel/mm/transparent_hugepage/enabled; do
        if [ -r "$f" ]; then echo "$(echo "$f" | sed 's#^/proc/sys/##; s#^/sys/##'): $(tr '\n' ' ' < "$f")"; else echo "$f: not readable"; fi
    done
    if [ -r /proc/config.gz ] && have zcat; then
        zcat /proc/config.gz 2>/dev/null | grep -E '^CONFIG_(ARM64_VA_BITS|ARM64_VA_BITS_[0-9]+|ARM64_[0-9]+K_PAGES|ARM64_BTI|ARM64_PTR_AUTH|ARM64_MTE|ARM64_USERFAULTFD|USERFAULTFD|ARM64_LSE_ATOMICS)=' | sort
    else echo "kernel config (/proc/config.gz): not readable"; fi
    echo "stack limit (ulimit -s): $(ulimit -s 2>/dev/null), user processes (ulimit -u): $(ulimit -u 2>/dev/null), open files (ulimit -n): $(ulimit -n 2>/dev/null)"
    echo "top of the address space as a process sees it (/proc/self/maps, address range, permissions, kind; highest last):"
    if [ -r /proc/self/maps ]; then
        tail -6 /proc/self/maps | awk '{ k = ($6 ~ /^\[/) ? $6 : ($6 == "" ? "anon" : "file"); print "  " $1 " " $2 " " k }'
        echo "mappings in this shell: $(wc -l < /proc/self/maps | tr -d ' ')"
    else echo "  no /proc/self/maps"; fi
    info_section "display"
    echo "gamescope: $(if have gamescope; then echo installed; else echo "not installed"; fi), running session socket: $(gamescope_socket | grep . || echo none)"
    echo "pgrep gamescope: $(if have pgrep; then pgrep -c -x gamescope 2>/dev/null || echo 0; else echo "no pgrep"; fi) process(es)"
    echo "session: XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-unset} XDG_CURRENT_DESKTOP=${XDG_CURRENT_DESKTOP:-unset} DISPLAY=${DISPLAY:-unset} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-unset}"
    [ -S /tmp/.X11-unix/X0 ] && echo "X display :0 socket present" || echo "no X display :0 socket"
    info_section "SDL3"
    echo "root: $SDL3_ROOT"
    echo "version from headers: $(sdl_version)"
    ls "$SDL3_ROOT"/lib/libSDL3.so* 2>/dev/null | sed 's#.*/##' | sed 's/^/lib: /'
    echo "system libSDL3 in the linker cache: $(ldc | grep -c 'libSDL3')"
    if have pkg-config; then echo "pkg-config sdl3: $(pkg-config --modversion sdl3 2>/dev/null || echo none)"; fi
    info_section "Vulkan"
    echo "loader: ${VK_LIB:-not found}"
    ls /usr/share/vulkan/icd.d 2>/dev/null | sed 's/^/icd: /'
    if have vulkaninfo; then
        echo "-- vulkaninfo --summary"
        timeout 30 vulkaninfo --summary 2>&1 | grep -viE 'uuid|serial' | head -80
    fi
    info_section "kit environment"
    echo "work folder: $WORK"
    echo "SDL3_ROOT=$SDL3_ROOT KERNEL_HEADERS=${KHEADERS:-none} timeout=$(if [ -n "$FAKE_TIMEOUT" ]; then echo "kit stand-in"; else echo coreutils; fi)"
    echo "free space for the work folder: $(df -k "$WORK" 2>/dev/null | awk 'NR == 2 { printf "%d MiB", $4 / 1024 }')"
}

# ---------------------------------------------------------------------------------------------------- counting
scrape() {   # scrape <log>: "checks failed" scraped from the output (approximate: every test prints its own way)
    awk '
    { s = $0
      while (match(s, /[0-9]+ checks/)) { c += substr(s, RSTART, RLENGTH) + 0; hasc = 1; s = substr(s, RSTART + RLENGTH) }
      s = $0
      while (match(s, /[0-9]+ failed/)) { f += substr(s, RSTART, RLENGTH) + 0; s = substr(s, RSTART + RLENGTH) }
      if ($0 ~ /^ok   /) c++
      if ($0 ~ /^FAIL / && $0 !~ /^FAIL line [0-9]+:/) f++ }
    END { printf "%d %d\n", c, f }' "$1"
}

# ---------------------------------------------------------------------------------------------------- run
TSV=$RES/summary.tsv
: > "$TSV"
INTERRUPTED=""
trap 'INTERRUPTED=1' INT TERM
record() { printf '%s|%s|%s|%s|%s|%s\n' "$1" "$2" "$3" "$4" "$5" "$6" >> "$TSV"; }

run_suite() {
    name=$1
    log=$RES/$name.log
    reason=$(blocked_reason "$name")
    if [ -n "$reason" ]; then
        printf 'SKIP: %s\n' "$reason" > "$log"
        record "$name" SKIP - - 0 "$reason"
        echo "  $name: SKIP ($reason)"
        return
    fi
    tmo=$(suite_timeout "$name")
    echo "  $name: running (timeout ${tmo}s) ..."
    t0=$(now)
    (
        cd "$ROOT" || exit 2
        FRAME_WORK=$WORK/obj/$name; TMPDIR=$WORK/tmp
        FRAME_QUICK=$QUICK; FK_QUICK=$QUICK; FK_CCS=$FK_CCS; FK_PAIRS=$PAIRS; FK_ITERS=$ITERS
        FRAME_REF_HASH=$REFHASH; LC_ALL=C; PYTHONDONTWRITEBYTECODE=1
        export FRAME_WORK TMPDIR FRAME_QUICK FK_QUICK FK_CCS FK_PAIRS FK_ITERS FRAME_REF_HASH LC_ALL PYTHONDONTWRITEBYTECODE
        [ -n "$REFHASH" ] || unset FRAME_REF_HASH
        mkdir -p "$FRAME_WORK"
        exec timeout -k 15 "$tmo" sh "$SELF" --child "$name"
    ) > "$log" 2>&1
    rc=$?
    t1=$(now)
    secs=$((t1 - t0))
    set -- $(scrape "$log")
    chk=$1; bad=$2
    note=""
    if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
        status=TIMEOUT; note="killed after ${tmo}s"
        if have pkill; then pkill -f -- "$WORK/obj/$name" >/dev/null 2>&1; fi
    elif [ $rc -eq 77 ]; then
        status=SKIP; note=$(grep -m1 '^SKIP' "$log" | sed 's/^SKIP: *//')
    elif [ $rc -ne 0 ]; then
        status=FAIL; note="exit $rc"
    elif [ "$bad" -gt 0 ]; then
        status=FAIL; note="exit 0 but $bad failure(s) in the output"
    else
        status=PASS
        if grep -q '^SKIP' "$log"; then status=PARTIAL; note=$(grep '^SKIP' "$log" | sed 's/^SKIP: *//' | head -3 | tr '\n' ';' | sed 's/;$//'); fi
    fi
    [ "$chk" -gt 0 ] || chk=-
    record "$name" "$status" "$chk" "$bad" "$secs" "$note"
    echo "  $name: $status (${secs}s${note:+, $note})"
}

print_table() {
    printf '  %-8s %-8s %12s %6s %6s  %s\n' suite result checks failed secs note
    while IFS='|' read -r n st c f s note; do
        printf '  %-8s %-8s %12s %6s %6s  %s\n' "$n" "$st" "$c" "$f" "$s" "$note"
    done < "$TSV"
}

# ---------------------------------------------------------------------------------------------------- go
echo "$KIT_VERSION: ${QUICK:+quick }${DRY:+dry-run }suites:$(for s in $WANT; do printf ' %s' "$s"; done)"
echo "checkout: $ROOT"
echo "work folder: $WORK"
echo
preflight > "$RES/preflight.txt"
cat "$RES/preflight.txt"
echo

if [ -n "$DRY" ]; then
    echo "Plan (nothing is built or run):"
    for s in $WANT; do
        r=$(blocked_reason "$s")
        printf '  %-8s timeout %5ss  %s\n' "$s" "$(suite_timeout "$s")" "$(suite_desc "$s")"
        if [ -n "$r" ]; then echo "           would be SKIPPED: $r"; else echo "           runs: $(suite_cmd "$s")"; fi
    done
    echo
    echo "Would write: $WORK/ssbu-frame-<stamp>.txt and $WORK/ssbu-frame-<stamp>.tar.gz"
    rm -rf "$RES"
    exit 0
fi

# fresh build outputs (only these two folders of the work folder are ever emptied)
rm -rf "$WORK/obj" "$WORK/tmp"
mkdir -p "$WORK/obj" "$WORK/tmp"
collect_info > "$RES/device.txt" 2>&1

echo "Running:"
for s in $WANT; do
    if [ -n "$INTERRUPTED" ]; then record "$s" "NOT-RUN" - - 0 "interrupted"; continue; fi
    run_suite "$s"
done
echo

# ---------------------------------------------------------------------------------------------------- verdict + report
NFAIL=$(grep -cE '\|(FAIL|TIMEOUT)\|' "$TSV")
NRAN=$(grep -cE '\|(PASS|PARTIAL|FAIL|TIMEOUT)\|' "$TSV")
NSKIP=$(grep -cE '\|(SKIP|NOT-RUN)\|' "$TSV")
if [ -n "$INTERRUPTED" ]; then VERDICT="INTERRUPTED: $NRAN suite(s) ran, $NFAIL failed"
elif [ "$NRAN" -eq 0 ]; then VERDICT="NOTHING RAN ($NSKIP skipped)"
elif [ "$NFAIL" -gt 0 ]; then VERDICT="FAILURES: $NFAIL of $NRAN suite(s) that ran failed ($NSKIP skipped)"
else VERDICT="ALL PASSED: $NRAN suite(s) ran ($NSKIP skipped)"; fi
END=$(now)

REPORT=$RES/report.txt
TAR=$WORK/ssbu-frame-$STAMP.tar.gz
{
    echo "SSBU Steam Frame device report"
    echo "=============================="
    echo "$KIT_VERSION, $MODE_NAME run, finished $(date -u '+%Y-%m-%d %H:%M:%S') UTC, $((END - START)) s in all"
    echo "Verdict: $VERDICT"
    echo
    echo "Summary"
    print_table
    echo
    echo "Counts are scraped from each test's output (approximate); the result column is the exit status."
    echo "PARTIAL = passed, but part of it was skipped (see the note and 'SKIP:' lines in the log)."
    echo
    cat "$RES/preflight.txt"
    echo
    echo "Device"
    echo "------"
    sed '1{/^$/d;}' "$RES/device.txt"
    echo
    echo "Suite details (the full logs are in the .tar.gz)"
    echo "------------------------------------------------"
    while IFS='|' read -r n st c f s note; do
        echo
        echo "### $n: $st  (${s}s)  $(suite_desc "$n")"
        echo "    command: $(suite_cmd "$n")"
        [ -n "$note" ] && echo "    note: $note"
        if [ -f "$RES/$n.log" ]; then
            echo "    key lines:"
            grep -E 'FAIL|failed|PASS|SKIP|CHECK failed|error:|GAP|soft-hash|stub called|Segmentation|Abort|ok   ' "$RES/$n.log" | head -60 | sed 's/^/      /'
            if [ "$st" = FAIL ] || [ "$st" = TIMEOUT ]; then
                echo "    last lines of the log:"
                tail -25 "$RES/$n.log" | sed 's/^/      /'
            fi
        fi
    done < "$TSV"
    echo
    echo "Files"
    echo "-----"
    echo "report: $WORK/ssbu-frame-$STAMP.txt"
    echo "logs:   $TAR"
} > "$REPORT"

# scrub every text file: $HOME -> ~, checkout -> <repo>, work folder -> <work>, other users' home folders, user and host names
esc() { printf '%s' "$1" | sed 's,[][\\.*^$#/|+?(){}],\\&,g'; }
SED=$WORK/tmp/redact.sed
: > "$SED"
printf 's#%s#<work>#g\n' "$(esc "$WORK")" >> "$SED"
printf 's#%s#<repo>#g\n' "$(esc "$ROOT")" >> "$SED"
if [ -n "${HOME:-}" ] && [ "$HOME" != / ]; then printf 's#%s#~#g\n' "$(esc "$HOME")" >> "$SED"; fi
{
    echo 's#/home/[^/ :]*#/home/<user>#g'
    echo 's#/var/home/[^/ :]*#/var/home/<user>#g'
    echo 's#/run/media/[^/ :]*#/run/media/<user>#g'
    echo 's#/Users/[^/ :]*#/Users/<user>#g'
    echo 's#/run/user/[0-9]*#/run/user/<uid>#g'
} >> "$SED"
U=$(id -un 2>/dev/null); H=$(uname -n 2>/dev/null)
if [ ${#U} -ge 3 ] && [ "$U" != root ]; then printf 's#(^|[^A-Za-z0-9_.-])%s([^A-Za-z0-9_.-]|$)#\\1<user>\\2#g\n' "$(esc "$U")" >> "$SED"; fi
if [ ${#H} -ge 3 ]; then printf 's#(^|[^A-Za-z0-9_.-])%s([^A-Za-z0-9_.-]|$)#\\1<host>\\2#g\n' "$(esc "$H")" >> "$SED"; fi
find "$RES" -type f ! -name '*.png' ! -name '*.bmp' | while IFS= read -r f; do
    if sed -E -f "$SED" "$f" > "$f.scrub" 2>/dev/null; then mv "$f.scrub" "$f"; else rm -f "$f.scrub"; fi
done

if [ -n "$WITH_SHOTS" ] && [ -d "$WORK/obj/picker/picker-shots" ]; then
    mkdir -p "$RES/picker-shots" && cp -R "$WORK/obj/picker/picker-shots/." "$RES/picker-shots/"
fi
cp "$REPORT" "$WORK/ssbu-frame-$STAMP.txt"
tar -czf "$TAR" -C "$WORK/results" "ssbu-frame-$STAMP"

echo "$VERDICT"
echo
print_table
echo
echo "Report: $WORK/ssbu-frame-$STAMP.txt"
echo "Logs:   $TAR"
echo "Bring both files back (the work folder $WORK can be deleted afterwards)."
if [ -d "$WORK/obj/picker/picker-shots" ] && [ -z "$WITH_SHOTS" ]; then echo "(picker screenshots are in $WORK/obj/picker/picker-shots, not in the .tar.gz: --with-shots includes them)"; fi
if [ -n "$INTERRUPTED" ]; then exit 1; fi
if [ "$NRAN" -eq 0 ]; then exit 3; fi
if [ "$NFAIL" -gt 0 ]; then exit 1; fi
exit 0

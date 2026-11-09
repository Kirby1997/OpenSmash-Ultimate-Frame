#!/bin/sh
# nnfontll.c's FreeType outline path (the Linux / macOS build's glyph source) against FreeType itself (test_font.c): for every glyph of a
# TrueType face (quadratic: the conic -> cubic raise must be exact), a CFF face (FreeType's own cubics) and a collection (face 2) the
# flattened outline equals FreeType's evaluated at the same steps, hmtx advances equal FreeType's, the 16.16 advance is within
# 1/65536 px, and the 8-bit glyph maps of 100+ characters stay within 8% of FT_Render_Glyph. No game files: the fonts are the system's
# (FONT, FONT_CFF, FONT_TTC to name others; a face that is not found is skipped and said so).
# Needs FreeType's headers and library (ft2build.h, libfreetype): SteamOS has the library and no headers, so on the Frame this
# skips unless FT_INC / FT_LIB point at a FreeType (toolchain/sync_sysroot.sh brings the sysroot's headers to a PC, not to the device).
# Not ported from linux-arm64's tests/port/io: test_pad.c (nnhid.c + nnpad_sdl.c on SDL3 virtual joysticks) and test_audio.c (nnaudio.c's
# SDL3 AudioOut and libopus): both need SDL3's headers and a device, which this tree's test layer does not have (see docs/STEAM_FRAME.md).
#   tests/port/io/run.sh [mode]      native (default) | mac      OUT (default ${TMPDIR:-/tmp}/ssbu-io-<mode>), CC, CFLAGS_EXTRA
set -eu
cd "$(dirname "$0")/../../.."
MODE=${1:-native}
OUT=${OUT:-${TMPDIR:-/tmp}/ssbu-io-$MODE}; mkdir -p "$OUT"
T=tests/port/io
. tests/port/native_env.sh
BASE="-O1 -g -w -fvisibility=hidden -fno-omit-frame-pointer -include runtime/plat/plat_pre.h -I runtime/plat -I runtime ${CFLAGS_EXTRA:-}"
FT_INC=${FT_INC:-}; FT_LIB=${FT_LIB:--lfreetype}
if [ -z "$FT_INC" ]; then
    for d in /usr/include/freetype2 /opt/homebrew/include/freetype2 /usr/local/include/freetype2 "$HOME/ssbu-port/deps/freetype/include/freetype2"; do
        [ -z "$FT_INC" ] && [ -f "$d/ft2build.h" ] && FT_INC=$d
    done
fi
case "$MODE" in
native)
    CC=${CC:-cc}
    F="-fno-pie $BASE $NAT_INC"
    L="-no-pie $NAT_SDL_LIBS $NAT_SSL_LIBS -lm -lpthread -ldl" ;;
mac)
    CC=${CC:-clang}
    SDK=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk          # (the default SDK of some installs does not link)
    [ -d "$SDK" ] || SDK=$(xcrun --show-sdk-path)
    NAT_SSL_LIBS=""; NAT_SDL_LIBS=""; NAT_SYM_PREFIX=_
    F="-isysroot $SDK -mmacosx-version-min=14.4 -DPLAT_LINUX_PATHS_ON_MAC $BASE -idirafter tests/port/threads/stub"
    L=""
    [ -d /opt/homebrew/lib ] && FT_LIB="-L/opt/homebrew/lib $FT_LIB" ;;
*) echo "mode: native | mac"; exit 2 ;;
esac
if [ -z "$FT_INC" ]; then echo "io: FreeType's headers (ft2build.h) not found (FT_INC=<folder holding it>): test_font skipped"; exit 0; fi
$CC $F -c runtime/plat_posix.c -o "$OUT/plat_posix.o"
$CC $(echo "$F" | sed 's#-include runtime/plat/plat_pre.h##; s#-I runtime/plat ##') -std=gnu11 -c runtime/plat_crash.c -o "$OUT/plat_crash.o"
$CC $F -I "$FT_INC" -c $T/test_font.c -o "$OUT/test_font.o"
nat_stubs "$OUT/stubs.c" "$OUT/plat_posix.o" "$OUT/test_font.o"
$CC $F -c "$OUT/stubs.c" -o "$OUT/stubs.o"
$CC $F "$OUT/test_font.o" "$OUT/plat_posix.o" "$OUT/plat_crash.o" "$OUT/stubs.o" $L $FT_LIB -o "$OUT/test_font"
first() { for f in "$@"; do if [ -f "$f" ]; then echo "$f"; return 0; fi; done; return 0; }
FONT=${FONT:-$(first /usr/share/fonts/TTF/Hack-Regular.ttf /usr/share/fonts/TTF/DejaVuSans.ttf /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
    /System/Library/Fonts/Supplemental/Arial.ttf /System/Library/Fonts/Supplemental/Courier\ New.ttf /Library/Fonts/Arial.ttf)}
FONT_CFF=${FONT_CFF:-$(first /usr/share/fonts/adobe-source-code-pro/SourceCodePro-Regular.otf /usr/share/fonts/OTF/SourceCodePro-Regular.otf \
    /System/Library/Fonts/Supplemental/NotoSansJavanese-Regular.otf /System/Library/Fonts/Supplemental/NotoSansMasaramGondi-Regular.otf)}
FONT_TTC=${FONT_TTC:-$(first /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc /System/Library/Fonts/Helvetica.ttc /System/Library/Fonts/Courier.ttc)}
fail=0
ran=0
run() { # <label> <font> [face] [px]
    [ -n "$2" ] || { echo "== $1: no font found, skipped (name one with FONT / FONT_CFF / FONT_TTC)"; return 0; }
    echo "== $1: $2 ${3:+face $3}"
    ran=$((ran + 1))
    timeout 600 "$OUT/test_font" "$2" ${3:-0} ${4:-16} || { echo "$1: exit $?"; fail=1; }
}
run "TrueType (quadratic)" "$FONT" 0
run "CFF (cubic)" "$FONT_CFF" 0
run "collection" "$FONT_TTC" 2
[ "$ran" -gt 0 ] || echo "io: no font file found at all: nothing was compared"
exit $fail

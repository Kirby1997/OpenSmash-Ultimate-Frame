# Sourced (from the repository root) by the "native" modes of tests/port/boot/run.sh, tests/port/fs/run.sh and
# tests/cspike/c3_build.sh: what a build ON the Steam Frame needs and may not have. Sets
#   SDL3_ROOT      $HOME/ssbu-port/deps/sdl3 unless set (toolchain/build_deps_on_device.sh builds it there)
#   NAT_INC        compiler flags for the headers: the real SDL3 headers when SDL3_ROOT has them; the stand-ins of
#                  tests/port/threads/stub (SDL3/SDL.h, openssl/evp.h) as a last resort (-idirafter: real system headers win)
#   NAT_SDL_LIBS   link flags of the real libSDL3, empty when there is none
#   NAT_SSL_LIBS   path of the system libcrypto.so.3 (no -dev package or symlink needed), empty when there is none
# nat_stubs OUT.c obj...  writes abort() stubs for the SDL_* / EVP_* symbols the objects need and no real library provides.
# No test that uses the stubs reaches those functions; one that does aborts with "stub called: <name>".
SDL3_ROOT=${SDL3_ROOT:-$HOME/ssbu-port/deps/sdl3}
NAT_INC="-idirafter tests/port/threads/stub"
NAT_SDL_LIBS=""
NAT_SSL_LIBS=""
if [ -f "$SDL3_ROOT/include/SDL3/SDL.h" ]; then
    NAT_INC="-I$SDL3_ROOT/include $NAT_INC"
    if ls "$SDL3_ROOT"/lib/libSDL3.so* >/dev/null 2>&1; then NAT_SDL_LIBS="-L$SDL3_ROOT/lib -Wl,-rpath,$SDL3_ROOT/lib -lSDL3"; fi
fi
for _d in /usr/lib /usr/lib64 /lib /lib64 /usr/lib/aarch64-linux-gnu /usr/lib/x86_64-linux-gnu; do
    for _n in libcrypto.so.3 libcrypto.so; do
        if [ -z "$NAT_SSL_LIBS" ] && [ -e "$_d/$_n" ]; then NAT_SSL_LIBS="$_d/$_n"; fi
    done
done
nat_stubs() {
    _out=$1; shift
    _pat=""
    [ -n "$NAT_SDL_LIBS" ] || _pat="SDL"
    [ -n "$NAT_SSL_LIBS" ] || _pat="${_pat:+$_pat|}EVP"
    {
        echo 'int dprintf(int, const char *, ...); void abort(void);'
        if [ -n "$_pat" ]; then
            nm -u "$@" | sed -E -n "s/^ *(U +)?${NAT_SYM_PREFIX:-}(($_pat)_[A-Za-z0-9_]+)\$/void \\2(void) { dprintf(2, \"stub called: \\2\\\\n\"); abort(); }/p" | sort -u
        fi
    } > "$_out"
}

/* stubs_cxxrt.c - the rest of the runtime that nncxxrt.c, nncxxlib.c and nnlibc2.c reach, for test_cxxrt (no game image:
 * hostfn addresses are the host functions themselves, which stubs.c's call_guest calls directly). Build it with
 * game_tables.h forced in (-include), like the units it stands in for. Anything that needs the game aborts loudly. */
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"
#include "game_data.h"

GameData game_data;                                  /* sdklift.h reads the lifted SDK tables through it: all empty */
uint64_t main_export(const char *name) { (void)name; return 0; }
static const FnEntry no_fns[1];
static const DynSym no_syms[1];
static const int zero;
const FnEntry (*game_fn_table_p)[] = (const FnEntry (*)[])&no_fns;
const int *game_fn_table_count_p = &zero;
const DynSym (*game_dyn_syms_p)[] = (const DynSym (*)[])&no_syms;
const int *game_dyn_sym_count_p = &zero;

static __attribute__((noreturn)) void needs_game(const char *what) { fprintf(stderr, "test_cxxrt: %s needs the game (not in this test)\n", what); abort(); }
uint64_t hostfn_address(const char *name, ImportFn fn) { (void)name; return (uint64_t)(uintptr_t)fn; }
const char *hostfn_current_name(void) { return "?"; }
void import_set_address(const char *name, void *addr) { (void)name; (void)addr; }
MainLayout g_main;                                   /* hostrt.c load_layout; cxxrt_selftest.h reads it (game DLL only). ga(): stubs.c */
uint64_t import_address(int sym) { return IMPORT_BASE + (uint64_t)sym * IMPORT_SLOT; }
uint64_t guest_function_of(uint64_t host_rip) { (void)host_rip; return 0; }
/* the C++ runtime's fatal paths end here: the exit code is what the tests look at (SSBU_EH_STATE=table control) */
void report_and_exit(int code) { fflush(stdout); fprintf(stderr, "report_and_exit(%d)\n", code); _exit(code); }
void nro_catch_call(Ctx *c, void (*body)(Ctx *, uint64_t)) { (void)c; (void)body; needs_game("nro_catch_call"); }
void register_unwind_imports(void) { needs_game("register_unwind_imports"); }
void cxx_string_init(Ctx *c, uint64_t str, const char *p, size_t n) { (void)c; (void)str; (void)p; (void)n; needs_game("cxx_string_init (libcxx.c, the game allocator)"); }
const char *cxx_string_data(uint64_t str, size_t *n) { (void)str; (void)n; needs_game("cxx_string_data"); }
uint32_t fs_mount_plain(const char *name, const char *host_dir) { (void)name; (void)host_dir; return 1; }
const char *fs_mount_work(const char *name) { (void)name; return ""; }
int nnfs_handle_fd(uint64_t h) { (void)h; return -1; }
int sdk_image_ready(void) { return 0; }
ImportFn sdk_lifted(const char *name) { (void)name; return NULL; }
uint64_t sdk_symbol(const char *name) { (void)name; return 0; }
void sdk_host_override(const char *name, ImportFn fn) { (void)name; (void)fn; }
int sdk_install(const char *name) { (void)name; return 0; }

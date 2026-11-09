/* test_modules.c - how the exe finds the game library and its entry points, through plat_posix.c's Win32 module calls:
 * nncxxrt.c eh_bind_main finds the module that holds fn_table (GetModuleHandleExA with the address) and its game_eh_bind
 * (GetProcAddress), also when the table is in the executable itself; LoadLibraryA("x.dll") loads x.so (x.dylib) from the
 * exe's folder; GetModuleFileNameA names the exe and the library in Windows form. The library (dlbind_lib.c) is linked
 * as the game library is (DT_NEEDED, rpath $ORIGIN), hidden visibility except its two entry points.
 * Was tests/port/crash/test_dlbind.c of linux-arm64 (dladdr / dlopen(RTLD_NOLOAD) by hand there). */
#include "tp.h"
#include <string.h>

const int *game_table_p(void);                              /* dlbind_lib: a table inside the library */
const int exe_table[2] = {5, 6};                            /* a table inside the executable */
typedef int (*BindFn)(int);
static BindFn eh_bind(const void *table_entry, HMODULE *mod) {     /* the code of eh_bind_main */
    HMODULE dll = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)table_entry, &dll)) return NULL;
    if (mod) *mod = dll;
    return (BindFn)(void *)GetProcAddress(dll, "game_eh_bind");
}
static int ends_with(const char *s, const char *suffix) { size_t a = strlen(s), b = strlen(suffix); return a >= b && !strcmp(s + a - b, suffix); }

int main(void) {
    HMODULE lib = NULL;
    SECTION("table in the shared library: its module, game_eh_bind");
    BindFn b = eh_bind(&game_table_p()[0], &lib);
    CHECK(b != NULL && lib != NULL);
    CHECK(b && b(1) == 42);
    CHECK(GetProcAddress(lib, "game_table_stub") == NULL);               /* hidden */
    CHECK(GetProcAddress(lib, "no_such_function") == NULL);
    SECTION("table in the executable: the global scope still finds the library's export");
    HMODULE self = NULL;
    b = eh_bind(&exe_table[0], &self);
    CHECK(b != NULL && self != NULL && self != lib);
    CHECK(b && b(0) == 41);
    SECTION("LoadLibraryA / GetModuleHandleA / GetModuleFileNameA / FreeLibrary");
    HMODULE l2 = LoadLibraryA("dlbind_lib.dll");                          /* x.dll -> x.so (x.dylib), looked for next to the exe */
    CHECK(l2 != NULL);
    CHECK(l2 && GetProcAddress(l2, "game_eh_bind") == (FARPROC)(void *)b);   /* the same module as the one linked in */
    CHECK(GetModuleHandleA("dlbind_lib.dll") != NULL);
    CHECK(GetModuleHandleA("not_loaded_here.dll") == NULL);
    char path[1024] = "";
    DWORD k = GetModuleFileNameA(l2, path, sizeof path);
    CHECKF(k > 0 && k < sizeof path && (ends_with(path, "dlbind_lib.so") || ends_with(path, "dlbind_lib.dylib")), "library path '%s'", path);
    CHECK(strchr(path, '/') == NULL);                                     /* Windows form: callers cut at the last '\\' */
    k = GetModuleFileNameA(NULL, path, sizeof path);
    CHECKF(k > 0 && strstr(path, "test_modules") != NULL && strchr(path, '/') == NULL, "exe path '%s'", path);
    char small[8];
    k = GetModuleFileNameA(NULL, small, sizeof small);
    CHECKF(k <= sizeof small, "short buffer: %u", (unsigned)k);
    if (l2) CHECK(FreeLibrary(l2));
    CHECK(LoadLibraryA("no_such_library_anywhere.dll") == NULL);
    CHECK(GetProcAddress(NULL, "game_eh_bind") != NULL);                  /* NULL module: the global scope (a DT_NEEDED export) */
    return tp_done("test_modules");
}

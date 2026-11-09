/* nn::ro on the rebuilt Linux layer, end to end without game data: runtime/nnro.c is compiled INTO this test (its statics are
 * reachable), the platform layer is the real one (plat_posix.c), and the script modules are fake ones (fake_mod.c + the real
 * runtime/nro_shim.c, linked like tools/lifter/build.py links a Linux module: -fPIC -fvisibility=hidden, -shared -Wl,-Bsymbolic).
 *   - the NRR check (nnro_selftest: SHA-256 "abc" through BCryptHash, NRR registration), the NRR lock: readers vs. register / unregister
 *   - LoadModule: an unregistered hash is refused (0x1216); a module found in <exe dir>/$SSBU_NRO_BIN/<name>.so through LoadLibraryA
 *     ("x.dll" -> x.so); the image copied, relocated (RELATIVE / ABS64 / JUMP_SLOT), bss cleared, text read-only, the rest RW, DT_INIT called
 *   - two modules with the same internal symbol names, the exe exporting the same names too (-rdynamic): each module reaches its own
 *     copies; its call_indirect / nro_import reach the host through the pointers nro_bind got
 *   - UnloadModule: decommitted (the range stays reserved), DT_FINI; the SSBU_RO_RELOAD_TEST cycle; the unreserved path
 *   - dlclose + dlopen gives a fresh module; only nro_bind is exported; controls: default visibility without -Bsymbolic IS interposed by
 *     the exe (Linux), so the interposition check above can fail
 * Was tests/port/modules/test_nnro.c of linux-arm64 (WP9). Not ported: fighter_sdk/win32_posix.h's checks (see docs/STEAM_FRAME.md). */
#include "nnro.c"
#include "mt.h"
#include <dlfcn.h>
#include <limits.h>
#include <sys/stat.h>
#include <pthread.h>

#undef fn_table
#undef fn_table_count
#undef dyn_syms
#undef dyn_sym_count

/* ---------------------------------------------------------------- what the rest of the runtime would provide */
static void main_func_impl(Ctx *c) { c->x[2] = 0x1234 + c->x[3]; }
static const FnEntry main_fns[] = {{GUEST_BASE + 0x100, main_func_impl}};
static const int main_fn_count = 1;
static const DynSym main_syms[] = {{"", 0, 0}, {"main_func", 0x100, 1}, {"fake_export_fake_a", 0, 0}};
static const int main_sym_count = 3;
const FnEntry (*game_fn_table_p)[] = &main_fns;
const int *game_fn_table_count_p = &main_fn_count;
const DynSym (*game_dyn_syms_p)[] = &main_syms;
const int *game_dyn_sym_count_p = &main_sym_count;

static const NroInfo reg[] = {
    {"fake_a", 0x7200000000ULL, 0x3000, 0x2000, {0xa0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19}},
    {"fake_b", 0x7200200000ULL, 0x3000, 0x2000, {0xb0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19}},
};
GameData game_data = {.version = GAME_DATA_VERSION, .size = (int)sizeof(GameData), .nro_tab = reg, .nro_n = 2};     /* nnro.c's nro_registry is game_data.nro_tab */

static int g_exe_indirect;                    /* calls of the executable's call_indirect */
static uint64_t g_guest_called[8]; static int g_guest_n;
void call_indirect(Ctx *c, uint64_t addr) {
    g_exe_indirect++;
    if (nro_call_indirect(c, addr)) return;
    ImportFn f = fn_in_table(main_fns, main_fn_count, addr);
    if (!f) { fprintf(stderr, "call_indirect: nothing at %llx\n", (unsigned long long)addr); abort(); }
    f(c);
}
void call_guest(Ctx *c, uint64_t addr) { (void)c; if (g_guest_n < 8) g_guest_called[g_guest_n++] = addr; }
void host_import(Ctx *c, int sym) { (void)c; fprintf(stderr, "host_import %d\n", sym); abort(); }
void guest_trap(const char *what, uint64_t addr) { fprintf(stderr, "guest_trap: %s at %llx\n", what, (unsigned long long)addr); abort(); }
void guest_unimpl(uint64_t pc, uint32_t insn, const char *what) { (void)insn; fprintf(stderr, "guest_unimpl %s %llx\n", what, (unsigned long long)pc); abort(); }
void guest_svc(Ctx *c, uint32_t n) { (void)c; fprintf(stderr, "guest_svc %u\n", n); abort(); }
void stx_diag(Ctx *c, uint64_t a, uint64_t e, uint64_t s, uint64_t v, int bits) { (void)c; (void)a; (void)e; (void)s; (void)v; (void)bits; }
void nro_catch_call(Ctx *c, void (*body)(Ctx *, uint64_t)) { body(c, 0); }
int eh_register_image(uint64_t base, uint64_t size, uint64_t hdr) { (void)base; (void)size; (void)hdr; return 1; }
void eh_unregister_image(uint64_t base) { (void)base; }
uint64_t import_address(int sym) { return IMPORT_BASE + (uint64_t)sym * IMPORT_SLOT; }
void input_mark(const char *what) { (void)what; }
volatile LONG g_guest_frozen;
void guest_freeze_wait(void) {}
int guest_thread_info(int i, Ctx **c, const char **name, HANDLE *h) { (void)i; (void)c; (void)name; (void)h; return 0; }
void trace_printf(const char *fmt, ...) { (void)fmt; }
uint64_t ga(uint64_t a) { return a; }                /* hostrt.c GA(): 13.0.4 is the reference build, so the identity */
/* the rest of what nnro.c (and the skyline.c / fighter_sdk files it includes) reaches in the exe: nothing of it runs here */
void debug_watch(const uint64_t *a, int n) { (void)a; (void)n; }
ImportFn extra_import_lookup(const char *name) { (void)name; return NULL; }
uint32_t fs_mount_direct(const char *name, const char *host_dir) { (void)name; (void)host_dir; return 1; }
void (*g_plugin_report_hook)(void);
int (*g_svc_handler)(Ctx *, uint32_t);
int hostentry_hook(uint64_t addr, void (*fn)(Ctx *, void *), void *user, uint64_t *orig) { (void)addr; (void)fn; (void)user; (void)orig; return 0; }
uint64_t hostfn_address(const char *name, ImportFn fn) { (void)name; return (uint64_t)(uintptr_t)fn; }
uint64_t hostfn_closure(const char *name, void (*fn)(Ctx *, void *), void *user) { (void)name; (void)fn; (void)user; return 0; }
void hostfn_placeholders_readable(void) { }
uint64_t os_main_thread_type(void) { return 0; }
int rbn_handshake_wanted(void) { return 0; }
uint64_t rbn_match_seed(void) { return 0; }
void rbn_hash_add(const void *p, size_t n) { (void)p; (void)n; }
int rbn_match_cancelled(void) { return 0; }
void register_plugin_imports(const ImportDef *defs, int count) { (void)defs; (void)count; }
int sdk_image_ready(void) { return 0; }
uint64_t sdk_symbol(const char *name) { (void)name; return 0; }
void unwind_interp_call(Ctx *c, void (*orig)(Ctx *)) { (void)c; (void)orig; }
void unwind_set_interpreter(uint64_t ret_addr, uint64_t resume_lp) { (void)ret_addr; (void)resume_lp; }
static ImportDef g_imp[64]; static int g_imp_n;
void register_imports(const ImportDef *defs, int count) {
    for (int k = 0; k < count; k++) {
        int i = 0;
        while (i < g_imp_n && strcmp(g_imp[i].name, defs[k].name)) i++;
        if (i == g_imp_n && g_imp_n < 64) g_imp_n++;
        if (i < 64) g_imp[i] = defs[k];
    }
}
static ImportFn imp(const char *name) { for (int i = 0; i < g_imp_n; i++) if (!strcmp(g_imp[i].name, name)) return g_imp[i].fn; return NULL; }
ImportFn import_lookup(const char *name) { return imp(name); }
__attribute__((visibility("default"), noinline)) int fake_whoami(void) { return 'E'; }    /* exported (-rdynamic): what an interposed module would call */

/* ---------------------------------------------------------------- a fake NRO image (0x2000 file bytes + 0x1000 bss) */
static uint8_t *make_nro(const NroInfo *in) {
    uint8_t *p = aligned_alloc(0x1000, 0x2000);
    uint64_t a = (uint64_t)(uintptr_t)p;
    memset(p, 0, 0x2000);
    wr32(a + 4, 0x80);                                      /* MOD0 offset */
    wr32(a + 0x10, 0x304f524eu); wr32(a + 0x18, in->file_size); wr32(a + 0x30, 0x1000); wr32(a + 0x38, in->image_size - in->file_size);
    memcpy(p + 0x40, in->build_id, 20);
    wr32(a + 0x80, 0x30444f4du);                            /* "MOD0" */
    wr32(a + 0x84, 0x400 - 0x80); wr32(a + 0x88, 0x1800 - 0x80); wr32(a + 0x8c, 0x3000 - 0x80); wr32(a + 0x98, 0x1100 - 0x80);
    static const uint64_t dyn[] = {7, 0x500, 8, 48, 23, 0x560, 2, 24, 12, 0x100, 13, 0x140, 0, 0};
    memcpy(p + 0x400, dyn, sizeof dyn);
    static const uint64_t rela[] = {0x1200, 0x403, 0x180, 0x1208, (1ull << 32) | 0x101, 8};
    memcpy(p + 0x500, rela, sizeof rela);
    static const uint64_t jmp[] = {0x1210, (1ull << 32) | 0x402, 0};
    memcpy(p + 0x560, jmp, sizeof jmp);
    memset(p + 0x1800, 0xcc, 0x800);                        /* inside the bss range: LoadModule clears it */
    wr64(a + 0x1300, 0x1122334455667788ull);
    return p;
}

static uint64_t g_ret;
static void call(ImportFn f, Ctx *c, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[4] = a4; c->x[30] = 0x7100001004ull;
    f(c); g_ret = c->x[0];
}
static DWORD query(uint64_t a, DWORD *protect) {
    MEMORY_BASIC_INFORMATION mi; memset(&mi, 0, sizeof mi);
    VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi);
    if (protect) *protect = mi.Protect;
    return mi.State;
}

/* ---------------------------------------------------------------- NRR lock: readers and a writer on threads */
static uint8_t *g_lock_img; static volatile int g_lock_stop, g_lock_bad; static uint8_t g_lock_hash[32];
static void *lock_reader(void *arg) {
    (void)arg; uint8_t h[32]; long n = 0;
    while (!__atomic_load_n(&g_lock_stop, __ATOMIC_ACQUIRE)) {
        int r = nro_hash_registered((uint64_t)(uintptr_t)g_lock_img, h);
        if (r != 1 || memcmp(h, g_lock_hash, 32)) __atomic_add_fetch(&g_lock_bad, 1, __ATOMIC_RELAXED);
        n++;
    }
    return (void *)n;
}

/* ---------------------------------------------------------------- FLS (fighter_host.c's FsFiber: one block per guest fiber) */
static void *g_fls_seen[2];
static DWORD g_fls_ix; static LPVOID g_main_fiber;
static void WINAPI fls_fiber(LPVOID arg) {
    g_fls_seen[0] = FlsGetValue(g_fls_ix);                  /* a new fiber: nothing yet */
    FlsSetValue(g_fls_ix, arg);
    g_fls_seen[1] = FlsGetValue(g_fls_ix);
    SwitchToFiber(g_main_fiber);
}
static void *fls_thread(void *arg) { void *before = FlsGetValue(g_fls_ix); FlsSetValue(g_fls_ix, arg); return FlsGetValue(g_fls_ix) == arg && !before ? arg : NULL; }

static void dl_module(const char *dir, const char *name, Ctx *c, uint64_t base, int *who, uint64_t *count, void **handle) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s" TEST_SO_EXT, dir, name);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    *who = 0; *count = 0;
    if (handle) *handle = h;
    if (!h) { fprintf(stderr, "dlopen %s: %s\n", path, dlerror()); return; }
    NroBindFn bind; void *s = dlsym(h, "nro_bind"); memcpy(&bind, &s, sizeof bind);
    if (!bind) return;
    static NroModule fake; NroTables t; memset(&t, 0, sizeof t);
    bind(&g_host, &fake, &t);
    ImportFn f = fn_in_table(t.fns, t.fn_count, base + 0x100);
    if (!f) return;
    f(c); *who = (int)c->x[0]; *count = c->x[1];
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *bindir = argc > 1 ? argv[1] : NULL;          /* absolute folder of the extra .so files (fake_c, fake_def, fake_int) */
    Ctx *c = aligned_alloc(16, sizeof(Ctx));
    memset(c, 0, sizeof *c);

    SECTION("register_ro_imports: the module range is reserved (vmem), handlers registered");
    register_ro_imports();
    CHECK(g_ro_reserved == 1);
    CHECK(query(NRO_BASE, NULL) == MEM_RESERVE);
    ImportFn ro_init = imp("_ZN2nn2ro10InitializeEv"), ro_reg = imp("_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv");
    ImportFn ro_load = imp("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi"), ro_unload = imp("_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE");
    ImportFn ro_unreg = imp("_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE");
    CHECK(ro_init && ro_reg && ro_load && ro_unload && ro_unreg);
    if (!(ro_init && ro_reg && ro_load && ro_unload && ro_unreg)) return mt_done("test_nnro");
    call(ro_init, c, 0, 0, 0, 0, 0);

    SECTION("nnro_selftest: SHA-256 \"abc\" and the NRR registration rules");
    CHECK(nnro_selftest(c) == 0);

    uint8_t *img_a = make_nro(&nro_registry[0]), *img_b = make_nro(&nro_registry[1]);
    uint64_t A = nro_registry[0].base, B = nro_registry[1].base;
    uint8_t bss[16], mod_a[0x20], mod_b[0x20], info[0x20];
    #define U(p) ((uint64_t)(uintptr_t)(p))

    SECTION("LoadModule before its NRR is registered: 0x1216");
    call(ro_load, c, U(mod_a), U(img_a), U(bss), sizeof bss, 2);
    CHECK(g_ret == RO_NOT_AUTHORIZED);

    uint8_t *nrr = aligned_alloc(0x1000, 0x1000);
    memset(nrr, 0, 0x1000);
    wr32(U(nrr), 0x3052524eu); wr32(U(nrr) + 0x338, 0x1000); wr32(U(nrr) + 0x340, 0x400); wr32(U(nrr) + 0x344, 2);
    sha256(img_a, 0x2000, nrr + 0x400); sha256(img_b, 0x2000, nrr + 0x420);
    call(ro_reg, c, U(info), U(nrr), 0, 0, 0);
    CHECK(g_ret == RO_OK);

    SECTION("NRR lock (SRWLOCK over pthread_rwlock): 4 readers vs. register/unregister of a second NRR, 200 ms");
    {   g_lock_img = img_a; memcpy(g_lock_hash, nrr + 0x400, 32);
        uint8_t *nrr2 = aligned_alloc(0x1000, 0x1000), info2[0x20]; memcpy(nrr2, nrr, 0x1000);
        pthread_t th[4]; long reads = 0; int regs = 0, regbad = 0;
        Ctx *c2 = aligned_alloc(16, sizeof(Ctx)); memset(c2, 0, sizeof *c2);
        for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, lock_reader, NULL);
        for (ULONGLONG t0 = GetTickCount64(); GetTickCount64() - t0 < 200; regs++) {
            call(ro_reg, c2, U(info2), U(nrr2), 0, 0, 0); regbad += g_ret != RO_OK;
            call(ro_unreg, c2, U(info2), 0, 0, 0, 0); regbad += g_ret != RO_OK;
        }
        __atomic_store_n(&g_lock_stop, 1, __ATOMIC_RELEASE);
        for (int i = 0; i < 4; i++) { void *r; pthread_join(th[i], &r); reads += (long)r; }
        printf("    %ld lookups, %d register/unregister pairs\n", reads, regs);
        CHECK(g_lock_bad == 0 && regbad == 0 && reads > 0 && regs > 0);
        free(nrr2); free(c2); }

    SECTION("LoadModule fake_a: dlopen <exe dir>/$SSBU_NRO_BIN/fake_a.so, copy, relocate, bss, protection, DT_INIT");
    g_guest_n = 0;
    call(ro_load, c, U(mod_a), U(img_a), U(bss), sizeof bss, 2);
    CHECKF(g_ret == RO_OK, "result 0x%llx", (unsigned long long)g_ret);
    if (g_ret != RO_OK) return mt_done("test_nnro");
    NroModule *ma = module_at(A);
    CHECK(ma && ma->loaded && ma->dll && ma->fn_count == 3 && ma->sym_count == 3 && ma->resume_count == 0);
    CHECK(rd32(A + 0x10) == 0x304f524eu && rd64(A + 0x1300) == 0x1122334455667788ull);
    CHECK(rd64(A + 0x1200) == A + 0x180);                               /* R_AARCH64_RELATIVE */
    CHECK(rd64(A + 0x1208) == GUEST_BASE + 0x108);                      /* ABS64 main_func + 8 */
    CHECK(rd64(A + 0x1210) == GUEST_BASE + 0x100);                      /* JUMP_SLOT main_func */
    {   int zero = 1; for (uint64_t o = 0x1800; o < 0x3000; o++) zero &= *(uint8_t *)(uintptr_t)(A + o) == 0; CHECK(zero); }
    DWORD prot = 0;
    CHECK(query(A, &prot) == MEM_COMMIT && prot == PAGE_READONLY);
    CHECK(query(A + 0x1000, &prot) == MEM_COMMIT && prot == PAGE_READWRITE);
    CHECK(query(A + 0x2000, &prot) == MEM_COMMIT && prot == PAGE_READWRITE);
    CHECK(rd64(U(mod_a)) == A + 0x1100 && rd32(U(mod_a) + 8) == 1 && rd64(A + 0x1100) == A + 0x1100 && rd64(A + 0x1120) == A);
    CHECK(g_guest_n == 1 && g_guest_called[0] == A + 0x100);           /* DT_INIT */

    SECTION("fake_a's code: own fake_whoami (exe exports one too), call_indirect and nro_import reach the host");
    CHECK(nro_call_indirect(c, A + 0x100) == 1 && c->x[0] == 'A' && c->x[1] == 1);
    int before = g_exe_indirect;
    c->x[0] = GUEST_BASE + 0x100; c->x[2] = 0;
    CHECK(nro_call_indirect(c, A + 0x140) == 1 && c->x[2] == 0x1234 + 'A' && g_exe_indirect == before + 1);
    c->x[2] = 0;
    CHECK(nro_call_indirect(c, A + 0x180) == 1 && c->x[2] == 0x1234 + 'A' && g_exe_indirect == before + 1);
    {   ImportFn e = imp("fake_export_fake_a"); CHECK(e != NULL); if (e) { e(c); CHECK(c->x[0] == 'A' && c->x[1] == 2); } }

    SECTION("LoadModule fake_b (same internal names): both modules keep their own copies");
    call(ro_load, c, U(mod_b), U(img_b), U(bss), sizeof bss, 2);
    CHECK(g_ret == RO_OK);
    CHECK(nro_call_indirect(c, B + 0x100) == 1 && c->x[0] == 'B' && c->x[1] == 1);
    CHECK(nro_call_indirect(c, A + 0x100) == 1 && c->x[0] == 'A' && c->x[1] == 3);
    c->x[2] = 0; CHECK(nro_call_indirect(c, B + 0x180) == 1 && c->x[2] == 0x1234 + 'B');
    c->x[0] = GUEST_BASE + 0x100; c->x[2] = 0; CHECK(nro_call_indirect(c, B + 0x140) == 1 && c->x[2] == 0x1234 + 'B');
    CHECK(module_at(B) && module_at(B)->dll != ma->dll);
    CHECK(fake_whoami() == 'E');

    SECTION("a changed image is refused (hash not in the NRR)");
    {   uint8_t *bad = make_nro(&nro_registry[1]); bad[0x1300] ^= 1; uint8_t m2[0x20];
        call(ro_load, c, U(m2), U(bad), U(bss), sizeof bss, 2); CHECK(g_ret == RO_NOT_AUTHORIZED); free(bad); }

    SECTION("UnloadModule fake_a: DT_FINI, decommitted, range still reserved; Module says not loaded");
    g_guest_n = 0;
    call(ro_unload, c, U(mod_a), 0, 0, 0, 0);
    CHECK(rd32(U(mod_a) + 8) == 0 && !ma->loaded && module_at(A) == NULL);
    CHECK(g_guest_n == 1 && g_guest_called[0] == A + 0x140);
    CHECK(query(A, NULL) == MEM_RESERVE && query(A + 0x2000, NULL) == MEM_RESERVE);
    CHECK(IsBadReadPtr((void *)(uintptr_t)A, 1));

    SECTION("SSBU_RO_RELOAD_TEST: load, unload, load again (close5 test in nnro.c)");
    setenv("SSBU_RO_RELOAD_TEST", "1", 1);
    call(ro_LoadModule_entry, c, U(mod_a), U(img_a), U(bss), sizeof bss, 2);
    unsetenv("SSBU_RO_RELOAD_TEST");
    CHECK(g_ret == RO_OK && g_reload_ok == 1 && g_reload_bad == 0);
    CHECK(ma->loaded && rd64(A + 0x1200) == A + 0x180 && query(A, &prot) == MEM_COMMIT && prot == PAGE_READONLY);

    SECTION("without the start-up reservation: reserve+commit per module, MEM_RELEASE without a size");
    call(ro_unload, c, U(mod_a), 0, 0, 0, 0);
    call(ro_unload, c, U(mod_b), 0, 0, 0, 0);
    CHECK(VirtualFree((void *)(uintptr_t)NRO_BASE, 0, MEM_RELEASE));
    CHECK(query(NRO_BASE, NULL) == MEM_FREE);
    g_ro_reserved = 0;
    call(ro_load, c, U(mod_a), U(img_a), U(bss), sizeof bss, 2);
    CHECK(g_ret == RO_OK && query(A, &prot) == MEM_COMMIT && prot == PAGE_READONLY && rd64(A + 0x1208) == GUEST_BASE + 0x108);
    {   MEMORY_BASIC_INFORMATION mi; VirtualQuery((void *)(uintptr_t)(A + 0x2000), &mi, sizeof mi); CHECK(mi.AllocationBase == (void *)(uintptr_t)A); }
    call(ro_unload, c, U(mod_a), 0, 0, 0, 0);
    CHECK(query(A, NULL) == MEM_FREE && query(A + 0x2000, NULL) == MEM_FREE);
    call(ro_load, c, U(mod_a), U(img_a), U(bss), sizeof bss, 2);
    CHECK(g_ret == RO_OK && nro_call_indirect(c, A + 0x100) == 1 && c->x[0] == 'A');
    call(ro_unload, c, U(mod_a), 0, 0, 0, 0);

    if (bindir) {
        SECTION("dlclose + dlopen: a fresh module (static state back to zero); only nro_bind exported");
        int who; uint64_t count; void *h;
        dl_module(bindir, "fake_c", c, 0x7200400000ULL, &who, &count, &h);
        CHECK(h && who == 'C' && count == 1);
        if (h) {
            CHECK(dlsym(h, "nro_bind") != NULL);
            CHECK(dlsym(h, "call_indirect") == NULL && dlsym(h, "nro_import") == NULL && dlsym(h, "fake_whoami") == NULL);
            CHECK(dlsym(h, "fn_table_fake_c") == NULL && dlsym(h, "nromod_fake_c") == NULL);
            NroBindFn bind; void *s = dlsym(h, "nro_bind"); memcpy(&bind, &s, sizeof bind);
            static NroModule fake; NroTables t; memset(&t, 0, sizeof t); bind(&g_host, &fake, &t);
            fn_in_table(t.fns, t.fn_count, 0x7200400100ULL)(c);
            CHECK(c->x[1] == 2);                                                 /* state of THIS load */
            CHECK(dlclose(h) == 0);
            char p[PATH_MAX]; snprintf(p, sizeof p, "%s/fake_c" TEST_SO_EXT, bindir);
            CHECK(dlopen(p, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD) == NULL);         /* really unloaded */
        }
        dl_module(bindir, "fake_c", c, 0x7200400000ULL, &who, &count, &h);
        CHECK(h && who == 'C' && count == 1);
        if (h) dlclose(h);

#ifdef __linux__                                  /* ELF symbol interposition; macOS binds by library (two-level namespace) */
        SECTION("controls: default visibility + -Bsymbolic binds to itself; default visibility alone is interposed by the exe");
        dl_module(bindir, "fake_def", c, 0x7200600000ULL, &who, &count, NULL);
        CHECKF(who == 'D', "fake_def reached '%c'", who);
        dl_module(bindir, "fake_int", c, 0x7200800000ULL, &who, &count, NULL);
        CHECKF(who == 'E', "fake_int reached '%c' (expected the executable's copy: the check above would not catch interposition)", who);
#endif
    }

    SECTION("FLS: per fiber and per thread");
    {   g_fls_ix = FlsAlloc(NULL);
        CHECK(g_fls_ix != FLS_OUT_OF_INDEXES && FlsGetValue(g_fls_ix) == NULL);
        CHECK(FlsSetValue(g_fls_ix, (void *)0x11) && FlsGetValue(g_fls_ix) == (void *)0x11);       /* plain thread */
        g_main_fiber = ConvertThreadToFiber(NULL);
        CHECK(g_main_fiber != NULL);
        CHECK(FlsSetValue(g_fls_ix, (void *)0x22) && FlsGetValue(g_fls_ix) == (void *)0x22);       /* now a fiber: its own slot */
        LPVOID fb = CreateFiber(1 << 16, fls_fiber, (void *)0x33);
        SwitchToFiber(fb);
        CHECK(g_fls_seen[0] == NULL && g_fls_seen[1] == (void *)0x33);
        CHECK(FlsGetValue(g_fls_ix) == (void *)0x22);
        DeleteFiber(fb);
        pthread_t t; void *r = NULL; pthread_create(&t, NULL, fls_thread, (void *)0x44); pthread_join(t, &r);
        CHECK(r == (void *)0x44 && FlsGetValue(g_fls_ix) == (void *)0x22); }

    free(img_a); free(img_b); free(nrr); free(c);
    return mt_done("test_nnro");
}

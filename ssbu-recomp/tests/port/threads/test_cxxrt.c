/* test_cxxrt.c - runtime/nncxxrt.c on Linux, built into this test (it #includes the file, so its statics and its
 * self-test, cxxrt_selftest.h, are reachable) with nncxxlib.c, nnlibc2.c, clib.c, os_threads.c and plat:
 *   - the exception state uses fiber-local storage on Linux too (eh_state mode 1, not the 4,096-entry table);
 *   - cxxrt_selftest.h: every section that needs no game image (binary128, sscanf/snprintf, gmtime/localeconv/
 *     random_device, guard variables under 12 threads, exception objects and vtables, facets); the sections that read
 *     main's tables, RTTI, data or code (personality vectors, the throw / catch flow at main's call sites, __dynamic_cast,
 *     promise / future's future_error vtable, the unwinder, mode 2) need the game and are left to SSBU_CXXRT_SELFTEST
 *     in the game (WP11);
 *   - the throw state per guest fiber: two nn::os fibers throw on the SAME Ctx (the table, keyed by Ctx, would mix
 *     them), each is resumed by another nn::os thread with its exception still in flight or caught and finishes it
 *     there; the threads' own state stays apart; FinalizeFiber frees the fiber's state;
 *   - soak: 10,500 nn::os fibers and 10,500 host threads (distinct Ctx each) throw and catch; the value sets come back;
 *   - control: the same with SSBU_EH_STATE=table's table stops at 4,096 contexts (report_and_exit(3), in a child).
 * "Guest" functions are host functions void f(Ctx *) (stubs.c); stubs_cxxrt.c has the rest.
 * Was tests/port/threads/test_cxxrt.c of linux-arm64 (WP11); on the rebuilt layer the exception state sits on plat_posix.c's
 * FLS (C3b) and the file is upstream's nncxxrt.c. */
#include "nncxxrt.c"
#undef CHECK
#include <setjmp.h>
#include <malloc.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

ImportFn tst_import(const char *name);
size_t plat_fls_sets(void);                          /* plat_posix.c: live FLS value sets */
static int t_fail, t_checks;
#define TCHECK(cond, ...) do { t_checks++; if (!(cond)) { t_fail++; printf("  FAILED %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define SECTION(s) do { printf("  - %s\n", s); fflush(stdout); } while (0)
#define OS(n) "_ZN2nn2os" n
#define I_CREATE OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi")
#define I_START OS("11StartThreadEPNS0_10ThreadTypeE")
#define I_WAIT OS("10WaitThreadEPNS0_10ThreadTypeE")
#define I_DESTROY OS("13DestroyThreadEPNS0_10ThreadTypeE")
#define I_FIB_INIT OS("15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi")
#define I_FIB_FINI OS("13FinalizeFiberEPNS0_9FiberTypeE")
#define I_FIB_SWITCH OS("13SwitchToFiberEPNS0_9FiberTypeE")
static uint64_t imp(const char *name, Ctx *c, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[4] = a4; c->x[5] = a5;
    tst_import(name)(c);
    return c->x[0];
}
#ifdef __linux__
static pid_t tid_now(void) { return (pid_t)syscall(SYS_gettid); }
#else
static pid_t tid_now(void) { return (pid_t)GetCurrentThreadId(); }
#endif
static uint8_t g_gstack[0x10000];
static void run_guest_thread(uint8_t *tt, void (*fn)(Ctx *), uint64_t arg) {
    Ctx c;
    memset(&c, 0, sizeof c);
    imp(I_CREATE, &c, P64(tt), P64(fn), arg, P64(g_gstack), sizeof g_gstack, 16);
    imp(I_START, &c, P64(tt), 0, 0, 0, 0, 0);
    imp(I_WAIT, &c, P64(tt), 0, 0, 0, 0, 0);
    imp(I_DESTROY, &c, P64(tt), 0, 0, 0, 0, 0);
}

/* ---- throw / catch without the game: the unwinder hook jumps back to the thrower, the "landing pad" is made up */
static jmp_buf *g_raise_to;                         /* one fiber runs at a time in these tests */
static void raise_hook(Ctx *c, int reason) { (void)c; (void)reason; longjmp(*g_raise_to, 1); }
static int g_exc_dtors;
static void tst_exc_dtor(Ctx *c) { (void)c; __atomic_add_fetch(&g_exc_dtors, 1, __ATOMIC_RELAXED); }
static uint64_t throw_on(Ctx *c) {                  /* throw std::out_of_range-typed object; returns it, in flight */
    uint64_t e = st_import(c, "__cxa_allocate_exception", 16, 0, 0, 0);
    jmp_buf jb;
    g_raise_to = &jb;
    if (setjmp(jb) == 0) { st_import(c, "__cxa_throw", e, P64(g_ti_out_of_range), hostfn_address("test exception destructor", tst_exc_dtor), 0); return 0; }
    return e;
}
static int begin_catch(Ctx *c, uint64_t e) {        /* a handler for e is entered: returns 1 if __cxa_begin_catch gave e */
    EhPad pad;
    memset(&pad, 0, sizeof pad);
    pad.kind = EH_HANDLER; pad.selector = 1; pad.exception = e; pad.adjusted = e;
    eh_enter_pad(c, &pad);
    return st_import(c, "__cxa_begin_catch", c->x[0], 0, 0, 0) == e;
}
static void end_catch(Ctx *c) { st_import(c, "__cxa_end_catch", 0, 0, 0, 0); }

void register_clib_imports(void);
static void locale_dtor(Ctx *c) { (void)c; }        /* libcxx.c's (the game's locale): st_facets' test stream holds no locale */
static const ImportDef g_test_imports[] = {{"_ZNSt3__16localeD1Ev", locale_dtor}};
static void setup(void) {
    cxxrt_install_data();
    register_clib_imports();                         /* sscanf / snprintf of the self-test (clib.c) */
    tst_import(I_FIB_INIT);                          /* stubs.c registers os_threads.c's handlers (std::mutex of promise / future too) */
    register_imports(cxxrt_replace, (int)(sizeof cxxrt_replace / sizeof cxxrt_replace[0]));
    for (int i = 0; i < (int)(sizeof cxxrt_fill / sizeof cxxrt_fill[0]); i++)
        if (!import_lookup(cxxrt_fill[i].name)) register_imports(&cxxrt_fill[i], 1);
    register_cxxlib_imports();
    register_libc2_imports();
    register_imports(g_test_imports, 1);
    eh_set_unwinder(raise_hook);
}

/* ---------------------------------------------------------------- control: the old table */
static void test_table_control(void) {
    SECTION("control: SSBU_EH_STATE=table stops at 4,096 contexts (child process)");
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        g_eh_mode = 2;
        for (int i = 0; i < 5000; i++) { Ctx *c = calloc(1, sizeof *c); eh_in_flight(c); }
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    TCHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3, "child status %x", st);
}

/* ---------------------------------------------------------------- cxxrt_selftest.h */
static DWORD WINAPI selftest_thread(LPVOID p) {
    (void)p;
    printf("binary128 soft-float\n"); st_quad();
    printf("sscanf / printf\n"); st_scanf();
    printf("gmtime, localeconv, random_device\n"); st_libc();
    printf("guard variables\n"); st_guard();
    printf("exception classes and vtables\n"); st_exceptions();
    printf("locale facets and stream vtables\n"); st_facets();
    return 0;
}
static void test_selftest(void) {
    SECTION("cxxrt_selftest.h: the sections that need no game image");
    HANDLE h = CreateThread(NULL, (SIZE_T)64 << 20, selftest_thread, NULL, 0, NULL);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    printf("    self-test: %d checks, %d failed\n", st_checks, st_fail);
    TCHECK(st_fail == 0 && st_checks >= 60, "%d of %d failed", st_fail, st_checks);
}

/* ---------------------------------------------------------------- the throw state per guest fiber */
static uint8_t g_ef[2][0x100], g_ett[2][0x200];
static Ctx *g_S;                                    /* the context both fibers throw on */
static uint64_t g_e[2], g_eA;
static int g_ebad;
static pid_t g_etid[2][3];
static void eh_step(int i, int step) {
    Ctx *S = g_S;
    g_etid[i][step] = tid_now();
    switch (i * 10 + step) {
    case 0:  /* fiber 0 on thread A */
        if (eh_in_flight(S)) g_ebad |= 1;                                 /* not thread A's exception */
        g_e[0] = throw_on(S);
        if (!g_e[0] || eh_in_flight(S) != g_e[0]) g_ebad |= 2;
        break;
    case 1:  /* fiber 0 on thread B: its exception came along; caught and ended here */
        if (eh_in_flight(S) != g_e[0]) g_ebad |= 4;
        if (!begin_catch(S, g_e[0])) g_ebad |= 8;
        end_catch(S);
        if (eh_in_flight(S) || eh_state(S)->caught || eh_state(S)->uncaught) g_ebad |= 16;
        break;
    case 10: /* fiber 1 on thread A */
        if (eh_in_flight(S)) g_ebad |= 32;
        g_e[1] = throw_on(S);
        if (!g_e[1] || eh_in_flight(S) != g_e[1]) g_ebad |= 64;
        break;
    case 11: /* fiber 1 on thread B: caught here, kept caught across a switch */
        if (eh_in_flight(S) != g_e[1] || !begin_catch(S, g_e[1]) || eh_state(S)->caught != g_e[1]) g_ebad |= 128;
        break;
    case 12: /* fiber 1 on thread B again */
        if (eh_state(S)->caught != g_e[1]) g_ebad |= 256;
        end_catch(S);
        if (eh_state(S)->caught) g_ebad |= 512;
        break;
    default: g_ebad |= 1 << 30;
    }
}
static void eh_fiber(Ctx *c) {
    int i = (int)c->x[0];
    for (int step = 0;; step++) {
        eh_step(i, step);
        imp(I_FIB_SWITCH, c, 0, 0, 0, 0, 0, 0);
    }
}
static int g_tbad;
static size_t sets_settled(void) {                  /* nn::os threads free their sets after WaitThread returns (key destructor) */
    size_t a;
    do { a = plat_fls_sets(); Sleep(20); } while (plat_fls_sets() != a);
    return a;
}
static void eh_thread_a(Ctx *c) {
    for (int i = 0; i < 2; i++) imp(I_FIB_INIT, c, P64(g_ef[i]), P64(eh_fiber), (uint64_t)i, P64(g_gstack), sizeof g_gstack, 0);
    g_eA = throw_on(g_S);                                                 /* thread A's own exception, in flight on S */
    imp(I_FIB_SWITCH, c, P64(g_ef[0]), 0, 0, 0, 0, 0);
    if (eh_in_flight(g_S) != g_eA) g_tbad |= 1;
    imp(I_FIB_SWITCH, c, P64(g_ef[1]), 0, 0, 0, 0, 0);
    if (eh_in_flight(g_S) != g_eA || !begin_catch(g_S, g_eA)) g_tbad |= 2;
    end_catch(g_S);
}
static void eh_thread_b(Ctx *c) {
    if (eh_in_flight(g_S) || eh_state(g_S)->caught) g_tbad |= 4;
    imp(I_FIB_SWITCH, c, P64(g_ef[0]), 0, 0, 0, 0, 0);
    imp(I_FIB_SWITCH, c, P64(g_ef[1]), 0, 0, 0, 0, 0);
    if (eh_state(g_S)->caught) g_tbad |= 8;                               /* fiber 1's catch is not B's */
    imp(I_FIB_SWITCH, c, P64(g_ef[1]), 0, 0, 0, 0, 0);
}
static void test_eh_fibers(void) {
    SECTION("throw state per nn::os fiber: two fibers on one Ctx, each finished on another thread; the threads' own apart");
    g_S = calloc(1, sizeof *g_S);
    int dtors0 = g_exc_dtors;
    run_guest_thread(g_ett[0], eh_thread_a, 0);
    run_guest_thread(g_ett[1], eh_thread_b, 1);
    TCHECK(g_eh_mode == 1 && g_eh_fls != FLS_OUT_OF_INDEXES, "mode %d", (int)g_eh_mode);   /* fiber-local, not the table */
    TCHECK(g_ebad == 0 && g_tbad == 0, "fibers %x threads %x", g_ebad, g_tbad);
    TCHECK(g_etid[0][0] != g_etid[0][1] && g_etid[1][0] != g_etid[1][1] && g_etid[1][1] == g_etid[1][2], "the fibers did not move as planned");
    TCHECK(g_exc_dtors - dtors0 == 3, "%d exception destructors", g_exc_dtors - dtors0);
    size_t sets = sets_settled();
    Ctx c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < 2; i++) imp(I_FIB_FINI, &c, P64(g_ef[i]), 0, 0, 0, 0, 0);
    TCHECK(plat_fls_sets() == sets - 2, "%zu -> %zu", sets, plat_fls_sets());   /* their EhFiber lists went (eh_fls_free) */
}

/* ---------------------------------------------------------------- soak */
#define SOAK 10500
static uint8_t g_sf[0x100];
static int g_sbad;
static void soak_fiber(Ctx *c) {                     /* c: this fiber's own context (os_threads.c makes one per fiber) */
    uint64_t e = throw_on(c);
    if (!e || eh_in_flight(c) != e || !begin_catch(c, e)) g_sbad |= 1;
    end_catch(c);
    c->x[0] = 0;
}
static DWORD WINAPI soak_driver(LPVOID p) {
    (void)p;
    Ctx c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < SOAK; i++) {
        imp(I_FIB_INIT, &c, P64(g_sf), P64(soak_fiber), 0, P64(g_gstack), sizeof g_gstack, 0);
        imp(I_FIB_SWITCH, &c, P64(g_sf), 0, 0, 0, 0, 0);
        imp(I_FIB_FINI, &c, P64(g_sf), 0, 0, 0, 0, 0);
    }
    return 0;
}
static Ctx *g_tctx[SOAK];
static DWORD WINAPI soak_thread(LPVOID p) {
    Ctx *c = p;
    uint64_t e = throw_on(c);
    if (!e || eh_in_flight(c) != e || !begin_catch(c, e)) g_sbad |= 2;
    end_catch(c);
    return 0;
}
static void test_soak(void) {
    SECTION("soak: 10,500 nn::os fibers and 10,500 host threads each throw and catch on their own context");
    size_t sets0 = sets_settled();
    int dtors0 = g_exc_dtors;
    int64_t t0 = (int64_t)GetTickCount64();
    HANDLE h = CreateThread(NULL, 0, soak_driver, NULL, 0, NULL);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    for (int i = 0; i < SOAK; i++) {
        g_tctx[i] = calloc(1, sizeof(Ctx));                               /* all alive at once: 10,500 distinct contexts */
        h = CreateThread(NULL, 1 << 16, soak_thread, g_tctx[i], 0, NULL);
        if (!h) { TCHECK(0, "CreateThread %d", i); break; }
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    for (int i = 0; i < SOAK; i++) free(g_tctx[i]);
    TCHECK(g_sbad == 0, "mask %x", g_sbad);
    TCHECK(g_exc_dtors - dtors0 == 2 * SOAK, "%d destructors", g_exc_dtors - dtors0);
    TCHECK(plat_fls_sets() == sets0, "%zu -> %zu", sets0, plat_fls_sets());
    printf("    %.1f s; eh_state still fiber-local (mode %d)\n", ((int64_t)GetTickCount64() - t0) / 1000.0, (int)g_eh_mode);
}

/* ---------------------------------------------------------------- cxxrt_readable (the unwinder's and RTTI's "may I read this") */
static void test_readable(void) {
    SECTION("cxxrt_readable: reserved / committed / read-only / PROT_NONE / decommitted / released / host memory / the null page; the per-thread record follows cxxrt_mem_changed");
    DWORD old;
    uint8_t *r = VirtualAlloc(NULL, 0x40000, MEM_RESERVE, PAGE_NOACCESS);
    TCHECK(r != NULL, "VirtualAlloc");
    if (!r) return;
    cxxrt_guest_region(P64(r), 0x40000);                    /* memory the game manages: answers are remembered per thread */
    TCHECK(!cxxrt_readable(P64(r), 8), "reserved only");
    TCHECK(VirtualAlloc(r + 0x10000, 0x20000, MEM_COMMIT, PAGE_READWRITE) == r + 0x10000, "commit");
    TCHECK(cxxrt_readable(P64(r + 0x10000), 0x20000), "committed run");
    TCHECK(cxxrt_readable(P64(r + 0x18000), 8) && cxxrt_readable(P64(r + 0x1ffff), 1), "inside the run, from the record");
    TCHECK(!cxxrt_readable(P64(r + 0x10000), 0x20001), "runs past the end");
    TCHECK(!cxxrt_readable(P64(r + 0x10000 - 4), 8), "straddles into the reserved part");
    memset(r + 0x10000, 1, 0x20000);
    /* a page loses its access: only the run's part that the change touches goes */
    cxxrt_mem_changed(P64(r + 0x28000), 0x1000);
    TCHECK(VirtualProtect(r + 0x28000, 0x1000, PAGE_NOACCESS, &old), "VirtualProtect");
    cxxrt_mem_changed(P64(r + 0x28000), 0x1000);
    TCHECK(!cxxrt_readable(P64(r + 0x28000), 8), "PROT_NONE page");
    TCHECK(!cxxrt_readable(P64(r + 0x27ffc), 8), "straddles into the PROT_NONE page");
    TCHECK(cxxrt_readable(P64(r + 0x10000), 0x18000) && cxxrt_readable(P64(r + 0x29000), 0x7000), "the parts on both sides stay readable");
    TCHECK(VirtualProtect(r + 0x28000, 0x1000, PAGE_READONLY, &old), "VirtualProtect read-only");
    cxxrt_mem_changed(P64(r + 0x28000), 0x1000);
    TCHECK(cxxrt_readable(P64(r + 0x28000), 8), "read-only is readable");
    /* decommit and release */
    cxxrt_mem_changed(P64(r + 0x20000), 0x10000);
    TCHECK(VirtualFree(r + 0x20000, 0x10000, MEM_DECOMMIT), "decommit");
    cxxrt_mem_changed(P64(r + 0x20000), 0x10000);
    TCHECK(!cxxrt_readable(P64(r + 0x20000), 8) && !cxxrt_readable(P64(r + 0x2fff8), 8), "decommitted = reserved");
    TCHECK(cxxrt_readable(P64(r + 0x10000), 0x10000), "the part below stays readable");
    cxxrt_mem_changed(P64(r), 0);                          /* a release: the whole region */
    TCHECK(VirtualFree(r, 0, MEM_RELEASE), "release");
    cxxrt_mem_changed(P64(r), 0);
    TCHECK(!cxxrt_readable(P64(r + 0x10000), 8) && !cxxrt_readable(P64(r), 8), "released");
    /* host memory is asked every time */
    volatile uint64_t local = 1;
    char *h = malloc(1000);
    TCHECK(cxxrt_readable(P64(&local), 8) && cxxrt_readable(P64(h), 1000), "stack and heap");
    free(h);
    TCHECK(!cxxrt_readable(0, 8) && !cxxrt_readable(0x8000, 8) && !cxxrt_readable(0xff00, 1), "the null page (below 0x10000) is never readable");
    TCHECK(!cxxrt_readable(0xfffffffffffffff0ull, 0x20), "wrap-around");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setup();
    test_table_control();
    test_selftest();
    test_eh_fibers();
    test_soak();
    test_readable();
    printf("%-14s %s (%d checks, %d failed)\n", "test_cxxrt", t_fail ? "FAIL" : "PASS", t_checks, t_fail);
    return t_fail ? 1 : 0;
}

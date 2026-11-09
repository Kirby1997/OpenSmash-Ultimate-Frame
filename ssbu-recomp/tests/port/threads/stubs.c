/* stubs.c - what runtime/os_threads.c needs from the rest of the runtime, for tests/port/threads/test_os_fibers.c.
 * "Guest" addresses are host functions void f(Ctx *) here: call_guest calls them directly. Every import table passed to
 * register_imports is kept (a later registration of a name wins, as in hostrt.c), so the test calls the nn::os handlers
 * by their mangled names (tst_import). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "hostrt.h"

void call_guest(Ctx *c, uint64_t addr) { ((void (*)(Ctx *))(uintptr_t)addr)(c); }
void guest_trap(const char *what, uint64_t addr) { fprintf(stderr, "guest_trap: %s (at %llx)\n", what, (unsigned long long)addr); abort(); }
void hitch_cur_ctx(Ctx *c) { (void)c; }
long long nvn_front_frame_next(void) { return 0; }
void trace_printf(const char *fmt, ...) { (void)fmt; }
uint64_t ga(uint64_t a) { return a; }
int display_l1_trace(void) { return 0; }
double display_l1_ms(void) { return 0; }
void display_perf_thread(void) { }
unsigned long nvn_front_present_tid(void) { return 0; }
static struct { const ImportDef *defs; int count; } g_tables[256];
static int g_ntables;
void register_imports(const ImportDef *defs, int count) {
    if (g_ntables == 256) { fprintf(stderr, "stubs.c: too many import tables\n"); abort(); }
    g_tables[g_ntables].defs = defs; g_tables[g_ntables].count = count; g_ntables++;
}
void register_thread_imports(void);
ImportFn tst_import(const char *name) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, register_thread_imports);
    for (int t = g_ntables - 1; t >= 0; t--)
        for (int i = 0; i < g_tables[t].count; i++) if (!strcmp(g_tables[t].defs[i].name, name)) return g_tables[t].defs[i].fn;
    fprintf(stderr, "no import %s\n", name);
    abort();
}

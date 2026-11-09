/* C-spike: a stand-in for the game library (ssbu_game_eh_o1.so) with no game code and no dump-derived data, so the host exe
 * links and its modes that run before runtime_init() (SSBU_PAD_SELFTEST, --pad-test) can be run without a dump.
 * One dummy function in the table (runtime_init sorts and prints it), no imports, an empty GameData. */
#include <string.h>
#include "runtime.h"
#include "game_data.h"

typedef struct { void *f[6]; } GameHost;
typedef struct { const FnEntry *table; const int *table_count; const DynSym *syms; const int *sym_count; } GameTables;

static void f_dummy(Ctx *c) { (void)c; }
static const FnEntry tab[1] = {{0x7100000000ULL, f_dummy}};
static const int tab_n = 1;
static const DynSym syms[1] = {{"", 0, 0}};
static const int syms_n = 1;

__attribute__((visibility("default"))) void game_bind(const GameHost *h, GameTables *t) {
    (void)h;
    t->table = tab; t->table_count = &tab_n; t->syms = syms; t->sym_count = &syms_n;
}
__attribute__((visibility("default"))) void game_data_bind(const GameDataHost *h, GameData *d) {
    (void)h;
    memset(d, 0, sizeof *d);
    d->version = GAME_DATA_VERSION;
    d->size = (int)sizeof *d;
}

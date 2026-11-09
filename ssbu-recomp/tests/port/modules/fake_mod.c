/* A fake lifted NRO module, linked with the real runtime/nro_shim.c into <name>.so the way build_nro.py does it
 * (-fPIC -fvisibility=hidden -fno-semantic-interposition, -shared -Wl,-Bsymbolic). Built twice (fake_a, fake_b): both
 * define the same internal names - call_indirect, host_import, guest_trap, nro_import ... (nro_shim.c) and fake_whoami
 * (fake_who.c) - as every real module does. Compile with -DNRO_MODULE=<name> -DWHO=<char> -DMOD_BASE=<guest base>.
 * The guest image (fake NRO) that test_nnro.c builds has: 0x100 f_100, 0x140 f_140, 0x180 f_180 (text); export
 * "fake_export_<WHO>" = 0x100; imports "main_func". */
#include "runtime.h"
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define STR2(x) #x
#define STR(x) STR2(x)

int fake_whoami(void);                       /* fake_who.c: a separate object, so the call goes through the PLT unless bound locally */
struct NroModule;
extern struct NroModule CAT(nromod_, NRO_MODULE);
void nro_import(Ctx *c, struct NroModule *m, int sym);

static uint64_t g_calls;                     /* module state: starts again at 0 after dlclose + dlopen */
static void f_100(Ctx *c) { c->x[0] = (uint64_t)fake_whoami(); c->x[1] = ++g_calls; }
static void f_140(Ctx *c) { c->x[3] = WHO; call_indirect(c, c->x[0]); }               /* an indirect call: shim -> host */
static void f_180(Ctx *c) { c->x[3] = WHO; nro_import(c, &CAT(nromod_, NRO_MODULE), 1); }   /* a PLT stub of import #1 */

const FnEntry CAT(fn_table_, NRO_MODULE)[] = {{MOD_BASE + 0x100, f_100}, {MOD_BASE + 0x140, f_140}, {MOD_BASE + 0x180, f_180}};
const int CAT(fn_table_count_, NRO_MODULE) = 3;
const DynSym CAT(dyn_syms_, NRO_MODULE)[] = {{"", 0, 0}, {"main_func", 0, 0}, {"fake_export_" STR(NRO_MODULE), 0x100, 1}};
const int CAT(dyn_sym_count_, NRO_MODULE) = 3;
typedef struct { uint64_t lp; void (*fn)(Ctx *, uint64_t); } NroResume;
const NroResume CAT(resume_table_, NRO_MODULE)[1] = {{0, 0}};
const int CAT(resume_count_, NRO_MODULE) = 0;

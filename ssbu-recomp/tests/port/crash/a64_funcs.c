/* a64_funcs.c - handlers of the shapes hostrt.c's fake audit classifies, compiled by the compiler under test (built
 * like the runtime: -O1 -fno-omit-frame-pointer, in its own file so nothing is inlined into the test). */
#include <stdint.h>
typedef struct { uint64_t x[32]; uint64_t pc, fn; struct { double d[2]; } v[32]; uint32_t w; uint8_t b; } TCtx;
void f_empty(TCtx *c) { (void)c; }
void f_ret0(TCtx *c) { c->x[0] = 0; }
void f_ret1(TCtx *c) { c->x[0] = 1; }
void f_ret_result(TCtx *c) { c->x[0] = 0x1015; }
void f_ret_neg(TCtx *c) { c->x[0] = (uint64_t)-1; }
void f_ret_big(TCtx *c) { c->x[0] = 0x123456789abcULL; }
void f_ret_u32(TCtx *c) { c->x[0] = 0xC0000000u; }
void f_ret_mask(TCtx *c) { c->x[0] = 0xff00ff00ff00ff00ULL; }
void f_many(TCtx *c) { c->x[0] = 0; c->x[1] = 0; c->x[5] = 7; c->w = 0; c->b = 1; c->v[0].d[0] = 0; c->v[0].d[1] = 0; }
void f_vec(TCtx *c) { for (int i = 0; i < 4; i++) c->v[i].d[0] = c->v[i].d[1] = 0; }
void f_real(TCtx *c) { c->x[0] = c->x[1] + 1; }
void f_cond(TCtx *c) { c->x[0] = c->x[1] ? 1 : 2; }
extern void a64_ext(TCtx *c);
void f_call(TCtx *c) { a64_ext(c); c->x[0] = 0; }
void a64_ext(TCtx *c) { c->x[3] = c->x[4]; }

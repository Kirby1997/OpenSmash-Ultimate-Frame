/* test_a64.c - runtime/a64_trivial.h (hostrt.c audit_trivial on Linux aarch64): hand-assembled A64 sequences (encodings
 * from llvm-mc) and the same shapes compiled by the compiler under test (a64_funcs.c, built like the runtime: -O1
 * -fno-omit-frame-pointer, in its own file so nothing is inlined into the test). Pure: it runs on any arm64 host, a Mac too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "a64_trivial.h"

static int tp_fail, tp_checks;
#define CHECK(c) do { tp_checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); tp_fail++; } } while (0)
#define CHECKF(c, ...) do { tp_checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, #c); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); tp_fail++; } } while (0)
#define SECTION(s) do { printf("  - %s\n", s); fflush(stdout); } while (0)
static int tp_done(const char *name) { printf("%-14s %s (%d checks, %d failed)\n", name, tp_fail ? "FAIL" : "PASS", tp_checks, tp_fail); return tp_fail ? 1 : 0; }

typedef struct { const char *name; uint32_t code[16]; int kind; long long x0; int sets; } Case;
#define RET 0xd65f03c0u
static const Case cases[] = {
    {"ret", {RET}, 1, 0, 0},
    {"retaa", {0xd65f0bffu}, 1, 0, 0},
    {"bti c; nop; ret", {0xd503245fu, 0xd503201fu, RET}, 1, 0, 0},
    {"str xzr,[x0]", {0xf900001fu, RET}, 2, 0, 1},
    {"mov w8,#1; str x8,[x0]", {0x52800028u, 0xf9000008u, RET}, 2, 1, 1},
    {"movz/movk x8 = 0x123456789abc", {0xd2935788u, 0xf2aacf08u, 0xf2c24688u, 0xf9000008u, RET}, 2, 0x123456789abcLL, 1},
    {"movn x8,#0 (-1)", {0x92800008u, 0xf9000008u, RET}, 2, -1, 1},
    {"movz w8,#0xc000,lsl16 (zero-extends)", {0x52b80008u, 0xf9000008u, RET}, 2, 0xC0000000LL, 1},
    {"orr w8,wzr,#0x55555555", {0x3200f3e8u, 0xf9000008u, RET}, 2, 0x55555555LL, 1},
    {"orr x8,xzr,#0xff00ff00ff00ff00", {0xb2089fe8u, 0xf9000008u, RET}, 2, (long long)0xff00ff00ff00ff00ULL, 1},
    {"mov x9,x8 (known)", {0x52800028u, 0xaa0803e9u, 0xf9000009u, RET}, 2, 1, 1},
    {"mov w9,wzr", {0x2a1f03e9u, 0xf9000009u, RET}, 2, 0, 1},
    {"str w at offset 0 (32-bit value)", {0x52b80008u, 0xb9000008u, RET}, 2, 0xC0000000LL, 1},
    {"stp w8,w9,[x0] (x0 = w9:w8)", {0x52800028u, 0x2a1f03e9u, 0x29002408u, RET}, 2, 1, 1},
    {"many stores, x0 = 0", {0x528000e8u, 0xa9007c1fu, 0xf9001408u, 0xb903201fu, 0x390c9008u, 0x79064c08u, 0xf81f8008u, 0xb8003008u, RET}, 2, 0, 1},
    {"stores elsewhere only", {0x528000e8u, 0xf9001408u, 0xb903201fu, RET}, 2, 0, 0},
    {"movi v0.2d,#0; stp q0,q0; str q0", {0x6f00e400u, 0xad080000u, 0x3d804800u, RET}, 2, 0, 0},
    {"movi v0.2d,#0; str q0,[x0]", {0x6f00e400u, 0x3d800000u, RET}, 2, 0, 1},
    {"movi .16b/.4s #0, stur q1", {0x4f00e401u, 0x4f000402u, 0x3c9f0001u, RET}, 2, 0, 0},
    {"fmov d0,xzr / s5,wzr / d6,#1.0; str d/s; stp d", {0x9e6703e0u, 0x1e2703e5u, 0x1e6e1006u, 0xfd008800u, 0xbd011805u, 0x6d131800u, RET}, 2, 0, 0},
    {"bti/paciasp ... autiasp; ret", {0xd503245fu, 0xd503233fu, 0xf900001fu, 0xd50323bfu, RET}, 2, 0, 1},
    {"frame record push/pop", {0xa9bf7bfdu, 0x910003fdu, 0xf900001fu, 0xa8c17bfdu, RET}, 2, 0, 1},
    {"b over garbage to ret", {0x14000002u, 0x00000000u, RET}, 1, 0, 0},
    {"b to a constant store (tail call)", {0x52800068u, 0xf9000408u, 0x14000002u, 0x00000000u, 0xf900001fu, RET}, 2, 0, 1},
    /* real code */
    {"ldr; add; str", {0xf9400408u, 0x91000508u, 0xf9000008u, RET}, 0, 0, 0},
    {"str of an unknown register", {0xf9000008u, RET}, 0, 0, 0},
    {"store through x1", {0xf900003fu, RET}, 0, 0, 0},
    {"x0 overwritten", {0xd2800020u, 0xf900001fu, RET}, 0, 0, 0},
    {"pre-index stp (writes x0 back)", {0xa9817c1fu, RET}, 0, 0, 0},
    {"post-index str", {0x52800028u, 0xf8008408u, RET}, 0, 0, 0},
    {"udf", {0x00000000u}, 0, 0, 0},
    {"brk", {0xd42000e0u}, 0, 0, 0},
    {"movk of an unknown register", {0xf280002au, 0xf900000au, RET}, 0, 0, 0},
    {"store to the stack", {0x52800028u, 0xf90007e8u, RET}, 0, 0, 0},
    {"mov x8,x0 (copy of the context)", {0xaa0003e8u, 0xf900011fu, RET}, 0, 0, 0},
    {"orr x8,x9,#0xff with x9 unknown", {0xb2401d28u, 0xf9000008u, RET}, 0, 0, 0},
    {"mvni v3.4s,#0 (all ones)", {0x6f000403u, 0x3d800003u, RET}, 0, 0, 0},
    {"movi v4.2d,#0xff", {0x6f00e424u, 0x3d800004u, RET}, 0, 0, 0},
    {"b . (endless)", {0x14000000u}, 0, 0, 0},
    {"bl", {0x94000002u, RET, RET}, 0, 0, 0},
};

typedef struct { uint64_t x[32]; uint64_t pc, fn; struct { double d[2]; } v[32]; uint32_t w; uint8_t b; } TCtx;
typedef void (*Fn)(TCtx *);
extern void f_empty(TCtx *), f_ret0(TCtx *), f_ret1(TCtx *), f_ret_result(TCtx *), f_ret_neg(TCtx *), f_ret_big(TCtx *), f_ret_u32(TCtx *),
    f_ret_mask(TCtx *), f_many(TCtx *), f_vec(TCtx *), f_real(TCtx *), f_cond(TCtx *), f_call(TCtx *);
typedef struct { const char *name; Fn fn; int kind; long long x0; int sets; } FCase;
static const FCase fcases[] = {
    {"f_empty", f_empty, 1, 0, 0},
    {"f_ret0", f_ret0, 2, 0, 1},
    {"f_ret1", f_ret1, 2, 1, 1},
    {"f_ret_result", f_ret_result, 2, 0x1015, 1},
    {"f_ret_neg", f_ret_neg, 2, -1, 1},
    {"f_ret_big", f_ret_big, 2, 0x123456789abcLL, 1},
    {"f_ret_u32", f_ret_u32, 2, 0xC0000000LL, 1},
    {"f_ret_mask", f_ret_mask, 2, (long long)0xff00ff00ff00ff00ULL, 1},
    {"f_many", f_many, 2, 0, 1},
    {"f_vec", f_vec, 2, 0, 0},
    {"f_real", f_real, 0, 0, 0},
    {"f_cond", f_cond, 0, 0, 0},
    {"f_call", f_call, 0, 0, 0},
};

static void dump(const void *p) {
    const uint32_t *w = p;
    printf("    code:");
    for (int i = 0; i < 10; i++) printf(" %08x", w[i]);
    printf("\n");
}

int main(void) {
    SECTION("hand-assembled sequences");
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        long long x0 = 12345; int sets = 9;
        int k = a64_trivial(cases[i].code, &x0, &sets);
        int ok = k == cases[i].kind && (k != 2 || (sets == cases[i].sets && (!sets || x0 == cases[i].x0)));
        CHECKF(ok, "%s: kind %d x0 %llx sets %d (want %d %llx %d)", cases[i].name, k, (unsigned long long)x0, sets, cases[i].kind, (unsigned long long)cases[i].x0, cases[i].sets);
    }
    {   long long x0; int sets;
        CHECK(a64_trivial(NULL, &x0, &sets) == 0); }

    SECTION("compiled handlers (" __VERSION__ ")");
    for (size_t i = 0; i < sizeof fcases / sizeof fcases[0]; i++) {
        long long x0 = 12345; int sets = 9;
        int k = a64_trivial((const void *)fcases[i].fn, &x0, &sets);
        int ok = k == fcases[i].kind && (k != 2 || (sets == fcases[i].sets && (!sets || x0 == fcases[i].x0)));
        CHECKF(ok, "%s: kind %d x0 %llx sets %d (want %d %llx %d)", fcases[i].name, k, (unsigned long long)x0, sets, fcases[i].kind, (unsigned long long)fcases[i].x0, fcases[i].sets);
        if (!ok) dump((const void *)fcases[i].fn);
        TCtx c; memset(&c, 0, sizeof c); c.x[1] = 5;   /* and they do run */
        fcases[i].fn(&c);
    }
    return tp_done("test_a64");
}

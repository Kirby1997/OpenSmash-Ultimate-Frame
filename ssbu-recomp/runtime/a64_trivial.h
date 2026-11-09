/* a64_trivial.h - the "trivial function" test of the fake audit (hostrt.c audit_trivial, plan\FAKE_AUDIT.md) on A64 code.
 *
 * audit_trivial reads the machine code of a handler to tell a body that does nothing but store constants into the guest context
 * and return from real code. The x86-64 decoder in hostrt.c is the Windows build's; on an aarch64 host the same bytes are another
 * instruction set, and until this file the test classified nothing there (SSBU_STRICT on the Frame passed whatever was bound).
 * hostrt.c uses this file on Linux aarch64 only. It is pure (a function of the instruction words), so tests/port/crash/test_a64.c
 * includes it on any arm64 host, also a Mac.
 *
 * a64_trivial: 0 = real code; 1 = empty body (ret); 2 = only stores constants into the context x0 points to, then returns (*x0v
 * := the value stored at [x0, #0] when *sets_x0). Follows up to 4 unconditional branches (tail calls / thunks); hints (nop, bti,
 * paciasp / autiasp) are ignored. Accepts mov / movz / movn / movk / orr-immediate / mov-register into scratch registers, movi #0 /
 * fmov from zr into vector registers, and str / stur / stp (integer and vector, signed offset) with base x0 of a known value.
 * From linux-arm64's plat_crash.c (WP-U4), which kept it with the crash code; here it stands alone. */
#ifndef SSBU_A64_TRIVIAL_H
#define SSBU_A64_TRIVIAL_H
#include <stdint.h>
#include <string.h>

/* DecodeBitMasks (Arm ARM) for a logical immediate; 0 if the encoding is reserved. */
static inline int a64_bitmask(int sf, unsigned n, unsigned immr, unsigned imms, uint64_t *out) {
    unsigned x = (n << 6) | (~imms & 0x3f);
    if (!x || (!sf && n)) return 0;
    int len = 31 - __builtin_clz(x);
    if (len < 1) return 0;
    unsigned esize = 1u << len, levels = esize - 1, s = imms & levels, r = immr & levels;
    if (s == levels) return 0;
    uint64_t emask = esize == 64 ? ~0ull : (1ull << esize) - 1;
    uint64_t w = s + 1 == 64 ? ~0ull : (1ull << (s + 1)) - 1;
    uint64_t e = r ? ((w >> r) | (w << (esize - r))) & emask : w;
    uint64_t v = 0;
    for (unsigned i = 0; i < 64; i += esize) v |= e << i;
    *out = sf ? v : v & 0xffffffffull;
    return 1;
}
static inline int a64_trivial(const void *code, long long *x0v, int *sets_x0) {
    const unsigned char *p = code;
    uint64_t val[32] = {0};
    uint32_t known = 1u << 31, vknown = 0, vzero = 0;      /* x31 as a source of mov/str = zr */
    int stores = 0, hops = 0;
    *x0v = 0; *sets_x0 = 0;
    if (!p) return 0;
#define SETX(rd, v, sf) do { if ((rd) == 0 || (rd) == 31) return 0; val[rd] = (sf) ? (uint64_t)(v) : (uint64_t)(uint32_t)(v); known |= 1u << (rd); } while (0)
#define KNOWN(r) ((known >> (r)) & 1u)
#define STORE_X0(v) do { *x0v = (long long)(v); *sets_x0 = 1; } while (0)
    for (int step = 0; step < 48; step++) {
        uint32_t i;
        memcpy(&i, p, 4);
        unsigned rd = i & 31, rn = (i >> 5) & 31;
        if (i == 0xd65f03c0u || i == 0xd65f0bffu) return stores ? 2 : 1;               /* ret, retaa */
        if ((i & 0xfffff01fu) == 0xd503201fu) { p += 4; continue; }                     /* hints: nop, bti, paciasp, autiasp */
        if ((i & 0xfc000000u) == 0x14000000u) {                                         /* b: tail call / thunk */
            if (++hops > 4) return 0;
            p += (int64_t)((int32_t)(i << 6) >> 4);
            continue;
        }
        if ((i & 0xffc07fffu) == 0xa9807bfdu || (i & 0xffc07fffu) == 0xa8c07bfdu || i == 0x910003fdu) { p += 4; continue; }   /* frame record push/pop, mov x29, sp */
        if ((i & 0x1f800000u) == 0x12800000u) {                                         /* movn / movz / movk */
            int sf = i >> 31, opc = (i >> 29) & 3, hw = (i >> 21) & 3;
            uint64_t imm = (uint64_t)((i >> 5) & 0xffff) << (16 * hw);
            if ((!sf && hw > 1) || opc == 1) return 0;
            if (opc == 0) SETX(rd, ~imm, sf);
            else if (opc == 2) SETX(rd, imm, sf);
            else { if (!KNOWN(rd) || rd == 31) return 0; SETX(rd, (val[rd] & ~((uint64_t)0xffff << (16 * hw))) | imm, sf); }
            p += 4; continue;
        }
        if ((i & 0x7f800000u) == 0x32000000u) {                                         /* orr rd, rn, #bitmask (mov rd, #imm when rn = zr) */
            uint64_t imm;
            int sf = i >> 31;
            if (!a64_bitmask(sf, (i >> 22) & 1, (i >> 16) & 63, (i >> 10) & 63, &imm) || !KNOWN(rn)) return 0;
            SETX(rd, (rn == 31 ? 0 : val[rn]) | imm, sf);
            p += 4; continue;
        }
        if ((i & 0x7fe0ffe0u) == 0x2a0003e0u) {                                         /* mov rd, rm (orr rd, zr, rm) */
            unsigned rm = (i >> 16) & 31;
            if (!KNOWN(rm)) return 0;
            SETX(rd, rm == 31 ? 0 : val[rm], i >> 31);
            p += 4; continue;
        }
        if ((i & 0xffffffe0u) == 0x9e6703e0u || (i & 0xffffffe0u) == 0x1e2703e0u) { vknown |= 1u << rd; vzero |= 1u << rd; p += 4; continue; }   /* fmov d/s, xzr/wzr */
        if ((i & 0xff201fe0u) == 0x1e201000u) { vknown |= 1u << rd; vzero &= ~(1u << rd); p += 4; continue; }   /* fmov s/d/h, #imm */
        if ((i & 0x9ff80c00u) == 0x0f000400u && !((i >> 5) & 31) && !((i >> 16) & 7)) {   /* movi / mvni / fmov (vector, immediate) with imm 0 */
            unsigned op = (i >> 29) & 1, cmode = (i >> 12) & 15;
            int zero = (!op && !(cmode & 1) && cmode != 12) || (op && cmode == 14);
            int other = (!op && cmode == 15) || (op && cmode == 15);                    /* fmov #2.0 */
            if (!zero && !other) return 0;
            vknown |= 1u << rd;
            if (zero) vzero |= 1u << rd; else vzero &= ~(1u << rd);
            p += 4; continue;
        }
        /* integer stores, base x0 */
        if ((i & 0x3fc00000u) == 0x39000000u || (i & 0x3fe00c00u) == 0x38000000u) {     /* str (unsigned offset) / stur */
            unsigned size = i >> 30;
            int64_t off = (i & 0x01000000u) ? (int64_t)(((i >> 10) & 0xfff) << size) : (int64_t)((int32_t)(i << 11) >> 23);
            if (rn != 0 || !KNOWN(rd)) return 0;
            uint64_t v = rd == 31 ? 0 : val[rd];
            if (off == 0 && size == 3) STORE_X0(v);
            else if (off == 0 && size == 2) STORE_X0((uint32_t)v);
            stores++; p += 4; continue;
        }
        if ((i & 0x7fc00000u) == 0x29000000u) {                                         /* stp w/x (signed offset) */
            unsigned opc = i >> 30, rt2 = (i >> 10) & 31;
            if (opc != 0 && opc != 2) return 0;
            int64_t off = (int64_t)((int32_t)(i << 10) >> 25) * (opc ? 8 : 4);
            if (rn != 0 || !KNOWN(rd) || !KNOWN(rt2)) return 0;
            uint64_t v1 = rd == 31 ? 0 : val[rd], v2 = rt2 == 31 ? 0 : val[rt2];
            if (off == 0) STORE_X0(opc ? v1 : (uint32_t)v1 | ((uint64_t)(uint32_t)v2 << 32));
            else if (opc && off == -8) STORE_X0(v2);
            stores++; p += 4; continue;
        }
        /* vector stores, base x0 */
        if ((i & 0x3f400000u) == 0x3d000000u || (i & 0x3f600c00u) == 0x3c000000u) {     /* str / stur b/h/s/d/q */
            unsigned scale = ((i >> 30) & 3) | (((i >> 23) & 1) << 2);
            int64_t off = (i & 0x01000000u) ? (int64_t)(((i >> 10) & 0xfff) << scale) : (int64_t)((int32_t)(i << 11) >> 23);
            if (rn != 0 || !((vknown >> rd) & 1)) return 0;
            if (off == 0 && scale >= 2 && ((vzero >> rd) & 1)) STORE_X0(0);
            stores++; p += 4; continue;
        }
        if ((i & 0x3fc00000u) == 0x2d000000u) {                                         /* stp s/d/q (signed offset) */
            unsigned opc = i >> 30, rt2 = (i >> 10) & 31;
            if (opc == 3) return 0;
            int64_t off = (int64_t)((int32_t)(i << 10) >> 25) * (4 << opc);
            if (rn != 0 || !((vknown >> rd) & 1) || !((vknown >> rt2) & 1)) return 0;
            if (off == 0 && opc >= 1 && ((vzero >> rd) & 1)) STORE_X0(0);
            stores++; p += 4; continue;
        }
        return 0;
    }
#undef SETX
#undef KNOWN
#undef STORE_X0
    return 0;
}

#endif /* SSBU_A64_TRIVIAL_H */

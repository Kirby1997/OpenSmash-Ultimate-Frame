/* uint64_t tp_regcheck(void (*fn)(void *, void *), void *a0, void *a1, uint64_t seed)   (tests/port/threads, C3b)
 * Loads x19..x28 and d8..d15 with values made from seed, calls fn(a0, a1) through blr (an indirect branch: BTI checks
 * its target), and when fn returns (for SwitchToFiber: when something switches back, maybe on another thread) checks
 * every one of them. Returns a mask of the registers that changed: bit n-19 for xn, bit 10+k-8 for dk. AAPCS64 asks
 * fn to keep exactly these. Builds as ELF (Linux) and as Mach-O (the macOS check: leading underscore). */
#if defined(__aarch64__)
#ifdef __APPLE__
#define SYM(s) _##s
#else
#define SYM(s) s
#endif
        .text
        .globl  SYM(tp_regcheck)
#ifndef __APPLE__
        .type   tp_regcheck, %function
#endif
        .p2align 4
SYM(tp_regcheck):
        hint    #34                     /* bti c */
        stp     x29, x30, [sp, #-176]!
        mov     x29, sp
        stp     x19, x20, [sp, #16]
        stp     x21, x22, [sp, #32]
        stp     x23, x24, [sp, #48]
        stp     x25, x26, [sp, #64]
        stp     x27, x28, [sp, #80]
        stp     d8, d9, [sp, #96]
        stp     d10, d11, [sp, #112]
        stp     d12, d13, [sp, #128]
        stp     d14, d15, [sp, #144]
        str     x3, [sp, #160]
        mov     x9, x0
        mov     x0, x1
        mov     x1, x2
        .irp    n, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28
        add     x\n, x3, #\n
        .endr
        .irp    k, 8, 9, 10, 11, 12, 13, 14, 15
        add     x10, x3, #(0x100 + \k)
        fmov    d\k, x10
        .endr
        blr     x9
        ldr     x2, [sp, #160]
        mov     x0, #0
        .irp    n, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28
        add     x9, x2, #\n
        cmp     x\n, x9
        cset    x10, ne
        orr     x0, x0, x10, lsl #(\n - 19)
        .endr
        .irp    k, 8, 9, 10, 11, 12, 13, 14, 15
        add     x9, x2, #(0x100 + \k)
        fmov    x11, d\k
        cmp     x11, x9
        cset    x10, ne
        orr     x0, x0, x10, lsl #(10 + \k - 8)
        .endr
        ldp     x19, x20, [sp, #16]
        ldp     x21, x22, [sp, #32]
        ldp     x23, x24, [sp, #48]
        ldp     x25, x26, [sp, #64]
        ldp     x27, x28, [sp, #80]
        ldp     d8, d9, [sp, #96]
        ldp     d10, d11, [sp, #112]
        ldp     d12, d13, [sp, #128]
        ldp     d14, d15, [sp, #144]
        ldp     x29, x30, [sp], #176
        ret
#ifndef __APPLE__
        .size   tp_regcheck, .-tp_regcheck
        .section .note.GNU-stack, "", %progbits
        .pushsection .note.gnu.property, "a"     /* GNU_PROPERTY_AARCH64_FEATURE_1_AND: BTI | PAC */
        .balign 8
        .long   4, 16, 5
        .asciz  "GNU"
        .long   0xc0000000, 4, 3, 0
        .popsection
#endif
#endif

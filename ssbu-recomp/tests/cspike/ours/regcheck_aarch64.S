/* uint64_t tp_switch_check(plat_fiber *from, plat_fiber *to, uint64_t seed)
 * Loads x19..x28 and d8..d15 with values derived from seed, calls plat_fiber_switch(from, to), and on return (when
 * something switches back to `from`) checks every one of them: returns a bitmask of registers that changed
 * (bit n-19 for xn, bit 10+k-8 for dk). Callee-saved registers of the caller are preserved as AAPCS64 requires. */
#if defined(__aarch64__)
        .text
        .globl  tp_switch_check
        .type   tp_switch_check, %function
        .balign 16
tp_switch_check:
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
        str     x2, [sp, #160]
        .irp    n, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28
        add     x\n, x2, #\n
        .endr
        .irp    k, 8, 9, 10, 11, 12, 13, 14, 15
        add     x9, x2, #(0x100 + \k)
        fmov    d\k, x9
        .endr
        bl      plat_fiber_switch
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
        .size   tp_switch_check, .-tp_switch_check
        .section .note.GNU-stack, "", %progbits
        .pushsection .note.gnu.property, "a"
        .balign 8
        .long   4, 16, 5
        .asciz  "GNU"
        .long   0xc0000000, 4, 3, 0
        .popsection
#endif

/* Included by hostrt.c (and tests/port/calls/test_ci_direct.c): the fast path of call_indirect.
 *
 * call_indirect (hostrt.c) is how lifted code makes every indirect call and branch (blr / br to a computed guest address).
 * Nearly all of them go to a function of main. This is a direct-mapped table over main's code: one slot per 16-byte block,
 * holding the lifted function that starts at that block as a 32-bit offset from `base`, 0 = none.
 *
 *     key  = ror64(addr - GUEST_BASE, 4)     a misaligned address (low 4 bits set) or one outside main becomes >= slots
 *     hit  = key < slots && dir[key] != 0    then the function is base + dir[key]
 *
 * One load of the slot, no probing, no multiply, no key compare. 16-byte blocks because main's function entries are
 * aligned to 16 (13.0.4: 112,800 of 112,890; the 90 others, and anything that is no function of main, take the slow path,
 * which finds them as before). Every slot is filled when the table is built, before it is published, and never changes
 * afterwards: the modules loaded at run time (nnro.c, 0x72xxxxxxxx...), the SDK code, the host functions and the
 * import placeholders are outside main's range and keep their own lookups.
 *
 * Memory: 4 bytes per 16 bytes of main's code, 15.2 MB for 13.0.4 (text 0x39c7e90).
 * Concurrency: the slots are written before `slots` is stored with release; the reader loads `slots` with acquire and the
 * slot with acquire too (LDAPR on the Frame, a plain load on x86-64). Before the table is published `slots` is 0 and
 * every address misses. No locks, fences or system calls on the lookup.
 *
 * The includer defines GUEST_BASE and has runtime.h (Ctx, FnEntry). */

typedef struct {
    uint64_t slots;                 /* number of slots in dir; 0 = no table. Stored last, with release. */
    const uint32_t *dir;            /* per 16-byte block of main: host address - base, 0 = none */
    uintptr_t base;
} CiDirect;

static inline uint64_t ci_direct_key(uint64_t addr) {
    uint64_t off = addr - GUEST_BASE;
    return (off >> 4) | (off << 60);
}

/* The slot of guest `addr`: nonzero = the lifted function that starts there is ci_direct_fn(t, slot); 0 = not in the table
 * (the caller takes the slow path). Two calls rather than one returning a pointer: the caller tests the 32-bit slot, not
 * base + slot, which the compiler cannot prove nonzero. */
static inline uint32_t ci_direct_slot(const CiDirect *t, uint64_t addr) {
    uint64_t k = ci_direct_key(addr);
    if (k >= __atomic_load_n(&t->slots, __ATOMIC_ACQUIRE)) return 0;
    return __atomic_load_n(&t->dir[k], __ATOMIC_ACQUIRE);
}
static inline void (*ci_direct_fn(const CiDirect *t, uint32_t slot))(Ctx *) { return (void (*)(Ctx *))(t->base + slot); }
/* The same as one call: the function, or NULL */
static inline void (*ci_direct_find(const CiDirect *t, uint64_t addr))(Ctx *) {
    uint32_t e = ci_direct_slot(t, addr);
    return e ? ci_direct_fn(t, e) : NULL;
}

/* Slots a table needs for the entries of `tab` inside main (guest offset below `limit`), 0 if it has none. */
static uint64_t ci_direct_slots(const FnEntry *tab, int n, uint64_t limit) {
    uint64_t top = 0; int any = 0;
    for (int i = 0; i < n; i++) {
        uint64_t off = tab[i].addr - GUEST_BASE;
        if (off < limit && !(off & 15) && tab[i].fn) { any = 1; if (off > top) top = off; }
    }
    return any ? (top >> 4) + 1 : 0;
}

/* Fills dir (`slots` slots, zero on entry) from tab and publishes it in t. Returns the number of entries placed; the others
 * (misaligned, outside main, or a host function more than 4 GB from the lowest) are left to the slow path. The first entry
 * for an address wins, as in hostrt.c's hash table. Call once, before or while other threads look up (they miss until the
 * release store). */
static int ci_direct_build(CiDirect *t, uint32_t *dir, uint64_t slots, const FnEntry *tab, int n) {
    uintptr_t lo = UINTPTR_MAX;
    for (int i = 0; i < n; i++) {
        uint64_t off = tab[i].addr - GUEST_BASE;
        if ((off >> 4) < slots && !(off & 15) && tab[i].fn && (uintptr_t)tab[i].fn < lo) lo = (uintptr_t)tab[i].fn;
    }
    if (lo == UINTPTR_MAX) return 0;
    uintptr_t base = lo - 1;                                        /* the lowest function is offset 1: 0 stays "none" */
    int placed = 0;
    for (int i = 0; i < n; i++) {
        uint64_t off = tab[i].addr - GUEST_BASE;
        if ((off >> 4) >= slots || (off & 15) || !tab[i].fn) continue;
        uintptr_t d = (uintptr_t)tab[i].fn - base;
        if (d > 0xffffffffu || dir[off >> 4]) continue;
        __atomic_store_n(&dir[off >> 4], (uint32_t)d, __ATOMIC_RELAXED);
        placed++;
    }
    t->dir = dir;
    t->base = base;
    __atomic_store_n(&t->slots, slots, __ATOMIC_RELEASE);
    return placed;
}

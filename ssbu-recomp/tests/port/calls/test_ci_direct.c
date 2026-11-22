/* call_indirect's fast table (runtime/ci_direct.inc) against the lookup hostrt.c had before it (the first fn_table entry
 * with the address), on a synthetic table shaped like main's (13.0.4: 112,890 functions in 0x39c7e90 bytes of code, all but
 * 90 aligned to 16): every entry, every 16-byte block, every misaligned neighbour, the edges of the range, duplicates, a host
 * function too far from the others for 32 bits, before and after publication, and readers racing the build.
 * No game data.   tests/port/calls/run.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "runtime.h"

#define GUEST_BASE 0x7100000000ULL
#include "ci_direct.inc"

typedef void (*Fn)(Ctx *);
static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { if (++g_fail <= 25) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng = 0x9e3779b97f4a7c15ULL;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

#define TEXT 0x39c7e90ULL                 /* 13.0.4's code size */
#define IMAGE 0x7500000ULL                /* hostrt.c IMAGE_SIZE */
static FnEntry *tab; static int ntab, cap;
static void add(uint64_t addr, uintptr_t fn) {
    if (ntab == cap) { cap = cap ? cap * 2 : 1 << 17; tab = realloc(tab, (size_t)cap * sizeof *tab); }
    tab[ntab].addr = addr; tab[ntab].fn = (Fn)fn; ntab++;
}

/* the reference: the first entry with the address (hostrt.c's hash table and binary search answer the same) */
typedef struct { uint64_t addr; int idx; } Ref;
static Ref *ref; static int nref;
static int cmp_ref(const void *a, const void *b) {
    const Ref *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr ? 1 : x->idx - y->idx;
}
static void ref_build(void) {
    ref = malloc((size_t)ntab * sizeof *ref);
    for (int i = 0; i < ntab; i++) { ref[i].addr = tab[i].addr; ref[i].idx = i; }
    qsort(ref, (size_t)ntab, sizeof *ref, cmp_ref);
    nref = 0;
    for (int i = 0; i < ntab; i++) if (!nref || ref[nref - 1].addr != ref[i].addr) ref[nref++] = ref[i];
}
static Fn ref_find(uint64_t addr) {
    int lo = 0, hi = nref - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ref[mid].addr == addr) return tab[ref[mid].idx].fn;
        if (ref[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static uintptr_t g_far;                  /* a host function more than 4 GB above the others (not placed: slow path) */
/* what the fast path must answer: the reference's function where the table can hold it, else NULL (the slow path) */
static Fn want(uint64_t addr) {
    Fn r = ref_find(addr);
    uint64_t off = addr - GUEST_BASE;
    if (!r || off >= IMAGE || (off & 15) || (uintptr_t)r == g_far) return NULL;
    return r;
}

static CiDirect g_t;
static uint32_t *g_dir;
static uint64_t g_slots;

/* readers racing the build: every answer is NULL (not published yet) or the right function */
static volatile int g_go, g_stop;
static uint64_t *g_sample; static Fn *g_sample_want; static int g_nsample;
static void *reader(void *arg) {
    long long bad = 0, hits = 0, n = 0, after = 0;
    uint64_t r = (uint64_t)(uintptr_t)arg * 0x2545f4914f6cdd1dULL + 1;
    while (!__atomic_load_n(&g_go, __ATOMIC_ACQUIRE)) { }
    while (after < 300000) {                     /* through the build, then 300,000 lookups after it */
        if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) after++;
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        int i = (int)(r % (uint64_t)g_nsample);
        Fn f = ci_direct_find(&g_t, g_sample[i]);
        if (f) { hits++; if (f != g_sample_want[i]) bad++; }
        n++;
    }
    long long *out = malloc(3 * sizeof *out);
    out[0] = bad; out[1] = hits; out[2] = n;
    return out;
}

int main(void) {
    /* main's shape: functions 16 bytes apart or more (mean ~530), 0.08 % at +4 / +8 / +12, a few after an aligned one in
     * the same block, duplicates (the first wins), host functions in another order than the guest's */
    uintptr_t host_base = (uintptr_t)0x40000000ULL;
    add(GUEST_BASE, host_base + 0x20);                                 /* main's first function */
    uint64_t off = 16;
    while (off < TEXT - 64) {
        uintptr_t host = host_base + (uintptr_t)((rnd() % 0x12f00000ULL) & ~3ULL);
        uint64_t a = GUEST_BASE + off;
        unsigned m = (unsigned)(rnd() % 10000);
        if (m < 8) a += 4 * (1 + rnd() % 3);                         /* misaligned entry */
        add(a, host);
        if (m >= 8 && m < 11) add(a + 4 * (1 + rnd() % 3), host + 0x40);   /* a second entry in the same 16-byte block */
        if (m == 11) add(a, host + 0x80);                             /* duplicate: must lose to the first */
        off += 16 * (1 + rnd() % 66);
    }
    add(GUEST_BASE, host_base + 0x10);                                 /* a duplicate of it, later in the table */
    add(GUEST_BASE + TEXT - 16, host_base);                           /* the lowest host function: slot value 1 */
    uint64_t top = GUEST_BASE + TEXT - 16;
    add(0x7200000000ULL, host_base + 0x100);                          /* outside main: a module, the SDK, below main */
    add(0x7500001000ULL, host_base + 0x104);
    add(0x70fffffff0ULL, host_base + 0x108);
    add(GUEST_BASE + IMAGE + 0x100, host_base + 0x10c);               /* inside 0x71.. but past the image */
    add(GUEST_BASE + 0x1230, 0);                                       /* no function pointer */
    if (sizeof(uintptr_t) == 8) { g_far = host_base + ((uintptr_t)5 << 32); add(GUEST_BASE + 0x2000000, g_far); }
    /* GUEST_BASE + 0x1230 / 0x2000000 may coincide with a generated entry; the first entry decides there, as in the reference */
    ref_build();
    printf("synthetic table: %d entries (%d distinct addresses), code 0x%llx bytes\n", ntab, nref, (unsigned long long)TEXT);

    /* sizing */
    g_slots = ci_direct_slots(tab, ntab, IMAGE);
    CHECK(g_slots == ((top - GUEST_BASE) >> 4) + 1, "slots %llu, want %llu", (unsigned long long)g_slots, (unsigned long long)(((top - GUEST_BASE) >> 4) + 1));
    printf("slots %llu = %.1f MB\n", (unsigned long long)g_slots, g_slots * 4 / 1048576.0);
    {   FnEntry none[2] = {{0x7200000000ULL, (Fn)(uintptr_t)0x1000}, {GUEST_BASE + 4, (Fn)(uintptr_t)0x1000}};
        CHECK(ci_direct_slots(none, 2, IMAGE) == 0, "a table without aligned main entries needs no slots");
        CiDirect e = {0}; uint32_t one = 0;
        CHECK(ci_direct_build(&e, &one, 0, none, 2) == 0 && e.slots == 0, "an empty build publishes nothing");
        CHECK(ci_direct_find(&e, GUEST_BASE) == NULL, "empty table misses");
    }

    /* a guard after the last slot: a lookup that reads past the table finds a nonzero "function" there and fails */
    g_dir = calloc((size_t)g_slots + 64, sizeof *g_dir);
    for (int i = 0; i < 64; i++) g_dir[g_slots + i] = 0x7777u;
    /* before publication: every lookup misses */
    for (int i = 0; i < nref; i += 97) CHECK(ci_direct_find(&g_t, ref[i].addr) == NULL, "hit before the table exists (%llx)", (unsigned long long)ref[i].addr);

    /* readers start before the build and run through it */
    g_nsample = 1 << 16;
    g_sample = malloc((size_t)g_nsample * sizeof *g_sample); g_sample_want = malloc((size_t)g_nsample * sizeof *g_sample_want);
    for (int i = 0; i < g_nsample; i++) { int j = (int)(rnd() % (uint64_t)nref); g_sample[i] = ref[j].addr + (rnd() % 8 == 0 ? 4 : 0); g_sample_want[i] = want(g_sample[i]); }
    enum { NR = 3 };
    pthread_t th[NR];
    for (long i = 0; i < NR; i++) pthread_create(&th[i], NULL, reader, (void *)(i + 1));
    __atomic_store_n(&g_go, 1, __ATOMIC_RELEASE);
    int placed = ci_direct_build(&g_t, g_dir, g_slots, tab, ntab);
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    long long rbad = 0, rhits = 0, rn = 0;
    for (int i = 0; i < NR; i++) { long long *o; pthread_join(th[i], (void **)&o); rbad += o[0]; rhits += o[1]; rn += o[2]; free(o); }
    CHECK(rbad == 0, "%lld wrong answers of %lld lookups racing the build", rbad, rn);
    CHECK(rhits > NR * 300000 / 2, "the readers hardly saw the table (%lld hits of %lld lookups)", rhits, rn);
    printf("racing readers: %lld lookups, %lld hits, %lld wrong\n", rn, rhits, rbad);

    /* every entry and its misaligned neighbours */
    int want_hits = 0, slow = 0;
    for (int i = 0; i < nref; i++) {
        uint64_t a = ref[i].addr;
        Fn w = want(a), f = ci_direct_find(&g_t, a);
        CHECK(f == w, "entry %llx: %p, want %p", (unsigned long long)a, (void *)(uintptr_t)f, (void *)(uintptr_t)w);
        if (w) want_hits++; else slow++;
        for (int d = 4; d < 16; d += 4)
            CHECK(ci_direct_find(&g_t, a + d) == ((a + d) & 15 ? NULL : want(a + d)), "neighbour %llx of %llx", (unsigned long long)(a + d), (unsigned long long)a);
    }
    CHECK(placed == want_hits, "placed %d, want %d", placed, want_hits);
    printf("entries: %d in the table, %d left to the slow path (misaligned, duplicates, outside main, too far, no function)\n", want_hits, slow);
    /* every 16-byte block of the range: a hit exactly where an aligned entry is */
    long long blocks_hit = 0;
    for (uint64_t s = 0; s < g_slots; s++) {
        uint64_t a = GUEST_BASE + s * 16;
        Fn f = ci_direct_find(&g_t, a);
        if (f) blocks_hit++;
        if ((f != NULL) != (want(a) != NULL) || (f && f != want(a))) CHECK(0, "block %llx: %p, want %p", (unsigned long long)a, (void *)(uintptr_t)f, (void *)(uintptr_t)want(a));
    }
    CHECK(blocks_hit == want_hits, "%lld blocks hit, %d entries placed", blocks_hit, want_hits);
    /* the slot encoding: the lowest host function is 1, 0 means none */
    CHECK(ci_direct_slot(&g_t, top) == 1, "lowest host function: slot %u, want 1", ci_direct_slot(&g_t, top));
    CHECK(ci_direct_find(&g_t, GUEST_BASE) == tab[0].fn, "the first of two entries at GUEST_BASE wins");
    /* edges: below, past the last slot, past the image, high bits that the rotation must not fold back into range */
    uint64_t edge[] = {0, 1, 15, GUEST_BASE - 16, GUEST_BASE - 1, GUEST_BASE + g_slots * 16, GUEST_BASE + g_slots * 16 + 16,
                       GUEST_BASE + IMAGE, GUEST_BASE + IMAGE + 0x100, 0x7200000000ULL, 0x7500001000ULL, 0x7400000000ULL,
                       0x7300000000ULL, GUEST_BASE + (1ULL << 32), GUEST_BASE + (1ULL << 36), GUEST_BASE + (1ULL << 60),
                       GUEST_BASE + (1ULL << 63), GUEST_BASE + 0x10 + (1ULL << 60), (1ULL << 47), ~0ULL, ~0ULL - 15,
                       GUEST_BASE + 4, GUEST_BASE + 8, GUEST_BASE + 12};
    for (unsigned i = 0; i < sizeof edge / sizeof *edge; i++) {
        Fn f = ci_direct_find(&g_t, edge[i]);
        CHECK(f == want(edge[i]), "edge %llx: %p, want %p", (unsigned long long)edge[i], (void *)(uintptr_t)f, (void *)(uintptr_t)want(edge[i]));
        CHECK(ci_direct_key(edge[i]) < g_slots ? !((edge[i] - GUEST_BASE) & 15) && edge[i] - GUEST_BASE < g_slots * 16 : 1,
              "key of %llx lands in the table", (unsigned long long)edge[i]);
    }
    /* random addresses anywhere */
    for (int i = 0; i < 2000000; i++) {
        uint64_t a = i & 1 ? GUEST_BASE + (rnd() % (IMAGE + 0x1000)) : rnd();
        if (i % 3 == 0) a &= ~15ULL;
        Fn f = ci_direct_find(&g_t, a);
        if (f != want(a)) CHECK(0, "random %llx: %p, want %p", (unsigned long long)a, (void *)(uintptr_t)f, (void *)(uintptr_t)want(a));
    }
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "ok", g_checks, g_fail);
    return g_fail != 0;
}

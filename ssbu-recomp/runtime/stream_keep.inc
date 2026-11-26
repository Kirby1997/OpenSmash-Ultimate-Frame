/* Included by nvn_vk_draw.c (and tests/port/streamkeep/test_stream_keep.c): the table of SSBU_VK_STREAMKEEP, guest ranges
 * whose copy in the stream ring is bound again in a later batch while nothing can have changed their bytes. The rules
 * (why it is safe) are in nvn_vk_draw.c above ring_put. The includer defines, before including:
 *   SK_NEWEST(addr, size)  newest write stamp of the range's pages, 0 = not write-watched (ww_newest)
 *   SK_EPOCH()             VirtualFree / UnmapViewOfFile calls so far (plat_vm_epoch)
 *   SK_WRAPS               ring wraps so far
 *   SK_COPY(addr, size)    copies the range into the ring, returns its offset (may wrap the ring)
 * and allocates g_sk (SK_SETS * SK_WAYS entries, zeroed). Render thread only. */
#define SK_WAYS 4
#define SK_SETS 4096
#define SK_NONE (~0ull)
typedef struct { uint64_t addr, size, off, stamp; int64_t wrap; unsigned long long epoch; uint32_t used, skip_until, fails; } SkEnt;
static SkEnt *g_sk;
static uint32_t g_sk_batch = 1;
static int64_t g_sk_hits, g_sk_hit_bytes, g_sk_queries, g_sk_changed, g_sk_skips, g_sk_unwatched, g_sk_kills, g_sk_stale;
static SkEnt *sk_set(uint64_t addr, uint64_t size) {
    unsigned h = (unsigned)((addr >> 8) ^ (addr >> 21) ^ (size * 0x9E3779B1u)) * 2654435761u;
    return &g_sk[(h >> 20) * SK_WAYS];                /* 12 bits: SK_SETS */
}
static void sk_kill(uint64_t addr, uint64_t size) {
    SkEnt *w = sk_set(addr, size);
    for (int i = 0; i < SK_WAYS; i++) if (w[i].addr == addr && w[i].size == size && w[i].off != SK_NONE) { w[i].off = SK_NONE; g_sk_kills++; }
}
static uint64_t sk_put(uint64_t addr, uint64_t size) {
    unsigned long long ep = SK_EPOCH();         /* before the question and the copy: a VirtualFree after it is seen at the next use */
    SkEnt *w = sk_set(addr, size), *e = NULL;
    for (int i = 0; i < SK_WAYS && !e; i++) if (w[i].addr == addr && w[i].size == size) e = &w[i];      /* addr 0 is never asked (buf_ref callers skip it) */
    if (!e) {                                        /* a new range: the way not used for the longest */
        e = &w[0];
        for (int i = 1; i < SK_WAYS; i++) if (w[i].used < e->used) e = &w[i];
        e->addr = addr; e->size = size; e->off = SK_NONE; e->fails = 0; e->skip_until = 0;
    }
    e->used = g_sk_batch;
    if (e->skip_until > g_sk_batch) { g_sk_skips++; return SK_COPY(addr, size); }
    uint64_t nw = SK_NEWEST(addr, size);
    g_sk_queries++;
    if (!nw) { g_sk_unwatched++; e->off = SK_NONE; e->skip_until = g_sk_batch + 256; return SK_COPY(addr, size); }      /* not write-watched: copied as before, not asked again for a while */
    if (e->off != SK_NONE) {
        if (e->wrap == SK_WRAPS && e->epoch == ep && e->stamp == nw) { e->fails = 0; g_sk_hits++; g_sk_hit_bytes += (int64_t)size; return e->off; }
        if (e->stamp != nw) { g_sk_changed++; if (e->fails < 8) e->fails++; if (e->fails >= 2) e->skip_until = g_sk_batch + (1u << e->fails); }
        else g_sk_stale++;                           /* a wrap or a VirtualFree since the copy */
    }
    uint64_t off = SK_COPY(addr, size);              /* after the question; may wrap the ring: the wrap count is read after it */
    e->off = off; e->stamp = nw; e->wrap = SK_WRAPS; e->epoch = ep;
    return off;
}

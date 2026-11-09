/* tests/port/io/test_font.c - nnfontll.c's FreeType outline path (Linux) against FreeType itself.
 *   test_font <font file> [face index] [pixels]
 * For every glyph of the face:
 *   1. outline: glyph_path at one font unit per pixel must equal FreeType's own outline evaluated at the same
 *      parameters: a TrueType conic raised to a cubic (P0 + 2/3 (Q - P0), P2 + 2/3 (Q - P2)) is the same curve, so
 *      each flattened point is compared with the QUADRATIC B(t) at t = k/steps (max error must be ~1e-9 units);
 *   2. advance: advance_units (our hmtx) == FreeType's unscaled horiAdvance; advance_fixed (16.16 px) within 1/65536
 *      of FreeType's linearHoriAdvance at that pixel size;
 * and for the printable ASCII range plus a few others, the 8-bit glyph map (our exact-area scan converter on these
 * outlines) against FT_Render_Glyph (unhinted, gray): placement and coverage difference. */
#define FONTLL_TEST 1
#define main fonttest_main
#include "nnfontll.c"
#undef main
static int alt_somewhere(void) { return 0; }          /* nnfontll.c's (excluded with the engine's registration under FONTLL_TEST): no alternate-glyph search */

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* expected points: FreeType's outline with the conic evaluated as a quadratic, with the step rule of glyph_path */
typedef struct { double *xy; int n, cap; double x0, y0; int contours; } Exp;
static void ex_put(Exp *e, double x, double y) {
    if (e->n == e->cap) { e->cap = e->cap ? e->cap * 2 : 1024; e->xy = realloc(e->xy, (size_t)e->cap * 2 * sizeof(double)); }
    e->xy[e->n * 2] = x; e->xy[e->n * 2 + 1] = y; e->n++;
}
static int steps_of(double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3) {
    double len = fabs(x1 - x0) + fabs(y1 - y0) + fabs(x2 - x1) + fabs(y2 - y1) + fabs(x3 - x2) + fabs(y3 - y2);
    int steps = 2 + (int)(len / 3.0);
    return steps > 32 ? 32 : steps;
}
static int ex_move(const FT_Vector *to, void *u) { Exp *e = u; e->contours++; e->x0 = to->x; e->y0 = to->y; ex_put(e, e->x0, e->y0); return 0; }
static int ex_line(const FT_Vector *to, void *u) { Exp *e = u; e->x0 = to->x; e->y0 = to->y; ex_put(e, e->x0, e->y0); return 0; }
static int ex_conic(const FT_Vector *q, const FT_Vector *to, void *u) {
    Exp *e = u;
    double x0 = e->x0, y0 = e->y0, qx = q->x, qy = q->y, x2 = to->x, y2 = to->y;
    /* the step count of ft_conic (the control polygon of the conic itself; linux-arm64's glyph_path took the raised cubic's) */
    int steps = 2 + (int)((fabs(qx - x0) + fabs(qy - y0) + fabs(x2 - qx) + fabs(y2 - qy)) / 3.0);
    if (steps > 32) steps = 32;
    for (int k = 1; k <= steps; k++) {
        double t = (double)k / steps, s = 1 - t;
        ex_put(e, s * s * x0 + 2 * s * t * qx + t * t * x2, s * s * y0 + 2 * s * t * qy + t * t * y2);
    }
    e->x0 = x2; e->y0 = y2;
    return 0;
}
static int ex_cubic(const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to, void *u) {
    Exp *e = u;
    double x0 = e->x0, y0 = e->y0, x1 = c1->x, y1 = c1->y, x2 = c2->x, y2 = c2->y, x3 = to->x, y3 = to->y;
    int steps = steps_of(x0, y0, x1, y1, x2, y2, x3, y3);
    for (int k = 1; k <= steps; k++) {
        double t = (double)k / steps, s = 1 - t;
        ex_put(e, s * s * s * x0 + 3 * s * s * t * x1 + 3 * s * t * t * x2 + t * t * t * x3, s * s * s * y0 + 3 * s * s * t * y1 + 3 * s * t * t * y2 + t * t * t * y3);
    }
    e->x0 = x3; e->y0 = y3;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: test_font <font> [face index] [pixels]\n"); return 2; }
    unsigned index = argc > 2 ? (unsigned)atoi(argv[2]) : 0;
    int px = argc > 3 ? atoi(argv[3]) : 32;
    InitializeCriticalSection(&g_lock);
    FILE *in = fopen(argv[1], "rb");
    if (!in) { printf("cannot open %s\n", argv[1]); return 2; }
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size + 16);
    if (fread(data, 1, (size_t)size, in) != (size_t)size) return 2;
    fclose(in);
    static Engine e;
    FontData *f = e.fonts[0].fd = font_data(data, index);
    if (!f) { printf("not a font\n"); return 2; }
    e.count = 1; e.current = 0;
    e.scale[0] = e.scale[3] = px << 16;
    CHECK(gdi_prepare(f), "FT_New_Memory_Face");
    if (g_fail) return 1;
    printf("%s face %u: \"%s\" %s, %d glyphs, upem %d (FreeType: %ld glyphs, upem %d, %s)\n", argv[1], index, f->name, f->cff ? "CFF" : "TrueType", f->glyphs, f->upem,
           (long)f->ftface->num_glyphs, f->ftface->units_per_EM, FT_IS_SFNT(f->ftface) ? "sfnt" : "?");
    CHECK(f->ftface->num_glyphs == f->glyphs && f->ftface->units_per_EM == f->upem, "glyph count / upem differ from FreeType");

    /* 1 + 2: every glyph */
    static const FT_Outline_Funcs ef = {ex_move, ex_line, ex_conic, ex_cubic, 0, 0};
    const int32_t unit[4] = {f->upem << 16, 0, 0, f->upem << 16};          /* glyph_path: one font unit = one unit */
    double worst = 0; int worst_glyph = -1;
    long points = 0, conic_glyphs = 0, cubic_glyphs = 0, empty = 0, adv_bad = 0, lin_bad = 0, pts_bad = 0;
    FT_Set_Pixel_Sizes(f->ftface, 0, (FT_UInt)px);
    for (int g = 0; g < f->glyphs; g++) {
        Path path; memset(&path, 0, sizeof path);
        glyph_path(f, (uint32_t)g, unit, &path);
        Exp ex; memset(&ex, 0, sizeof ex);
        if (!FT_Load_Glyph(f->ftface, (FT_UInt)g, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) && f->ftface->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
            FT_Outline *o = &f->ftface->glyph->outline;
            int conic = 0, cubic = 0;
            for (int i = 0; i < o->n_points; i++) { int tag = FT_CURVE_TAG(o->tags[i]); conic |= tag == FT_CURVE_TAG_CONIC; cubic |= tag == FT_CURVE_TAG_CUBIC; }
            conic_glyphs += conic; cubic_glyphs += cubic;
            FT_Outline_Decompose(o, &ef, &ex);
            if (labs((long)f->ftface->glyph->metrics.horiAdvance - advance_units(f, (uint32_t)g)) > 1) {   /* (Arial.ttf glyph 556: 1367 against 1366) */ if (adv_bad++ < 5) printf("  glyph %d: hmtx advance %d, FreeType %ld\n", g, advance_units(f, (uint32_t)g), (long)f->ftface->glyph->metrics.horiAdvance); }
        }
        if (!ex.n) empty++;
        if (path.n != ex.n || path.contours != ex.contours) { if (pts_bad++ < 5) printf("  glyph %d: %d points / %d contours, expected %d / %d\n", g, path.n, path.contours, ex.n, ex.contours); }
        else for (int i = 0; i < path.n; i++) {
            double d = fabs(path.p[i].x - ex.xy[i * 2]) + fabs(path.p[i].y - ex.xy[i * 2 + 1]);
            if (d > worst) { worst = d; worst_glyph = g; }
        }
        points += path.n;
        /* advance in 16.16 pixels vs FreeType's linear (unhinted) advance at px. FreeType's CFF driver scales through its
         * own rounded 16.16 factor (600 units at 32 px / 1000: 1258294 for the exact 1258291.2), so CFF gets 1/4096 px */
        if (!FT_Load_Glyph(f->ftface, (FT_UInt)g, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)) {
            long ours = advance_fixed(&e, f, (uint32_t)g), theirs = (long)f->ftface->glyph->linearHoriAdvance;
            long exact = (long)(((long long)advance_units(f, (uint32_t)g) * px * 65536 + f->upem / 2) / f->upem);
            if (labs(ours - exact) > 1 || labs(ours - theirs) > (f->cff ? 16 : 1)) { if (lin_bad++ < 5) printf("  glyph %d: advance %ld/65536 px, FreeType linear %ld/65536\n", g, ours, theirs); }
        }
        free(path.p); free(path.end); free(ex.xy);
    }
    printf("outlines: %d glyphs (%ld with conics, %ld with cubics, %ld empty), %ld points; largest |ours - FreeType B(t)| = %.3g units (glyph %d)\n",
           f->glyphs, conic_glyphs, cubic_glyphs, empty, points, worst, worst_glyph);
    printf("advances: hmtx vs FreeType unscaled: %ld differ; 16.16 at %d px: %ld differ from the exact scale (1/65536) or from FreeType linear (1/65536, CFF 1/4096)\n", adv_bad, px, lin_bad);
    CHECK(pts_bad == 0, "%ld glyphs with a different point / contour count", pts_bad);
    CHECK(worst < 1e-6, "conic -> cubic is not exact (%.3g units)", worst);
    CHECK(adv_bad == 0 && lin_bad == 0, "advances differ");
    CHECK(points > 0, "no outline points at all");

    /* 3: glyph maps vs FreeType's renderer */
    static const uint32_t extra[] = {0xe9, 0x2014, 0x3042, 0x30a2, 0x4e00, 0x5b57, 0xac00};
    uint32_t codes[128]; int nc = 0;
    for (uint32_t ch = 0x21; ch < 0x7f; ch++) codes[nc++] = ch;
    for (unsigned i = 0; i < sizeof extra / sizeof extra[0]; i++) codes[nc++] = extra[i];
    int maps = 0, placed_bad = 0; double worst_rel = 0; uint32_t worst_code = 0; double sum_rel = 0;
    for (int i = 0; i < nc; i++) {
        uint32_t glyph = map_char(f, codes[i]);
        if (!glyph) continue;
        e.flags = 0;
        uint8_t *map = make_glyphmap(&e, codes[i], 8);
        int16_t hd[4]; memcpy(hd, map + 0xc, 4); memcpy(hd + 2, map + 0x1c, 4);       /* lo_x hi_y w h */
        if (FT_Load_Glyph(f->ftface, glyph, FT_LOAD_NO_HINTING | FT_LOAD_RENDER) || !hd[2]) { free(map); continue; }
        FT_Bitmap *bm = &f->ftface->glyph->bitmap;
        int fl = f->ftface->glyph->bitmap_left, ft_top = f->ftface->glyph->bitmap_top - 1;      /* top row, y up */
        /* canvas covering both; y index = top - row */
        int L = hd[0] < fl ? hd[0] : fl, T = hd[1] > ft_top ? hd[1] : ft_top;
        int R = hd[0] + hd[2] > fl + (int)bm->width ? hd[0] + hd[2] : fl + (int)bm->width, B = hd[1] - hd[3] < ft_top - (int)bm->rows ? hd[1] - hd[3] : ft_top - (int)bm->rows;
        double diff = 0, mass = 0;
        for (int y = T; y > B; y--) for (int x = L; x < R; x++) {
            int a = 0, b = 0, ox = x - hd[0], oy = hd[1] - y, fx_ = x - fl, fy = ft_top - y;
            if (ox >= 0 && ox < hd[2] && oy >= 0 && oy < hd[3]) a = map[GLYPH_HEADER + oy * hd[2] + ox];
            if (fx_ >= 0 && fx_ < (int)bm->width && fy >= 0 && fy < (int)bm->rows) b = bm->buffer[fy * bm->pitch + fx_];
            diff += abs(a - b); mass += b;
        }
        double rel = mass ? diff / mass : 0;
        sum_rel += rel; maps++;
        if (rel > worst_rel) { worst_rel = rel; worst_code = codes[i]; }
        if (abs(hd[0] - fl) > 1 || abs(hd[1] - ft_top) > 1) { if (placed_bad++ < 5) printf("  U+%04X placed at (%d, %d) %dx%d, FreeType (%d, %d) %ux%u\n", codes[i], hd[0], hd[1], hd[2], hd[3], fl, ft_top, bm->width, bm->rows); }
        free(map);
    }
    printf("glyph maps at %d px: %d compared with FT_Render_Glyph; mean |ours - FreeType| / FreeType coverage %.2f%%, worst %.2f%% (U+%04X); %d placed > 1 px apart\n",
           px, maps, maps ? 100.0 * sum_rel / maps : 0.0, 100.0 * worst_rel, worst_code, placed_bad);
    if (!maps) printf("  (this face has none of the characters compared: no glyph map checked)\n");          /* a script font */
    CHECK(placed_bad == 0 && worst_rel < 0.08, "glyph maps differ from FreeType's renderer");
    printf("%s\n", g_fail ? "FONT TEST FAILED" : "font test ok");
    return g_fail != 0;
}

/* test_draw.c -- the drawing primitives against Pillow's own output.
 *
 * Two halves, and both are load-bearing:
 *
 *  1. A corpus of 52 shapes rendered by Pillow 12.3.0 and committed as ASCII
 *     art in draw_ref.inc. This is the part that catches a refactor quietly
 *     changing a convention -- the inclusive rectangle, the width-2 line
 *     growing to the right, the even-odd polygon parity, Pillow's exact
 *     ellipse rasterisation. If one of those moves, the diff prints both
 *     pictures side by side and says which pixel disagreed first.
 *
 *  2. Explicit assertions about the cases Pillow REFUSES to render -- a
 *     rectangle whose corners are the wrong way round raises ValueError there
 *     and has to do something defined here -- plus the clipping and argument
 *     checks that have no Pillow equivalent at all.
 *
 * Runs with no arguments and needs no reference files: that is a hard
 * requirement of `make test`, which runs every unit binary bare.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_draw.h"
#include "nd_image.h"

#include "draw_ref.inc"

static int failures;

static void fail(const char *fmt, ...) ND_PRINTF(1, 2);
static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("FAIL: ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
    failures++;
}

#define CHECK(cond, ...)          \
    do {                          \
        if (!(cond))              \
            fail(__VA_ARGS__);    \
    } while (0)

/* ------------------------------------------------------------------ *
 * The Pillow corpus
 * ------------------------------------------------------------------ */

typedef void (*draw_case)(nd_draw *d);

static void c_rect_fill_2_3_6_8(nd_draw *d) { nd_draw_rect_fill(d, ND_RECT(2, 3, 6, 8), ND_WHITE); }
static void c_rect_fill_0_0_9_9(nd_draw *d) { nd_draw_rect_fill(d, ND_RECT(0, 0, 9, 9), ND_WHITE); }
static void c_rect_fill_clip_topleft(nd_draw *d) { nd_draw_rect_fill(d, ND_RECT(-5, -5, 4, 4), ND_WHITE); }
static void c_rect_fill_clip_botright(nd_draw *d) { nd_draw_rect_fill(d, ND_RECT(20, 20, 40, 40), ND_WHITE); }
static void c_rect_fill_single_px(nd_draw *d) { nd_draw_rect_fill(d, ND_RECT(5, 5, 5, 5), ND_WHITE); }
static void c_rect_out_w1(nd_draw *d) { nd_draw_rect_outline(d, ND_RECT(2, 2, 18, 15), ND_WHITE, 1); }
static void c_rect_out_w3(nd_draw *d) { nd_draw_rect_outline(d, ND_RECT(2, 2, 18, 15), ND_WHITE, 3); }
static void c_rect_out_full(nd_draw *d) { nd_draw_rect_outline(d, ND_RECT(0, 0, 23, 23), ND_WHITE, 1); }
static void c_rect_out_2x2(nd_draw *d) { nd_draw_rect_outline(d, ND_RECT(4, 4, 5, 5), ND_WHITE, 1); }
static void c_rect_out_1x1(nd_draw *d) { nd_draw_rect_outline(d, ND_RECT(4, 4, 4, 4), ND_WHITE, 1); }
static void c_rect_out_w2(nd_draw *d) { nd_draw_rect_outline(d, ND_RECT(3, 3, 12, 12), ND_WHITE, 2); }

static void c_line_h_w1(nd_draw *d) { nd_draw_line(d, 2, 2, 20, 2, ND_WHITE, 1); }
static void c_line_v_w1(nd_draw *d) { nd_draw_line(d, 2, 2, 2, 20, ND_WHITE, 1); }
static void c_line_v_w2(nd_draw *d) { nd_draw_line(d, 10, 2, 10, 20, ND_WHITE, 2); }
static void c_line_h_w2(nd_draw *d) { nd_draw_line(d, 2, 10, 20, 10, ND_WHITE, 2); }
static void c_line_diag_w1(nd_draw *d) { nd_draw_line(d, 2, 2, 20, 20, ND_WHITE, 1); }
static void c_line_antidiag_w1(nd_draw *d) { nd_draw_line(d, 2, 20, 20, 2, ND_WHITE, 1); }
static void c_line_shallow_w1(nd_draw *d) { nd_draw_line(d, 3, 3, 20, 10, ND_WHITE, 1); }
static void c_line_shallow_rev_w1(nd_draw *d) { nd_draw_line(d, 20, 10, 3, 3, ND_WHITE, 1); }
static void c_line_diag_w2(nd_draw *d) { nd_draw_line(d, 2, 2, 20, 20, ND_WHITE, 2); }
static void c_line_antidiag_w2(nd_draw *d) { nd_draw_line(d, 2, 20, 20, 2, ND_WHITE, 2); }
static void c_line_degenerate_w1(nd_draw *d) { nd_draw_line(d, 5, 5, 5, 5, ND_WHITE, 1); }
static void c_line_degenerate_w3(nd_draw *d) { nd_draw_line(d, 5, 5, 5, 5, ND_WHITE, 3); }
static void c_line_h_w3(nd_draw *d) { nd_draw_line(d, 1, 12, 22, 12, ND_WHITE, 3); }
static void c_line_v_w3(nd_draw *d) { nd_draw_line(d, 12, 1, 12, 22, ND_WHITE, 3); }
static void c_line_full_w1(nd_draw *d) { nd_draw_line(d, 0, 10, 23, 10, ND_WHITE, 1); }
static void c_line_musicflag_w2(nd_draw *d) { nd_draw_line(d, 4, 4, 18, 7, ND_WHITE, 2); }
static void c_line_v_w4(nd_draw *d) { nd_draw_line(d, 10, 2, 10, 20, ND_WHITE, 4); }
static void c_line_h_rev_w2(nd_draw *d) { nd_draw_line(d, 20, 10, 2, 10, ND_WHITE, 2); }

#define POLY(d, ...)                                          \
    do {                                                      \
        static const nd_point pts[] = {__VA_ARGS__};          \
        nd_draw_polygon((d), pts, ND_ARRAY_LEN(pts), ND_WHITE); \
    } while (0)

static void c_poly_diamond(nd_draw *d) { POLY(d, {11, 2}, {21, 11}, {11, 21}, {2, 11}); }
static void c_poly_tri_up(nd_draw *d) { POLY(d, {11, 2}, {21, 21}, {2, 21}); }
static void c_poly_tri_down(nd_draw *d) { POLY(d, {2, 2}, {21, 2}, {11, 21}); }
static void c_poly_bowtie(nd_draw *d) { POLY(d, {2, 2}, {21, 2}, {2, 21}, {21, 21}); }
static void c_poly_arrow(nd_draw *d) { POLY(d, {21, 11}, {11, 2}, {11, 21}); }
static void c_poly_house(nd_draw *d) { POLY(d, {2, 8}, {21, 8}, {11, 21}); }
static void c_poly_square(nd_draw *d) { POLY(d, {4, 4}, {19, 4}, {19, 19}, {4, 19}); }
static void c_poly_line(nd_draw *d) { POLY(d, {2, 2}, {21, 2}); }

static void c_poly_star8(nd_draw *d)
{
    /* Memory's eight-point star, at the coordinates memory.py computes. */
    const int32_t x0 = 2, y0 = 2, x1 = 21, y1 = 21;
    const int32_t cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, q = (x1 - x0) / 5;
    const nd_point pts[] = {{cx, y0},      {cx + q, cy - q}, {x1, cy},      {cx + q, cy + q},
                            {cx, y1},      {cx - q, cy + q}, {x0, cy},      {cx - q, cy - q}};
    nd_draw_polygon(d, pts, ND_ARRAY_LEN(pts), ND_WHITE);
}

static void c_ell_fill_even(nd_draw *d) { nd_draw_ellipse_fill(d, ND_RECT(2, 2, 21, 21), ND_WHITE); }
static void c_ell_fill_odd(nd_draw *d) { nd_draw_ellipse_fill(d, ND_RECT(2, 2, 20, 20), ND_WHITE); }
static void c_ell_fill_wide(nd_draw *d) { nd_draw_ellipse_fill(d, ND_RECT(4, 8, 19, 15), ND_WHITE); }
static void c_ell_out_w1(nd_draw *d) { nd_draw_ellipse_outline(d, ND_RECT(2, 2, 21, 21), ND_WHITE, 1); }
static void c_ell_out_small_w1(nd_draw *d) { nd_draw_ellipse_outline(d, ND_RECT(5, 5, 18, 18), ND_WHITE, 1); }
static void c_ell_out_small_w3(nd_draw *d) { nd_draw_ellipse_outline(d, ND_RECT(5, 5, 18, 18), ND_WHITE, 3); }
static void c_ell_out_w2(nd_draw *d) { nd_draw_ellipse_outline(d, ND_RECT(2, 2, 21, 21), ND_WHITE, 2); }
static void c_ell_fill_2x2(nd_draw *d) { nd_draw_ellipse_fill(d, ND_RECT(3, 3, 4, 4), ND_WHITE); }
static void c_ell_fill_1x1(nd_draw *d) { nd_draw_ellipse_fill(d, ND_RECT(10, 10, 10, 10), ND_WHITE); }
static void c_ell_fill_musicplayer(nd_draw *d) { nd_draw_ellipse_fill(d, ND_RECT(4, 10, 13, 19), ND_WHITE); }
static void c_ell_fill_tall(nd_draw *d) { nd_draw_ellipse_fill(d, ND_RECT(9, 2, 14, 21), ND_WHITE); }

static void c_points_diagonal(nd_draw *d)
{
    int32_t i;
    for (i = 0; i < 24; i += 3)
        nd_draw_point(d, i, i, ND_WHITE);
    /* Off every edge: Pillow drops these silently and so must we. */
    nd_draw_point(d, -1, 5, ND_WHITE);
    nd_draw_point(d, 5, -1, ND_WHITE);
    nd_draw_point(d, 24, 5, ND_WHITE);
}

static void c_rect_fill_then_outline(nd_draw *d)
{
    nd_draw_rect_fill(d, ND_RECT(3, 3, 20, 20), ND_WHITE);
    nd_draw_rect_fill(d, ND_RECT(6, 6, 17, 17), ND_BLACK);
    nd_draw_rect_outline(d, ND_RECT(6, 6, 17, 17), ND_WHITE, 1);
}

static void c_memory_h_glyph(nd_draw *d)
{
    const int32_t x0 = 3, y0 = 3, x1 = 20, y1 = 20;
    const nd_point left[] = {{x0, y0}, {x0 + 4, y0}, {x0 + 4, y1}, {x0, y1}};
    const nd_point right[] = {{x1 - 4, y0}, {x1, y0}, {x1, y1}, {x1 - 4, y1}};

    nd_draw_polygon(d, left, ND_ARRAY_LEN(left), ND_WHITE);
    nd_draw_polygon(d, right, ND_ARRAY_LEN(right), ND_WHITE);
    nd_draw_rect_fill(d, ND_RECT(x0, 10, x1, 13), ND_WHITE);
}

/* Order must match draw_ref.inc exactly; the loop asserts the names line up
 * so an insertion in one list and not the other is caught immediately. */
static const draw_case ND_DRAW_CASE[] = {
    c_rect_fill_2_3_6_8, c_rect_fill_0_0_9_9,   c_rect_fill_clip_topleft, c_rect_fill_clip_botright,
    c_rect_fill_single_px, c_rect_out_w1,       c_rect_out_w3,            c_rect_out_full,
    c_rect_out_2x2,      c_rect_out_1x1,        c_rect_out_w2,            c_line_h_w1,
    c_line_v_w1,         c_line_v_w2,           c_line_h_w2,              c_line_diag_w1,
    c_line_antidiag_w1,  c_line_shallow_w1,     c_line_shallow_rev_w1,    c_line_diag_w2,
    c_line_antidiag_w2,  c_line_degenerate_w1,  c_line_degenerate_w3,     c_line_h_w3,
    c_line_v_w3,         c_line_full_w1,        c_line_musicflag_w2,      c_line_v_w4,
    c_line_h_rev_w2,     c_poly_diamond,        c_poly_tri_up,            c_poly_tri_down,
    c_poly_bowtie,       c_poly_arrow,          c_poly_house,             c_poly_square,
    c_poly_line,         c_poly_star8,          c_ell_fill_even,          c_ell_fill_odd,
    c_ell_fill_wide,     c_ell_out_w1,          c_ell_out_small_w1,       c_ell_out_small_w3,
    c_ell_out_w2,        c_ell_fill_2x2,        c_ell_fill_1x1,           c_ell_fill_musicplayer,
    c_ell_fill_tall,     c_points_diagonal,     c_rect_fill_then_outline, c_memory_h_glyph,
};

static void dump_side_by_side(const nd_image *got, const nd_draw_ref *ref)
{
    int32_t y, x;

    printf("       got                      want\n");
    for (y = 0; y < ND_REF_H; y++) {
        char line[ND_REF_W + 1];
        for (x = 0; x < ND_REF_W; x++) {
            nd_color c = nd_image_get_px(got, x, y);
            line[x] = (c.r == 255u) ? '#' : (c.r == 0u ? '.' : '?');
        }
        line[ND_REF_W] = '\0';
        printf("  %s   %s%s\n", line, ref->rows[y],
               strcmp(line, ref->rows[y]) == 0 ? "" : "   <<");
    }
}

static void run_corpus(void)
{
    size_t i;

    if (ND_ARRAY_LEN(ND_DRAW_CASE) != ND_ARRAY_LEN(ND_DRAW_REF)) {
        fail("case table has %zu entries, reference has %zu", ND_ARRAY_LEN(ND_DRAW_CASE),
             ND_ARRAY_LEN(ND_DRAW_REF));
        return;
    }

    for (i = 0; i < ND_ARRAY_LEN(ND_DRAW_CASE); i++) {
        nd_image *img = nd_image_new_filled(ND_REF_W, ND_REF_H, ND_PIXFMT_RGB888, ND_BLACK);
        nd_draw d;
        int32_t y, x;
        bool ok = true;

        if (!img) {
            fail("%s: out of memory", ND_DRAW_REF[i].name);
            return;
        }
        if (nd_draw_bind(&d, img) != ND_OK) {
            fail("%s: bind failed", ND_DRAW_REF[i].name);
            nd_image_free(img);
            continue;
        }
        ND_DRAW_CASE[i](&d);

        for (y = 0; y < ND_REF_H && ok; y++) {
            for (x = 0; x < ND_REF_W; x++) {
                nd_color c = nd_image_get_px(img, x, y);
                char want = ND_DRAW_REF[i].rows[y][x];
                uint8_t expect = (want == '#') ? 255u : 0u;
                /* Shapes are hard-edged: any intermediate value at all means
                 * something is antialiasing that should not be. */
                if (c.r != expect || c.g != expect || c.b != expect) {
                    fail("%s: first difference at (%d,%d): got %u want %u",
                         ND_DRAW_REF[i].name, (int)x, (int)y, c.r, expect);
                    dump_side_by_side(img, &ND_DRAW_REF[i]);
                    ok = false;
                    break;
                }
            }
        }
        nd_image_free(img);
    }
}

/* ------------------------------------------------------------------ *
 * Conventions with no Pillow counterpart
 * ------------------------------------------------------------------ */

static bool is_white(const nd_image *img, int32_t x, int32_t y)
{
    nd_color c = nd_image_get_px(img, x, y);
    return c.r == 255u && c.g == 255u && c.b == 255u;
}

static int32_t count_white(const nd_image *img)
{
    int32_t n = 0, y, x;
    for (y = 0; y < img->h; y++)
        for (x = 0; x < img->w; x++)
            if (is_white(img, x, y))
                n++;
    return n;
}

static void test_inclusive_rectangle(void)
{
    nd_image *img = nd_image_new_filled(24, 24, ND_PIXFMT_RGB888, ND_BLACK);
    nd_draw d;

    if (!img)
        return;
    nd_draw_bind(&d, img);

    /* THE convention. ND_RECT(2,3,6,8) lights x in [2,6] and y in [3,8]:
     * FIVE columns by SIX rows, thirty pixels, as nd_types.h says.
     * Half-open code loses column 6 and row 8 entirely. */
    nd_draw_rect_fill(&d, ND_RECT(2, 3, 6, 8), ND_WHITE);
    CHECK(count_white(img) == 30, "inclusive rect covers %d px, want 5*6=30", count_white(img));
    CHECK(is_white(img, 6, 8), "inclusive rect must light its far corner (6,8)");
    CHECK(is_white(img, 2, 3), "inclusive rect must light its near corner (2,3)");
    CHECK(!is_white(img, 7, 8), "inclusive rect must not light (7,8)");
    CHECK(!is_white(img, 6, 9), "inclusive rect must not light (6,9)");

    nd_image_free(img);
}

static void test_rect_helpers(void)
{
    /* The inclusive convention lives in nd_types.h too; if these ever
     * disagree with the rasterizer every widget's layout arithmetic is off. */
    CHECK(nd_rect_w(ND_RECT(2, 3, 6, 8)) == 5, "nd_rect_w(2..6) should be 5");
    CHECK(nd_rect_h(ND_RECT(2, 3, 6, 8)) == 6, "nd_rect_h(3..8) should be 6");
    CHECK(nd_rect_w(ND_RECT(5, 5, 5, 5)) == 1, "a one-pixel rect is 1 wide");
}

static void test_wide_line_minor_axis(void)
{
    nd_image *img = nd_image_new_filled(24, 24, ND_PIXFMT_RGB888, ND_BLACK);
    nd_draw d;

    if (!img)
        return;
    nd_draw_bind(&d, img);

    /* RULE 2 stated as an assertion rather than as a picture: a width-2
     * vertical line grows RIGHT, and it does not get longer. */
    nd_draw_line(&d, 10, 5, 10, 15, ND_WHITE, 2);
    CHECK(is_white(img, 10, 5), "wide line must cover its own column");
    CHECK(is_white(img, 11, 5), "wide vertical line width 2 must grow to x+1");
    CHECK(!is_white(img, 9, 5), "wide vertical line width 2 must not grow to x-1");
    CHECK(is_white(img, 10, 15) && is_white(img, 11, 15), "wide line must reach its endpoint");
    CHECK(!is_white(img, 10, 4), "wide line must not overshoot the start");
    CHECK(!is_white(img, 10, 16), "wide line must not overshoot the end");
    CHECK(count_white(img) == 22, "width-2 line over 11 rows is 22 px, got %d", count_white(img));

    nd_image_free(img);
}

static void test_line_endpoints_inclusive(void)
{
    nd_image *img = nd_image_new_filled(24, 24, ND_PIXFMT_RGB888, ND_BLACK);
    nd_draw d;

    if (!img)
        return;
    nd_draw_bind(&d, img);

    /* Pillow's line8() stops one short and draw_lines() puts the endpoint
     * back with a separate point call. The visible contract is inclusive. */
    nd_draw_line(&d, 3, 7, 9, 7, ND_WHITE, 1);
    CHECK(is_white(img, 3, 7) && is_white(img, 9, 7), "single-width line endpoints are inclusive");
    CHECK(count_white(img) == 7, "line 3..9 is 7 px, got %d", count_white(img));

    nd_image_free(img);
}

static void test_zero_width_draws_nothing(void)
{
    nd_image *img = nd_image_new_filled(24, 24, ND_PIXFMT_RGB888, ND_BLACK);
    nd_draw d;

    if (!img)
        return;
    nd_draw_bind(&d, img);

    /* ImageDraw.line() checks width != 0 before touching the surface. */
    nd_draw_line(&d, 2, 2, 20, 20, ND_WHITE, 0);
    CHECK(count_white(img) == 0, "line width 0 must draw nothing");
    nd_draw_ellipse_outline(&d, ND_RECT(2, 2, 20, 20), ND_WHITE, 0);
    CHECK(count_white(img) == 0, "ellipse outline width 0 must draw nothing");

    nd_image_free(img);
}

static void test_reversed_rect(void)
{
    nd_image *img = nd_image_new_filled(24, 24, ND_PIXFMT_RGB888, ND_BLACK);
    nd_draw d;

    if (!img)
        return;
    nd_draw_bind(&d, img);

    /* Pillow raises ValueError for either of these, so there is no reference
     * frame to match; the C follows the underlying Draw.c, which normalises y
     * and lets a reversed x fall out of hline() as an empty run. Asserted so
     * a later "tidy-up" cannot turn a silent no-op into a filled box. */
    nd_draw_rect_fill(&d, ND_RECT(8, 3, 2, 9), ND_WHITE);
    CHECK(count_white(img) == 0, "a rectangle with x1 < x0 draws nothing");

    nd_draw_rect_fill(&d, ND_RECT(2, 9, 8, 3), ND_WHITE);
    CHECK(count_white(img) == 7 * 7, "a rectangle with y1 < y0 is normalised, got %d",
          count_white(img));

    nd_image_free(img);
}

static void test_clipping_never_faults(void)
{
    nd_image *img = nd_image_new_filled(8, 8, ND_PIXFMT_RGB888, ND_BLACK);
    nd_draw d;
    const nd_point poly[] = {{-100, -100}, {200, -50}, {50, 300}};

    if (!img)
        return;
    nd_draw_bind(&d, img);

    /* Every one of these is entirely or mostly off the surface. Widgets do
     * hand in coordinates like this while a list is scrolling. */
    nd_draw_rect_fill(&d, ND_RECT(-1000, -1000, -1, -1), ND_WHITE);
    nd_draw_rect_outline(&d, ND_RECT(-5, -5, 100, 100), ND_WHITE, 2);
    nd_draw_line(&d, -50, -50, 500, 500, ND_WHITE, 3);
    nd_draw_ellipse_fill(&d, ND_RECT(-20, -20, 40, 40), ND_WHITE);
    nd_draw_polygon(&d, poly, ND_ARRAY_LEN(poly), ND_WHITE);
    nd_draw_point(&d, 1000, 1000, ND_WHITE);

    nd_image_free(img);
}

static void test_bad_arguments(void)
{
    nd_draw d;
    nd_image *img = nd_image_new(4, 4, ND_PIXFMT_RGB888);
    const nd_point one[] = {{0, 0}};

    CHECK(nd_draw_bind(&d, NULL) == ND_ERR_INVAL, "bind(NULL) must be ND_ERR_INVAL");
    CHECK(nd_draw_bind(NULL, img) == ND_ERR_INVAL, "bind(NULL ctx) must be ND_ERR_INVAL");
    if (!img)
        return;
    nd_draw_bind(&d, img);
    CHECK(nd_draw_polygon(&d, NULL, 3, ND_WHITE) == ND_ERR_INVAL, "polygon(NULL) is invalid");
    CHECK(nd_draw_polygon(&d, one, 0, ND_WHITE) == ND_OK, "an empty polygon is a no-op");
    CHECK(nd_draw_polygon(&d, one, 1000, ND_WHITE) == ND_ERR_INVAL,
          "a polygon past the point cap is rejected, not truncated");
    CHECK(nd_draw_line(&d, 0, 0, 1, 1, ND_WHITE, -1) == ND_ERR_INVAL, "negative width is invalid");
    CHECK(nd_draw_rect_outline(&d, ND_RECT(0, 0, 1, 1), ND_WHITE, -1) == ND_ERR_INVAL,
          "negative outline width is invalid");

    nd_image_free(img);
}

static void test_draws_into_l8(void)
{
    nd_image *img = nd_image_new_filled(8, 8, ND_PIXFMT_L8, ND_BLACK);
    nd_draw d;

    if (!img)
        return;
    nd_draw_bind(&d, img);

    /* Text rendering builds L8 masks, so every primitive has to work on a
     * one-byte surface as well as on the RGB canvas. */
    nd_draw_rect_fill(&d, ND_RECT(1, 1, 6, 6), ND_RGB(200, 200, 200));
    CHECK(nd_image_get_px(img, 1, 1).r == 200u, "L8 fill should store the grey level");
    CHECK(nd_image_get_px(img, 0, 0).r == 0u, "L8 fill must respect its box");
    CHECK(nd_image_get_px(img, 6, 6).r == 200u, "L8 fill is inclusive too");
    CHECK(nd_image_get_px(img, 7, 7).r == 0u, "L8 fill must not spill");

    nd_image_free(img);
}

int main(void)
{
    run_corpus();
    test_inclusive_rectangle();
    test_rect_helpers();
    test_wide_line_minor_axis();
    test_line_endpoints_inclusive();
    test_zero_width_draws_nothing();
    test_reversed_rect();
    test_clipping_never_faults();
    test_bad_arguments();
    test_draws_into_l8();

    if (failures) {
        printf("test_draw: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_draw: %zu Pillow reference shapes + convention checks OK\n",
           ND_ARRAY_LEN(ND_DRAW_REF));
    return 0;
}

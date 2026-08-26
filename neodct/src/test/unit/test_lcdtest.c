/* test_lcdtest.c -- the LCDTest engineering app, app id 9001.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. The four patterns are main.py's, in main.py's order, with main.py's
 *     eleven hex colours. The order is what the third press of Next shows and
 *     the colours are what a dead subpixel is judged against, so both are
 *     asserted literally rather than through a frame.
 *
 *  2. The two geometry rules. top_h is int(content_bottom * 0.7) = 101 on
 *     this panel -- NOT 102, which is what a rounding port produces -- and
 *     mid_y is 123.
 *
 *  3. Column spans. Seven bars over 240 px is 34 each with the seventh
 *     stretched to the edge, and the boxes overlap by one column because
 *     Pillow's rectangle is inclusive of both corners.
 *
 *  4. The TV card really lands where 2 and 3 say it does, checked at the
 *     seams by reading pixels back off the canvas.
 *
 *  5. THE GOLDEN FRAME. eng-lcdtest is the FIRST pattern, red, committed
 *     TWICE -- softkey.update() presents and then fb.update() presents the
 *     same pixels again. Both the frame count and the digest are pinned,
 *     because collapsing the double present would still produce a matching
 *     image and would silently change the virtual clock's tick count.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set.
 */

#include <stdio.h>
#include <string.h>

#include "smallapp_test.h"

#include "../../apps/LCDTest/lcdtest.h"

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    int32_t (*top_h)(int32_t);
    int32_t (*mid_y)(int32_t);
    void (*span)(int32_t, int32_t, int32_t, int32_t *, int32_t *);
    void (*draw_color)(nd_ui *, nd_color);
    void (*draw_tv)(nd_ui *);
    const char *const *names;
    const nd_color *fills;
    const nd_color *bars;
    const nd_color *stripes;
    const nd_color *band;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&api.top_h = sa_sym(h, "nd_lcdtest_top_h");
    *(void **)&api.mid_y = sa_sym(h, "nd_lcdtest_mid_y");
    *(void **)&api.span = sa_sym(h, "nd_lcdtest_span");
    *(void **)&api.draw_color = sa_sym(h, "nd_lcdtest_draw_color");
    *(void **)&api.draw_tv = sa_sym(h, "nd_lcdtest_draw_tv");
    api.names = dlsym(h, "nd_lcdtest_names");
    api.fills = dlsym(h, "nd_lcdtest_fills");
    api.bars = dlsym(h, "nd_lcdtest_bars");
    api.stripes = dlsym(h, "nd_lcdtest_stripes");
    api.band = dlsym(h, "nd_lcdtest_band");

    return api.run != NULL && api.shutdown != NULL && api.top_h != NULL && api.mid_y != NULL &&
           api.span != NULL && api.draw_color != NULL && api.draw_tv != NULL &&
           api.names != NULL && api.fills != NULL && api.bars != NULL && api.stripes != NULL &&
           api.band != NULL;
}

static void check_rgb(nd_color got, uint8_t r, uint8_t g, uint8_t b, const char *what)
{
    sa_checks++;
    if (got.r != r || got.g != g || got.b != b) {
        sa_failures++;
        fprintf(stderr, "FAIL %s: got #%02X%02X%02X want #%02X%02X%02X\n", what, got.r, got.g,
                got.b, r, g, b);
    }
}

/* ------------------------------------------------------------------ *
 * 1. The patterns
 * ------------------------------------------------------------------ */

static void test_patterns(void)
{
    CHECK_STR(api.names[0], "Red", "patterns[0]");
    CHECK_STR(api.names[1], "Green", "patterns[1]");
    CHECK_STR(api.names[2], "Blue", "patterns[2]");
    CHECK_STR(api.names[3], "TV Test", "patterns[3]");

    check_rgb(api.fills[0], 0xFF, 0x00, 0x00, "Red is #FF0000");
    check_rgb(api.fills[1], 0x00, 0xFF, 0x00, "Green is #00FF00");
    check_rgb(api.fills[2], 0x00, 0x00, 0xFF, "Blue is #0000FF");

    /* The SMPTE order: white, yellow, cyan, green, magenta, red, blue. */
    check_rgb(api.bars[0], 0xFF, 0xFF, 0xFF, "bars[0]");
    check_rgb(api.bars[1], 0xFF, 0xFF, 0x00, "bars[1]");
    check_rgb(api.bars[2], 0x00, 0xFF, 0xFF, "bars[2]");
    check_rgb(api.bars[3], 0x00, 0xFF, 0x00, "bars[3]");
    check_rgb(api.bars[4], 0xFF, 0x00, 0xFF, "bars[4]");
    check_rgb(api.bars[5], 0xFF, 0x00, 0x00, "bars[5]");
    check_rgb(api.bars[6], 0x00, 0x00, 0xFF, "bars[6]");

    check_rgb(api.stripes[0], 0x00, 0x21, 0x4A, "stripes[0] #00214A");
    check_rgb(api.stripes[1], 0xFF, 0xFF, 0xFF, "stripes[1] #FFFFFF");
    check_rgb(api.stripes[2], 0x32, 0x00, 0x6A, "stripes[2] #32006A");
    check_rgb(api.stripes[3], 0x00, 0x00, 0x00, "stripes[3] #000000");
    check_rgb(*api.band, 0x20, 0x20, 0x20, "the #202020 band");
}

/* ------------------------------------------------------------------ *
 * 2 and 3. Geometry
 * ------------------------------------------------------------------ */

static void test_geometry(void)
{
    /* 145 * 0.7 is 101.49999999999999 as a double AND int() truncates, so
     * both roads reach 101. A port that rounds gets 102 and shifts every
     * stripe below it by a pixel. */
    CHECK_INT(api.top_h(145), 101, "int(145 * 0.7)");
    CHECK_INT(api.mid_y(145), 123, "101 + (145 - 101) // 2");
    /* A taller panel, to show the rule is not two constants: int(200*0.7)
     * is 140 exactly and 140 + 60//2 = 170. */
    CHECK_INT(api.top_h(200), 140, "int(200 * 0.7)");
    CHECK_INT(api.mid_y(200), 170, "mid_y at 200");
}

static void expect_span(int32_t n, int32_t i, int32_t want0, int32_t want1)
{
    int32_t x0 = -1;
    int32_t x1 = -1;
    char what[64];

    api.span(240, n, i, &x0, &x1);
    (void)snprintf(what, sizeof what, "span(240, %d, %d).x0", n, i);
    CHECK_INT(x0, want0, what);
    (void)snprintf(what, sizeof what, "span(240, %d, %d).x1", n, i);
    CHECK_INT(x1, want1, what);
}

static void test_spans(void)
{
    /* 240 // 7 = 34, and 7 * 34 = 238 -- the last bar is stretched to 240 so
     * the two columns the division loses do not show whatever was under
     * them. */
    expect_span(7, 0, 0, 34);
    expect_span(7, 1, 34, 68);
    expect_span(7, 5, 170, 204);
    expect_span(7, 6, 204, 240);
    /* 240 // 4 = 60 divides exactly, so only the last box's end differs from
     * (i+1)*w by nothing at all -- and it is still written as screen_w. */
    expect_span(4, 0, 0, 60);
    expect_span(4, 3, 180, 240);

    /* max(1, ...): a panel narrower than the bar count still paints. */
    {
        int32_t x0 = -1;
        int32_t x1 = -1;

        api.span(4, 7, 0, &x0, &x1);
        CHECK_INT(x0, 0, "narrow panel, bar 0 x0");
        CHECK_INT(x1, 1, "narrow panel, bar 0 is 1 px wide, not 0");
    }
}

/* ------------------------------------------------------------------ *
 * 4. The card really lands there
 * ------------------------------------------------------------------ */

static void test_tv_pattern(void)
{
    sa_fixture fx;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }

    api.draw_tv(&fx.ui);

    /* Row 0 walks the bars. x=34 is the seam between bar 0 and bar 1: both
     * rectangles cover it and the later one wins, so it is yellow. */
    check_rgb(nd_image_get_px(fx.canvas, 0, 0), 0xFF, 0xFF, 0xFF, "TV (0,0) white");
    check_rgb(nd_image_get_px(fx.canvas, 33, 0), 0xFF, 0xFF, 0xFF, "TV (33,0) still white");
    check_rgb(nd_image_get_px(fx.canvas, 34, 0), 0xFF, 0xFF, 0x00, "TV (34,0) the seam is yellow");
    check_rgb(nd_image_get_px(fx.canvas, 239, 0), 0x00, 0x00, 0xFF, "TV (239,0) blue");

    /* Row 101 is top_h and belongs to BOTH the bars and the #202020 band --
     * the band is drawn second and wins. */
    check_rgb(nd_image_get_px(fx.canvas, 10, 100), 0xFF, 0xFF, 0xFF, "TV (10,100) last bar row");
    check_rgb(nd_image_get_px(fx.canvas, 10, 101), 0x20, 0x20, 0x20, "TV (10,101) band over bar");
    check_rgb(nd_image_get_px(fx.canvas, 10, 122), 0x20, 0x20, 0x20, "TV (10,122) band");

    /* Row 123 is mid_y and belongs to both the band and the stripes; the
     * stripes are drawn second. */
    check_rgb(nd_image_get_px(fx.canvas, 10, 123), 0x00, 0x21, 0x4A, "TV (10,123) stripe 0");
    check_rgb(nd_image_get_px(fx.canvas, 70, 144), 0xFF, 0xFF, 0xFF, "TV (70,144) stripe 1");
    check_rgb(nd_image_get_px(fx.canvas, 130, 130), 0x32, 0x00, 0x6A, "TV (130,130) stripe 2");
    check_rgb(nd_image_get_px(fx.canvas, 239, 145), 0x00, 0x00, 0x00, "TV (239,145) stripe 3");

    /* Row 146 is the softkey's and the card never reaches it. */
    check_rgb(nd_image_get_px(fx.canvas, 10, 146), 0x00, 0x00, 0x00, "TV stops at content_bottom");

    sa_fx_free(&fx);
}

static void test_flood_stops_at_content_bottom(void)
{
    sa_fixture fx;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    /* Prove row 145 IS painted and row 146 is not: the app relies on the
     * softkey bar repainting 145..174 immediately afterwards. */
    api.draw_color(&fx.ui, ND_RGB(0xFF, 0x00, 0x00));
    check_rgb(nd_image_get_px(fx.canvas, 0, 144), 0xFF, 0x00, 0x00, "flood row 144");
    check_rgb(nd_image_get_px(fx.canvas, 0, 145), 0xFF, 0x00, 0x00, "flood row 145 inclusive");
    check_rgb(nd_image_get_px(fx.canvas, 0, 146), 0x00, 0x00, 0x00, "flood stops at 146");
    check_rgb(nd_image_get_px(fx.canvas, 239, 0), 0xFF, 0x00, 0x00, "flood reaches the last col");
    sa_fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 5. The golden frame
 * ------------------------------------------------------------------ */

static void test_golden_frame(void)
{
    sa_fixture fx;
    int rc;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    /* A held Back: LCDTest blocks in wait_for_key() with the frame already
     * committed, which is where uistub's ScriptExhausted caught the Python. */
    if (!sa_hold(&fx, ND_KEY_CLEAR)) {
        CHECK(false, "held key");
        sa_fx_free(&fx);
        return;
    }

    nd_vclock_enable();
    rc = api.run(&fx.ui);

    CHECK_INT(rc, 0, "Back returns 0");
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 2, "TWO frames: softkey.update() then fb.update()");
    /* The two are the same pixels, so both match -- and that is the point:
     * only the count can tell the double present was kept. */
    sa_expect_golden(&fx, nd_capture_recent(fx.cap, 0u), "eng-lcdtest");
    sa_expect_golden(&fx, nd_capture_recent(fx.cap, 1u), "eng-lcdtest");

    nd_vclock_disable();
    sa_fx_free(&fx);
}

/* Next steps the pattern and wraps. Driven by queueing four Enters ahead of
 * the held Back: nothing in this app flushes the channel, so a queued script
 * arrives in order. Four Enters is a full cycle, so the frame after them is
 * red again. */
static void test_next_cycles(void)
{
    static const int32_t KEYS[] = {ND_KEY_ENTER, ND_KEY_ENTER, ND_KEY_ENTER, ND_KEY_ENTER};
    sa_fixture fx;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    if (!sa_send_all(&fx, KEYS, ND_ARRAY_LEN(KEYS)) || !sa_hold(&fx, ND_KEY_CLEAR)) {
        CHECK(false, "key script");
        sa_fx_free(&fx);
        return;
    }

    nd_vclock_enable();
    CHECK_INT(api.run(&fx.ui), 0, "run returns 0");
    /* Five patterns drawn (red, green, blue, TV, red again), two commits
     * each. */
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 10, "five patterns at two commits each");
    check_rgb(nd_image_get_px(fx.canvas, 0, 0), 0xFF, 0x00, 0x00, "the fifth pattern is red again");
    nd_vclock_disable();
    sa_fx_free(&fx);
}

static void test_null_safety(void)
{
    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown();
    api.draw_color(NULL, ND_BLACK);
    api.draw_tv(NULL);
    api.span(240, 7, 0, NULL, NULL);
    sa_checks++;
}

int main(void)
{
    void *h = sa_begin("LCDTest", "ndlcdtest");

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }

    RUN(test_patterns);
    RUN(test_geometry);
    RUN(test_spans);
    RUN(test_tv_pattern);
    RUN(test_flood_stops_at_content_bottom);
    RUN(test_golden_frame);
    RUN(test_next_cycles);
    RUN(test_null_safety);

    return sa_end(h, "test_lcdtest");
}

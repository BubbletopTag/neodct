/* test_launchfx.c -- the glass themes' launch transition, and the framebuffer
 * readback it dissolves out of.
 *
 * What is pinned is what a person would notice if it broke: the last frame is
 * the app's frame exactly (a transition that ends on a blend leaves the app
 * looking smeared until its next repaint), the first is the outgoing screen,
 * the middle frames are neither, and the pop really does overshoot. The look
 * of the in-between frames is not pinned -- that is a design choice, and the
 * way to review it is to render it.
 *
 * Runs with no arguments and touches no files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_capture.h" /* nd_fb_open_mem() */
#include "nd_fb.h"
#include "nd_image.h"
#include "nd_launchfx.h"
#include "nd_types.h"

/* nd_fb_open_sink() is libneodct-internal; see test_nd_fb.c. */
#include "../../lib/nd_fb_priv.h"

static int failures;
static int checks;

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

#define CHECK(cond, ...)       \
    do {                       \
        checks++;              \
        if (!(cond))           \
            fail(__VA_ARGS__); \
    } while (0)

#define W 240
#define H 175

/* Structured enough that a blur or a shift changes it everywhere: a
 * checkerboard of 8-pixel cells in two colours. */
static nd_image *checker(nd_color a, nd_color b)
{
    nd_image *img = nd_image_new(W, H, ND_PIXFMT_RGB888);
    int32_t x;
    int32_t y;

    if (img == NULL)
        return NULL;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            nd_image_set_px(img, x, y, (((x >> 3) ^ (y >> 3)) & 1) != 0 ? a : b);
    return img;
}

static bool same(const nd_image *a, const nd_image *b)
{
    int32_t y;

    for (y = 0; y < a->h; y++)
        if (memcmp(a->pixels + (size_t)y * a->stride, b->pixels + (size_t)y * b->stride,
                   (size_t)a->w * 3u) != 0)
            return false;
    return true;
}

static void test_the_ends_are_exact(void)
{
    nd_image *before = checker(ND_RGB(255, 0, 0), ND_RGB(0, 0, 255));
    nd_image *after = checker(ND_RGB(255, 255, 255), ND_RGB(0, 128, 0));
    nd_image *frame = nd_image_new(W, H, ND_PIXFMT_RGB888);
    nd_launchfx *fx = nd_launchfx_new(before, after);
    int32_t step;

    CHECK(fx != NULL && frame != NULL, "setup");
    if (fx == NULL || frame == NULL)
        goto out;

    CHECK(nd_launchfx_frame(fx, frame, 0, ND_LAUNCHFX_STEPS) == ND_OK, "step 0");
    CHECK(same(frame, before), "step 0 is not the outgoing screen");
    for (step = 1; step < ND_LAUNCHFX_STEPS; step++) {
        CHECK(nd_launchfx_frame(fx, frame, step, ND_LAUNCHFX_STEPS) == ND_OK, "step %d", step);
        CHECK(!same(frame, before) && !same(frame, after), "step %d is an end frame", step);
    }
    /* Out of order and repeated, the way a caller that stopped early and
     * started again would ask: the cached blur must not leak between. */
    CHECK(nd_launchfx_frame(fx, frame, ND_LAUNCHFX_STEPS, ND_LAUNCHFX_STEPS) == ND_OK, "last");
    CHECK(same(frame, after), "the last step is not the app's frame");
    CHECK(nd_launchfx_frame(fx, frame, 3, ND_LAUNCHFX_STEPS) == ND_OK, "rewind");
    CHECK(nd_launchfx_frame(fx, frame, 99, ND_LAUNCHFX_STEPS) == ND_OK, "past the end");
    CHECK(same(frame, after), "past the end is not clamped to the app's frame");

out:
    nd_launchfx_free(fx);
    nd_image_free(frame);
    nd_image_free(before);
    nd_image_free(after);
}

/* The caller's buffers are snapshotted: the app goes on drawing into its
 * canvas the moment the transition is set up. */
static void test_the_inputs_are_copied(void)
{
    nd_image *before = checker(ND_RGB(10, 20, 30), ND_RGB(40, 50, 60));
    nd_image *after = checker(ND_RGB(200, 100, 0), ND_RGB(0, 100, 200));
    nd_image *keep = nd_image_copy(after);
    nd_image *frame = nd_image_new(W, H, ND_PIXFMT_RGB888);
    nd_launchfx *fx = nd_launchfx_new(before, after);

    CHECK(fx != NULL && keep != NULL && frame != NULL, "setup");
    if (fx != NULL && keep != NULL && frame != NULL) {
        (void)nd_image_fill(after, ND_RGB(1, 2, 3));
        (void)nd_launchfx_frame(fx, frame, ND_LAUNCHFX_STEPS, ND_LAUNCHFX_STEPS);
        CHECK(same(frame, keep), "the transition read the caller's buffer late");
    }
    nd_launchfx_free(fx);
    nd_image_free(frame);
    nd_image_free(keep);
    nd_image_free(before);
    nd_image_free(after);
}

static void test_mismatched_frames_are_refused(void)
{
    nd_image *a = nd_image_new(W, H, ND_PIXFMT_RGB888);
    nd_image *b = nd_image_new(W, H - 1, ND_PIXFMT_RGB888);
    nd_image *l = nd_image_new(W, H, ND_PIXFMT_L8);
    nd_launchfx *fx;

    CHECK(nd_launchfx_new(a, b) == NULL, "different sizes accepted");
    CHECK(nd_launchfx_new(a, l) == NULL, "a one-channel frame accepted");
    CHECK(nd_launchfx_new(NULL, a) == NULL, "NULL accepted");
    fx = nd_launchfx_new(a, a);
    CHECK(fx != NULL, "a legal pair refused");
    CHECK(nd_launchfx_frame(fx, b, 1, ND_LAUNCHFX_STEPS) == ND_ERR_INVAL, "wrong-size dst");
    CHECK(nd_launchfx_frame(fx, a, 1, 0) == ND_ERR_INVAL, "zero steps");
    nd_launchfx_free(fx);
    nd_launchfx_free(NULL);
    nd_image_free(a);
    nd_image_free(b);
    nd_image_free(l);
}

/* The pop: small at the start, over full size somewhere in the middle, and
 * exactly full size at the end. */
static void test_the_scale_pops(void)
{
    double peak = 0.0;
    int i;

    CHECK(nd_launchfx_scale(0.0) < 1.0, "does not start small");
    CHECK(nd_launchfx_scale(1.0) == 1.0, "does not end at full size");
    for (i = 1; i < 100; i++) {
        double s = nd_launchfx_scale((double)i / 100.0);
        if (s > peak)
            peak = s;
    }
    CHECK(peak > 1.005 && peak < 1.05, "overshoot %f is not a pop", peak);
    CHECK(nd_launchfx_ease(0.0) == 0.0 && nd_launchfx_ease(1.0) == 1.0, "ease ends");
}

/* nd_fb_read() hands back what nd_fb_update() wrote: exactly at 32bpp, and to
 * the precision of the panel at 16. */
static void test_readback(void)
{
    static const int32_t depths[] = {32, 16};
    nd_image *src = checker(ND_RGB(250, 130, 7), ND_RGB(3, 66, 199));
    size_t i;

    for (i = 0u; i < sizeof depths / sizeof depths[0]; i++) {
        nd_fb *fb = NULL;
        nd_image *back = nd_image_new(W, H, ND_PIXFMT_RGB888);
        nd_color got;

        CHECK(nd_fb_open_mem(&fb, W, 240, depths[i], 0u) == ND_OK, "%d bpp open", depths[i]);
        if (fb == NULL || back == NULL || src == NULL) {
            nd_fb_close(fb);
            nd_image_free(back);
            continue;
        }
        CHECK(nd_fb_update(fb, src) == ND_OK, "%d bpp update", depths[i]);
        CHECK(nd_fb_read(fb, back) == ND_OK, "%d bpp read", depths[i]);
        got = nd_image_get_px(back, 0, 0);
        if (depths[i] == 32) {
            CHECK(same(back, src), "32 bpp readback differs");
        } else {
            /* (0,0) is the checker's second colour, 3,66,199. */
            CHECK(got.r == (3 & 0xF8) && got.g == (66 & 0xFC) && got.b == (199 & 0xF8),
                  "16 bpp readback %d,%d,%d", got.r, got.g, got.b);
        }
        nd_fb_close(fb);
        nd_image_free(back);
    }
    nd_image_free(src);
}

/* A sink has nothing to read, and says so rather than inventing black: the
 * launch then simply happens without its transition. */
static nd_err discard(void *ctx, const nd_image *src)
{
    (void)ctx;
    (void)src;
    return ND_OK;
}

static void test_a_sink_has_nothing_to_read(void)
{
    nd_fb *fb = NULL;
    nd_image *back = nd_image_new(W, H, ND_PIXFMT_RGB888);

    CHECK(nd_fb_open_sink(&fb, W, H, 32, discard, NULL) == ND_OK, "sink open");
    if (fb != NULL && back != NULL)
        CHECK(nd_fb_read(fb, back) == ND_ERR_UNSUPPORTED, "a sink read back");
    nd_fb_close(fb);
    nd_image_free(back);
}

int main(void)
{
    test_the_ends_are_exact();
    test_the_inputs_are_copied();
    test_mismatched_frames_are_refused();
    test_the_scale_pops();
    test_readback();
    test_a_sink_has_nothing_to_read();

    printf("test_launchfx: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

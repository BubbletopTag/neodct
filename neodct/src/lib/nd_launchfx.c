/* nd_launchfx.c -- the blur-and-pop app launch. See nd_launchfx.h for what it
 * looks like and why it is a theme switch; this is how a frame is computed.
 *
 * ============ WHY A BOX BLUR, TWICE ============
 *
 * A running-sum box blur costs the same at radius 8 as at radius 1, which is
 * the only property that matters on a Cortex-A7 at 50 frames a second. One
 * pass has a visible square footprint; two in a row is a tent, and at these
 * radii on a 240-pixel screen a tent cannot be told from a gaussian.
 *
 * ============ WHY THE RESAMPLE CARRIES COVERAGE ============
 *
 * While the arriving frame is smaller than the screen its edge falls between
 * pixels, and a nearest or clamped sample there draws a hard, crawling line
 * round a picture that is supposed to be out of focus. So every bilinear tap
 * that lands outside the source counts as transparent rather than as the
 * nearest edge pixel, and the edge comes out anti-aliased for free.
 *
 * All of it is integer: 16.16 source coordinates, 8-bit filter weights and
 * 8-bit alpha, because there is no FPU budget per pixel worth spending here.
 */

#include "nd_launchfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct nd_launchfx {
    nd_image *before; /* the outgoing screen, as it was         */
    nd_image *after;  /* the app's first frame, as it will be   */
    nd_image *bg;     /* `before`, defocused for this frame     */
    nd_image *fg;     /* `after`, defocused for this frame      */
    nd_image *tmp;    /* the blur's halfway point               */
    int32_t bg_r;     /* the radius bg was last blurred at      */
    int32_t fg_r;     /* and fg; -1 before the first frame      */
    uint8_t *row;     /* one row of inputs for a horizontal pass */
    uint32_t *sums;   /* one running sum per byte of a row      */
    int32_t *map_x;   /* per-column 16.16 source x, this frame  */
    int32_t *map_y;   /* per-row    16.16 source y, this frame  */
};

/* ------------------------------------------------------------------ *
 * Curves
 * ------------------------------------------------------------------ */

double nd_launchfx_ease(double t)
{
    double u;

    if (t <= 0.0)
        return 0.0;
    if (t >= 1.0)
        return 1.0;
    u = 1.0 - t;
    return 1.0 - u * u * u;
}

/* easeOutBack, the textbook curve: it passes full size at about two thirds
 * of the way and comes back from roughly 2% over, which on a 240-pixel
 * screen is five pixels of pop -- felt rather than watched. */
double nd_launchfx_scale(double t)
{
    const double c1 = 1.70158;
    const double c3 = c1 + 1.0;
    double u;

    if (t <= 0.0)
        return ND_LAUNCHFX_SCALE0;
    if (t >= 1.0)
        return 1.0;
    u = t - 1.0;
    return ND_LAUNCHFX_SCALE0 + (1.0 - ND_LAUNCHFX_SCALE0) * (1.0 + c3 * u * u * u + c1 * u * u);
}

/* ------------------------------------------------------------------ *
 * Blur
 * ------------------------------------------------------------------ */

/* 2^20 / window, so a pass divides by multiplying. The largest window sum
 * is 255 * 17, which times this stays well inside 32 bits. */
static uint32_t box_mul(int32_t r)
{
    uint32_t win = (uint32_t)(2 * r + 1);

    return ((1u << 20) + win / 2u) / win;
}

/* One horizontal box pass over every row of an RGB888 image, in place, the
 * edges clamped. `row` holds one row's inputs so the pass can overwrite. */
static void box_h(nd_image *img, int32_t r, uint8_t *row)
{
    uint32_t mul = box_mul(r);
    int32_t last = img->w - 1;
    int32_t y;

    for (y = 0; y < img->h; y++) {
        uint8_t *p = img->pixels + (size_t)y * img->stride;
        int32_t c;

        memcpy(row, p, (size_t)img->w * 3u);
        for (c = 0; c < 3; c++) {
            uint32_t sum = 0u;
            int32_t i;

            for (i = -r; i <= r; i++)
                sum += row[(size_t)nd_clamp32(i, 0, last) * 3u + (size_t)c];
            for (i = 0; i < img->w; i++) {
                p[(size_t)i * 3u + (size_t)c] = (uint8_t)((sum * mul + (1u << 19)) >> 20);
                sum += row[(size_t)nd_min32(i + r + 1, last) * 3u + (size_t)c];
                sum -= row[(size_t)nd_max32(i - r, 0) * 3u + (size_t)c];
            }
        }
    }
}

/* One vertical box pass from src into dst, walking ROWS with a running sum
 * per column rather than walking each column: a column walk strides 720
 * bytes per step, which on the A7's 32 KB data cache is a miss per pixel. */
static void box_v(const nd_image *src, nd_image *dst, int32_t r, uint32_t *sums)
{
    uint32_t mul = box_mul(r);
    size_t n = (size_t)src->w * 3u;
    int32_t last = src->h - 1;
    int32_t y;
    int32_t i;
    size_t k;

    memset(sums, 0, n * sizeof *sums);
    for (i = -r; i <= r; i++) {
        const uint8_t *q = src->pixels + (size_t)nd_clamp32(i, 0, last) * src->stride;

        for (k = 0u; k < n; k++)
            sums[k] += q[k];
    }
    for (y = 0; y < src->h; y++) {
        const uint8_t *add = src->pixels + (size_t)nd_min32(y + r + 1, last) * src->stride;
        const uint8_t *sub = src->pixels + (size_t)nd_max32(y - r, 0) * src->stride;
        uint8_t *d = dst->pixels + (size_t)y * dst->stride;

        for (k = 0u; k < n; k++) {
            d[k] = (uint8_t)((sums[k] * mul + (1u << 19)) >> 20);
            sums[k] = sums[k] + add[k] - sub[k];
        }
    }
}

/* `src` blurred into `dst` at radius r: two horizontal-then-vertical box
 * passes, bouncing through `tmp`. */
static void blur(const nd_image *src, nd_image *dst, int32_t r, nd_launchfx *fx)
{
    memcpy(dst->pixels, src->pixels, src->stride * (size_t)src->h);
    if (r <= 0)
        return;
    box_h(dst, r, fx->row);
    box_v(dst, fx->tmp, r, fx->sums);
    box_h(fx->tmp, r, fx->row);
    box_v(fx->tmp, dst, r, fx->sums);
}

/* ------------------------------------------------------------------ *
 * Lifetime
 * ------------------------------------------------------------------ */

static nd_image *snapshot(const nd_image *src)
{
    nd_image *img = nd_image_new(src->w, src->h, ND_PIXFMT_RGB888);
    int32_t y;

    if (img == NULL)
        return NULL;
    for (y = 0; y < src->h; y++) {
        const uint8_t *s = src->pixels + (size_t)y * src->stride;
        uint8_t *d = img->pixels + (size_t)y * img->stride;
        int32_t x;

        for (x = 0; x < src->w; x++, s += src->bpp, d += 3) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
    return img;
}

static bool usable(const nd_image *img)
{
    return img != NULL && img->pixels != NULL && img->w > 0 && img->h > 0 && img->bpp >= 3u;
}

nd_launchfx *nd_launchfx_new(const nd_image *before, const nd_image *after)
{
    nd_launchfx *fx;

    if (!usable(before) || !usable(after) || before->w != after->w || before->h != after->h)
        return NULL;
    fx = calloc(1u, sizeof *fx);
    if (fx == NULL)
        return NULL;
    fx->before = snapshot(before);
    fx->after = snapshot(after);
    fx->bg = snapshot(before);
    fx->fg = snapshot(after);
    fx->tmp = snapshot(after);
    fx->bg_r = -1;
    fx->fg_r = -1;
    fx->row = malloc((size_t)after->w * 3u);
    fx->sums = calloc((size_t)after->w * 3u, sizeof *fx->sums);
    fx->map_x = calloc((size_t)after->w * 2u, sizeof *fx->map_x);
    fx->map_y = calloc((size_t)after->h * 2u, sizeof *fx->map_y);
    if (fx->before == NULL || fx->after == NULL || fx->bg == NULL || fx->fg == NULL ||
        fx->tmp == NULL || fx->row == NULL || fx->sums == NULL || fx->map_x == NULL ||
        fx->map_y == NULL) {
        nd_launchfx_free(fx);
        return NULL;
    }
    return fx;
}

void nd_launchfx_free(nd_launchfx *fx)
{
    if (fx == NULL)
        return;
    nd_image_free(fx->before);
    nd_image_free(fx->after);
    nd_image_free(fx->bg);
    nd_image_free(fx->fg);
    nd_image_free(fx->tmp);
    free(fx->row);
    free(fx->sums);
    free(fx->map_x);
    free(fx->map_y);
    free(fx);
}

/* ------------------------------------------------------------------ *
 * One frame
 * ------------------------------------------------------------------ */

/* For each destination index along an axis of length n, the integer source
 * index and the 8-bit weight of the NEXT one, scaling about the centre.
 * Pixel centres map to pixel centres, so scale 1 is the identity. */
static void build_map(int32_t *map, int32_t n, double scale)
{
    double half = (double)n / 2.0;
    int32_t i;

    for (i = 0; i < n; i++) {
        double u = ((double)i + 0.5 - half) / scale + half - 0.5;
        double f = floor(u);
        int32_t w = (int32_t)((u - f) * 256.0 + 0.5);

        if (w >= 256) {
            f += 1.0;
            w = 0;
        }
        map[2 * i] = (int32_t)f;
        map[2 * i + 1] = w;
    }
}

/* The tap's weight, or 0 when it falls outside the source. */
static int32_t tap_w(int32_t idx, int32_t n, int32_t w)
{
    return (idx < 0 || idx >= n) ? 0 : w;
}

nd_err nd_launchfx_frame(nd_launchfx *fx, nd_image *dst, int32_t step, int32_t steps)
{
    double t;
    double e;
    int32_t alpha; /* 0..256, how much of the arriving frame shows */
    int32_t keep;  /* 0..256, how much light the outgoing one keeps */
    int32_t w;
    int32_t h;
    int32_t x;
    int32_t y;

    if (fx == NULL || !usable(dst) || dst->w != fx->after->w || dst->h != fx->after->h ||
        steps <= 0)
        return ND_ERR_INVAL;
    w = dst->w;
    h = dst->h;
    step = nd_clamp32(step, 0, steps);
    t = (double)step / (double)steps;
    e = nd_launchfx_ease(t);

    /* The ends are exact, not "close": a launch that stops early must leave
     * the real frame, not a blend of it. */
    if (step == steps || step == 0) {
        const nd_image *src = step == 0 ? fx->before : fx->after;

        for (y = 0; y < h; y++) {
            const uint8_t *s = src->pixels + (size_t)y * src->stride;
            uint8_t *d = dst->pixels + (size_t)y * dst->stride;

            for (x = 0; x < w; x++, s += 3, d += dst->bpp) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            }
        }
        return ND_OK;
    }

    /* The radii are whole pixels and the ease is flat at the ends, so
     * consecutive frames often share one -- and then the blur is already
     * done. Always from the pristine copy, never a blur of a blur. */
    {
        int32_t rb = (int32_t)lround((double)ND_LAUNCHFX_BLUR_OUT * e);
        /* The arriving blur falls off more slowly than everything else,
         * quadratically rather than on the cubic ease: on the ease it is gone
         * by the halfway frame, before the eye has registered it at all. */
        int32_t rf = (int32_t)lround((double)ND_LAUNCHFX_BLUR_IN * (1.0 - t) * (1.0 - t));

        if (rb != fx->bg_r) {
            blur(fx->before, fx->bg, rb, fx);
            fx->bg_r = rb;
        }
        if (rf != fx->fg_r) {
            blur(fx->after, fx->fg, rf, fx);
            fx->fg_r = rf;
        }
    }

    /* Opaque well before the blur has cleared, so the sharpening is seen on
     * the app itself and not through the screen it is replacing. */
    alpha = nd_clamp32((int32_t)lround(e * 1.6 * 256.0), 0, 256);
    keep = 256 - (int32_t)lround(e * 0.3 * 256.0);

    build_map(fx->map_x, w, nd_launchfx_scale(t));
    build_map(fx->map_y, h, nd_launchfx_scale(t));

    for (y = 0; y < h; y++) {
        int32_t sy = fx->map_y[2 * y];
        int32_t wy = fx->map_y[2 * y + 1];
        int32_t wy0 = tap_w(sy, h, 256 - wy);
        int32_t wy1 = tap_w(sy + 1, h, wy);
        const uint8_t *r0 = fx->fg->pixels + (size_t)nd_clamp32(sy, 0, h - 1) * fx->fg->stride;
        const uint8_t *r1 = fx->fg->pixels + (size_t)nd_clamp32(sy + 1, 0, h - 1) * fx->fg->stride;
        const uint8_t *b = fx->bg->pixels + (size_t)y * fx->bg->stride;
        uint8_t *d = dst->pixels + (size_t)y * dst->stride;

        for (x = 0; x < w; x++, b += 3, d += dst->bpp) {
            int32_t sx = fx->map_x[2 * x];
            int32_t wx = fx->map_x[2 * x + 1];
            int32_t wx0 = tap_w(sx, w, 256 - wx);
            int32_t wx1 = tap_w(sx + 1, w, wx);
            size_t o0 = (size_t)nd_clamp32(sx, 0, w - 1) * 3u;
            size_t o1 = (size_t)nd_clamp32(sx + 1, 0, w - 1) * 3u;
            int32_t w00 = wx0 * wy0;
            int32_t w10 = wx1 * wy0;
            int32_t w01 = wx0 * wy1;
            int32_t w11 = wx1 * wy1;
            int32_t cov = w00 + w10 + w01 + w11; /* 0..65536 */
            int32_t a = (alpha * cov) >> 16;     /* 0..256   */
            int32_t c;

            for (c = 0; c < 3; c++) {
                /* Premultiplied by coverage already: the taps outside the
                 * source contributed nothing. */
                int32_t fgp = (r0[o0 + (size_t)c] * w00 + r0[o1 + (size_t)c] * w10 +
                               r1[o0 + (size_t)c] * w01 + r1[o1 + (size_t)c] * w11 + 32768) >>
                              16;
                int32_t bgv = (b[c] * keep) >> 8;
                int32_t v = bgv + ((alpha * fgp) >> 8) - ((bgv * a) >> 8);

                d[c] = (uint8_t)nd_clamp32(v, 0, 255);
            }
        }
    }
    return ND_OK;
}

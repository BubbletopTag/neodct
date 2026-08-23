/* nd_resample.c -- Image.resize() and Image.thumbnail(), ported from Pillow.
 *
 * Two filters, because the shipped code asks for exactly two:
 *
 *   LANCZOS  the wallpaper, every app icon, the crash image and DetailPage's
 *            hero picture. A port of Pillow's Resample.c -- 3 lobes, support
 *            3.0, coefficients normalised in double then frozen to 22-bit
 *            fixed point, horizontal pass into a strip and then a vertical
 *            pass over that strip. Every step of that is observable: dropping
 *            the fixed-point step, or normalising after quantising, moves
 *            antialiased edges by a level or two on almost every icon.
 *
 *   NEAREST  Koki's costumes and MusicPlayer's album art. Pillow does NOT
 *            take the resample path for NEAREST at all -- it runs an affine
 *            transform in 16.16 fixed point, and the accumulated rounding of
 *            that is not the same as recomputing the ratio per column. It is
 *            reproduced here as the accumulation Pillow performs, including
 *            its habit of leaving a source-out-of-range pixel at zero rather
 *            than clamping to the last row.
 *
 * These run at asset-load time, never per frame, so they allocate.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "nd_image.h"
#include "nd_image_priv.h"

/* Pillow's Resample.c: 8 bits of result, plus two bits of headroom because a
 * lanczos kernel has negative lobes and the running sum can overshoot both
 * ends before it is clipped. */
#define ND_PRECISION_BITS (32 - 8 - 2)

#define ND_LANCZOS_SUPPORT 3.0

static double sinc_filter(double x)
{
    if (x == 0.0)
        return 1.0;
    x = x * M_PI;
    return sin(x) / x;
}

static double lanczos_filter(double x)
{
    /* truncated sinc; the asymmetric bound is Pillow's, not a typo */
    if (-3.0 <= x && x < 3.0)
        return sinc_filter(x) * sinc_filter(x / 3.0);
    return 0.0;
}

static uint8_t clip8(int32_t v)
{
    int32_t s = v >> ND_PRECISION_BITS;
    if (s < 0)
        return 0u;
    if (s > 255)
        return 255u;
    return (uint8_t)s;
}

/* One output position's contributing source range and its coefficients.
 * bounds[i*2+0] is the first source index, bounds[i*2+1] the count. */
typedef struct {
    int32_t ksize;
    int32_t *bounds; /* owned here; freed by coeffs_free() */
    int32_t *kk;     /* owned here; ksize per output position, 22-bit fixed */
} coeffs;

static void coeffs_free(coeffs *c)
{
    free(c->bounds);
    free(c->kk);
    c->bounds = NULL;
    c->kk = NULL;
}

/* Pillow's precompute_coeffs() followed by normalize_coeffs_8bpc(). The two
 * are separate functions there and fused here because nothing else uses the
 * double form. The normalisation happens in double and the quantisation
 * afterwards -- doing it the other way round loses a level on wide kernels. */
static nd_err precompute_coeffs(int32_t in_size, int32_t out_size, coeffs *out)
{
    double support, scale, filterscale, inv_filterscale;
    int32_t ksize, xx, x;
    double *k = NULL;
    nd_err rc = ND_OK;

    out->bounds = NULL;
    out->kk = NULL;

    scale = (double)in_size / (double)out_size;
    filterscale = scale < 1.0 ? 1.0 : scale;
    support = ND_LANCZOS_SUPPORT * filterscale;
    ksize = (int32_t)ceil(support) * 2 + 1;

    /* owned by out; released by coeffs_free() */
    out->bounds = malloc(sizeof(int32_t) * (size_t)out_size * 2u);
    out->kk = malloc(sizeof(int32_t) * (size_t)out_size * (size_t)ksize);
    /* scratch for one position's double coefficients; freed before return */
    k = malloc(sizeof(double) * (size_t)ksize);
    if (!out->bounds || !out->kk || !k) {
        rc = ND_ERR_NOMEM;
        goto done;
    }

    inv_filterscale = 1.0 / filterscale;
    for (xx = 0; xx < out_size; xx++) {
        double center = ((double)xx + 0.5) * scale;
        double ww = 0.0;
        int32_t xmin = (int32_t)(center - support + 0.5);
        int32_t xmax = (int32_t)(center + support + 0.5);

        if (xmin < 0)
            xmin = 0;
        if (xmax > in_size)
            xmax = in_size;
        xmax -= xmin;

        for (x = 0; x < xmax; x++) {
            double w = lanczos_filter(((double)(x + xmin) - center + 0.5) * inv_filterscale);
            k[x] = w;
            ww += w;
        }
        if (ww != 0.0) {
            for (x = 0; x < xmax; x++)
                k[x] /= ww;
        }
        for (; x < ksize; x++)
            k[x] = 0.0;

        for (x = 0; x < ksize; x++) {
            double q = k[x] * (double)(1 << ND_PRECISION_BITS);
            /* Pillow rounds away from zero here, in both directions. */
            out->kk[xx * ksize + x] = (int32_t)(q < 0.0 ? q - 0.5 : q + 0.5);
        }
        out->bounds[xx * 2 + 0] = xmin;
        out->bounds[xx * 2 + 1] = xmax;
    }
    out->ksize = ksize;

done:
    free(k);
    if (rc != ND_OK)
        coeffs_free(out);
    return rc;
}

/* Horizontal pass: reads rows [y_off, y_off + out->h) of src. */
static void resample_horizontal(nd_image *dst, const nd_image *src, int32_t y_off,
                                const coeffs *c)
{
    int32_t yy, xx, x;
    uint8_t bands = nd_img_bands(src->fmt);

    for (yy = 0; yy < dst->h; yy++) {
        const uint8_t *line_in = nd_img_row(src, yy + y_off);
        uint8_t *line_out = nd_img_row(dst, yy);

        for (xx = 0; xx < dst->w; xx++) {
            int32_t xmin = c->bounds[xx * 2 + 0];
            int32_t xmax = c->bounds[xx * 2 + 1];
            const int32_t *k = &c->kk[xx * c->ksize];
            uint8_t b;

            for (b = 0; b < bands; b++) {
                int32_t ss = 1 << (ND_PRECISION_BITS - 1);
                for (x = 0; x < xmax; x++)
                    ss += (int32_t)line_in[(size_t)(x + xmin) * src->bpp + b] * k[x];
                line_out[(size_t)xx * dst->bpp + b] = clip8(ss);
            }
        }
    }
}

static void resample_vertical(nd_image *dst, const nd_image *src, const coeffs *c)
{
    int32_t yy, xx, y;
    uint8_t bands = nd_img_bands(src->fmt);

    for (yy = 0; yy < dst->h; yy++) {
        uint8_t *line_out = nd_img_row(dst, yy);
        int32_t ymin = c->bounds[yy * 2 + 0];
        int32_t ymax = c->bounds[yy * 2 + 1];
        const int32_t *k = &c->kk[yy * c->ksize];

        for (xx = 0; xx < dst->w; xx++) {
            uint8_t b;
            for (b = 0; b < bands; b++) {
                int32_t ss = 1 << (ND_PRECISION_BITS - 1);
                for (y = 0; y < ymax; y++)
                    ss += (int32_t)nd_img_row(src, y + ymin)[(size_t)xx * src->bpp + b] * k[y];
                line_out[(size_t)xx * dst->bpp + b] = clip8(ss);
            }
        }
    }
}

nd_image *nd_image_resize_lanczos(const nd_image *src, int32_t w, int32_t h)
{
    coeffs cv = {0, NULL, NULL};
    coeffs ch = {0, NULL, NULL};
    nd_image *temp = NULL;
    nd_image *out = NULL;
    bool need_h, need_v;
    int32_t ybox_first, ybox_last, i;

    if (!src || w <= 0 || h <= 0 || w > ND_IMAGE_MAX_DIM || h > ND_IMAGE_MAX_DIM)
        return NULL;

    need_h = (w != src->w);
    need_v = (h != src->h);

    if (precompute_coeffs(src->h, h, &cv) != ND_OK)
        return NULL;

    /* Only the source rows the vertical pass will actually read are put
     * through the horizontal pass. On a 240x175 wallpaper from a 1024x768
     * photo that is the difference between one strip and the whole image. */
    ybox_first = cv.bounds[0];
    ybox_last = cv.bounds[(h - 1) * 2] + cv.bounds[(h - 1) * 2 + 1];

    if (need_h) {
        if (precompute_coeffs(src->w, w, &ch) != ND_OK)
            goto done;
        for (i = 0; i < h; i++)
            cv.bounds[i * 2] -= ybox_first;

        temp = nd_image_new(w, ybox_last - ybox_first, src->fmt);
        if (!temp)
            goto done;
        resample_horizontal(temp, src, ybox_first, &ch);
        src = temp;
    }

    if (need_v) {
        out = nd_image_new(src->w, h, src->fmt);
        if (!out)
            goto done;
        resample_vertical(out, src, &cv);
    } else if (temp) {
        out = temp;
        temp = NULL;
    } else {
        out = nd_image_copy(src);
    }

done:
    /* owned by the caller; free with nd_image_free() */
    nd_image_free(temp);
    coeffs_free(&cv);
    coeffs_free(&ch);
    return out;
}

nd_image *nd_image_resize_nearest(const nd_image *src, int32_t w, int32_t h)
{
    nd_image *out = NULL;
    int32_t a0, a4, a2, a5;
    int32_t x, y;

    if (!src || w <= 0 || h <= 0 || w > ND_IMAGE_MAX_DIM || h > ND_IMAGE_MAX_DIM)
        return NULL;

    /* owned by the caller; free with nd_image_free() */
    out = nd_image_new(w, h, src->fmt);
    if (!out)
        return NULL;

    /* Pillow's affine_fixed(): 16.16 with the half-pixel offset folded into
     * the starting accumulator, and the STEP rounded once rather than the
     * position recomputed per column. FIX() is floor(v * 65536 + 0.5). */
#define ND_FIX(v) ((int32_t)floor((v) * 65536.0 + 0.5))
    a0 = ND_FIX((double)src->w / (double)w);
    a4 = ND_FIX((double)src->h / (double)h);
    a2 = ND_FIX(0.5 * (double)src->w / (double)w);
    a5 = ND_FIX(0.5 * (double)src->h / (double)h);
#undef ND_FIX

    for (y = 0; y < h; y++) {
        uint8_t *dp = nd_img_row(out, y);
        int32_t yin = a5 >> 16;
        int32_t xx = a2;

        /* Pillow memsets the destination row first and only overwrites where
         * the source coordinate is in range, so an accumulator that runs one
         * step past the last row leaves black rather than repeating it. */
        memset(dp, 0, out->stride);
        if (yin >= 0 && yin < src->h) {
            const uint8_t *sp = nd_img_row(src, yin);
            for (x = 0; x < w; x++, dp += out->bpp) {
                int32_t xin = xx >> 16;
                if (xin >= 0 && xin < src->w)
                    memcpy(dp, sp + (size_t)xin * src->bpp, out->bpp);
                xx += a0;
            }
        }
        a5 += a4;
    }
    return out;
}

nd_err nd_image_thumbnail(nd_image *img, int32_t max_w, int32_t max_h)
{
    nd_image *scaled = NULL;
    double aspect;
    int32_t x, y;

    if (!img || max_w <= 0 || max_h <= 0)
        return ND_ERR_INVAL;

    /* Image.thumbnail() returns immediately when the picture already fits.
     * It never upscales, which is why a 40x40 icon asked to fit 82x82 stays
     * 40x40 and the AppSelector grid has small icons in it. */
    if (max_w >= img->w && max_h >= img->h)
        return ND_OK;

    x = max_w;
    y = max_h;
    aspect = (double)img->w / (double)img->h;

    /* Pillow's round_aspect(): try floor and ceil, keep whichever reproduces
     * the aspect ratio more closely, and never go below 1. Plain rounding
     * differs by a pixel on several of the shipped icons. */
    if ((double)x / (double)y >= aspect) {
        double want = (double)y * aspect;
        int32_t lo = (int32_t)floor(want);
        int32_t hi = (int32_t)ceil(want);
        double dlo = fabs(aspect - (double)lo / (double)y);
        double dhi = fabs(aspect - (double)hi / (double)y);
        x = (dhi < dlo) ? hi : lo;
        if (x < 1)
            x = 1;
    } else {
        double want = (double)x / aspect;
        int32_t lo = (int32_t)floor(want);
        int32_t hi = (int32_t)ceil(want);
        double dlo = (lo == 0) ? 0.0 : fabs(aspect - (double)x / (double)lo);
        double dhi = (hi == 0) ? 0.0 : fabs(aspect - (double)x / (double)hi);
        y = (dhi < dlo) ? hi : lo;
        if (y < 1)
            y = 1;
    }

    if (x == img->w && y == img->h)
        return ND_OK;

    scaled = nd_image_resize_lanczos(img, x, y);
    if (!scaled)
        return ND_ERR_NOMEM;

    /* In place, as PIL's is: the caller's handle stays valid. A borrowed
     * surface cannot be resized -- its pixels belong to somebody else. */
    if (img->borrowed) {
        nd_image_free(scaled);
        return ND_ERR_INVAL;
    }
    free(img->pixels);
    img->pixels = scaled->pixels;
    img->w = scaled->w;
    img->h = scaled->h;
    img->stride = scaled->stride;
    scaled->pixels = NULL;
    scaled->borrowed = true; /* the header alone is freed below */
    nd_image_free(scaled);
    return ND_OK;
}

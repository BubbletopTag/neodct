/* nd_image.c -- the surface type and the pointwise half of the Pillow subset.
 *
 * Everything here is a deliberate reimplementation of a specific routine in
 * Pillow 12.3.0, because the golden frames were captured from that version and
 * "looks the same" is not the standard the oracle applies. Where a formula
 * looks needlessly ugly it is because Pillow's is, and the ugliness is what
 * lands on the same pixel value:
 *
 *   - compositing is nd_blend8(), verified in this tree against Pillow over
 *     all 16,777,216 (dst, ink, mask) triples with zero mismatches. It is not
 *     MULDIV255 and it is not a rounding divide.
 *   - RGB -> L is ITU-R 601-2 as Pillow spells it: the 19595/38470/7471
 *     16.16 constants with a +0x8000 bias, not a float dot product.
 *   - brightness truncates, because ImageEnhance multiplies into a float and
 *     lets the uint8 store chop it.
 *
 * Nothing here allocates except the lifecycle, geometry and codec entry
 * points, all of which are asset-load-time. The blit family, the fills and
 * the pointwise operators are the render path and allocate nothing at all.
 *
 * ORDERING GUARANTEE CALLERS DEPEND ON: every function that writes into a
 * surface clips against that surface, so a widget may hand in coordinates off
 * any edge and get Pillow's behaviour (silently nothing) rather than a fault.
 */

#include "nd_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_image_priv.h"
#include "nd_paths.h"

/* ------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------ */

nd_image *nd_image_new(int32_t w, int32_t h, nd_pixfmt fmt)
{
    nd_image *img = NULL;
    uint8_t *pixels = NULL;
    uint8_t bpp;
    size_t stride;

    if (w <= 0 || h <= 0 || w > ND_IMAGE_MAX_DIM || h > ND_IMAGE_MAX_DIM || !nd_img_fmt_valid(fmt))
        return NULL;

    bpp = nd_pixfmt_bpp(fmt);
    stride = (size_t)w * bpp;

    /* 240x175 RGB888 == 126,000 bytes; 82x82 RGBA == 26,896. */
    /* owned by the caller; released together by nd_image_free() */
    pixels = malloc(stride * (size_t)h);
    if (!pixels)
        return NULL;

    img = malloc(sizeof *img);
    if (!img) {
        free(pixels);
        return NULL;
    }

    img->w = w;
    img->h = h;
    img->fmt = fmt;
    img->bpp = bpp;
    img->stride = stride;
    img->pixels = pixels;
    img->borrowed = false;
    return img;
}

nd_image *nd_image_new_filled(int32_t w, int32_t h, nd_pixfmt fmt, nd_color colour)
{
    /* owned by the caller; free with nd_image_free() */
    nd_image *img = nd_image_new(w, h, fmt);
    if (!img)
        return NULL;
    (void)nd_image_fill(img, colour);
    return img;
}

nd_image *nd_image_borrow(void *pixels, int32_t w, int32_t h, nd_pixfmt fmt, size_t stride)
{
    nd_image *img = NULL;
    uint8_t bpp;

    if (!pixels || w <= 0 || h <= 0 || !nd_img_fmt_valid(fmt))
        return NULL;

    bpp = nd_pixfmt_bpp(fmt);
    if (stride < (size_t)w * bpp)
        return NULL;

    /* owned by the caller; nd_image_free() releases only this header */
    img = malloc(sizeof *img);
    if (!img)
        return NULL;

    img->w = w;
    img->h = h;
    img->fmt = fmt;
    img->bpp = bpp;
    img->stride = stride;
    img->pixels = pixels;
    img->borrowed = true;
    return img;
}

nd_image *nd_image_copy(const nd_image *src)
{
    nd_image *dst = NULL;
    int32_t y;

    if (!src)
        return NULL;

    /* owned by the caller; free with nd_image_free() */
    dst = nd_image_new(src->w, src->h, src->fmt);
    if (!dst)
        return NULL;

    for (y = 0; y < src->h; y++)
        memcpy(nd_img_row(dst, y), nd_img_row(src, y), dst->stride);
    return dst;
}

void nd_image_free(nd_image *img)
{
    if (!img)
        return;
    if (!img->borrowed)
        free(img->pixels);
    free(img);
}

/* ------------------------------------------------------------------ *
 * Reading pixels out
 * ------------------------------------------------------------------ */

nd_err nd_image_tobytes(const nd_image *img, uint8_t *out, size_t out_sz)
{
    size_t packed;
    int32_t y;

    if (!img || !out)
        return ND_ERR_INVAL;

    packed = (size_t)img->w * img->bpp;
    if (out_sz < packed * (size_t)img->h)
        return ND_ERR_TOOLONG;

    for (y = 0; y < img->h; y++)
        memcpy(out + packed * (size_t)y, nd_img_row(img, y), packed);
    return ND_OK;
}

nd_color nd_image_get_px(const nd_image *img, int32_t x, int32_t y)
{
    uint8_t q[4];

    if (!img || x < 0 || y < 0 || x >= img->w || y >= img->h)
        return ND_RGBA(0, 0, 0, 0);

    nd_img_px_read(nd_img_px(img, x, y), img->fmt, q);
    return ND_RGBA(q[0], q[1], q[2], q[3]);
}

void nd_image_set_px(nd_image *img, int32_t x, int32_t y, nd_color c)
{
    uint8_t q[4];

    if (!img || x < 0 || y < 0 || x >= img->w || y >= img->h)
        return;

    nd_img_colour_quad(c, q);
    nd_img_px_write(nd_img_px(img, x, y), img->fmt, q);
}

/* ------------------------------------------------------------------ *
 * Fills
 * ------------------------------------------------------------------ */

nd_err nd_image_fill(nd_image *img, nd_color colour)
{
    if (!img)
        return ND_ERR_INVAL;
    return nd_image_fill_rect(img, ND_RECT(0, 0, img->w - 1, img->h - 1), colour);
}

nd_err nd_image_fill_rect(nd_image *img, nd_rect rect, nd_color colour)
{
    uint8_t q[4];
    uint8_t stamp[4];
    int32_t x0, y0, x1, y1, x, y;
    uint8_t bpp;

    if (!img)
        return ND_ERR_INVAL;

    x0 = nd_max32(rect.x0, 0);
    y0 = nd_max32(rect.y0, 0);
    x1 = nd_min32(rect.x1, img->w - 1);
    y1 = nd_min32(rect.y1, img->h - 1);
    if (x0 > x1 || y0 > y1)
        return ND_OK;

    nd_img_colour_quad(colour, q);
    nd_img_px_write(stamp, img->fmt, q);
    bpp = img->bpp;

    for (y = y0; y <= y1; y++) {
        uint8_t *p = nd_img_px(img, x0, y);
        if (bpp == 1) {
            memset(p, stamp[0], (size_t)(x1 - x0 + 1));
        } else {
            for (x = x0; x <= x1; x++, p += bpp)
                memcpy(p, stamp, bpp);
        }
    }
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * Compositing
 * ------------------------------------------------------------------ *
 *
 * Pillow's paste() clips by moving the source origin rather than by refusing
 * the call: a paste at (-4, -4) draws the source from (4,4) on. Every blit
 * below shares clip_blit() so they all agree about that.
 */

typedef struct {
    int32_t sx; /* first source column/row actually copied */
    int32_t sy;
    int32_t dx; /* where it lands in the destination        */
    int32_t dy;
    int32_t w;
    int32_t h;
} blit_span;

static bool clip_blit(const nd_image *dst, int32_t src_w, int32_t src_h, int32_t x, int32_t y,
                      blit_span *out)
{
    int32_t sx = 0, sy = 0, w = src_w, h = src_h;

    if (x < 0) {
        sx = -x;
        w += x;
        x = 0;
    }
    if (y < 0) {
        sy = -y;
        h += y;
        y = 0;
    }
    if (x + w > dst->w)
        w = dst->w - x;
    if (y + h > dst->h)
        h = dst->h - y;
    if (w <= 0 || h <= 0)
        return false;

    out->sx = sx;
    out->sy = sy;
    out->dx = x;
    out->dy = y;
    out->w = w;
    out->h = h;
    return true;
}

nd_err nd_image_blit(nd_image *dst, const nd_image *src, int32_t x, int32_t y)
{
    blit_span s;
    int32_t iy, ix;

    if (!dst || !src)
        return ND_ERR_INVAL;
    if (!clip_blit(dst, src->w, src->h, x, y, &s))
        return ND_OK;

    for (iy = 0; iy < s.h; iy++) {
        const uint8_t *sp = nd_img_px(src, s.sx, s.sy + iy);
        uint8_t *dp = nd_img_px(dst, s.dx, s.dy + iy);
        if (src->fmt == dst->fmt) {
            memcpy(dp, sp, (size_t)s.w * dst->bpp);
            continue;
        }
        for (ix = 0; ix < s.w; ix++, sp += src->bpp, dp += dst->bpp) {
            uint8_t q[4];
            nd_img_px_read(sp, src->fmt, q);
            nd_img_px_write(dp, dst->fmt, q);
        }
    }
    return ND_OK;
}

nd_err nd_image_blit_region(nd_image *dst, const nd_image *src, nd_rect src_rect, int32_t x,
                            int32_t y)
{
    blit_span s;
    int32_t rx0, ry0, rx1, ry1, iy, ix;

    if (!dst || !src)
        return ND_ERR_INVAL;

    /* src_rect is inclusive. Trim it to the source before offsetting, and
     * carry the trim into the destination position so a region that starts
     * off the left edge still lands where the caller asked for its first
     * in-bounds column. */
    rx0 = src_rect.x0;
    ry0 = src_rect.y0;
    rx1 = nd_min32(src_rect.x1, src->w - 1);
    ry1 = nd_min32(src_rect.y1, src->h - 1);
    if (rx0 < 0) {
        x -= rx0;
        rx0 = 0;
    }
    if (ry0 < 0) {
        y -= ry0;
        ry0 = 0;
    }
    if (rx0 > rx1 || ry0 > ry1)
        return ND_OK;

    if (!clip_blit(dst, rx1 - rx0 + 1, ry1 - ry0 + 1, x, y, &s))
        return ND_OK;

    for (iy = 0; iy < s.h; iy++) {
        const uint8_t *sp = nd_img_px(src, rx0 + s.sx, ry0 + s.sy + iy);
        uint8_t *dp = nd_img_px(dst, s.dx, s.dy + iy);
        if (src->fmt == dst->fmt) {
            memcpy(dp, sp, (size_t)s.w * dst->bpp);
            continue;
        }
        for (ix = 0; ix < s.w; ix++, sp += src->bpp, dp += dst->bpp) {
            uint8_t q[4];
            nd_img_px_read(sp, src->fmt, q);
            nd_img_px_write(dp, dst->fmt, q);
        }
    }
    return ND_OK;
}

/* The shared body of paste(src, box, mask). mask_stride/mask_off pick the
 * byte inside the mask surface that carries coverage: byte 0 of an L8 mask,
 * byte 3 of an RGBA source used as its own mask. */
static void blit_blended(nd_image *dst, const nd_image *src, const uint8_t *mask_base,
                         size_t mask_stride, uint8_t mask_bpp, uint8_t mask_off, const blit_span *s)
{
    int32_t iy, ix;

    for (iy = 0; iy < s->h; iy++) {
        const uint8_t *sp = nd_img_px(src, s->sx, s->sy + iy);
        uint8_t *dp = nd_img_px(dst, s->dx, s->dy + iy);
        const uint8_t *mp =
            mask_base + (size_t)(s->sy + iy) * mask_stride + (size_t)s->sx * mask_bpp + mask_off;

        for (ix = 0; ix < s->w; ix++, sp += src->bpp, dp += dst->bpp, mp += mask_bpp) {
            uint8_t m = *mp;
            uint8_t sq[4], dq[4], oq[4];

            if (m == 0)
                continue;

            nd_img_px_read(sp, src->fmt, sq);
            if (m == 255u) {
                nd_img_px_write(dp, dst->fmt, sq);
                continue;
            }
            nd_img_px_read(dp, dst->fmt, dq);

            /* Pillow blends every band of the destination, alpha included,
             * which is why pasting a translucent sprite onto an RGBA canvas
             * lifts the canvas's alpha rather than replacing it. */
            oq[0] = nd_blend8(dq[0], sq[0], m);
            oq[1] = nd_blend8(dq[1], sq[1], m);
            oq[2] = nd_blend8(dq[2], sq[2], m);
            oq[3] = nd_blend8(dq[3], sq[3], m);
            nd_img_px_write(dp, dst->fmt, oq);
        }
    }
}

nd_err nd_image_blit_alpha(nd_image *dst, const nd_image *src, int32_t x, int32_t y)
{
    blit_span s;

    if (!dst || !src)
        return ND_ERR_INVAL;
    if (src->fmt != ND_PIXFMT_RGBA8888)
        return ND_ERR_INVAL;
    if (!clip_blit(dst, src->w, src->h, x, y, &s))
        return ND_OK;

    blit_blended(dst, src, src->pixels, src->stride, src->bpp, 3u, &s);
    return ND_OK;
}

nd_err nd_image_blit_mask(nd_image *dst, const nd_image *src, const nd_image *mask, int32_t x,
                          int32_t y)
{
    blit_span s;

    if (!dst || !src || !mask)
        return ND_ERR_INVAL;
    if (mask->fmt != ND_PIXFMT_L8)
        return ND_ERR_INVAL;
    if (mask->w != src->w || mask->h != src->h)
        return ND_ERR_INVAL;
    if (!clip_blit(dst, src->w, src->h, x, y, &s))
        return ND_OK;

    blit_blended(dst, src, mask->pixels, mask->stride, mask->bpp, 0u, &s);
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * Geometry
 * ------------------------------------------------------------------ */

static nd_image *crop_common(const nd_image *src, nd_rect box, bool zeropad)
{
    nd_image *out = NULL;
    int32_t w, h, y;

    if (!src)
        return NULL;

    w = nd_rect_w(box);
    h = nd_rect_h(box);

    if (!zeropad) {
        /* Clip to the source instead of padding: the caller asked for the
         * pixels that exist. Every crop in the shipped code is in bounds, so
         * this and the padded form agree in practice. */
        int32_t x0 = nd_max32(box.x0, 0);
        int32_t y0 = nd_max32(box.y0, 0);
        int32_t x1 = nd_min32(box.x1, src->w - 1);
        int32_t y1 = nd_min32(box.y1, src->h - 1);
        if (x0 > x1 || y0 > y1)
            return NULL;
        box = ND_RECT(x0, y0, x1, y1);
        w = nd_rect_w(box);
        h = nd_rect_h(box);
    }

    if (w <= 0 || h <= 0)
        return NULL;

    /* owned by the caller; free with nd_image_free() */
    out = nd_image_new(w, h, src->fmt);
    if (!out)
        return NULL;

    for (y = 0; y < h; y++) {
        int32_t sy = box.y0 + y;
        uint8_t *dp = nd_img_row(out, y);

        if (sy < 0 || sy >= src->h) {
            memset(dp, 0, out->stride);
            continue;
        }
        /* Left and right overhang are zero, per PIL's crop(). */
        {
            int32_t cx0 = nd_max32(box.x0, 0);
            int32_t cx1 = nd_min32(box.x1, src->w - 1);
            if (cx0 > cx1) {
                memset(dp, 0, out->stride);
                continue;
            }
            memset(dp, 0, out->stride);
            memcpy(dp + (size_t)(cx0 - box.x0) * out->bpp, nd_img_px(src, cx0, sy),
                   (size_t)(cx1 - cx0 + 1) * out->bpp);
        }
    }
    return out;
}

nd_image *nd_image_crop(const nd_image *src, nd_rect box)
{
    return crop_common(src, box, false);
}

nd_image *nd_image_crop_zeropad(const nd_image *src, nd_rect box)
{
    return crop_common(src, box, true);
}

nd_err nd_image_flip_h(nd_image *img)
{
    int32_t y, x;
    uint8_t bpp;

    if (!img)
        return ND_ERR_INVAL;

    bpp = img->bpp;
    for (y = 0; y < img->h; y++) {
        uint8_t *lo = nd_img_row(img, y);
        uint8_t *hi = lo + (size_t)(img->w - 1) * bpp;
        while (lo < hi) {
            for (x = 0; x < bpp; x++) {
                uint8_t t = lo[x];
                lo[x] = hi[x];
                hi[x] = t;
            }
            lo += bpp;
            hi -= bpp;
        }
    }
    return ND_OK;
}

nd_image *nd_image_convert(const nd_image *src, nd_pixfmt fmt)
{
    nd_image *out = NULL;
    int32_t y, x;

    if (!src || !nd_img_fmt_valid(fmt))
        return NULL;

    /* A new image even when the format already matches, so the caller's
     * ownership rule never depends on the source's mode. */
    out = nd_image_new(src->w, src->h, fmt);
    if (!out)
        return NULL;

    if (src->fmt == fmt) {
        for (y = 0; y < src->h; y++)
            memcpy(nd_img_row(out, y), nd_img_row(src, y), out->stride);
        return out;
    }

    for (y = 0; y < src->h; y++) {
        const uint8_t *sp = nd_img_row(src, y);
        uint8_t *dp = nd_img_row(out, y);
        for (x = 0; x < src->w; x++, sp += src->bpp, dp += out->bpp) {
            uint8_t q[4];
            nd_img_px_read(sp, src->fmt, q);
            nd_img_px_write(dp, fmt, q);
        }
    }
    return out;
}

/* ------------------------------------------------------------------ *
 * The two Koki oddities, plus brightness
 * ------------------------------------------------------------------ */

nd_err nd_image_point_lut(nd_image *img, const uint8_t lut_r[256], const uint8_t lut_g[256],
                          const uint8_t lut_b[256], const uint8_t lut_a[256])
{
    const uint8_t *luts[4];
    int32_t y, x;
    uint8_t band, bands;

    if (!img)
        return ND_ERR_INVAL;

    luts[0] = lut_r;
    luts[1] = lut_g;
    luts[2] = lut_b;
    luts[3] = lut_a;

    /* An L8 surface is one band and takes the red table, which is how
     * Koki's alpha-threshold call reads once its mask has been split out. */
    bands = img->fmt == ND_PIXFMT_RGBA8888 ? 4u : (img->fmt == ND_PIXFMT_L8 ? 1u : 3u);

    for (y = 0; y < img->h; y++) {
        uint8_t *p = nd_img_row(img, y);
        for (x = 0; x < img->w; x++, p += img->bpp) {
            for (band = 0; band < bands; band++) {
                if (luts[band])
                    p[band] = luts[band][p[band]];
            }
        }
    }
    return ND_OK;
}

bool nd_image_masks_overlap(const nd_image *a, const nd_image *b)
{
    int32_t y, x;

    if (!a || !b)
        return false;
    if (a->w != b->w || a->h != b->h)
        return false;
    if (a->fmt != ND_PIXFMT_L8 || b->fmt != ND_PIXFMT_L8)
        return false;

    /* ImageChops.multiply(a,b).getbbox() is not None. On the 0/255 masks Koki
     * feeds it, (a*b)/255 is non-zero exactly where both are, so the product
     * image is never built. */
    for (y = 0; y < a->h; y++) {
        const uint8_t *pa = nd_img_row(a, y);
        const uint8_t *pb = nd_img_row(b, y);
        for (x = 0; x < a->w; x++) {
            if (pa[x] != 0 && pb[x] != 0 && ((uint32_t)pa[x] * pb[x]) / 255u != 0u)
                return true;
        }
    }
    return false;
}

bool nd_image_alpha_bbox(const nd_image *img, nd_rect *out)
{
    int32_t y, x;
    int32_t x0, y0, x1, y1;
    bool any = false;

    if (!img || !out)
        return false;

    x0 = img->w;
    y0 = img->h;
    x1 = -1;
    y1 = -1;

    for (y = 0; y < img->h; y++) {
        const uint8_t *p = nd_img_row(img, y);
        for (x = 0; x < img->w; x++, p += img->bpp) {
            bool empty;
            switch (img->fmt) {
            case ND_PIXFMT_L8:
                empty = p[0] == 0u;
                break;
            case ND_PIXFMT_RGBA8888:
                /* Pillow's all-channels rule, not the alpha-only default:
                 * a fully transparent WHITE pixel still counts as ink. */
                empty = (p[0] | p[1] | p[2] | p[3]) == 0u;
                break;
            case ND_PIXFMT_RGB888:
            default:
                empty = (p[0] | p[1] | p[2]) == 0u;
                break;
            }
            if (empty)
                continue;
            any = true;
            if (x < x0)
                x0 = x;
            if (x > x1)
                x1 = x;
            if (y < y0)
                y0 = y;
            if (y > y1)
                y1 = y;
        }
    }

    if (!any)
        return false;

    out->x0 = x0;
    out->y0 = y0;
    out->x1 = x1;
    out->y1 = y1;
    return true;
}

nd_err nd_image_brightness(nd_image *img, double factor)
{
    uint8_t lut[256];
    int32_t i;
    int32_t y, x;
    uint8_t bands;

    if (!img)
        return ND_ERR_INVAL;

    /* ImageEnhance.Brightness blends against a black image and stores through
     * a uint8, which TRUNCATES. Rounding here mismatches on 128 of the 256
     * possible input values and every wallpapered golden frame notices. */
    for (i = 0; i < 256; i++) {
        double v = (double)i * factor;
        if (v <= 0.0)
            lut[i] = 0u;
        else if (v >= 255.0)
            lut[i] = 255u;
        else
            lut[i] = (uint8_t)v;
    }

    bands = img->fmt == ND_PIXFMT_RGBA8888 ? 3u : (img->fmt == ND_PIXFMT_L8 ? 1u : 3u);

    for (y = 0; y < img->h; y++) {
        uint8_t *p = nd_img_row(img, y);
        for (x = 0; x < img->w; x++, p += img->bpp) {
            uint8_t b;
            for (b = 0; b < bands; b++)
                p[b] = lut[p[b]];
        }
    }
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * Codec front door
 * ------------------------------------------------------------------ */

nd_image *nd_image_open(const char *path)
{
    char resolved[ND_PATH_MAX];
    FILE *f = NULL;
    uint8_t magic[8];
    size_t got;
    nd_image *img = NULL;

    if (!path)
        return NULL;
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return NULL;

    f = fopen(resolved, "rb");
    if (!f)
        return NULL;
    got = fread(magic, 1u, sizeof magic, f);
    fclose(f);

    if (got >= 8u && memcmp(magic, "\x89PNG\r\n\x1a\n", 8) == 0)
        img = nd_image_load_png(path);
    else if (got >= 2u && magic[0] == 0xFFu && magic[1] == 0xD8u)
        img = nd_image_load_jpeg(path);
    else if (got >= 6u && memcmp(magic, "GIF8", 4) == 0 &&
             (memcmp(magic + 4u, "7a", 2) == 0 || memcmp(magic + 4u, "9a", 2) == 0))
        img = nd_image_load_gif(path);

    /* A missing or undecodable file is None in the Python, not an exception
     * anybody catches. Callers fall back to a placeholder. */
    return img;
}

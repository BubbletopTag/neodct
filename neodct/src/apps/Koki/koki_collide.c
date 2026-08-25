/* koki_collide.c -- "touching?" the way Scratch means it: pixels, not boxes.
 *
 * Scratch costumes carry a lot of transparent canvas around the drawing, so a
 * rectangle test has the boss killing the player before he is anywhere near.
 * The engine therefore keeps, per costume and per flip, a black-and-white
 * stencil of which pixels are solid (alpha > 40), and asks whether the two
 * stencils overlap anywhere in the region where their visible-pixel boxes
 * already agree.
 *
 * Three gates, cheapest first:
 *
 *   1. either sprite hidden        -> false
 *   2. visible-pixel boxes miss    -> false
 *   3. either sprite runtime-SCALED-> true, on the box result alone
 *   4. otherwise                   -> the mask overlap
 *
 * Gate 3 is not laziness, it is the Python's own shortcut: _paste_origin()
 * assumes scale 1, so the mask arithmetic is only valid when both sprites are
 * at their baked size. Every collision the gameplay depends on is; the scaled
 * cases are menu flourishes.
 *
 * ============ THE +1 ON THE CROP IS DELIBERATE ============
 *
 * The overlap region is cropped out of each mask with its far edges pushed
 * one pixel out, which regularly runs past the image. PIL pads a crop's
 * overhang with ZERO, and zero can never make a multiply non-zero, so the
 * over-crop is harmless and keeps the two crops the same size more often
 * than not. nd_image_crop_zeropad() is that padding rule; nd_image_crop()
 * would clip instead and change the answer.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nd_image.h"
#include "nd_types.h"

#include "koki_manifest.h"

/* int() truncates toward zero. Every operand here is non-negative -- the
 * bbox offset cannot make screen_rect's left smaller than paste_origin's x --
 * so this is floor in practice, and the truncation is kept anyway. */
static int32_t trunc_i(double v)
{
    return (int32_t)v;
}

static bool sprite_flipped(const koki_sprite *s)
{
    return s->rotation_style == KOKI_ROT_LEFT_RIGHT && s->direction < 0.0;
}

/* ------------------------------------------------------------------ *
 * Geometry
 * ------------------------------------------------------------------ */

void koki_screen_rect(koki_sprite *s, double inset, double out[4])
{
    const koki_costume *c;
    const nd_image *img;
    double bx0, by0, bx1, by1;
    double cx;
    double scale;
    double left, top, w, h;
    double iw;

    out[0] = out[1] = out[2] = out[3] = 0.0;
    if (s == NULL)
        return;
    c = &s->costumes[s->costume_i];
    img = koki_load_image(s->eng, c->img);
    if (img == NULL)
        return;
    iw = (double)img->w;

    if (c->has_bbox) {
        bx0 = (double)c->bx0;
        by0 = (double)c->by0;
        bx1 = (double)c->bx1;
        by1 = (double)c->by1;
    } else {
        bx0 = 0.0;
        by0 = 0.0;
        bx1 = iw;
        by1 = (double)img->h;
    }

    cx = c->cx;
    if (sprite_flipped(s)) {
        double t;

        cx = iw - cx;
        t = bx0;
        bx0 = iw - bx1;
        bx1 = iw - t;
    }

    scale = s->size / s->baked_size;
    left = KOKI_CENTER_X + s->x * KOKI_STAGE_SCALE + (bx0 - cx) * scale;
    top = KOKI_CENTER_Y - s->y * KOKI_STAGE_SCALE + (by0 - c->cy) * scale;
    w = (bx1 - bx0) * scale;
    h = (by1 - by0) * scale;

    if (inset != 0.0) {
        double dx = w * inset / 2.0;
        double dy = h * inset / 2.0;

        out[0] = left + dx;
        out[1] = top + dy;
        out[2] = left + w - dx;
        out[3] = top + h - dy;
        return;
    }
    out[0] = left;
    out[1] = top;
    out[2] = left + w;
    out[3] = top + h;
}

void koki_paste_origin(koki_sprite *s, double out[2])
{
    const koki_costume *c;
    double cx;

    out[0] = 0.0;
    out[1] = 0.0;
    if (s == NULL)
        return;
    c = &s->costumes[s->costume_i];
    cx = c->cx;
    if (sprite_flipped(s)) {
        const nd_image *img = koki_load_image(s->eng, c->img);

        if (img == NULL)
            return;
        cx = (double)img->w - cx;
    }
    out[0] = KOKI_CENTER_X + s->x * KOKI_STAGE_SCALE - cx;
    out[1] = KOKI_CENTER_Y - s->y * KOKI_STAGE_SCALE - c->cy;
}

/* ------------------------------------------------------------------ *
 * The stencil
 * ------------------------------------------------------------------ */

/* getchannel("A") -> transpose -> point(ALPHA_THRESH_LUT), in one pass.
 * Thresholding commutes with the flip (it is per-pixel), so doing it during
 * the extraction rather than after gives identical bytes and one fewer walk.
 *
 * Pixels are read directly rather than through nd_image_get_px(): this runs
 * once per costume per flip and is then cached, but a bounds-checked call per
 * pixel over a 200x200 costume is 40,000 function calls for a result that is
 * known to be in range. nd_image.h keeps the struct public for exactly this. */
static nd_image *build_mask(const nd_image *src, bool flip)
{
    nd_image *m;
    int32_t y;
    int32_t x;

    if (src == NULL || src->fmt != ND_PIXFMT_RGBA8888)
        return NULL;
    m = nd_image_new(src->w, src->h, ND_PIXFMT_L8);
    if (m == NULL)
        return NULL;

    for (y = 0; y < src->h; y++) {
        const uint8_t *sp = src->pixels + (size_t)y * src->stride;
        uint8_t *dp = m->pixels + (size_t)y * m->stride;

        for (x = 0; x < src->w; x++) {
            uint8_t a = sp[(size_t)x * 4u + 3u];
            int32_t dx = flip ? (src->w - 1 - x) : x;

            dp[dx] = (a > KOKI_ALPHA_SOLID) ? 255u : 0u;
        }
    }
    return m;
}

static const nd_image *alpha_mask(koki_sprite *s)
{
    char key[KOKI_CACHE_KEY_MAX];
    const koki_costume *c;
    const nd_image *img;
    nd_image *m;
    bool flip;

    c = &s->costumes[s->costume_i];
    flip = sprite_flipped(s);
    if (nd_snprintf(key, sizeof key, "%s|%d", c->img, flip ? 1 : 0) != ND_OK)
        return NULL;

    m = koki_lru_get(&s->eng->mask_cache, key);
    if (m != NULL)
        return m;

    img = koki_load_image(s->eng, c->img);
    if (img == NULL)
        return NULL;
    m = build_mask(img, flip);
    if (m == NULL)
        return NULL;
    koki_lru_put(&s->eng->mask_cache, key, m);
    return koki_lru_get(&s->eng->mask_cache, key);
}

/* ------------------------------------------------------------------ *
 * touching()
 * ------------------------------------------------------------------ */

bool koki_touching(koki_sprite *s, koki_sprite *other, double inset)
{
    double a[4];
    double b[4];
    double ox0, oy0, ox1, oy1;
    double oa[2];
    double ob[2];
    const nd_image *ma;
    const nd_image *mb;
    nd_image *ca = NULL;
    nd_image *cb = NULL;
    bool hit = false;
    int32_t ax0, ay0, ax1, ay1;
    int32_t bx0, by0, bx1, by1;

    if (s == NULL || other == NULL)
        return false;
    if (!s->visible || !other->visible)
        return false;

    koki_screen_rect(s, inset, a);
    koki_screen_rect(other, inset, b);
    ox0 = fmax(a[0], b[0]);
    oy0 = fmax(a[1], b[1]);
    ox1 = fmin(a[2], b[2]);
    oy1 = fmin(a[3], b[3]);
    if (ox0 >= ox1 || oy0 >= oy1)
        return false;

    /* Gate 3: a runtime-scaled sprite gets the box answer, because
     * _paste_origin() below assumes scale 1. */
    if (s->size != s->baked_size || other->size != other->baked_size)
        return true;

    koki_paste_origin(s, oa);
    koki_paste_origin(other, ob);

    /* PIL's crop box is half-open; nd_rect is inclusive, so the far corner
     * loses one -- which turns the Python's int(...)+1 into int(...). */
    ax0 = trunc_i(ox0 - oa[0]);
    ay0 = trunc_i(oy0 - oa[1]);
    ax1 = trunc_i(ox1 - oa[0]);
    ay1 = trunc_i(oy1 - oa[1]);
    bx0 = trunc_i(ox0 - ob[0]);
    by0 = trunc_i(oy0 - ob[1]);
    bx1 = trunc_i(ox1 - ob[0]);
    by1 = trunc_i(oy1 - ob[1]);

    /* Fetch and crop ONE MASK AT A TIME. A mask pointer belongs to the mask
     * cache, and the second alpha_mask() can insert an entry that evicts the
     * first -- 256 KB is a lot of stencils but not an infinite number, and
     * holding both pointers across the second fetch would be a
     * use-after-free waiting for the level that finally overflows it. The
     * crops are independent allocations, so once ca exists ma may go. */
    ma = alpha_mask(s);
    if (ma == NULL)
        return false;
    ca = nd_image_crop_zeropad(ma, ND_RECT(ax0, ay0, ax1, ay1));
    if (ca == NULL)
        return false;

    mb = alpha_mask(other);
    if (mb == NULL)
        goto done;
    cb = nd_image_crop_zeropad(mb, ND_RECT(bx0, by0, bx1, by1));
    if (cb == NULL)
        goto done;

    if (ca->w != cb->w || ca->h != cb->h) {
        /* The two crops can differ by a pixel when the truncations land
         * either side of an integer. Both shrink to the smaller box. */
        int32_t w = nd_min32(ca->w, cb->w);
        int32_t h = nd_min32(ca->h, cb->h);
        nd_image *na = nd_image_crop_zeropad(ca, ND_RECT(0, 0, w - 1, h - 1));
        nd_image *nb = nd_image_crop_zeropad(cb, ND_RECT(0, 0, w - 1, h - 1));

        nd_image_free(ca);
        nd_image_free(cb);
        ca = na;
        cb = nb;
        if (ca == NULL || cb == NULL)
            goto done;
    }

    /* ImageChops.multiply(ca, cb).getbbox() is not None. On 0/255 inputs
     * that is "is there an index where both are non-zero", which is what
     * nd_image_masks_overlap computes without building the product. */
    hit = nd_image_masks_overlap(ca, cb);

done:
    nd_image_free(ca);
    nd_image_free(cb);
    return hit;
}

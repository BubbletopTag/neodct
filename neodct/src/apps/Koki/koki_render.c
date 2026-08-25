/* koki_render.c -- one frame: a backdrop and up to twelve alpha blits.
 *
 * tools/build_assets.py has already scaled every costume to its final size,
 * so the common case here is genuinely just a clipped alpha paste into a
 * 240x175 canvas. The interesting work is deciding WHERE, and producing the
 * flipped/scaled/tinted variant when a script has asked for one.
 *
 * ============ THE ROUNDING IS PYTHON'S, NOT C'S ============
 *
 * The paste origin goes through Python's round(), which rounds halves TO
 * EVEN. C's round() rounds halves away from zero and gives a different
 * answer. It is observable: CharacterAnim/costume2 has cy = 19.5, so
 * py = 68 - y*0.5; at y = -75 that is 105.5 -> 106 and at y = -73 it is
 * 104.5 -> 104. Use nd_round_half_even() -- and never (int)(v + 0.5), which
 * is wrong for negatives as well as for halves.
 *
 * ============ THE FX CACHE IS KEPT ============
 *
 * spec-koki.md section 6.1 permits folding the effect LUTs and the
 * nearest-neighbour sample into the blend loop, which would remove this
 * megabyte. It is not done here, deliberately: the cached form is what the
 * Python does step for step, the port was verified frame-by-frame against
 * the Python, and a hand-fused blend is a second place for a rounding order
 * to drift with no oracle pointing at it. The budget is honoured either way
 * (NEODCT_KOKI_FX_CACHE_KB, default 1024 KB, 512 or 384 on a smaller
 * device). Fusing it is a measured optimisation for later, not a
 * correctness fix.
 *
 * ============ ghost >= 95 IS NOT DRAWN AT ALL ============
 *
 * Not "drawn at 5% alpha": skipped. A sprite fading out therefore vanishes
 * one step early, and the white-flash fades in game.py are timed against
 * that.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_image.h"
#include "nd_log.h"
#include "nd_types.h"
#include "nd_ui.h"

#include "koki_manifest.h"

/* ------------------------------------------------------------------ *
 * Decoding
 * ------------------------------------------------------------------ */

const nd_image *koki_load_image(koki_engine *eng, const char *rel)
{
    char path[ND_PATH_MAX];
    nd_image *img;
    nd_image *rgba;

    if (eng == NULL || rel == NULL)
        return NULL;
    img = koki_lru_get(&eng->img_cache, rel);
    if (img != NULL)
        return img;

    if (nd_snprintf(path, sizeof path, "%s/%s", eng->assets, rel) != ND_OK)
        return NULL;
    img = nd_image_open(path);
    if (img == NULL) {
        nd_log_err(ND_LOG_KOKI, "cannot decode %s", rel);
        return NULL;
    }
    /* engine.py's load_image does f.convert("RGBA"). 234 of the 235 costume
     * PNGs are already RGBA; the one that is not would otherwise reach
     * blit_alpha as RGB and be rejected. */
    if (img->fmt != ND_PIXFMT_RGBA8888) {
        rgba = nd_image_convert(img, ND_PIXFMT_RGBA8888);
        nd_image_free(img);
        img = rgba;
        if (img == NULL)
            return NULL;
    }

    koki_lru_put(&eng->img_cache, rel, img);
    /* put() owns it now, and may even have dropped it under memory pressure;
     * ask the cache for the pointer it actually kept. */
    return koki_lru_get(&eng->img_cache, rel);
}

/* ------------------------------------------------------------------ *
 * costume_variant -- flip, scale, brightness, ghost
 * ------------------------------------------------------------------ */

/* int(v) in Python truncates toward zero, and so does a C cast. Named
 * because it appears four times below and each one is a decision. */
static int32_t trunc_i(double v)
{
    return (int32_t)v;
}

static int32_t round_i(double v)
{
    return (int32_t)nd_round_half_even(v);
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

const nd_image *koki_costume_variant(koki_engine *eng, koki_sprite *s, double *out_cx,
                                     double *out_cy)
{
    char key[KOKI_CACHE_KEY_MAX];
    const koki_costume *c;
    const nd_image *img;
    nd_image *out;
    nd_image *cached;
    bool flip;
    int32_t size_q;
    int32_t ghost_q;
    int32_t bri_q;
    double scale;
    double cx;
    double cy;

    if (eng == NULL || s == NULL)
        return NULL;
    c = &s->costumes[s->costume_i];
    img = koki_load_image(eng, c->img);
    if (img == NULL)
        return NULL;

    flip = (s->rotation_style == KOKI_ROT_LEFT_RIGHT && s->direction < 0.0);
    size_q = round_i(s->size / s->baked_size * 20.0);          /* 5% steps  */
    ghost_q = trunc_i(floor(fmax(0.0, s->ghost) / 10.0)) * 10; /* 10% steps */
    bri_q = trunc_i(floor(s->brightness / 25.0)) * 25;         /* 25% steps */
    /* Scratch clamps the effects; scripts push the raw values outside. */
    ghost_q = (int32_t)clampd((double)ghost_q, 0.0, 90.0);
    bri_q = (int32_t)clampd((double)bri_q, -100.0, 100.0);

    if (!flip && size_q == 20 && ghost_q == 0 && bri_q == 0) {
        /* The fast path, and the one every sprite takes most frames: no
         * copy, no cache entry, the decoded costume straight from the image
         * cache. */
        *out_cx = c->cx;
        *out_cy = c->cy;
        return img;
    }

    scale = (double)size_q / 20.0;
    cx = c->cx;
    cy = c->cy;
    if (scale != 1.0) {
        cx *= scale;
        cy *= scale;
    }
    if (flip) {
        /* NOTE the unrounded width. The resized image is
         * max(1, (int)(w * scale)) pixels across, but this multiplies by the
         * float w * scale, so the two disagree by up to a pixel whenever the
         * scale is fractional. That discrepancy is in the Python and it is
         * kept: it decides which column a mirrored sprite lands on. */
        cx = (double)img->w * scale - cx;
    }
    *out_cx = cx;
    *out_cy = cy;

    if (nd_snprintf(key, sizeof key, "%s|%d|%d|%d|%d", c->img, size_q, flip ? 1 : 0, ghost_q,
                    bri_q) != ND_OK)
        return img;

    cached = koki_lru_get(&eng->fx_cache, key);
    if (cached != NULL)
        return cached;

    if (scale != 1.0) {
        /* size = 2 with baked = 100 gives size_q = 0, so scale = 0.0 and the
         * image really is resized to 1x1 with cx = cy = 0. The intro sprite
         * starts exactly there. */
        int32_t w = trunc_i((double)img->w * scale);
        int32_t h = trunc_i((double)img->h * scale);

        out = nd_image_resize_nearest(img, nd_max32(1, w), nd_max32(1, h));
    } else {
        /* PIL's transpose() and point() both return NEW images; C's flip and
         * LUT are in place, so the copy is what keeps the cached original
         * unmodified. */
        out = nd_image_copy(img);
    }
    if (out == NULL)
        return img;

    if (flip)
        (void)nd_image_flip_h(out);

    if (bri_q != 0) {
        uint8_t lut[256];
        int32_t add = trunc_i(2.55 * (double)bri_q); /* truncated: 50 -> 127 */
        int32_t v;

        for (v = 0; v < 256; v++)
            lut[v] = (uint8_t)nd_clamp32(v + add, 0, 255);
        /* Brightness touches R, G and B only; alpha keeps the identity. */
        (void)nd_image_point_lut(out, lut, lut, lut, NULL);
    }
    if (ghost_q != 0) {
        uint8_t lut[256];
        double factor = (double)(100 - ghost_q) / 100.0;
        int32_t v;

        for (v = 0; v < 256; v++)
            lut[v] = (uint8_t)trunc_i((double)v * factor); /* truncated */
        /* Ghost touches alpha only. Applied AFTER brightness, as two
         * separate passes -- fusing them changes nothing here, but the order
         * would matter if either clamped. */
        (void)nd_image_point_lut(out, NULL, NULL, NULL, lut);
    }

    koki_lru_put(&eng->fx_cache, key, out);
    cached = koki_lru_get(&eng->fx_cache, key);
    return (cached != NULL) ? cached : img;
}

/* ------------------------------------------------------------------ *
 * Backdrops
 * ------------------------------------------------------------------ */

void koki_backdrop(koki_engine *eng, const char *name)
{
    char want[KOKI_NAME_MAX];
    char path[ND_PATH_MAX];
    const koki_target *stage;
    size_t i;

    if (eng == NULL || name == NULL)
        return;
    stage = koki_manifest_target(eng->manifest, "Stage");
    if (stage == NULL)
        return;

    koki_lower(want, sizeof want, name);
    for (i = 0u; i < stage->n_costumes; i++) {
        const koki_costume *c = &stage->costumes[i];
        nd_image *raw;
        nd_image *rgb;
        nd_image *crop;
        int32_t left;
        int32_t top;

        if (strcmp(c->lname, want) != 0)
            continue;

        if (nd_snprintf(path, sizeof path, "%s/%s", eng->assets, c->img) != ND_OK)
            return;
        /* Decoded straight, NOT through the image cache: only the RGB screen
         * crop is kept, so caching the RGBA original would evict sprites for
         * pixels nothing reads again. */
        raw = nd_image_open(path);
        if (raw == NULL) {
            nd_log_err(ND_LOG_KOKI, "cannot decode backdrop %s", c->img);
            return;
        }
        rgb = nd_image_convert(raw, ND_PIXFMT_RGB888);
        nd_image_free(raw);
        if (rgb == NULL)
            return;

        left = round_i(c->cx - KOKI_CENTER_X);
        top = round_i(c->cy - KOKI_CENTER_Y);
        left = nd_max32(0, nd_min32(left, nd_max32(0, rgb->w - KOKI_SCREEN_W)));
        top = nd_max32(0, nd_min32(top, nd_max32(0, rgb->h - KOKI_SCREEN_H)));

        /* PIL's crop box is half-open and nd_rect is inclusive, so the far
         * corner loses one. The crop can run off the image -- backdrop5 is
         * 245x159 and the box is 175 tall -- and PIL pads the overhang with
         * ZERO. Those 16 black rows along the bottom of the lobby are real
         * on-screen output, not a bug to fix. */
        crop = nd_image_crop_zeropad(
            rgb, ND_RECT(left, top, left + KOKI_SCREEN_W - 1, top + KOKI_SCREEN_H - 1));
        nd_image_free(rgb);
        if (crop == NULL)
            return;

        nd_image_free(eng->backdrop_img);
        eng->backdrop_img = crop; /* 240 * 175 * 3 = 126,000 bytes */
        (void)nd_strlcpy(eng->backdrop_name, c->name, sizeof eng->backdrop_name);
        return;
    }
    nd_log(ND_LOG_KOKI, "unknown backdrop %s", name);
}

/* ------------------------------------------------------------------ *
 * The frame
 * ------------------------------------------------------------------ */

void koki_render(koki_engine *eng)
{
    nd_image *cv;
    size_t i;

    if (eng == NULL || eng->canvas == NULL)
        return;
    cv = eng->canvas;

    if (eng->backdrop_img != NULL)
        (void)nd_image_blit(cv, eng->backdrop_img, 0, 0);
    else
        (void)nd_image_fill_rect(cv, ND_RECT(0, 0, KOKI_SCREEN_W - 1, KOKI_SCREEN_H - 1), ND_BLACK);

    for (i = 0u; i < eng->n_layers; i++) {
        koki_sprite *s = eng->layers[i];
        const nd_image *img;
        double cx = 0.0;
        double cy = 0.0;
        int32_t px;
        int32_t py;

        if (!s->visible || s->ghost >= 95.0)
            continue;
        img = koki_costume_variant(eng, s, &cx, &cy);
        if (img == NULL)
            continue;

        px = round_i(KOKI_CENTER_X + s->x * KOKI_STAGE_SCALE - cx);
        py = round_i(KOKI_CENTER_Y - s->y * KOKI_STAGE_SCALE - cy);
        /* engine.py's own reject test, kept verbatim including the fact that
         * px == 240 is drawn (and clips to nothing) while px == 241 is not. */
        if (px > KOKI_SCREEN_W || py > KOKI_SCREEN_H || px + img->w < 0 || py + img->h < 0)
            continue;
        (void)nd_image_blit_alpha(cv, img, px, py);
    }

    if (eng->ui != NULL && eng->ui->fb != NULL) {
        if (nd_fb_update(eng->ui->fb, cv) != ND_OK) {
            /* uistub's CapturingFramebuffer raises ScriptExhausted when its
             * frame budget runs out, and that exception is what ends a
             * capture run. C has to look at the return value instead;
             * apps/CubeBench/main.c takes the same route. */
            eng->quit = true;
        }
    }
}

/* test_image.c -- the surface operations against Pillow's own output.
 *
 * Every case in image_ref.inc was produced by Pillow 12.3.0 and is checked as
 * a hash of the exact byte sequence nd_image_tobytes() returns -- the same
 * bytes manifest.json hashes for the golden frames, so a pass here means the
 * frames will agree for that operation. The source pixels come from a small
 * integer hash both languages evaluate identically, so nothing has to be
 * shipped alongside the test.
 *
 * The codec fixtures are real PNG and JPEG files embedded as byte arrays,
 * including a deliberately truncated JPEG, because "decodes as far as it got"
 * is behaviour the Python relies on and a rewrite could silently drop.
 *
 * Runs with no arguments. It writes only under NEODCT_ROOT, which `make test`
 * points at a fresh temporary directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_image.h"
#include "nd_paths.h"

#include "image_ref.inc"

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

/* ------------------------------------------------------------------ *
 * The shared synthetic source
 * ------------------------------------------------------------------ */

/* Must stay bit-for-bit identical to synth() in tools/gen_image_ref.py. All
 * arithmetic is unsigned 32-bit so the two languages cannot disagree about
 * overflow or shifts. */
static uint8_t synth(int32_t x, int32_t y, int32_t c)
{
    uint32_t v = ((uint32_t)x * 2654435761u) ^ ((uint32_t)y * 40503u) ^ ((uint32_t)c * 97u);
    v ^= v >> 13;
    v *= 0x5BD1E995u;
    v ^= v >> 15;
    return (uint8_t)(v & 0xFFu);
}

static nd_image *make(nd_pixfmt fmt, int32_t w, int32_t h)
{
    nd_image *img = nd_image_new(w, h, fmt);
    int32_t x, y, c;
    uint8_t bands = nd_pixfmt_bpp(fmt);

    if (!img)
        return NULL;
    for (y = 0; y < h; y++) {
        uint8_t *p = img->pixels + (size_t)y * img->stride;
        for (x = 0; x < w; x++)
            for (c = 0; c < bands; c++)
                *p++ = synth(x, y, c);
    }
    return img;
}

static uint64_t fnv(const uint8_t *b, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

static uint64_t hash_image(const nd_image *img)
{
    size_t n = (size_t)img->w * (size_t)img->h * img->bpp;
    uint8_t *buf = malloc(n);
    uint64_t h;

    if (!buf)
        return 0u;
    if (nd_image_tobytes(img, buf, n) != ND_OK) {
        free(buf);
        return 0u;
    }
    h = fnv(buf, n);
    free(buf);
    return h;
}

/* Cases are consumed in the order gen_image_ref.py emits them; the cursor is
 * how the two lists stay aligned without repeating every name twice. */
static size_t cursor;

static void expect(nd_image *got, const char *want_name)
{
    const nd_image_ref *ref;

    checks++;
    if (cursor >= ND_ARRAY_LEN(ND_IMAGE_REF)) {
        fail("%s: ran off the end of the reference table", want_name);
        nd_image_free(got);
        return;
    }
    ref = &ND_IMAGE_REF[cursor++];

    if (strcmp(ref->name, want_name) != 0) {
        fail("case order drift: C says %s, reference says %s", want_name, ref->name);
        nd_image_free(got);
        return;
    }
    if (!got) {
        fail("%s: produced no image", want_name);
        return;
    }
    if (got->w != ref->w || got->h != ref->h)
        fail("%s: size %dx%d, want %dx%d", want_name, (int)got->w, (int)got->h, (int)ref->w,
             (int)ref->h);
    else if (hash_image(got) != ref->hash)
        fail("%s: pixels differ from Pillow (%016llx, want %016llx)", want_name,
             (unsigned long long)hash_image(got), (unsigned long long)ref->hash);
    nd_image_free(got);
}

/* ------------------------------------------------------------------ *
 * Resampling
 * ------------------------------------------------------------------ */

static void case_resize(const char *name, nd_pixfmt fmt, int32_t sw, int32_t sh, int32_t dw,
                        int32_t dh, bool lanczos)
{
    nd_image *src = make(fmt, sw, sh);
    nd_image *out;

    if (!src) {
        fail("%s: out of memory", name);
        return;
    }
    out = lanczos ? nd_image_resize_lanczos(src, dw, dh) : nd_image_resize_nearest(src, dw, dh);
    nd_image_free(src);
    expect(out, name);
}

static void test_resize(void)
{
    case_resize("lanczos_rgb_down", ND_PIXFMT_RGB888, 64, 48, 24, 18, true);
    case_resize("lanczos_rgb_up", ND_PIXFMT_RGB888, 64, 48, 100, 75, true);
    case_resize("lanczos_rgba_icon", ND_PIXFMT_RGBA8888, 40, 40, 82, 82, true);
    case_resize("lanczos_rgba_wallpaper", ND_PIXFMT_RGBA8888, 200, 150, 240, 175, true);
    case_resize("lanczos_rgb_identity", ND_PIXFMT_RGB888, 64, 48, 64, 48, true);
    case_resize("lanczos_rgb_to_1px", ND_PIXFMT_RGB888, 64, 48, 1, 1, true);
    case_resize("lanczos_l_down", ND_PIXFMT_L8, 64, 48, 20, 20, true);
    case_resize("lanczos_rgb_wide_only", ND_PIXFMT_RGB888, 64, 48, 30, 48, true);
    case_resize("lanczos_rgb_tall_only", ND_PIXFMT_RGB888, 64, 48, 64, 20, true);

    case_resize("nearest_rgb_up", ND_PIXFMT_RGB888, 17, 13, 40, 30, false);
    case_resize("nearest_rgb_down", ND_PIXFMT_RGB888, 40, 30, 17, 13, false);
    case_resize("nearest_rgba_up", ND_PIXFMT_RGBA8888, 32, 32, 96, 96, false);
    case_resize("nearest_rgba_tiny", ND_PIXFMT_RGBA8888, 32, 32, 3, 3, false);
    /* The accumulated 16.16 step can run one past the last source column at
     * an extreme ratio; Pillow leaves those pixels black rather than
     * clamping, and so must this. */
    case_resize("nearest_1px_blown_up", ND_PIXFMT_RGB888, 1, 1, 256, 256, false);
    case_resize("nearest_l_down", ND_PIXFMT_L8, 63, 41, 8, 5, false);
}

static void case_thumb(const char *name, nd_pixfmt fmt, int32_t sw, int32_t sh, int32_t mw,
                       int32_t mh)
{
    nd_image *src = make(fmt, sw, sh);

    if (!src) {
        fail("%s: out of memory", name);
        return;
    }
    if (nd_image_thumbnail(src, mw, mh) != ND_OK) {
        fail("%s: thumbnail failed", name);
        nd_image_free(src);
        return;
    }
    expect(src, name);
}

static void test_thumbnail(void)
{
    case_thumb("thumb_landscape", ND_PIXFMT_RGBA8888, 100, 40, 30, 30);
    case_thumb("thumb_portrait", ND_PIXFMT_RGBA8888, 40, 100, 30, 30);
    /* Never upscales: a 30x30 icon asked to fit 82x82 stays 30x30. */
    case_thumb("thumb_noop", ND_PIXFMT_RGBA8888, 30, 30, 82, 82);
    case_thumb("thumb_square", ND_PIXFMT_RGBA8888, 82, 82, 40, 40);
    case_thumb("thumb_extreme", ND_PIXFMT_RGB888, 7, 3, 2, 2);
    case_thumb("thumb_wide_only", ND_PIXFMT_RGB888, 120, 20, 40, 90);
}

/* ------------------------------------------------------------------ *
 * Pointwise
 * ------------------------------------------------------------------ */

static void case_convert(const char *name, nd_pixfmt from, nd_pixfmt to)
{
    nd_image *src = make(from, 21, 17);
    nd_image *out;

    if (!src) {
        fail("%s: out of memory", name);
        return;
    }
    out = nd_image_convert(src, to);
    nd_image_free(src);
    expect(out, name);
}

static void test_convert(void)
{
    case_convert("convert_rgb_to_l", ND_PIXFMT_RGB888, ND_PIXFMT_L8);
    case_convert("convert_rgba_to_l", ND_PIXFMT_RGBA8888, ND_PIXFMT_L8);
    case_convert("convert_l_to_rgb", ND_PIXFMT_L8, ND_PIXFMT_RGB888);
    case_convert("convert_l_to_rgba", ND_PIXFMT_L8, ND_PIXFMT_RGBA8888);
    case_convert("convert_rgb_to_rgba", ND_PIXFMT_RGB888, ND_PIXFMT_RGBA8888);
    case_convert("convert_rgba_to_rgb", ND_PIXFMT_RGBA8888, ND_PIXFMT_RGB888);
}

static void case_bright(const char *name, nd_pixfmt fmt, double f)
{
    nd_image *src = make(fmt, 21, 17);

    if (!src) {
        fail("%s: out of memory", name);
        return;
    }
    nd_image_brightness(src, f);
    expect(src, name);
}

static void test_brightness(void)
{
    /* 0.3 is the wallpaper dimming factor; the others prove the truncation
     * is not a rounding accident that only happens to work at 0.3. */
    case_bright("bright_rgb_0_3", ND_PIXFMT_RGB888, 0.3);
    case_bright("bright_rgb_1_0", ND_PIXFMT_RGB888, 1.0);
    case_bright("bright_rgb_0_5", ND_PIXFMT_RGB888, 0.5);
    case_bright("bright_rgb_2_0", ND_PIXFMT_RGB888, 2.0);
    /* Alpha is left alone. */
    case_bright("bright_rgba_0_3", ND_PIXFMT_RGBA8888, 0.3);
    case_bright("bright_l_0_3", ND_PIXFMT_L8, 0.3);
}

static void test_geometry(void)
{
    nd_image *im;

    im = make(ND_PIXFMT_RGB888, 21, 17);
    if (im) {
        nd_image_flip_h(im);
        expect(im, "flip_rgb");
    }
    im = make(ND_PIXFMT_RGBA8888, 20, 17);
    if (im) {
        nd_image_flip_h(im);
        expect(im, "flip_rgba_even");
    }

    /* PIL crop boxes are half-open; nd_rect is inclusive, so the reference
     * (5,4,21,19) becomes ND_RECT(5,4,20,18). This conversion is the single
     * easiest place in the whole port to lose a pixel. */
    im = make(ND_PIXFMT_RGBA8888, 40, 30);
    if (im) {
        expect(nd_image_crop(im, ND_RECT(5, 4, 20, 18)), "crop_inside");
        nd_image_free(im);
    }
    im = make(ND_PIXFMT_RGB888, 40, 30);
    if (im) {
        expect(nd_image_crop(im, ND_RECT(0, 0, 39, 29)), "crop_full");
        nd_image_free(im);
    }
    im = make(ND_PIXFMT_RGBA8888, 40, 30);
    if (im) {
        expect(nd_image_crop_zeropad(im, ND_RECT(-6, -3, 11, 8)), "crop_zeropad");
        expect(nd_image_crop_zeropad(im, ND_RECT(34, 25, 49, 39)), "crop_zeropad_far");
        nd_image_free(im);
    }
}

/* ------------------------------------------------------------------ *
 * Compositing
 * ------------------------------------------------------------------ */

static void case_paste(const char *name, nd_pixfmt dfmt, nd_pixfmt sfmt, int32_t x, int32_t y,
                       int mask_kind)
{
    nd_image *dst = make(dfmt, 40, 30);
    nd_image *src = make(sfmt, 16, 12);
    nd_image *mask = (mask_kind == 2) ? make(ND_PIXFMT_L8, 16, 12) : NULL;

    if (!dst || !src || (mask_kind == 2 && !mask)) {
        fail("%s: out of memory", name);
        nd_image_free(dst);
        nd_image_free(src);
        nd_image_free(mask);
        return;
    }
    if (mask_kind == 1)
        nd_image_blit_alpha(dst, src, x, y);
    else if (mask_kind == 2)
        nd_image_blit_mask(dst, src, mask, x, y);
    else
        nd_image_blit(dst, src, x, y);

    nd_image_free(src);
    nd_image_free(mask);
    expect(dst, name);
}

static void case_region(const char *name, int32_t x, int32_t y, nd_rect box)
{
    nd_image *dst = make(ND_PIXFMT_RGB888, 40, 30);
    nd_image *src = make(ND_PIXFMT_RGB888, 24, 20);

    if (!dst || !src) {
        fail("%s: out of memory", name);
        nd_image_free(dst);
        nd_image_free(src);
        return;
    }
    nd_image_blit_region(dst, src, box, x, y);
    nd_image_free(src);
    expect(dst, name);
}

static void case_fill(const char *name, nd_pixfmt fmt, nd_rect box, nd_color c)
{
    nd_image *im = make(fmt, 40, 30);

    if (!im) {
        fail("%s: out of memory", name);
        return;
    }
    nd_image_fill_rect(im, box, c);
    expect(im, name);
}

static void test_paste(void)
{
    case_paste("paste_rgb_rgb", ND_PIXFMT_RGB888, ND_PIXFMT_RGB888, 5, 4, 0);
    /* A paste at a negative offset moves the SOURCE origin, it does not
     * refuse the call. Widgets do this while a list is scrolling. */
    case_paste("paste_rgb_rgb_negoff", ND_PIXFMT_RGB888, ND_PIXFMT_RGB888, -6, -3, 0);
    case_paste("paste_rgb_rgb_overhang", ND_PIXFMT_RGB888, ND_PIXFMT_RGB888, 32, 24, 0);
    case_paste("paste_rgb_rgba_alpha", ND_PIXFMT_RGB888, ND_PIXFMT_RGBA8888, 5, 4, 1);
    case_paste("paste_rgba_rgba_alpha", ND_PIXFMT_RGBA8888, ND_PIXFMT_RGBA8888, 5, 4, 1);
    case_paste("paste_rgba_rgba_alpha_negoff", ND_PIXFMT_RGBA8888, ND_PIXFMT_RGBA8888, -6, -3, 1);
    case_paste("paste_rgb_rgb_lmask", ND_PIXFMT_RGB888, ND_PIXFMT_RGB888, 5, 4, 2);
    case_paste("paste_rgba_rgba_lmask", ND_PIXFMT_RGBA8888, ND_PIXFMT_RGBA8888, 7, 2, 2);
    /* Mismatched modes are converted on the way in, exactly as PIL's paste
     * does before it touches the surface. */
    case_paste("paste_l_from_rgb", ND_PIXFMT_L8, ND_PIXFMT_RGB888, 5, 4, 0);
    case_paste("paste_rgb_from_l", ND_PIXFMT_RGB888, ND_PIXFMT_L8, 5, 4, 0);

    case_region("paste_region_mid", 6, 5, ND_RECT(4, 3, 14, 11));
    case_region("paste_region_overhang", 33, 26, ND_RECT(0, 0, 23, 19));
    case_region("paste_region_negoff", -4, -2, ND_RECT(2, 2, 19, 17));

    case_fill("fill_rect_rgb", ND_PIXFMT_RGB888, ND_RECT(5, 4, 20, 18), ND_RGB(200, 30, 90));
    case_fill("fill_rect_rgba", ND_PIXFMT_RGBA8888, ND_RECT(5, 4, 20, 18),
              ND_RGBA(200, 30, 90, 128));
    case_fill("fill_rect_clip", ND_PIXFMT_RGB888, ND_RECT(30, 22, 59, 49), ND_RGB(11, 22, 33));
    case_fill("fill_rect_l", ND_PIXFMT_L8, ND_RECT(5, 4, 20, 18), ND_RGB(77, 77, 77));
}

static void test_point_lut(void)
{
    uint8_t ident[256], thresh[256], half[256];
    int32_t i;
    nd_image *im;

    for (i = 0; i < 256; i++) {
        ident[i] = (uint8_t)i;
        thresh[i] = (i > 40) ? 255u : 0u;
        half[i] = (uint8_t)(i / 2);
    }

    /* Koki's three forms: threshold the alpha, halve the colour, and the
     * plain identity that proves a NULL table leaves a band alone. */
    im = make(ND_PIXFMT_RGBA8888, 21, 17);
    if (im) {
        nd_image_point_lut(im, NULL, NULL, NULL, thresh);
        expect(im, "point_alpha_thresh");
    }
    im = make(ND_PIXFMT_RGBA8888, 21, 17);
    if (im) {
        nd_image_point_lut(im, half, half, half, ident);
        expect(im, "point_rgb_half");
    }
    im = make(ND_PIXFMT_L8, 21, 17);
    if (im) {
        nd_image_point_lut(im, thresh, NULL, NULL, NULL);
        expect(im, "point_l_thresh");
    }
}

/* ------------------------------------------------------------------ *
 * Codecs
 * ------------------------------------------------------------------ */

static void test_codecs(void)
{
    size_t i;

    for (i = 0; i < ND_ARRAY_LEN(ND_CODEC_REF); i++) {
        const nd_codec_ref *r = &ND_CODEC_REF[i];
        nd_image *img = nd_image_open_mem(r->blob, r->blob_len);
        nd_image *rgb;

        checks++;
        if (!img) {
            fail("%s (%s): failed to decode", r->name, r->note);
            continue;
        }
        if (img->w != r->w || img->h != r->h) {
            fail("%s: decoded %dx%d, want %dx%d", r->name, (int)img->w, (int)img->h, (int)r->w,
                 (int)r->h);
            nd_image_free(img);
            continue;
        }
        /* The reference is always compared in RGB: which of RGB and RGBA the
         * decoder hands back depends on the file, and convert() is the same
         * normalisation every caller in the project applies anyway. */
        rgb = nd_image_convert(img, r->bands == 4 ? ND_PIXFMT_RGBA8888 : ND_PIXFMT_RGB888);
        nd_image_free(img);
        if (!rgb) {
            fail("%s: convert failed", r->name);
            continue;
        }
        if (hash_image(rgb) != r->hash)
            fail("%s (%s): pixels differ from Pillow", r->name, r->note);
        nd_image_free(rgb);
    }
}

static void test_png_roundtrip(void)
{
    /* Under NEODCT_ROOT, which `make test` points at a fresh mktemp -d. The
     * path is written the way the phone spells it and resolved by the path
     * layer, so this exercises the real call path rather than a /tmp shortcut. */
    static const char *path = "/NeoDCT/User/logs/test_image_roundtrip.png";
    nd_image *src = make(ND_PIXFMT_RGBA8888, 23, 19);
    nd_image *back = NULL;

    if (!src)
        return;
    if (nd_mkdir_p(ND_PATH_LOG_DIR, 0755u) != ND_OK) {
        fail("png roundtrip: cannot create %s", ND_PATH_LOG_DIR);
        nd_image_free(src);
        return;
    }
    CHECK(nd_image_save_png(src, path) == ND_OK, "nd_image_save_png failed");
    back = nd_image_open(path);
    CHECK(back != NULL, "nd_image_open could not read back the PNG just written");
    if (back) {
        CHECK(back->w == src->w && back->h == src->h, "roundtrip size changed");
        CHECK(back->fmt == ND_PIXFMT_RGBA8888, "roundtrip lost the alpha channel");
        CHECK(hash_image(back) == hash_image(src), "roundtrip changed the pixels");
        nd_image_free(back);
    }
    nd_image_free(src);
}

/* ------------------------------------------------------------------ *
 * Contract checks with no Pillow counterpart
 * ------------------------------------------------------------------ */

static void test_blend_formula(void)
{
    /* The one formula the whole text renderer stands on. These four triples
     * are values where the +127 truncating divide and the MULDIV255 macro
     * disagree, so a "tidier" blend cannot pass this by accident. */
    CHECK(nd_blend8(1u, 2u, 64u) == 1u, "nd_blend8(1,2,64) should be 1");
    /* Deliberately NOT symmetric: a half mask over black gives 128 and over
     * white gives 127, because the +127 bias sits on the numerator rather
     * than on each term. A "cleaner" blend makes these agree and loses. */
    CHECK(nd_blend8(0u, 255u, 128u) == 128u, "nd_blend8(0,255,128) should be 128");
    CHECK(nd_blend8(255u, 0u, 128u) == 127u, "nd_blend8(255,0,128) should be 127");
    CHECK(nd_blend8(200u, 50u, 0u) == 200u, "a zero mask leaves the destination alone");
    CHECK(nd_blend8(200u, 50u, 255u) == 50u, "a full mask replaces the destination");
    CHECK(nd_blend8(17u, 240u, 1u) == 18u, "nd_blend8(17,240,1) should be 18");
}

static void test_lifecycle(void)
{
    uint8_t backing[4 * 3 * 5];
    nd_image *img;

    CHECK(nd_image_new(0, 10, ND_PIXFMT_RGB888) == NULL, "a zero-width surface is refused");
    CHECK(nd_image_new(10, -1, ND_PIXFMT_RGB888) == NULL, "a negative height is refused");
    CHECK(nd_image_new(100000, 10, ND_PIXFMT_RGB888) == NULL, "an absurd width is refused");

    img = nd_image_new_filled(4, 3, ND_PIXFMT_RGBA8888, ND_RGBA(1, 2, 3, 4));
    CHECK(img != NULL, "nd_image_new_filled failed");
    if (img) {
        nd_color c = nd_image_get_px(img, 3, 2);
        CHECK(c.r == 1u && c.g == 2u && c.b == 3u && c.a == 4u, "fill did not reach the corner");
        CHECK(img->stride == 16u, "a 4px RGBA row is 16 bytes, got %zu", img->stride);
        /* Out of range reads are transparent black and writes are dropped;
         * widget code relies on both while scrolling. */
        c = nd_image_get_px(img, 99, 99);
        CHECK(c.r == 0u && c.a == 0u, "an out-of-range read should be transparent black");
        nd_image_set_px(img, -1, -1, ND_WHITE);
        nd_image_free(img);
    }

    /* A borrowed header must not free the caller's pixels, and must honour a
     * stride wider than the row -- that is how the framebuffer is wrapped. */
    memset(backing, 0x5A, sizeof backing);
    img = nd_image_borrow(backing, 3, 5, ND_PIXFMT_RGB888, 4 * 3);
    CHECK(img != NULL, "nd_image_borrow failed");
    if (img) {
        CHECK(img->borrowed, "a borrowed surface must say so");
        nd_image_set_px(img, 0, 1, ND_RGB(1, 2, 3));
        CHECK(backing[12] == 1u && backing[13] == 2u && backing[14] == 3u,
              "a borrowed write must land at row * stride");
        CHECK(backing[11] == 0x5Au, "the stride padding must be untouched");
        nd_image_free(img);
        CHECK(backing[12] == 1u, "freeing a borrowed header must not free the pixels");
    }
    CHECK(nd_image_borrow(backing, 3, 5, ND_PIXFMT_RGB888, 8) == NULL,
          "a stride narrower than the row is refused");
    nd_image_free(NULL);
}

static void test_tobytes(void)
{
    nd_image *img = make(ND_PIXFMT_RGB888, 5, 4);
    uint8_t buf[5 * 4 * 3];

    if (!img)
        return;
    CHECK(nd_image_tobytes(img, buf, sizeof buf) == ND_OK, "tobytes of an exact buffer");
    CHECK(nd_image_tobytes(img, buf, sizeof buf - 1u) == ND_ERR_TOOLONG,
          "tobytes must refuse a short buffer rather than overrun it");
    CHECK(nd_image_tobytes(NULL, buf, sizeof buf) == ND_ERR_INVAL, "tobytes(NULL) is invalid");
    nd_image_free(img);
}

static void test_masks_and_bbox(void)
{
    nd_image *a = nd_image_new_filled(8, 8, ND_PIXFMT_L8, ND_BLACK);
    nd_image *b = nd_image_new_filled(8, 8, ND_PIXFMT_L8, ND_BLACK);
    nd_image *rgba = nd_image_new_filled(8, 8, ND_PIXFMT_RGBA8888, ND_RGBA(0, 0, 0, 0));
    nd_rect box;

    if (!a || !b || !rgba) {
        nd_image_free(a);
        nd_image_free(b);
        nd_image_free(rgba);
        return;
    }

    nd_image_fill_rect(a, ND_RECT(1, 1, 4, 4), ND_RGB(255, 255, 255));
    nd_image_fill_rect(b, ND_RECT(5, 5, 7, 7), ND_RGB(255, 255, 255));
    CHECK(!nd_image_masks_overlap(a, b), "disjoint masks must not overlap");
    nd_image_fill_rect(b, ND_RECT(4, 4, 4, 4), ND_RGB(255, 255, 255));
    CHECK(nd_image_masks_overlap(a, b), "masks touching at one pixel do overlap");

    CHECK(!nd_image_alpha_bbox(rgba, &box), "an entirely empty surface has no bbox");
    /* Pillow's rule is all FOUR channels zero, not alpha alone: a fully
     * transparent white pixel still counts as ink. */
    nd_image_set_px(rgba, 6, 2, ND_RGBA(255, 255, 255, 0));
    CHECK(nd_image_alpha_bbox(rgba, &box), "a transparent white pixel is still ink");
    if (nd_image_alpha_bbox(rgba, &box))
        CHECK(box.x0 == 6 && box.y0 == 2 && box.x1 == 6 && box.y1 == 2,
              "bbox should be the single ink pixel, got (%d,%d,%d,%d)", (int)box.x0, (int)box.y0,
              (int)box.x1, (int)box.y1);
    nd_image_set_px(rgba, 1, 5, ND_RGBA(0, 0, 0, 9));
    if (nd_image_alpha_bbox(rgba, &box))
        CHECK(box.x0 == 1 && box.y0 == 2 && box.x1 == 6 && box.y1 == 5,
              "bbox is inclusive of both corners, got (%d,%d,%d,%d)", (int)box.x0, (int)box.y0,
              (int)box.x1, (int)box.y1);

    nd_image_free(a);
    nd_image_free(b);
    nd_image_free(rgba);
}

static void test_missing_files(void)
{
    /* A missing file is None in the Python, swallowed by a bare except. It
     * must not be a crash and must not be a log-worthy error here either. */
    CHECK(nd_image_open("/NeoDCT/User/definitely-not-here.png") == NULL,
          "opening a missing file returns NULL");
    CHECK(nd_image_open(NULL) == NULL, "opening NULL returns NULL");
    CHECK(nd_image_open_mem(NULL, 100) == NULL, "decoding NULL returns NULL");
    CHECK(nd_image_open_mem((const uint8_t *)"not an image at all", 19) == NULL,
          "decoding rubbish returns NULL");
}

int main(void)
{
    test_resize();
    test_thumbnail();
    test_convert();
    test_brightness();
    test_geometry();
    test_paste();
    test_point_lut();
    test_codecs();
    test_png_roundtrip();
    test_blend_formula();
    test_lifecycle();
    test_tobytes();
    test_masks_and_bbox();
    test_missing_files();

    if (cursor != ND_ARRAY_LEN(ND_IMAGE_REF))
        fail("only %zu of %zu reference cases were exercised", cursor, ND_ARRAY_LEN(ND_IMAGE_REF));

    if (failures) {
        printf("test_image: %d check(s), %d failure(s)\n", checks, failures);
        return 1;
    }
    printf("test_image: %d checks against Pillow, 0 failures\n", checks);
    return 0;
}

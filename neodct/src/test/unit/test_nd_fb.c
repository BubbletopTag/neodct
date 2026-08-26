/* test_nd_fb.c -- the framebuffer against the Python driver's own output.
 *
 * The reference in fb_ref.inc was produced by running the real Framebuffer
 * class out of System/core/main.py against a fake mmap (tools/gen_fb_ref.py),
 * so what is being compared is not a transcription of the driver but the
 * driver. Each case hashes the WHOLE mapping afterwards, which is what makes
 * the awkward parts checkable: the letterbox rows the fast path never touches,
 * the row padding a stride mismatch leaves alone, and the alpha-255 black the
 * slow path writes where the fast path leaves zeros.
 *
 * Runs with no arguments and touches no files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_capture.h"
#include "nd_fb.h"
#include "nd_image.h"
#include "nd_types.h"

#include "fb_ref.inc"

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

/* Bit-for-bit identical to synth() in tools/gen_fb_ref.py and
 * tools/gen_image_ref.py. Unsigned throughout so neither language can
 * disagree about overflow or shifts. */
static uint8_t synth(int32_t x, int32_t y, int32_t c)
{
    uint32_t v = ((uint32_t)x * 2654435761u) ^ ((uint32_t)y * 40503u) ^ ((uint32_t)c * 97u);

    v ^= v >> 13;
    v *= 0x5BD1E995u;
    v ^= v >> 15;
    return (uint8_t)(v & 0xFFu);
}

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325u;
    size_t i;

    for (i = 0u; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3u;
    }
    return h;
}

/* owned by the caller; free with nd_image_free() */
static nd_image *make_src(int32_t w, int32_t h, bool rgba)
{
    nd_image *img = nd_image_new(w, h, rgba ? ND_PIXFMT_RGBA8888 : ND_PIXFMT_RGB888);
    int32_t x, y, c;

    if (img == NULL)
        return NULL;
    for (y = 0; y < h; y++) {
        uint8_t *row = img->pixels + (size_t)y * img->stride;
        for (x = 0; x < w; x++) {
            for (c = 0; c < (int32_t)img->bpp; c++)
                row[(size_t)x * img->bpp + (size_t)c] = synth(x, y, c);
        }
    }
    return img;
}

/* ------------------------------------------------------------------ *
 * The corpus
 * ------------------------------------------------------------------ */

static void test_against_python(void)
{
    size_t i;

    for (i = 0u; i < ND_ARRAY_LEN(ND_FB_REF); i++) {
        const nd_fb_ref *r = &ND_FB_REF[i];
        nd_fb *fb = NULL;
        nd_image *src = NULL;
        const uint8_t *bytes;
        size_t size = 0u;
        nd_err rc;

        /* line_length is passed explicitly rather than left to the fallback,
         * because two of the cases exist precisely to exercise a stride the
         * fallback would not have produced. */
        rc = nd_fb_open_mem(&fb, r->xres, r->yres, r->bpp, r->line_length);
        CHECK(rc == ND_OK, "%s: nd_fb_open_mem -> %s", r->name, nd_strerror(rc));
        if (rc != ND_OK)
            continue;

        CHECK(nd_fb_xres(fb) == r->xres, "%s: xres %d != %d", r->name, nd_fb_xres(fb), r->xres);
        CHECK(nd_fb_yres(fb) == r->yres, "%s: yres %d != %d", r->name, nd_fb_yres(fb), r->yres);
        CHECK(nd_fb_bpp(fb) == r->bpp, "%s: bpp %d != %d", r->name, nd_fb_bpp(fb), r->bpp);
        CHECK(nd_fb_line_length(fb) == r->line_length, "%s: stride %zu != %zu", r->name,
              nd_fb_line_length(fb), r->line_length);

        bytes = nd_fb_mem_bytes(fb, &size);
        CHECK(bytes != NULL, "%s: no backing bytes", r->name);
        CHECK(size == r->map_size, "%s: mapping %zu != %zu", r->name, size, r->map_size);

        /* The mapping is zeroed at open and everything downstream depends on
         * it, so check that before anything is drawn. */
        if (bytes != NULL && size == r->map_size) {
            size_t j;
            bool clean = true;
            for (j = 0u; j < size; j++) {
                if (bytes[j] != 0u) {
                    clean = false;
                    break;
                }
            }
            CHECK(clean, "%s: mapping not zeroed at open", r->name);
        }

        src = make_src(r->sw, r->sh, r->src_rgba);
        CHECK(src != NULL, "%s: source allocation failed", r->name);
        if (src != NULL) {
            rc = nd_fb_update(fb, src);
            CHECK(rc == ND_OK, "%s: nd_fb_update -> %s", r->name, nd_strerror(rc));
            if (rc == ND_OK && bytes != NULL) {
                uint64_t got = fnv1a(bytes, size);
                CHECK(got == r->hash, "%s: mapping hash 0x%016llx != 0x%016llx", r->name,
                      (unsigned long long)got, (unsigned long long)r->hash);
            }
        }

        nd_image_free(src);
        nd_fb_close(fb);
    }
}

/* ------------------------------------------------------------------ *
 * The packers on their own
 * ------------------------------------------------------------------ */

static void test_packers(void)
{
    size_t i;

    for (i = 0u; i < ND_ARRAY_LEN(ND_FB_PX_REF); i++) {
        const nd_fb_px_ref *r = &ND_FB_PX_REF[i];
        nd_image *one = nd_image_new(1, 1, ND_PIXFMT_RGB888);
        uint8_t out[4];
        nd_err rc;

        CHECK(one != NULL, "1x1 allocation failed");
        if (one == NULL)
            continue;
        one->pixels[0] = r->rgb[0];
        one->pixels[1] = r->rgb[1];
        one->pixels[2] = r->rgb[2];

        memset(out, 0xAA, sizeof out);
        rc = nd_fb_pack_bgra(one, out, sizeof out);
        CHECK(rc == ND_OK, "pack_bgra -> %s", nd_strerror(rc));
        CHECK(memcmp(out, r->bgra, 4u) == 0,
              "bgra(%u,%u,%u) = %02x %02x %02x %02x, want %02x %02x %02x %02x", r->rgb[0],
              r->rgb[1], r->rgb[2], out[0], out[1], out[2], out[3], r->bgra[0], r->bgra[1],
              r->bgra[2], r->bgra[3]);

        memset(out, 0xAA, sizeof out);
        rc = nd_fb_pack_rgb565(one, out, sizeof out);
        CHECK(rc == ND_OK, "pack_rgb565 -> %s", nd_strerror(rc));
        CHECK(memcmp(out, r->rgb565, 2u) == 0, "rgb565(%u,%u,%u) = %02x %02x, want %02x %02x",
              r->rgb[0], r->rgb[1], r->rgb[2], out[0], out[1], r->rgb565[0], r->rgb565[1]);

        nd_image_free(one);
    }
}

static void test_packer_bounds(void)
{
    nd_image *img = nd_image_new(4, 3, ND_PIXFMT_RGB888);
    nd_image *grey = nd_image_new(4, 3, ND_PIXFMT_L8);
    uint8_t buf[4 * 3 * 4];

    CHECK(img != NULL && grey != NULL, "allocation failed");
    if (img == NULL || grey == NULL) {
        nd_image_free(img);
        nd_image_free(grey);
        return;
    }
    memset(img->pixels, 0x40, img->stride * 3u);

    CHECK(nd_fb_pack_bgra(img, buf, sizeof buf) == ND_OK, "exact-size bgra buffer refused");
    CHECK(nd_fb_pack_bgra(img, buf, sizeof buf - 1u) == ND_ERR_TOOLONG,
          "short bgra buffer accepted");
    CHECK(nd_fb_pack_rgb565(img, buf, 4u * 3u * 2u) == ND_OK, "exact-size 565 buffer refused");
    CHECK(nd_fb_pack_rgb565(img, buf, 4u * 3u * 2u - 1u) == ND_ERR_TOOLONG,
          "short 565 buffer accepted");

    /* nd_fb.h: the source must be RGB888 or RGBA8888. An L8 mask reaching the
     * packer would read two bytes past every pixel. */
    CHECK(nd_fb_pack_bgra(grey, buf, sizeof buf) == ND_ERR_INVAL, "L8 source accepted");
    CHECK(nd_fb_pack_rgb565(grey, buf, sizeof buf) == ND_ERR_INVAL, "L8 source accepted");
    CHECK(nd_fb_pack_bgra(NULL, buf, sizeof buf) == ND_ERR_INVAL, "NULL source accepted");
    CHECK(nd_fb_pack_bgra(img, NULL, sizeof buf) == ND_ERR_INVAL, "NULL destination accepted");

    nd_image_free(img);
    nd_image_free(grey);
}

/* ------------------------------------------------------------------ *
 * The band lands where the panel expects it
 * ------------------------------------------------------------------ */

/* test_uistub.py::test_device_frame_band_starts_at_row_32 pins this from the
 * other side. A 240x175 band in a genuine 240x240 framebuffer starts at row
 * 32, and rows 0..31 and 207..239 stay black -- which they are only because
 * the mapping was zeroed at open and the fast path never writes them. */
static void test_band_at_row_32(void)
{
    nd_fb *fb = NULL;
    nd_image *src = NULL;
    const uint8_t *bytes;
    size_t size = 0u;
    size_t line;
    int32_t y;
    bool letterbox_black = true;
    bool band_opaque = true;

    CHECK(nd_fb_open_mem(&fb, 240, 240, 32, 0u) == ND_OK, "240x240 open failed");
    if (fb == NULL)
        return;

    line = nd_fb_line_length(fb);
    CHECK(line == 960u, "fallback stride %zu != 960", line);

    src = nd_image_new_filled(240, 175, ND_PIXFMT_RGB888, ND_RGB(255, 0, 0));
    CHECK(src != NULL, "band allocation failed");
    if (src == NULL) {
        nd_fb_close(fb);
        return;
    }

    CHECK(nd_fb_update(fb, src) == ND_OK, "update failed");
    bytes = nd_fb_mem_bytes(fb, &size);
    CHECK(bytes != NULL && size == 240u * 960u, "unexpected mapping size");
    if (bytes == NULL) {
        nd_image_free(src);
        nd_fb_close(fb);
        return;
    }

    for (y = 0; y < 240; y++) {
        const uint8_t *row = bytes + (size_t)y * line;
        if (y < 32 || y >= 32 + 175) {
            size_t j;
            for (j = 0u; j < line; j++) {
                if (row[j] != 0u)
                    letterbox_black = false;
            }
        } else {
            /* Red is 00 00 ff ff. The alpha byte is the one that separates
             * the fast path's black from the slow path's. */
            if (row[0] != 0u || row[1] != 0u || row[2] != 255u || row[3] != 255u)
                band_opaque = false;
        }
    }
    CHECK(letterbox_black, "letterbox rows are not black");
    CHECK(band_opaque, "band pixel is not 00 00 ff ff");

    nd_image_free(src);
    nd_fb_close(fb);
}

/* Side padding as well: a 320-wide framebuffer puts a 240-wide band at
 * dst_x = 40, and the 40 columns on each side stay black. */
static void test_side_padding(void)
{
    nd_fb *fb = NULL;
    nd_image *src = NULL;
    const uint8_t *bytes;
    size_t size = 0u;
    bool margins_black = true;
    bool band_right = true;
    int32_t y;

    CHECK(nd_fb_open_mem(&fb, 320, 240, 32, 0u) == ND_OK, "320x240 open failed");
    if (fb == NULL)
        return;
    src = nd_image_new_filled(240, 175, ND_PIXFMT_RGB888, ND_RGB(0, 0, 255));
    CHECK(src != NULL, "band allocation failed");
    if (src == NULL) {
        nd_fb_close(fb);
        return;
    }
    CHECK(nd_fb_update(fb, src) == ND_OK, "update failed");
    bytes = nd_fb_mem_bytes(fb, &size);
    if (bytes == NULL) {
        nd_image_free(src);
        nd_fb_close(fb);
        return;
    }

    for (y = 32; y < 32 + 175; y++) {
        const uint8_t *row = bytes + (size_t)y * 1280u;
        size_t j;

        for (j = 0u; j < 40u * 4u; j++) {
            if (row[j] != 0u || row[1280u - 1u - j] != 0u)
                margins_black = false;
        }
        /* Blue is ff 00 00 ff. */
        if (row[40u * 4u] != 255u || row[40u * 4u + 3u] != 255u)
            band_right = false;
    }
    CHECK(margins_black, "side margins are not black");
    CHECK(band_right, "band does not start at column 40");

    nd_image_free(src);
    nd_fb_close(fb);
}

/* ------------------------------------------------------------------ *
 * Refusals
 * ------------------------------------------------------------------ */

static void test_bad_geometry(void)
{
    nd_fb *fb = NULL;

    CHECK(nd_fb_open_mem(&fb, 0, 175, 32, 0u) == ND_ERR_INVAL, "zero width accepted");
    CHECK(nd_fb_open_mem(&fb, 240, -1, 32, 0u) == ND_ERR_INVAL, "negative height accepted");
    CHECK(nd_fb_open_mem(&fb, 240, 175, 0, 0u) == ND_ERR_INVAL, "zero depth accepted");
    /* A stride shorter than a row of pixels is a driver bug. The Python
     * discovers it as an mmap write error halfway through a frame; refusing at
     * open names it instead. */
    CHECK(nd_fb_open_mem(&fb, 240, 175, 32, 900u) == ND_ERR_HARDWARE, "short stride accepted");
    CHECK(nd_fb_open_mem(NULL, 240, 175, 32, 0u) == ND_ERR_INVAL, "NULL out accepted");
    CHECK(fb == NULL, "a refused open left a framebuffer behind");
}

static void test_update_refusals(void)
{
    nd_fb *fb = NULL;
    nd_image *grey = nd_image_new(240, 175, ND_PIXFMT_L8);

    CHECK(nd_fb_open_mem(&fb, 240, 175, 32, 0u) == ND_OK, "open failed");
    CHECK(grey != NULL, "L8 allocation failed");
    if (fb == NULL || grey == NULL) {
        nd_image_free(grey);
        nd_fb_close(fb);
        return;
    }
    CHECK(nd_fb_update(fb, NULL) == ND_ERR_INVAL, "NULL source accepted");
    CHECK(nd_fb_update(NULL, grey) == ND_ERR_INVAL, "NULL framebuffer accepted");
    /* update() converts to RGB in Python; in C an L8 canvas is a caller bug,
     * because nothing in the project ever presents one. */
    CHECK(nd_fb_update(fb, grey) == ND_ERR_INVAL, "L8 canvas accepted");

    nd_image_free(grey);
    nd_fb_close(fb);
}

int main(void)
{
    test_against_python();
    test_packers();
    test_packer_bounds();
    test_band_at_row_32();
    test_side_padding();
    test_bad_geometry();
    test_update_refusals();

    printf("test_nd_fb: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

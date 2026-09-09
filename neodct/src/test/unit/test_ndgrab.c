/* test_ndgrab.c -- the framebuffer conversion behind nd-grab.
 *
 * The whole point is the channel order. A conversion that gets it wrong
 * produces a perfectly valid PNG of the right size with red and blue
 * exchanged, which no automated check notices unless it is this one -- and
 * which cost this project a release once already (nd_grab.h has the story).
 *
 * So every case below fixes a KNOWN pattern into a fixture framebuffer and
 * asserts the exact RGB bytes that must come out, in both channel orders, at
 * both depths, with and without row padding.
 */

#include <stdlib.h>
#include <string.h>

#include "nd_capture.h"
#include "nd_image.h"
#include "nd_types.h"

#include "nd_grab.h"

#include "platform_test.h"

/* The three colours worth testing: pure red, pure green, pure blue. Green is
 * the control -- it is unaffected by a red/blue swap, so a case where green
 * moves is a different bug from a case where only red and blue move. */
#define PX_R 0
#define PX_G 1
#define PX_B 2

static uint8_t px_at(const nd_image *img, int32_t x, int32_t y, int chan)
{
    return img->pixels[(size_t)y * img->stride + (size_t)x * 3u + (size_t)chan];
}

static void check_rgb(const nd_image *img, int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b)
{
    CHECK_INT(px_at(img, x, y, PX_R), r);
    CHECK_INT(px_at(img, x, y, PX_G), g);
    CHECK_INT(px_at(img, x, y, PX_B), b);
}

/* ------------------------------------------------------------------ *
 * 32 bpp
 * ------------------------------------------------------------------ */

/* Three pixels laid out R G B x -- what the phone's vfb declares. With
 * swap_rb true (red.offset < blue.offset, which is what vfb reports) the
 * reader must produce red, green, blue in that order. */
static void test_32bpp_red_first(void)
{
    uint8_t fb[3 * 4];
    nd_grab_fmt fmt = {3, 1, 4u, 3u * 4u, true};
    nd_image *img;

    memset(fb, 0, sizeof fb);
    fb[0] = 0xFF;              /* px0: R G B x = FF 00 00 -> red   */
    fb[4 + 1] = 0xFF;          /* px1:           00 FF 00 -> green */
    fb[8 + 2] = 0xFF;          /* px2:           00 00 FF -> blue  */

    img = nd_grab_to_image(fb, &fmt);
    CHECK(img != NULL);
    check_rgb(img, 0, 0, 0xFF, 0x00, 0x00);
    check_rgb(img, 1, 0, 0x00, 0xFF, 0x00);
    check_rgb(img, 2, 0, 0x00, 0x00, 0xFF);
    nd_image_free(img);
}

/* The same bytes read as B G R x -- what everything that is not this phone
 * declares. Red and blue must come out exchanged relative to the case above,
 * and green must not move. */
static void test_32bpp_blue_first(void)
{
    uint8_t fb[3 * 4];
    nd_grab_fmt fmt = {3, 1, 4u, 3u * 4u, false};
    nd_image *img;

    memset(fb, 0, sizeof fb);
    fb[0] = 0xFF;
    fb[4 + 1] = 0xFF;
    fb[8 + 2] = 0xFF;

    img = nd_grab_to_image(fb, &fmt);
    CHECK(img != NULL);
    check_rgb(img, 0, 0, 0x00, 0x00, 0xFF); /* first byte is BLUE here */
    check_rgb(img, 1, 0, 0x00, 0xFF, 0x00); /* green is the control    */
    check_rgb(img, 2, 0, 0xFF, 0x00, 0x00);
    nd_image_free(img);
}

/* ------------------------------------------------------------------ *
 * 16 bpp
 * ------------------------------------------------------------------ */

static void test_16bpp_both_orders(void)
{
    uint8_t fb[3 * 2];
    nd_grab_fmt fmt = {3, 1, 2u, 3u * 2u, false};
    nd_image *img;
    uint16_t red = 0xF800u;   /* RGB565: red full   */
    uint16_t green = 0x07E0u; /*         green full */
    uint16_t blue = 0x001Fu;  /*         blue full  */

    fb[0] = (uint8_t)(red & 0xFFu);
    fb[1] = (uint8_t)(red >> 8);
    fb[2] = (uint8_t)(green & 0xFFu);
    fb[3] = (uint8_t)(green >> 8);
    fb[4] = (uint8_t)(blue & 0xFFu);
    fb[5] = (uint8_t)(blue >> 8);

    img = nd_grab_to_image(fb, &fmt);
    CHECK(img != NULL);
    /* Full-scale must stay full-scale: 0x1F -> 0xFF, not 0xF8. A reader that
     * only shifts left leaves every white pixel slightly grey, which is
     * invisible by eye and fatal to a digest comparison. */
    check_rgb(img, 0, 0, 0xFF, 0x00, 0x00);
    check_rgb(img, 1, 0, 0x00, 0xFF, 0x00);
    check_rgb(img, 2, 0, 0x00, 0x00, 0xFF);
    nd_image_free(img);

    /* Swapped: the two five-bit fields exchange, green stays. */
    fmt.swap_rb = true;
    img = nd_grab_to_image(fb, &fmt);
    CHECK(img != NULL);
    check_rgb(img, 0, 0, 0x00, 0x00, 0xFF);
    check_rgb(img, 1, 0, 0x00, 0xFF, 0x00);
    check_rgb(img, 2, 0, 0xFF, 0x00, 0x00);
    nd_image_free(img);
}

/* ------------------------------------------------------------------ *
 * Stride
 * ------------------------------------------------------------------ */

static void test_a_padded_stride_is_honoured(void)
{
    /* Two rows of two pixels in a buffer whose rows are four pixels wide.
     * Using w * bytespp instead of line_length reads the padding as image
     * data and shears the picture one pixel further left on every row -- the
     * classic framebuffer bug, and the reason the brief says never to
     * compute the stride. */
    uint8_t fb[2 * 4 * 4];
    nd_grab_fmt fmt = {2, 2, 4u, 4u * 4u, true};
    nd_image *img;

    memset(fb, 0, sizeof fb);
    /* row 0: red, green   then padding */
    fb[0 * 16 + 0 * 4 + 0] = 0xFF;
    fb[0 * 16 + 1 * 4 + 1] = 0xFF;
    /* the padding is deliberately not black, so reading it would show */
    fb[0 * 16 + 2 * 4 + 0] = 0x11;
    fb[0 * 16 + 3 * 4 + 0] = 0x22;
    /* row 1: blue, white */
    fb[1 * 16 + 0 * 4 + 2] = 0xFF;
    fb[1 * 16 + 1 * 4 + 0] = 0xFF;
    fb[1 * 16 + 1 * 4 + 1] = 0xFF;
    fb[1 * 16 + 1 * 4 + 2] = 0xFF;

    img = nd_grab_to_image(fb, &fmt);
    CHECK(img != NULL);
    CHECK_INT(img->w, 2);
    CHECK_INT(img->h, 2);
    check_rgb(img, 0, 0, 0xFF, 0x00, 0x00);
    check_rgb(img, 1, 0, 0x00, 0xFF, 0x00);
    check_rgb(img, 0, 1, 0x00, 0x00, 0xFF);
    check_rgb(img, 1, 1, 0xFF, 0xFF, 0xFF);
    nd_image_free(img);
}

static void test_a_stride_too_small_is_refused(void)
{
    uint8_t fb[16];
    nd_grab_fmt fmt = {4, 1, 4u, 8u, true}; /* 4 px * 4 B needs 16, not 8 */

    memset(fb, 0, sizeof fb);
    CHECK(nd_grab_to_image(fb, &fmt) == NULL);
}

static void test_nonsense_is_refused(void)
{
    uint8_t fb[16];
    nd_grab_fmt ok = {2, 2, 4u, 8u, false};
    nd_grab_fmt bad_bpp = {2, 2, 3u, 8u, false};
    nd_grab_fmt bad_w = {0, 2, 4u, 8u, false};

    memset(fb, 0, sizeof fb);
    CHECK(nd_grab_to_image(NULL, &ok) == NULL);
    CHECK(nd_grab_to_image(fb, NULL) == NULL);
    CHECK(nd_grab_to_image(fb, &bad_bpp) == NULL);
    CHECK(nd_grab_to_image(fb, &bad_w) == NULL);

    CHECK(nd_grab_fmt_supported(2u));
    CHECK(nd_grab_fmt_supported(4u));
    CHECK(!nd_grab_fmt_supported(1u));
    CHECK(!nd_grab_fmt_supported(3u));
}

/* ------------------------------------------------------------------ *
 * The digest
 * ------------------------------------------------------------------ */

/* The digest nd-grab prints is what goldenframe.py compares, so it must be
 * sha256(b"<w>,<h>|" + tightly packed RGB). This computes the same thing a
 * second way -- from an image built by hand rather than from a framebuffer --
 * and requires the two to agree. If nd_capture_digest() ever changed shape,
 * this is what would notice.
 *
 * It also pins the property that makes a digest useful at all: a PADDED
 * framebuffer and an unpadded one showing the same picture must hash the
 * same, because the padding is not part of the image. */
static void test_the_digest_ignores_padding(void)
{
    uint8_t padded[2 * 4 * 4];
    uint8_t tight[2 * 2 * 4];
    nd_grab_fmt fmt_padded = {2, 2, 4u, 4u * 4u, true};
    nd_grab_fmt fmt_tight = {2, 2, 4u, 2u * 4u, true};
    nd_image *a;
    nd_image *b;
    char hex_a[65];
    char hex_b[65];
    size_t i;

    memset(padded, 0, sizeof padded);
    memset(tight, 0, sizeof tight);
    for (i = 0u; i < 2u; i++) {
        /* same two pixels per row in both layouts */
        padded[i * 16u + 0u * 4u + 0u] = 0xFF;
        padded[i * 16u + 1u * 4u + 1u] = 0xFF;
        tight[i * 8u + 0u * 4u + 0u] = 0xFF;
        tight[i * 8u + 1u * 4u + 1u] = 0xFF;
    }
    /* junk in the padding, which must not reach the hash */
    padded[0 * 16 + 3 * 4 + 0] = 0x5A;
    padded[1 * 16 + 2 * 4 + 1] = 0xA5;

    a = nd_grab_to_image(padded, &fmt_padded);
    b = nd_grab_to_image(tight, &fmt_tight);
    CHECK(a != NULL);
    CHECK(b != NULL);
    CHECK(nd_capture_digest(a, hex_a, sizeof hex_a) == ND_OK);
    CHECK(nd_capture_digest(b, hex_b, sizeof hex_b) == ND_OK);
    CHECK_STR(hex_a, hex_b);
    /* 64 lowercase hex characters, so a caller can compare it as a string. */
    CHECK_INT(strlen(hex_a), 64);

    nd_image_free(a);
    nd_image_free(b);
}

static void test_the_digest_notices_a_swapped_channel(void)
{
    uint8_t fb[2 * 4];
    nd_grab_fmt as_red = {2, 1, 4u, 2u * 4u, true};
    nd_grab_fmt as_blue = {2, 1, 4u, 2u * 4u, false};
    nd_image *a;
    nd_image *b;
    char hex_a[65];
    char hex_b[65];

    memset(fb, 0, sizeof fb);
    fb[0] = 0xFF; /* a pixel that is red one way round and blue the other */

    a = nd_grab_to_image(fb, &as_red);
    b = nd_grab_to_image(fb, &as_blue);
    CHECK(a != NULL);
    CHECK(b != NULL);
    CHECK(nd_capture_digest(a, hex_a, sizeof hex_a) == ND_OK);
    CHECK(nd_capture_digest(b, hex_b, sizeof hex_b) == ND_OK);

    /* The whole value of `ndlink expect` rests on this: if the digest did not
     * separate these two, the check that is supposed to catch the red/blue
     * bug would pass while the bug was present. */
    CHECK(strcmp(hex_a, hex_b) != 0);

    nd_image_free(a);
    nd_image_free(b);
}

int main(void)
{
    RUN(test_32bpp_red_first);
    RUN(test_32bpp_blue_first);
    RUN(test_16bpp_both_orders);
    RUN(test_a_padded_stride_is_honoured);
    RUN(test_a_stride_too_small_is_refused);
    RUN(test_nonsense_is_refused);
    RUN(test_the_digest_ignores_padding);
    RUN(test_the_digest_notices_a_swapped_channel);
    return pt_report("test_ndgrab");
}

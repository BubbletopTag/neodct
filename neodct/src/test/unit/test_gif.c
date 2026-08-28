/* test_gif.c -- the GIF decoder, against the format rather than against a
 * reference implementation.
 *
 * ============ WHY THERE ARE NO FIXTURE FILES ============
 *
 * Every GIF here is BUILT BY THIS FILE, byte by byte, and the pixels it must
 * produce are therefore known by construction from the spec -- not copied
 * from whatever Pillow, a browser or ImageMagick happens to do. That matters
 * for exactly one case and it is worth the whole apparatus: DISPOSAL METHOD
 * 3. Pillow's own writer emits frame deltas that only compose correctly under
 * method 1 and then labels them method 3, and its reader ignores the label to
 * match, so a file written by Pillow and read by Pillow agrees with itself
 * and with nothing else. GIF89a section 23 is unambiguous -- "restore the
 * area overwritten by the graphic with what was there prior to rendering" --
 * and that is what nd_gif.c does and what test_dispose_previous() below
 * asserts, with a file whose correct output can be worked out on paper.
 *
 * ============ THE ENCODER ============
 *
 * gif_lzw_literals() emits a legal but deliberately unhelpful LZW stream: a
 * clear code, then every pixel as its own 9-bit literal, with another clear
 * before the dictionary can widen the codes. Every GIF decoder must accept
 * it, it needs no compressor, and it means a fixture's bytes and its pixels
 * are the same list written twice.
 *
 * Runs with no arguments and writes only under NEODCT_ROOT.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_gif.h"
#include "nd_image.h"
#include "nd_paths.h"

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
 * A GIF builder
 * ------------------------------------------------------------------ */

#define BUF_MAX 65536u

typedef struct {
    uint8_t b[BUF_MAX];
    size_t n;
} buf;

static void put(buf *o, uint8_t v)
{
    if (o->n < BUF_MAX)
        o->b[o->n++] = v;
    else
        fail("fixture buffer overflow");
}

static void put_n(buf *o, const void *p, size_t n)
{
    size_t i;
    const uint8_t *s = p;

    for (i = 0u; i < n; i++)
        put(o, s[i]);
}

static void put_u16(buf *o, uint32_t v)
{
    put(o, (uint8_t)(v & 0xFFu));
    put(o, (uint8_t)((v >> 8) & 0xFFu));
}

/* GIF89a header + logical screen descriptor + a global colour table of
 * `n_colours` entries (rounded up to a power of two, as the format requires). */
static void gif_header(buf *o, uint32_t w, uint32_t h, const uint8_t *gct, uint32_t n_colours)
{
    uint32_t bits = 0u;
    uint32_t size = 2u;

    put_n(o, "GIF89a", 6u);
    put_u16(o, w);
    put_u16(o, h);
    while (size < n_colours) {
        size <<= 1;
        bits++;
    }
    put(o, (uint8_t)(0x80u | (0x07u << 4) | bits)); /* GCT present, size 2^(bits+1) */
    put(o, 0u);                                     /* background index */
    put(o, 0u);                                     /* pixel aspect ratio */
    put_n(o, gct, (size_t)size * 3u);
}

static void gif_gce(buf *o, uint32_t disposal, bool transparent, uint8_t t_index, uint32_t delay_cs)
{
    put(o, 0x21u);
    put(o, 0xF9u);
    put(o, 4u);
    put(o, (uint8_t)(((disposal & 0x07u) << 2) | (transparent ? 1u : 0u)));
    put_u16(o, delay_cs);
    put(o, t_index);
    put(o, 0u);
}

/* Every pixel as its own 9-bit literal code, in sub-blocks. */
static void gif_lzw_literals(buf *o, const uint8_t *px, size_t n)
{
    uint8_t block[255];
    size_t bn = 0u;
    uint32_t acc = 0u;
    uint32_t nbits = 0u;
    size_t since_clear = 0u;
    size_t i;
    bool started = false;

#define EMIT(code)                                \
    do {                                          \
        acc |= (uint32_t)(code) << nbits;         \
        nbits += 9u;                              \
        while (nbits >= 8u) {                     \
            block[bn++] = (uint8_t)(acc & 0xFFu); \
            acc >>= 8;                            \
            nbits -= 8u;                          \
            if (bn == 255u) {                     \
                put(o, 255u);                     \
                put_n(o, block, 255u);            \
                bn = 0u;                          \
            }                                     \
        }                                         \
    } while (0)

    put(o, 8u); /* LZW minimum code size: 256 roots, clear 256, end 257 */
    for (i = 0u; i < n; i++) {
        /* A clear every 250 codes keeps next_code below 511, so the code
         * width never has to grow and the encoder stays this short. */
        if (!started || since_clear >= 250u) {
            EMIT(256u);
            since_clear = 0u;
            started = true;
        }
        EMIT((uint32_t)px[i]);
        since_clear++;
    }
    EMIT(257u);
    if (nbits > 0u)
        block[bn++] = (uint8_t)(acc & 0xFFu);
    if (bn > 0u) {
        put(o, (uint8_t)bn);
        put_n(o, block, bn);
    }
    put(o, 0u); /* sub-block terminator */
#undef EMIT
}

static void gif_frame(buf *o, uint32_t left, uint32_t top, uint32_t w, uint32_t h, bool interlaced,
                      const uint8_t *px)
{
    put(o, 0x2Cu);
    put_u16(o, left);
    put_u16(o, top);
    put_u16(o, w);
    put_u16(o, h);
    put(o, (uint8_t)(interlaced ? 0x40u : 0x00u)); /* no local table */
    gif_lzw_literals(o, px, (size_t)w * (size_t)h);
}

static void gif_trailer(buf *o)
{
    put(o, 0x3Bu);
}

/* ------------------------------------------------------------------ *
 * Helpers
 * ------------------------------------------------------------------ */

/* index 0 black, 1 red, 2 green, 3 blue, and the rest zero. */
static const uint8_t PAL[4u * 3u] = {0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255};

static void px_is(const nd_image *img, int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b,
                  uint8_t a, const char *what)
{
    nd_color c = nd_image_get_px(img, x, y);

    CHECK(c.r == r && c.g == g && c.b == b && c.a == a,
          "%s at (%d,%d): got %u,%u,%u,%u want %u,%u,%u,%u", what, x, y, c.r, c.g, c.b, c.a, r, g,
          b, a);
}

/* ------------------------------------------------------------------ *
 * 1. A single opaque frame
 * ------------------------------------------------------------------ */

static void test_single_frame(void)
{
    buf o = {{0}, 0u};
    /* 3x2:  red green blue / blue green red */
    static const uint8_t PX[6] = {1, 2, 3, 3, 2, 1};
    nd_gif *g;
    const nd_image *f;
    int32_t delay = -1;

    gif_header(&o, 3u, 2u, PAL, 4u);
    gif_frame(&o, 0u, 0u, 3u, 2u, false, PX);
    gif_trailer(&o);

    g = nd_gif_open_mem(o.b, o.n);
    CHECK(g != NULL, "a 3x2 single-frame GIF opens");
    if (g == NULL)
        return;

    CHECK(nd_gif_width(g) == 3 && nd_gif_height(g) == 2, "logical screen is 3x2, got %dx%d",
          nd_gif_width(g), nd_gif_height(g));
    CHECK(nd_gif_frame_count(g) == 1u, "one frame, got %zu", nd_gif_frame_count(g));
    CHECK(!nd_gif_animated(g), "one frame is not animated");
    CHECK(nd_gif_duration_ms(g) == 0, "a still has no duration, got %d", nd_gif_duration_ms(g));

    f = nd_gif_next(g, &delay);
    CHECK(f != NULL, "frame 0 decodes");
    if (f != NULL) {
        CHECK(f->fmt == ND_PIXFMT_RGBA8888, "the canvas is RGBA");
        px_is(f, 0, 0, 255, 0, 0, 255, "red");
        px_is(f, 1, 0, 0, 255, 0, 255, "green");
        px_is(f, 2, 0, 0, 0, 255, 255, "blue");
        px_is(f, 0, 1, 0, 0, 255, 255, "blue");
        px_is(f, 2, 1, 255, 0, 0, 255, "red");
    }
    /* No GCE at all, so the delay is the "as fast as you can" default. */
    CHECK(delay == ND_GIF_DEFAULT_DELAY_MS, "a frame with no GCE gets the default delay, got %d",
          delay);
    nd_gif_close(g);
}

/* ------------------------------------------------------------------ *
 * 2. Frame count, delays and looping
 * ------------------------------------------------------------------ */

static void test_animation_and_loop(void)
{
    buf o = {{0}, 0u};
    static const uint8_t A[1] = {1};
    static const uint8_t B[1] = {2};
    static const uint8_t C[1] = {3};
    nd_gif *g;
    const nd_image *f;
    int32_t delay = 0;

    gif_header(&o, 1u, 1u, PAL, 4u);
    gif_gce(&o, 0u, false, 0u, 5u); /* 50 ms */
    gif_frame(&o, 0u, 0u, 1u, 1u, false, A);
    gif_gce(&o, 0u, false, 0u, 5u);
    gif_frame(&o, 0u, 0u, 1u, 1u, false, B);
    gif_gce(&o, 0u, false, 0u, 5u);
    gif_frame(&o, 0u, 0u, 1u, 1u, false, C);
    gif_trailer(&o);

    g = nd_gif_open_mem(o.b, o.n);
    CHECK(g != NULL, "a three-frame GIF opens");
    if (g == NULL)
        return;

    CHECK(nd_gif_frame_count(g) == 3u, "three frames, got %zu", nd_gif_frame_count(g));
    CHECK(nd_gif_animated(g), "three frames is animated");
    CHECK(nd_gif_duration_ms(g) == 150, "3 x 50 ms = 150, got %d", nd_gif_duration_ms(g));

    f = nd_gif_next(g, &delay);
    px_is(f, 0, 0, 255, 0, 0, 255, "frame 0 is red");
    CHECK(delay == 50, "delay 50 ms, got %d", delay);
    CHECK(nd_gif_position(g) == 0u, "position 0, got %zu", nd_gif_position(g));

    f = nd_gif_next(g, &delay);
    px_is(f, 0, 0, 0, 255, 0, 255, "frame 1 is green");
    CHECK(nd_gif_position(g) == 1u, "position 1, got %zu", nd_gif_position(g));

    f = nd_gif_next(g, &delay);
    px_is(f, 0, 0, 0, 0, 255, 255, "frame 2 is blue");

    /* The frame after the last is the first again, with the canvas cleared --
     * a caller can drive this forever and never check. */
    f = nd_gif_next(g, &delay);
    px_is(f, 0, 0, 255, 0, 0, 255, "the frame after the last wraps to red");
    CHECK(nd_gif_position(g) == 0u, "position wraps to 0, got %zu", nd_gif_position(g));

    /* And rewind gets there directly. */
    f = nd_gif_next(g, &delay);
    px_is(f, 0, 0, 0, 255, 0, 255, "still advancing after the wrap");
    nd_gif_rewind(g);
    f = nd_gif_next(g, &delay);
    px_is(f, 0, 0, 255, 0, 0, 255, "rewind returns to red");

    nd_gif_close(g);
}

/* ------------------------------------------------------------------ *
 * 3. Transparency means "leave what was there"
 * ------------------------------------------------------------------ */

static void test_transparency(void)
{
    buf o = {{0}, 0u};
    /* Frame 0 fills 2x1 with red. Frame 1 writes index 0 over both pixels
     * with index 0 declared transparent, so NOTHING should change. */
    static const uint8_t F0[2] = {1, 1};
    static const uint8_t F1[2] = {0, 2};
    nd_gif *g;
    const nd_image *f;

    gif_header(&o, 2u, 1u, PAL, 4u);
    gif_gce(&o, 1u, false, 0u, 4u); /* disposal 1: leave it in place */
    gif_frame(&o, 0u, 0u, 2u, 1u, false, F0);
    gif_gce(&o, 1u, true, 0u, 4u); /* index 0 is transparent */
    gif_frame(&o, 0u, 0u, 2u, 1u, false, F1);
    gif_trailer(&o);

    g = nd_gif_open_mem(o.b, o.n);
    CHECK(g != NULL, "the transparency fixture opens");
    if (g == NULL)
        return;

    f = nd_gif_next(g, NULL);
    px_is(f, 0, 0, 255, 0, 0, 255, "frame 0 left");
    px_is(f, 1, 0, 255, 0, 0, 255, "frame 0 right");

    f = nd_gif_next(g, NULL);
    px_is(f, 0, 0, 255, 0, 0, 255, "a transparent pixel keeps the red beneath it");
    px_is(f, 1, 0, 0, 255, 0, 255, "an opaque pixel beside it still paints");

    nd_gif_close(g);
}

/* ------------------------------------------------------------------ *
 * 4. Disposal 2 -- restore to background
 * ------------------------------------------------------------------ */

static void test_dispose_background(void)
{
    buf o = {{0}, 0u};
    static const uint8_t F0[2] = {1, 1}; /* both red        */
    static const uint8_t F1[1] = {2};    /* green, left only */
    nd_gif *g;
    const nd_image *f;

    gif_header(&o, 2u, 1u, PAL, 4u);
    gif_gce(&o, 2u, false, 0u, 4u); /* dispose to background after frame 0 */
    gif_frame(&o, 0u, 0u, 2u, 1u, false, F0);
    gif_gce(&o, 1u, true, 0u, 4u);
    gif_frame(&o, 0u, 0u, 1u, 1u, false, F1);
    gif_trailer(&o);

    g = nd_gif_open_mem(o.b, o.n);
    CHECK(g != NULL, "the disposal-2 fixture opens");
    if (g == NULL)
        return;

    f = nd_gif_next(g, NULL);
    px_is(f, 1, 0, 255, 0, 0, 255, "frame 0 painted the right pixel red");

    /* Frame 0's whole rectangle goes back to transparent before frame 1 is
     * composed, and frame 1 only covers the left pixel -- so the red on the
     * right is gone rather than showing through. */
    f = nd_gif_next(g, NULL);
    px_is(f, 0, 0, 0, 255, 0, 255, "frame 1 painted the left pixel green");
    px_is(f, 1, 0, 0, 0, 0, 0, "disposal 2 cleared what frame 0 left outside frame 1");

    nd_gif_close(g);
}

/* ------------------------------------------------------------------ *
 * 5. Disposal 3 -- restore to previous
 * ------------------------------------------------------------------ */

/* THE CASE PILLOW DISAGREES WITH. Worked out from GIF89a section 23:
 *
 *   frame 0  disposal 1  paints both pixels red, and stays
 *   frame 1  disposal 3  paints the LEFT pixel green
 *            -> after it, the area it overwrote goes back to what was there
 *               before it, i.e. red
 *   frame 2  disposal 1  paints the RIGHT pixel blue
 *            -> so the left pixel must be RED again, not green
 */
static void test_dispose_previous(void)
{
    buf o = {{0}, 0u};
    static const uint8_t F0[2] = {1, 1};
    static const uint8_t F1[1] = {2};
    static const uint8_t F2[1] = {3};
    nd_gif *g;
    const nd_image *f;

    gif_header(&o, 2u, 1u, PAL, 4u);
    gif_gce(&o, 1u, false, 0u, 4u);
    gif_frame(&o, 0u, 0u, 2u, 1u, false, F0);
    gif_gce(&o, 3u, false, 0u, 4u);
    gif_frame(&o, 0u, 0u, 1u, 1u, false, F1); /* left */
    gif_gce(&o, 1u, false, 0u, 4u);
    gif_frame(&o, 1u, 0u, 1u, 1u, false, F2); /* right */
    gif_trailer(&o);

    g = nd_gif_open_mem(o.b, o.n);
    CHECK(g != NULL, "the disposal-3 fixture opens");
    if (g == NULL)
        return;

    f = nd_gif_next(g, NULL);
    px_is(f, 0, 0, 255, 0, 0, 255, "frame 0 left is red");
    px_is(f, 1, 0, 255, 0, 0, 255, "frame 0 right is red");

    f = nd_gif_next(g, NULL);
    px_is(f, 0, 0, 0, 255, 0, 255, "frame 1 paints the left pixel green");
    px_is(f, 1, 0, 255, 0, 0, 255, "and leaves the right pixel red");

    f = nd_gif_next(g, NULL);
    px_is(f, 1, 0, 0, 0, 255, 255, "frame 2 paints the right pixel blue");
    px_is(f, 0, 0, 255, 0, 0, 255, "disposal 3 restored the left pixel to red");

    nd_gif_close(g);
}

/* ------------------------------------------------------------------ *
 * 6. Interlace
 * ------------------------------------------------------------------ */

/* Eight rows, stored in the four interlace passes: rows 0 and 4 (pass 1),
 * row 2 (pass 2)... written here in STORAGE order, so a decoder that ignores
 * the interlace flag produces a visibly different picture. Storage order for
 * h=8 is display rows 0, 4 | 2, 6 | 1, 3, 5, 7. */
static void test_interlace(void)
{
    buf o = {{0}, 0u};
    static const uint8_t STORED[8] = {1, 2, 3, 1, 2, 3, 1, 2};
    /* display row -> the value stored for it */
    static const uint8_t WANT[8] = {1, 2, 3, 3, 2, 1, 1, 2};
    nd_gif *g;
    const nd_image *f;
    int32_t y;

    gif_header(&o, 1u, 8u, PAL, 4u);
    gif_frame(&o, 0u, 0u, 1u, 8u, true, STORED);
    gif_trailer(&o);

    g = nd_gif_open_mem(o.b, o.n);
    CHECK(g != NULL, "the interlaced fixture opens");
    if (g == NULL)
        return;

    f = nd_gif_next(g, NULL);
    CHECK(f != NULL, "the interlaced frame decodes");
    if (f != NULL) {
        for (y = 0; y < 8; y++) {
            nd_color c = nd_image_get_px(f, 0, y);
            uint8_t want = WANT[y];

            CHECK(c.r == PAL[want * 3u + 0u] && c.g == PAL[want * 3u + 1u] &&
                      c.b == PAL[want * 3u + 2u],
                  "interlaced row %d: got %u,%u,%u want index %u", y, c.r, c.g, c.b, want);
        }
    }
    nd_gif_close(g);
}

/* ------------------------------------------------------------------ *
 * 7. Extensions the decoder must step over
 * ------------------------------------------------------------------ */

static void test_extensions_skipped(void)
{
    buf o = {{0}, 0u};
    static const uint8_t PX[1] = {2};
    nd_gif *g;
    const nd_image *f;

    gif_header(&o, 1u, 1u, PAL, 4u);

    /* NETSCAPE2.0 loop count -- read and ignored, per nd_gif.c's header. */
    put(&o, 0x21u);
    put(&o, 0xFFu);
    put(&o, 11u);
    put_n(&o, "NETSCAPE2.0", 11u);
    put(&o, 3u);
    put(&o, 1u);
    put_u16(&o, 0u);
    put(&o, 0u);

    /* A comment. */
    put(&o, 0x21u);
    put(&o, 0xFEu);
    put(&o, 5u);
    put_n(&o, "hello", 5u);
    put(&o, 0u);

    /* A Plain Text block, which nothing has rendered since 1990. */
    put(&o, 0x21u);
    put(&o, 0x01u);
    put(&o, 12u);
    put_n(&o, "\0\0\0\0\1\0\1\0\10\10\1\0", 12u);
    put(&o, 2u);
    put_n(&o, "hi", 2u);
    put(&o, 0u);

    gif_frame(&o, 0u, 0u, 1u, 1u, false, PX);
    gif_trailer(&o);

    g = nd_gif_open_mem(o.b, o.n);
    CHECK(g != NULL, "a GIF full of extensions still opens");
    if (g == NULL)
        return;
    CHECK(nd_gif_frame_count(g) == 1u, "extensions are not counted as frames, got %zu",
          nd_gif_frame_count(g));
    f = nd_gif_next(g, NULL);
    px_is(f, 0, 0, 0, 255, 0, 255, "the frame after three extensions still decodes");
    nd_gif_close(g);
}

/* ------------------------------------------------------------------ *
 * 8. Delay clamping
 * ------------------------------------------------------------------ */

static void test_delay_clamp(void)
{
    buf o = {{0}, 0u};
    static const uint8_t PX[1] = {1};
    nd_gif *g;
    int32_t delay = 0;

    gif_header(&o, 1u, 1u, PAL, 4u);
    gif_gce(&o, 0u, false, 0u, 0u); /* "as fast as you can" */
    gif_frame(&o, 0u, 0u, 1u, 1u, false, PX);
    gif_gce(&o, 0u, false, 0u, 1u); /* 10 ms, below the floor */
    gif_frame(&o, 0u, 0u, 1u, 1u, false, PX);
    gif_trailer(&o);

    g = nd_gif_open_mem(o.b, o.n);
    CHECK(g != NULL, "the delay fixture opens");
    if (g == NULL)
        return;

    (void)nd_gif_next(g, &delay);
    CHECK(delay == ND_GIF_DEFAULT_DELAY_MS, "a 0 ms delay becomes the default, got %d", delay);
    (void)nd_gif_next(g, &delay);
    CHECK(delay == ND_GIF_MIN_DELAY_MS, "a 10 ms delay is raised to the floor, got %d", delay);
    nd_gif_close(g);
}

/* ------------------------------------------------------------------ *
 * 9. Hostile and malformed input
 * ------------------------------------------------------------------ */

static void test_rejects_rubbish(void)
{
    buf o = {{0}, 0u};
    static const uint8_t PX[4] = {1, 2, 3, 1};
    static const uint8_t NOT_A_GIF[16] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13};
    nd_gif *g;
    size_t cut;

    CHECK(nd_gif_open_mem(NULL, 100u) == NULL, "NULL data is refused");
    CHECK(nd_gif_open_mem(NOT_A_GIF, 4u) == NULL, "a four-byte file is refused");
    CHECK(nd_gif_open_mem(NOT_A_GIF, sizeof NOT_A_GIF) == NULL, "a PNG is refused");
    CHECK(nd_gif_open(NULL) == NULL, "a NULL path is refused");
    CHECK(nd_gif_open("/NeoDCT/System/wallpapers/definitely-not-here.gif") == NULL,
          "a missing file is refused");

    /* A zero-sized logical screen. */
    o.n = 0u;
    gif_header(&o, 0u, 4u, PAL, 4u);
    gif_frame(&o, 0u, 0u, 1u, 1u, false, PX);
    gif_trailer(&o);
    CHECK(nd_gif_open_mem(o.b, o.n) == NULL, "a zero-width screen is refused");

    /* Structurally fine but with no image block at all. */
    o.n = 0u;
    gif_header(&o, 2u, 2u, PAL, 4u);
    gif_trailer(&o);
    CHECK(nd_gif_open_mem(o.b, o.n) == NULL, "a GIF with no frames is refused");

    /* Truncated at every length: none may crash, none may hang, and any that
     * opens must survive being played to the end. This is the one that would
     * catch an unbounded loop in the LZW walk. */
    o.n = 0u;
    gif_header(&o, 2u, 2u, PAL, 4u);
    gif_gce(&o, 2u, true, 0u, 4u);
    gif_frame(&o, 0u, 0u, 2u, 2u, false, PX);
    gif_trailer(&o);
    for (cut = 1u; cut < o.n; cut++) {
        g = nd_gif_open_mem(o.b, cut);
        if (g != NULL) {
            int i;

            for (i = 0; i < 8; i++)
                (void)nd_gif_next(g, NULL);
            nd_gif_close(g);
        }
    }
    checks++; /* reaching here without a crash or a hang IS the assertion */

    /* Every single-byte corruption of a valid file, same rule. */
    o.n = 0u;
    gif_header(&o, 2u, 2u, PAL, 4u);
    gif_frame(&o, 0u, 0u, 2u, 2u, false, PX);
    gif_trailer(&o);
    for (cut = 0u; cut < o.n; cut++) {
        uint8_t saved = o.b[cut];
        int variant;

        for (variant = 0; variant < 2; variant++) {
            o.b[cut] = variant == 0 ? 0xFFu : 0x00u;
            g = nd_gif_open_mem(o.b, o.n);
            if (g != NULL) {
                int i;

                for (i = 0; i < 4; i++)
                    (void)nd_gif_next(g, NULL);
                nd_gif_close(g);
            }
        }
        o.b[cut] = saved;
    }
    checks++;
}

/* ------------------------------------------------------------------ *
 * 10. nd_image_open() sniffs GIF, and gets the first frame
 * ------------------------------------------------------------------ */

static void test_image_open_gif(void)
{
    buf o = {{0}, 0u};
    static const uint8_t F0[1] = {1};
    static const uint8_t F1[1] = {2};
    nd_image *img;

    gif_header(&o, 1u, 1u, PAL, 4u);
    gif_gce(&o, 1u, false, 0u, 4u);
    gif_frame(&o, 0u, 0u, 1u, 1u, false, F0);
    gif_gce(&o, 1u, false, 0u, 4u);
    gif_frame(&o, 0u, 0u, 1u, 1u, false, F1);
    gif_trailer(&o);

    /* Through the memory path nd_image_open_mem() dispatches on. */
    img = nd_image_open_mem(o.b, o.n);
    CHECK(img != NULL, "nd_image_open_mem sniffs GIF89a");
    if (img != NULL) {
        px_is(img, 0, 0, 255, 0, 0, 255, "an animated GIF opens as its FIRST frame");
        nd_image_free(img);
    }

    /* And through a file, which is what a wallpaper actually is. The path is
     * spelled the way the phone spells it and resolved the way every other
     * path in the project is -- writing an absolute host path here would be
     * prefixed with NEODCT_ROOT a second time. */
    {
        static const char *const VIRT = "/NeoDCT/User/test_gif_fixture.gif";
        char real[ND_PATH_MAX];
        FILE *f;

        CHECK(nd_path_resolve(real, sizeof real, VIRT) == ND_OK, "the fixture path resolves");
        (void)nd_mkdir_p(ND_PATH_USER, 0755u);
        f = fopen(real, "wb");
        CHECK(f != NULL, "the fixture can be written");
        if (f != NULL) {
            (void)fwrite(o.b, 1u, o.n, f);
            (void)fclose(f);

            img = nd_image_load_gif(VIRT);
            CHECK(img != NULL, "nd_image_load_gif reads it back");
            if (img != NULL) {
                px_is(img, 0, 0, 255, 0, 0, 255, "the file's first frame");
                nd_image_free(img);
            }

            /* And nd_gif_open() itself, on the same file: a wallpaper is
             * opened by path, not from memory. */
            {
                nd_gif *g = nd_gif_open(VIRT);

                CHECK(g != NULL, "nd_gif_open reads a file");
                if (g != NULL) {
                    CHECK(nd_gif_frame_count(g) == 2u, "two frames from the file, got %zu",
                          nd_gif_frame_count(g));
                    nd_gif_close(g);
                }
            }
            (void)remove(real);
        }
    }
}

int main(void)
{
    test_single_frame();
    test_animation_and_loop();
    test_transparency();
    test_dispose_background();
    test_dispose_previous();
    test_interlace();
    test_extensions_skipped();
    test_delay_clamp();
    test_rejects_rubbish();
    test_image_open_gif();

    if (failures) {
        printf("test_gif: %d check(s), %d failure(s)\n", checks, failures);
        return 1;
    }
    printf("test_gif: %d checks, 0 failures\n", checks);
    return 0;
}

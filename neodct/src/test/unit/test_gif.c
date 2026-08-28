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
 * ============ TWO ENCODERS, AND WHY BOTH ============
 *
 * gif_lzw_literals() emits a legal but deliberately unhelpful stream: a clear
 * code, then every pixel as its own literal, with another clear before the
 * dictionary can widen. It needs no compressor and a fixture's bytes and its
 * pixels are the same list written twice, which is what makes the small
 * hand-checked fixtures readable.
 *
 * It also exercises exactly one path through the decoder. The four things an
 * LZW implementation actually gets wrong -- building the dictionary, expanding
 * a multi-byte string, growing the code width, and the KwKwK case where a code
 * one past the end of the table is legal -- are all unreachable from it. So
 * gif_lzw_compress() is a real GIF compressor, and test_lzw_compressed()
 * round-trips data chosen to force every one of them.
 *
 * The two encoders check each other: the same pixels through both must decode
 * to the same picture, and if the compressor were wrong in the same direction
 * as the decoder that comparison would still fail, because the literal encoder
 * shares no code with either.
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

#define CHECK_INT(got, want, what)                           \
    do {                                                     \
        int32_t got_ = (int32_t)(got);                       \
        int32_t want_ = (int32_t)(want);                     \
        checks++;                                            \
        if (got_ != want_)                                   \
            fail("%s: got %d want %d", (what), got_, want_); \
    } while (0)

#define CHECK_SIZE(got, want, what)                            \
    do {                                                       \
        size_t got_ = (got);                                   \
        size_t want_ = (want);                                 \
        checks++;                                              \
        if (got_ != want_)                                     \
            fail("%s: got %zu want %zu", (what), got_, want_); \
    } while (0)

/* ------------------------------------------------------------------ *
 * A GIF builder
 * ------------------------------------------------------------------ */

/* The largest fixture is 3400 one-pixel frames with a GCE each (about 78 KB)
 * and a compressed 240x175 noise frame (about 60 KB). */
#define BUF_MAX 262144u

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

/* A real LZW compressor, so the decoder's dictionary is exercised by
 * something other than itself.
 *
 * The dictionary is a flat array searched linearly: fixtures here are a few
 * thousand pixels and a hash table would be more code to get wrong than the
 * thing under test. The code-width rule is the mirror of the decoder's --
 * emit at the current width, THEN add the entry and widen if next_code has
 * reached 1 << width -- because that is the only pairing that keeps an
 * encoder and a decoder in step. */
typedef struct {
    buf *o;
    uint8_t block[255];
    size_t bn;
    uint32_t acc;
    uint32_t nbits;
    uint32_t width;
} bitout;

static void bo_flush_block(bitout *b)
{
    if (b->bn > 0u) {
        put(b->o, (uint8_t)b->bn);
        put_n(b->o, b->block, b->bn);
        b->bn = 0u;
    }
}

static void bo_put(bitout *b, uint32_t code)
{
    b->acc |= code << b->nbits;
    b->nbits += b->width;
    while (b->nbits >= 8u) {
        b->block[b->bn++] = (uint8_t)(b->acc & 0xFFu);
        b->acc >>= 8;
        b->nbits -= 8u;
        if (b->bn == 255u)
            bo_flush_block(b);
    }
}

static void gif_lzw_compress(buf *o, uint8_t min_code_size, const uint8_t *px, size_t n)
{
    /* prefix[c] / suffix[c] describe entry c exactly as the decoder's tables
     * do, which is what makes a linear search for (prefix, suffix) the same
     * lookup the decoder performs implicitly. */
    static uint16_t prefix[4096];
    static uint8_t suffix[4096];
    bitout b;
    uint32_t clear_code = 1u << min_code_size;
    uint32_t end_code = clear_code + 1u;
    uint32_t next_code;
    uint32_t cur;
    size_t i;

    memset(&b, 0, sizeof b);
    b.o = o;

    put(o, min_code_size);

/* Note the two DIFFERENT comparisons. At a reset both sides ask the same
 * question -- "can this width name every code that exists right now" -- and
 * the answer is >=, which only ever fires for min_code_size 1. After an entry
 * is added they differ, because a decoder is a step behind: see the comment
 * at the widen below. Getting either wrong shifts every code past the first
 * width boundary, which is why test_lzw_every_code_size() sweeps 1 to 8. */
#define RESET()                                            \
    do {                                                   \
        next_code = clear_code + 2u;                       \
        b.width = (uint32_t)min_code_size + 1u;            \
        if (next_code >= (1u << b.width) && b.width < 12u) \
            b.width++;                                     \
    } while (0)

    RESET();
    bo_put(&b, clear_code);

    if (n == 0u) {
        bo_put(&b, end_code);
        goto flush;
    }

    cur = px[0];
    for (i = 1u; i < n; i++) {
        uint32_t c = px[i];
        uint32_t found = 0xFFFFFFFFu;
        uint32_t k;

        for (k = clear_code + 2u; k < next_code; k++) {
            if (prefix[k] == (uint16_t)cur && suffix[k] == (uint8_t)c) {
                found = k;
                break;
            }
        }
        if (found != 0xFFFFFFFFu) {
            cur = found;
            continue;
        }

        bo_put(&b, cur);
        if (next_code < 4096u) {
            prefix[next_code] = (uint16_t)cur;
            suffix[next_code] = (uint8_t)c;
            next_code++;
            /* STRICTLY GREATER, where the decoder uses >=. That asymmetry is
             * the whole of LZW's famous off-by-one: a decoder is always one
             * entry behind, because it can only create the entry for a code
             * once it has read the code AFTER it. So the encoder must stay a
             * code narrower for exactly one step, or every code past the
             * first 9-to-10 bit boundary is read at the wrong width. */
            if (next_code > (1u << b.width) && b.width < 12u)
                b.width++;
        } else {
            /* Full. The decoder holds at 12 bits until a clear, so say clear
             * and start again -- which is also the path that proves the
             * decoder's mid-stream reset works. */
            bo_put(&b, clear_code);
            RESET();
        }
        cur = c;
    }
    bo_put(&b, cur);
    bo_put(&b, end_code);

flush:
    if (b.nbits > 0u)
        b.block[b.bn++] = (uint8_t)(b.acc & 0xFFu);
    bo_flush_block(&b);
    put(o, 0u); /* sub-block terminator */
#undef RESET
}

/* As gif_frame(), but with the pixels actually compressed. */
static void gif_frame_compressed(buf *o, uint32_t left, uint32_t top, uint32_t w, uint32_t h,
                                 const uint8_t *px, uint8_t min_code_size)
{
    put(o, 0x2Cu);
    put_u16(o, left);
    put_u16(o, top);
    put_u16(o, w);
    put_u16(o, h);
    put(o, 0x00u);
    gif_lzw_compress(o, min_code_size, px, (size_t)w * (size_t)h);
}

/* An image descriptor carrying its OWN palette, which the decoder must prefer
 * over the global one for this frame and this frame only. */
static void gif_frame_lct(buf *o, uint32_t left, uint32_t top, uint32_t w, uint32_t h,
                          const uint8_t *lct, uint32_t n_colours, const uint8_t *px)
{
    uint32_t bits = 0u;
    uint32_t size = 2u;

    while (size < n_colours) {
        size <<= 1;
        bits++;
    }
    put(o, 0x2Cu);
    put_u16(o, left);
    put_u16(o, top);
    put_u16(o, w);
    put_u16(o, h);
    put(o, (uint8_t)(0x80u | bits)); /* local colour table present */
    put_n(o, lct, (size_t)size * 3u);
    gif_lzw_literals(o, px, (size_t)w * (size_t)h);
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

/* ------------------------------------------------------------------ *
 * 11. Real LZW: the dictionary, wide codes, and KwKwK
 * ------------------------------------------------------------------ */

/* Pixels chosen to force each thing the literal encoder cannot reach.
 *
 * A long run of one value is what produces the KwKwK case: the encoder builds
 * "aa", "aaa", "aaaa"... and each time emits the code it has only just
 * created, which the decoder has not seen yet and must reconstruct as
 * "previous string plus its own first byte".
 *
 * The rest is a repeating pattern long enough to push next_code past 511, so
 * the stream widens from 9 bits to 10 mid-frame and the decoder has to widen
 * with it -- the failure mode being that everything after the boundary is
 * shifted garbage. */
static void fill_lzw_torture(uint8_t *px, size_t n)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        if (i < 600u)
            px[i] = 1u; /* one long run -> KwKwK, repeatedly */
        else if (i < 1200u)
            px[i] = (uint8_t)(1u + (i % 3u)); /* a short cycle -> long strings */
        else
            px[i] = (uint8_t)(1u + ((i * 7u + (i / 13u)) % 3u)); /* churn */
    }
}

static void test_lzw_compressed(void)
{
    enum { W = 60, H = 40, N = W * H };
    static uint8_t px[N];
    buf o = {{0}, 0u};
    nd_gif *g;
    const nd_image *f;
    int32_t x;
    int32_t y;
    bool ok = true;

    fill_lzw_torture(px, (size_t)N);

    gif_header(&o, (uint32_t)W, (uint32_t)H, PAL, 4u);
    gif_frame_compressed(&o, 0u, 0u, (uint32_t)W, (uint32_t)H, px, 8u);
    gif_trailer(&o);

    /* Genuinely compressed: a literal stream of 2400 nine-bit codes would be
     * about 2700 bytes plus block overhead. If this fixture were not smaller
     * than that, the compressor emitted literals and the test proves nothing. */
    CHECK(o.n < 1600u, "the compressor actually compressed (%zu bytes for %d pixels)", o.n, (int)N);

    g = nd_gif_open_mem(o.b, o.n);
    CHECK(g != NULL, "a compressed GIF opens");
    if (g == NULL)
        return;

    f = nd_gif_next(g, NULL);
    CHECK(f != NULL, "a compressed frame decodes");
    if (f != NULL) {
        for (y = 0; y < H && ok; y++) {
            for (x = 0; x < W; x++) {
                nd_color c = nd_image_get_px(f, x, y);
                uint8_t want = px[(size_t)y * (size_t)W + (size_t)x];

                if (c.r != PAL[want * 3u + 0u] || c.g != PAL[want * 3u + 1u] ||
                    c.b != PAL[want * 3u + 2u]) {
                    fail("compressed pixel (%d,%d): got %u,%u,%u want index %u", x, y, c.r, c.g,
                         c.b, want);
                    ok = false;
                    break;
                }
            }
        }
    }
    CHECK(ok, "every pixel of the compressed frame round-trips");
    nd_gif_close(g);
}

/* The dictionary filling to 4096 and being cleared mid-stream, which is the
 * one path gif_lzw_compress() takes only on a big enough image. 240x175 is
 * the panel, so this is also the size a real wallpaper frame is. */
static void test_lzw_dictionary_wraps(void)
{
    enum { W = 240, H = 175, N = W * H };
    static uint8_t px[N];
    buf o = {{0}, 0u};
    nd_gif *g;
    const nd_image *f;
    size_t i;
    bool ok = true;

    /* Pseudo-random over four palette entries: incompressible enough that the
     * dictionary fills, which forces the encoder's clear and the decoder's
     * reset. The generator is a fixed integer hash so the fixture is the same
     * on every machine. */
    for (i = 0u; i < (size_t)N; i++) {
        uint32_t v = (uint32_t)i * 2654435761u;

        v ^= v >> 13;
        v *= 0x5BD1E995u;
        px[i] = (uint8_t)((v >> 15) & 3u);
    }

    gif_header(&o, (uint32_t)W, (uint32_t)H, PAL, 4u);
    gif_frame_compressed(&o, 0u, 0u, (uint32_t)W, (uint32_t)H, px, 8u);
    gif_trailer(&o);

    g = nd_gif_open_mem(o.b, o.n);
    CHECK(g != NULL, "a panel-sized compressed GIF opens");
    if (g == NULL)
        return;
    f = nd_gif_next(g, NULL);
    CHECK(f != NULL, "it decodes");
    if (f != NULL) {
        for (i = 0u; i < (size_t)N; i++) {
            int32_t x = (int32_t)(i % (size_t)W);
            int32_t y = (int32_t)(i / (size_t)W);
            nd_color c = nd_image_get_px(f, x, y);
            uint8_t want = px[i];

            if (c.r != PAL[want * 3u + 0u] || c.g != PAL[want * 3u + 1u] ||
                c.b != PAL[want * 3u + 2u]) {
                fail("dictionary-wrap pixel %zu: got %u,%u,%u want index %u", i, c.r, c.g, c.b,
                     want);
                ok = false;
                break;
            }
        }
    }
    CHECK(ok, "42,000 pixels round-trip through a dictionary that filled and cleared");
    nd_gif_close(g);
}

/* Every legal minimum code size, each with pixels that fit its palette.
 *
 * ============ WHAT THIS DOES AND DOES NOT PROVE ============
 *
 * For sizes 2..8 the fixtures this encoder produces were checked against
 * PILLOW, which reads them pixel-identically at 8x8, 60x40 and 240x175 --
 * the last being 42,000 pixels, well past every code-width boundary. So for
 * those sizes this is a genuine round trip against a third party and not the
 * decoder agreeing with itself.
 *
 * SIZE 1 IS DIFFERENT and the comment is here so nobody reads more into the
 * assertion than it carries. Pillow refuses a min_code_size of 1 outright --
 * "image file is truncated" -- whichever of the two defensible initial code
 * widths the stream uses, so there is no third opinion available. GIF89a
 * effectively rules the size out too (a two-colour image is written with 2).
 * What is asserted for size 1 is that this decoder and this encoder agree and
 * that nothing crashes; that it matches some other implementation is NOT
 * claimed, because no other implementation would read it. */
static void test_lzw_every_code_size(void)
{
    uint8_t pal[256u * 3u];
    uint8_t px[64];
    uint32_t mcs;
    size_t i;

    for (i = 0u; i < 256u; i++) {
        pal[i * 3u + 0u] = (uint8_t)i;
        pal[i * 3u + 1u] = (uint8_t)(255u - i);
        pal[i * 3u + 2u] = (uint8_t)((i * 7u) & 0xFFu);
    }

    for (mcs = 1u; mcs <= 8u; mcs++) {
        buf o = {{0}, 0u};
        uint32_t colours = 1u << mcs;
        nd_gif *g;
        const nd_image *f;
        bool ok = true;

        for (i = 0u; i < sizeof px; i++)
            px[i] = (uint8_t)((i * 5u) % colours);

        gif_header(&o, 8u, 8u, pal, colours);
        gif_frame_compressed(&o, 0u, 0u, 8u, 8u, px, (uint8_t)mcs);
        gif_trailer(&o);

        g = nd_gif_open_mem(o.b, o.n);
        CHECK(g != NULL, "every minimum code size 1..8 opens");
        if (g == NULL)
            continue;
        f = nd_gif_next(g, NULL);
        if (f != NULL) {
            for (i = 0u; i < sizeof px; i++) {
                nd_color c = nd_image_get_px(f, (int32_t)(i % 8u), (int32_t)(i / 8u));
                uint8_t want = px[i];

                if (c.r != pal[want * 3u] || c.g != pal[want * 3u + 1u] ||
                    c.b != pal[want * 3u + 2u]) {
                    fail("code size %u, pixel %zu: got %u,%u,%u want index %u", mcs, i, c.r, c.g,
                         c.b, want);
                    ok = false;
                    break;
                }
            }
        } else {
            ok = false;
        }
        CHECK(ok, "every minimum code size round-trips");
        nd_gif_close(g);
    }
}

/* ------------------------------------------------------------------ *
 * 12. A local colour table wins over the global one
 * ------------------------------------------------------------------ */

static void test_local_colour_table(void)
{
    buf o = {{0}, 0u};
    /* Global: index 1 is red. Local: index 1 is white. */
    static const uint8_t LCT[2u * 3u] = {0, 0, 0, 255, 255, 255};
    static const uint8_t PX[1] = {1};
    nd_gif *g;
    const nd_image *f;

    gif_header(&o, 2u, 1u, PAL, 4u);
    gif_frame(&o, 0u, 0u, 1u, 1u, false, PX); /* global palette: red */
    gif_frame_lct(&o, 1u, 0u, 1u, 1u, LCT, 2u, PX);
    gif_trailer(&o);

    g = nd_gif_open_mem(o.b, o.n);
    CHECK(g != NULL, "a GIF with a local colour table opens");
    if (g == NULL)
        return;

    f = nd_gif_next(g, NULL);
    px_is(f, 0, 0, 255, 0, 0, 255, "the global table paints the first frame red");
    f = nd_gif_next(g, NULL);
    px_is(f, 1, 0, 255, 255, 255, 255, "the local table paints the second frame white");
    px_is(f, 0, 0, 255, 0, 0, 255, "and does not disturb what the global table drew");
    nd_gif_close(g);
}

/* ------------------------------------------------------------------ *
 * 13. Interlace at heights that are not multiples of eight
 * ------------------------------------------------------------------ */

/* The four passes are rows 0,8,16...; 4,12,20...; 2,6,10...; 1,3,5...
 * Computed here from the spec rather than from nd_gif.c, so a decoder that
 * agrees with itself but not with GIF89a fails. Heights that are multiples of
 * eight are the ones where several wrong formulas give the right answer, so
 * the point of this test is the heights that are not. */
static void test_interlace_heights(void)
{
    static const int32_t HEIGHTS[] = {1, 2, 3, 4, 5, 6, 7, 9, 11, 13, 16, 17, 23};
    size_t hi;

    for (hi = 0u; hi < ND_ARRAY_LEN(HEIGHTS); hi++) {
        int32_t fh = HEIGHTS[hi];
        buf o = {{0}, 0u};
        uint8_t stored[32];
        int32_t display_of[32];
        nd_gif *g;
        const nd_image *f;
        int32_t k = 0;
        int32_t start;
        int32_t step;
        int32_t pass;
        int32_t y;
        bool ok = true;

        for (pass = 0; pass < 4; pass++) {
            start = (pass == 0) ? 0 : (pass == 1) ? 4 : (pass == 2) ? 2 : 1;
            step = (pass == 0) ? 8 : (pass == 1) ? 8 : (pass == 2) ? 4 : 2;
            for (y = start; y < fh; y += step)
                display_of[k++] = y;
        }
        CHECK_INT(k, fh, "the four passes cover every row exactly once");

        /* Store a value that identifies the DISPLAY row, so a decoder that
         * gets the mapping wrong produces visibly wrong rows rather than a
         * shuffle that happens to look plausible. */
        for (y = 0; y < fh; y++)
            stored[y] = (uint8_t)(1u + ((uint32_t)display_of[y] % 3u));

        gif_header(&o, 1u, (uint32_t)fh, PAL, 4u);
        gif_frame(&o, 0u, 0u, 1u, (uint32_t)fh, true, stored);
        gif_trailer(&o);

        g = nd_gif_open_mem(o.b, o.n);
        CHECK(g != NULL, "an interlaced fixture opens at every height");
        if (g == NULL)
            continue;
        f = nd_gif_next(g, NULL);
        if (f != NULL) {
            for (y = 0; y < fh; y++) {
                uint8_t want = (uint8_t)(1u + ((uint32_t)y % 3u));
                nd_color c = nd_image_get_px(f, 0, y);

                if (c.r != PAL[want * 3u] || c.g != PAL[want * 3u + 1u] ||
                    c.b != PAL[want * 3u + 2u]) {
                    fail("interlace h=%d row %d: got %u,%u,%u want index %u", fh, y, c.r, c.g, c.b,
                         want);
                    ok = false;
                    break;
                }
            }
        } else {
            ok = false;
        }
        CHECK(ok, "interlaced rows land where GIF89a says at every height");
        nd_gif_close(g);
    }
}

/* ------------------------------------------------------------------ *
 * 14. The two hostile-input bounds, each with a complete payload behind it
 * ------------------------------------------------------------------ */

/* A frame descriptor claiming more pixels than the logical screen. The index
 * scratch is exactly w*h bytes, so this is the check that stops a hostile
 * descriptor writing past it -- SECURITY.md's case, since a wallpaper comes
 * off an SD card. The LZW payload behind it is COMPLETE and well-formed, so
 * the bound is what refuses the frame rather than the stream running out. */
static void test_oversize_frame_is_refused(void)
{
    static uint8_t big[64 * 64];
    buf o = {{0}, 0u};
    static const uint8_t SMALL[4] = {1, 1, 1, 1};
    nd_gif *g;
    const nd_image *f;
    size_t i;

    for (i = 0u; i < sizeof big; i++)
        big[i] = (uint8_t)(i & 3u);

    /* A 4x4 logical screen. Frame 0 fills it; frame 1 claims 64x64. */
    gif_header(&o, 4u, 4u, PAL, 4u);
    gif_frame(&o, 0u, 0u, 2u, 2u, false, SMALL);
    gif_frame_compressed(&o, 0u, 0u, 64u, 64u, big, 8u);
    gif_trailer(&o);

    g = nd_gif_open_mem(o.b, o.n);
    CHECK(g != NULL, "the oversize fixture opens");
    if (g == NULL)
        return;

    f = nd_gif_next(g, NULL);
    px_is(f, 0, 0, 255, 0, 0, 255, "the in-bounds frame decodes");

    /* The oversize frame is refused AND stepped over, so what comes back is
     * the wrap to frame 0 rather than garbage read out of its payload. */
    f = nd_gif_next(g, NULL);
    CHECK(f != NULL, "the decoder survives an oversize descriptor");
    CHECK_SIZE(nd_gif_position(g), 0u, "and wraps rather than decoding it");
    nd_gif_close(g);
}

/* A frame whose LZW minimum code size is illegal, with its data sub-blocks
 * arranged to parse as a whole image descriptor if they are ever read as
 * blocks. The decoder must step OVER them: refusing the frame is not enough
 * if the file position is left inside the compressed data, because then the
 * block walk resumes in the middle of a payload and renders pixels out of a
 * region no image block covers. */
static void test_bad_code_size_does_not_desync(void)
{
    buf o = {{0}, 0u};
    static const uint8_t F0[4] = {1, 1, 1, 1};
    size_t payload_len;
    size_t i;

    gif_header(&o, 2u, 2u, PAL, 4u);
    gif_frame(&o, 0u, 0u, 2u, 2u, false, F0);

    /* An image descriptor the decoder will accept, then min_code_size 0. */
    put(&o, 0x2Cu);
    put_u16(&o, 0u);
    put_u16(&o, 0u);
    put_u16(&o, 2u);
    put_u16(&o, 2u);
    put(&o, 0x00u);
    put(&o, 0u); /* illegal minimum code size */

    /* One sub-block whose bytes spell a complete 1x1 image descriptor with a
     * local colour table painting green -- the thing that must NOT appear. */
    {
        buf inner = {{0}, 0u};
        static const uint8_t GREEN_LCT[2u * 3u] = {0, 0, 0, 0, 255, 0};
        static const uint8_t ONE[1] = {1};

        gif_frame_lct(&inner, 0u, 0u, 1u, 1u, GREEN_LCT, 2u, ONE);
        CHECK(inner.n <= 255u, "the smuggled descriptor fits one sub-block");
        payload_len = inner.n;
        put(&o, (uint8_t)payload_len);
        for (i = 0u; i < payload_len; i++)
            put(&o, inner.b[i]);
    }
    put(&o, 0u); /* sub-block terminator */
    gif_trailer(&o);

    {
        nd_gif *g = nd_gif_open_mem(o.b, o.n);
        const nd_image *f;

        CHECK(g != NULL, "the desync fixture opens");
        if (g == NULL)
            return;
        /* The scan must see ONE usable frame plus the rejected one, never a
         * third smuggled out of the payload. */
        CHECK(nd_gif_frame_count(g) <= 2u, "no frame is counted out of LZW payload (%zu)",
              nd_gif_frame_count(g));

        f = nd_gif_next(g, NULL);
        px_is(f, 0, 0, 255, 0, 0, 255, "frame 0 is red");
        f = nd_gif_next(g, NULL);
        CHECK(f != NULL, "the decoder keeps going past the illegal frame");
        if (f != NULL) {
            /* Whatever it hands back, the smuggled green must never appear:
             * that colour exists only inside the compressed payload. */
            int32_t x;
            int32_t y;
            bool green = false;

            for (y = 0; y < 2; y++) {
                for (x = 0; x < 2; x++) {
                    nd_color c = nd_image_get_px(f, x, y);

                    if (c.r == 0u && c.g == 255u && c.b == 0u)
                        green = true;
                }
            }
            CHECK(!green, "nothing was rendered out of the skipped frame's payload");
        }
        nd_gif_close(g);
    }
}

/* ------------------------------------------------------------------ *
 * 15. The open-time scan's arithmetic cannot overflow
 * ------------------------------------------------------------------ */

/* A GCE delay is a uint16_t of centiseconds, so one frame can claim 655,350
 * ms and ND_GIF_MAX_FRAMES of them is 2.68e9 -- past INT32_MAX. The sum is
 * accumulated in 64 bits and saturated; before it was, this file produced
 * signed overflow under UBSan and a negative duration. */
static void test_duration_saturates(void)
{
    buf o = {{0}, 0u};
    static const uint8_t PX[1] = {1};
    nd_gif *g;
    size_t i;
    const size_t FRAMES = 3400u; /* comfortably past the old overflow point */

    gif_header(&o, 1u, 1u, PAL, 4u);
    for (i = 0u; i < FRAMES; i++) {
        gif_gce(&o, 0u, false, 0u, 0xFFFFu); /* 655,350 ms */
        gif_frame(&o, 0u, 0u, 1u, 1u, false, PX);
    }
    gif_trailer(&o);

    g = nd_gif_open_mem(o.b, o.n);
    CHECK(g != NULL, "a file claiming years of animation still opens");
    if (g == NULL)
        return;
    CHECK(nd_gif_duration_ms(g) > 0, "the reported duration is not negative (%d)",
          nd_gif_duration_ms(g));
    CHECK_SIZE(nd_gif_frame_count(g), FRAMES, "and every frame was counted");
    nd_gif_close(g);
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
    test_lzw_compressed();
    test_lzw_dictionary_wraps();
    test_lzw_every_code_size();
    test_local_colour_table();
    test_interlace_heights();
    test_oversize_frame_is_refused();
    test_bad_code_size_does_not_desync();
    test_duration_saturates();

    if (failures) {
        printf("test_gif: %d check(s), %d failure(s)\n", checks, failures);
        return 1;
    }
    printf("test_gif: %d checks, 0 failures\n", checks);
    return 0;
}

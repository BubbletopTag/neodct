/* nd_gif.c -- a self-contained GIF87a/GIF89a decoder.
 *
 * Written rather than linked because giflib is not in the Buildroot config
 * and adding a package to decode one wallpaper format costs more than the
 * six hundred lines below -- LZW is a 1984 algorithm with a fixed 4096-entry
 * dictionary and no options.
 *
 * ============ THE FRAME IS COMPOSED, NOT RETURNED RAW ============
 *
 * A GIF frame is a delta: a sub-rectangle of new pixels, some of which are
 * "transparent" meaning "leave what was there". So a decoder that handed back
 * each sub-image would hand back something nobody can display. This one keeps
 * the composed canvas and returns that, which is what every viewer shows and
 * what Pillow's seek()/convert("RGBA") produces.
 *
 * Disposal is applied at the START of the next frame, not the end of this
 * one, because method 3 ("restore to previous") is defined against the canvas
 * as it was before the frame that asked for it was drawn. Doing it eagerly at
 * the end of a frame would restore over pixels the viewer never saw.
 *
 * ============ WHAT IS DELIBERATELY NOT IMPLEMENTED ============
 *
 * Plain Text extensions (0x01) are skipped. They are rendered by essentially
 * no viewer written since 1990, the spec's own font is unspecified, and
 * honouring them would mean a second text renderer that agrees with nobody.
 *
 * The NETSCAPE2.0 loop count is read but ignored: a wallpaper loops forever
 * regardless of what the file asks for, and "play three times then stop" is
 * not a thing a background can usefully do.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_gif.h"
#include "nd_image.h"
#include "nd_image_priv.h"
#include "nd_log.h"
#include "nd_paths.h"

/* GIF block introducers, spec section 15 onwards. */
#define BLK_EXTENSION 0x21u
#define BLK_IMAGE     0x2Cu
#define BLK_TRAILER   0x3Bu

#define EXT_PLAINTEXT   0x01u
#define EXT_GRAPHICCTRL 0xF9u
#define EXT_COMMENT     0xFEu
#define EXT_APPLICATION 0xFFu

/* The LZW dictionary is 12 bits wide and that is not configurable: it is in
 * the file format, not in this implementation. */
#define LZW_MAX_CODES 4096u
#define LZW_MAX_BITS  12u

/* Disposal methods, image descriptor packed bits 4..2. */
#define DISPOSE_NONE       0u
#define DISPOSE_KEEP       1u
#define DISPOSE_BACKGROUND 2u
#define DISPOSE_PREVIOUS   3u

struct nd_gif {
    /* The source. Exactly one of `f` and `buf` is set; the reader below hides
     * which, so the block walk is written once. */
    FILE *f;
    uint8_t *buf; /* owned; only for nd_gif_open_mem() */
    size_t buf_len;
    size_t buf_pos;

    int32_t w; /* logical screen */
    int32_t h;

    uint8_t gct[256u * 3u];
    int32_t gct_n; /* entries, 0 when the file has no global table */
    uint8_t bg_index;

    /* Filled by the open-time structural scan, so a caller knows the shape of
     * the animation before a single pixel has been decoded. */
    size_t n_frames;
    int32_t duration_ms;

    long first_block; /* where the block walk starts, for rewind */

    size_t pos; /* index of the frame nd_gif_next() last returned */
    bool started;

    nd_image *canvas; /* RGBA8888, w*h*4 bytes -- the composed frame     */
    nd_image *backup; /* the same again, allocated only if a frame uses  */
                      /* DISPOSE_PREVIOUS. Most files never do.          */
    uint8_t *indices; /* w*h bytes: one sub-image's palette indices      */

    /* The disposal owed by the frame just returned, applied before the next
     * one is composed. See the header comment. */
    uint8_t pending_disposal;
    nd_rect pending_rect;

    /* The LZW dictionary. 4096 * (2 + 1 + 1) = 16,384 bytes, allocated once
     * per decoder rather than once per frame: at 25 fps that is 25 malloc
     * pairs a second saved, on a device where that is not free. */
    uint16_t *lzw_prefix;
    uint8_t *lzw_suffix;
    uint8_t *lzw_stack;

    /* Set once, so a file full of the same complaint says it once. */
    bool warned_oversize;
};

/* ------------------------------------------------------------------ *
 * The reader -- FILE or memory, one interface
 * ------------------------------------------------------------------ */

static bool rd(nd_gif *g, void *out, size_t n)
{
    if (g->f != NULL)
        return fread(out, 1u, n, g->f) == n;
    if (g->buf_pos + n > g->buf_len)
        return false;
    memcpy(out, g->buf + g->buf_pos, n);
    g->buf_pos += n;
    return true;
}

static bool rd_u8(nd_gif *g, uint8_t *out)
{
    return rd(g, out, 1u);
}

static bool rd_u16(nd_gif *g, uint16_t *out)
{
    uint8_t b[2];

    if (!rd(g, b, 2u))
        return false;
    /* Little-endian on the wire, and this runs on a little-endian ARM -- but
     * assembling it by hand is what makes that irrelevant. */
    *out = (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    return true;
}

static bool rd_skip(nd_gif *g, size_t n)
{
    if (g->f != NULL)
        return fseek(g->f, (long)n, SEEK_CUR) == 0;
    if (g->buf_pos + n > g->buf_len)
        return false;
    g->buf_pos += n;
    return true;
}

static long rd_tell(const nd_gif *g)
{
    if (g->f != NULL)
        return ftell(g->f);
    return (long)g->buf_pos;
}

static bool rd_seek(nd_gif *g, long off)
{
    if (off < 0)
        return false;
    if (g->f != NULL)
        return fseek(g->f, off, SEEK_SET) == 0;
    if ((size_t)off > g->buf_len)
        return false;
    g->buf_pos = (size_t)off;
    return true;
}

/* Walk a chain of sub-blocks without interpreting them: [len][len bytes]...
 * terminated by a zero length. Every extension this decoder ignores, and
 * every frame it refuses, is disposed of with this. */
static bool skip_subblocks(nd_gif *g)
{
    for (;;) {
        uint8_t len;

        if (!rd_u8(g, &len))
            return false;
        if (len == 0u)
            return true;
        if (!rd_skip(g, len))
            return false;
    }
}

/* ------------------------------------------------------------------ *
 * The sub-block byte stream that LZW reads through
 * ------------------------------------------------------------------ */

typedef struct {
    nd_gif *g;
    uint8_t block[255];
    uint8_t have;
    uint8_t used;
    bool ended;
} subblock_stream;

static void sb_init(subblock_stream *s, nd_gif *g)
{
    memset(s, 0, sizeof *s);
    s->g = g;
}

static bool sb_byte(subblock_stream *s, uint8_t *out)
{
    while (s->used >= s->have) {
        uint8_t len;

        if (s->ended)
            return false;
        if (!rd_u8(s->g, &len))
            return false;
        if (len == 0u) {
            s->ended = true;
            return false;
        }
        if (!rd(s->g, s->block, len))
            return false;
        s->have = len;
        s->used = 0u;
    }
    *out = s->block[s->used++];
    return true;
}

/* Consume whatever is left of the stream, so the file position ends up at the
 * block after this frame however early the decoder stopped. */
static void sb_drain(subblock_stream *s)
{
    if (!s->ended)
        (void)skip_subblocks(s->g);
}

/* ------------------------------------------------------------------ *
 * LZW
 * ------------------------------------------------------------------ */

/* Decode one sub-image's worth of palette indices into g->indices.
 *
 * `count` is fw * fh and is guaranteed by the caller to fit g->indices. A
 * truncated or corrupt stream fills as far as it got and returns the number
 * of indices written -- the same forgiveness nd_image.h grants a truncated
 * JPEG, and for the same reason: a wallpaper that decodes 90% is better on
 * screen than a black rectangle. */
static size_t lzw_decode(nd_gif *g, uint8_t min_code_size, size_t count)
{
    subblock_stream sb;
    uint16_t *prefix = g->lzw_prefix;
    uint8_t *suffix = g->lzw_suffix;
    uint8_t *stack = g->lzw_stack;
    size_t sp = 0u;
    size_t written = 0u;
    uint32_t clear_code;
    uint32_t end_code;
    uint32_t next_code;
    uint32_t code_size;
    uint32_t prev = 0xFFFFFFFFu;
    uint8_t first = 0u;
    uint32_t bit_buf = 0u;
    uint32_t bit_cnt = 0u;

    /* min_code_size 1..8. Anything else is not a GIF this decoder will
     * pretend to understand -- 0 would make clear_code 1 and the dictionary
     * degenerate, and >8 cannot index a 256-entry palette. */
    if (min_code_size < 1u || min_code_size > 8u)
        return 0u;

    sb_init(&sb, g);
    clear_code = 1u << min_code_size;
    end_code = clear_code + 1u;
    next_code = clear_code + 2u;
    code_size = (uint32_t)min_code_size + 1u;
    /* min_code_size 1 is the awkward one: clear=2, end=3, first free code=4,
     * and 4 does not fit the 2 bits that (min + 1) gives. Every other size
     * starts with room to spare, so this widening fires exactly once and only
     * for two-colour files. */
    if (next_code >= (1u << code_size) && code_size < LZW_MAX_BITS)
        code_size++;

    /* Roots are their own single-byte strings; the two control codes have no
     * expansion and are never followed. */
    {
        uint32_t i;

        for (i = 0u; i < clear_code; i++) {
            prefix[i] = 0xFFFFu;
            suffix[i] = (uint8_t)i;
        }
    }

    while (written < count) {
        uint32_t code;

        while (bit_cnt < code_size) {
            uint8_t byte;

            if (!sb_byte(&sb, &byte))
                goto done;
            bit_buf |= (uint32_t)byte << bit_cnt;
            bit_cnt += 8u;
        }
        code = bit_buf & ((1u << code_size) - 1u);
        bit_buf >>= code_size;
        bit_cnt -= code_size;

        if (code == clear_code) {
            next_code = clear_code + 2u;
            code_size = (uint32_t)min_code_size + 1u;
            if (next_code >= (1u << code_size) && code_size < LZW_MAX_BITS)
                code_size++;
            prev = 0xFFFFFFFFu;
            continue;
        }
        if (code == end_code)
            goto done;

        /* The KwKwK case: a code one past the end of the dictionary is legal
         * exactly once, and means "the previous string plus its own first
         * byte". Anything further out is corruption. */
        if (code > next_code || (code == next_code && prev == 0xFFFFFFFFu))
            goto done;

        {
            uint32_t walk = code;

            if (code == next_code) {
                stack[sp++] = first;
                walk = prev;
            }
            /* sp cannot pass LZW_MAX_CODES: a dictionary entry's expansion is
             * at most as long as the dictionary is deep, and every prefix
             * link points strictly downwards because entries are only ever
             * appended. The bound is asserted anyway -- this is the one loop
             * in the file a corrupt table could otherwise run forever. */
            while (walk >= clear_code && sp < LZW_MAX_CODES) {
                stack[sp++] = suffix[walk];
                walk = prefix[walk];
                if (walk == 0xFFFFu)
                    break;
            }
            if (sp >= LZW_MAX_CODES)
                goto done;
            first = suffix[walk < LZW_MAX_CODES ? walk : 0u];
            stack[sp++] = first;
        }

        while (sp > 0u && written < count)
            g->indices[written++] = stack[--sp];
        sp = 0u;

        if (prev != 0xFFFFFFFFu && next_code < LZW_MAX_CODES) {
            prefix[next_code] = (uint16_t)prev;
            suffix[next_code] = first;
            next_code++;
            /* One more entry than the current width can name means the next
             * code on the wire is one bit wider. The dictionary stops growing
             * at 4096 and the width stays at 12 until a clear code. */
            if (next_code >= (1u << code_size) && code_size < LZW_MAX_BITS)
                code_size++;
        }
        prev = code;
    }

done:
    sb_drain(&sb);
    return written;
}

/* ------------------------------------------------------------------ *
 * Composition
 * ------------------------------------------------------------------ */

static void canvas_clear(nd_gif *g, nd_rect r)
{
    int32_t y;

    if (r.x0 < 0)
        r.x0 = 0;
    if (r.y0 < 0)
        r.y0 = 0;
    if (r.x1 >= g->w)
        r.x1 = g->w - 1;
    if (r.y1 >= g->h)
        r.y1 = g->h - 1;
    if (r.x1 < r.x0 || r.y1 < r.y0)
        return;

    for (y = r.y0; y <= r.y1; y++) {
        uint8_t *p = nd_img_px(g->canvas, r.x0, y);

        /* Transparent black, not the background colour: an undrawn GIF pixel
         * is "nothing", and a caller flattening to RGB888 then gets the
         * panel's own background rather than a colour the file made up. */
        memset(p, 0, (size_t)nd_rect_w(r) * 4u);
    }
}

/* The four passes of a GIF interlaced image, spec appendix E. */
static int32_t interlace_row(int32_t i, int32_t fh)
{
    int32_t n;

    n = (fh + 7) / 8;
    if (i < n)
        return i * 8;
    i -= n;
    n = (fh + 7 - 4) / 8;
    if (i < n)
        return i * 8 + 4;
    i -= n;
    n = (fh + 3 - 2) / 4;
    if (i < n)
        return i * 4 + 2;
    i -= n;
    return i * 2 + 1;
}

/* ------------------------------------------------------------------ *
 * The structural scan
 * ------------------------------------------------------------------ */

/* Walk the whole file once at open, counting frames and summing delays
 * without running LZW. Two things come out of it that a streaming decoder
 * could not otherwise offer: an exact frame count, and the knowledge that the
 * block structure holds together all the way to the trailer.
 *
 * Leaves the read position wherever it ended; the caller seeks back. */
static bool scan_structure(nd_gif *g)
{
    uint8_t delay_pending_valid = 0u;
    int32_t delay_ms = 0;
    int32_t total = 0;
    size_t frames = 0u;

    for (;;) {
        uint8_t intro;

        if (!rd_u8(g, &intro))
            break; /* truncated: keep the frames we counted */
        if (intro == BLK_TRAILER)
            break;

        if (intro == BLK_EXTENSION) {
            uint8_t label;

            if (!rd_u8(g, &label))
                break;
            if (label == EXT_GRAPHICCTRL) {
                uint8_t size;
                uint8_t packed;
                uint16_t cs;
                uint8_t tidx;
                uint8_t term;

                if (!rd_u8(g, &size) || size != 4u)
                    break;
                if (!rd_u8(g, &packed) || !rd_u16(g, &cs) || !rd_u8(g, &tidx) || !rd_u8(g, &term))
                    break;
                /* Centiseconds on the wire. */
                delay_ms = (int32_t)cs * 10;
                delay_pending_valid = 1u;
            } else if (!skip_subblocks(g)) {
                break;
            }
            continue;
        }

        if (intro != BLK_IMAGE)
            break; /* a byte that is not a block introducer: stop here */

        {
            uint16_t l, t, fw, fh;
            uint8_t packed;
            uint8_t mcs;

            if (!rd_u16(g, &l) || !rd_u16(g, &t) || !rd_u16(g, &fw) || !rd_u16(g, &fh) ||
                !rd_u8(g, &packed))
                break;
            if ((packed & 0x80u) != 0u) {
                uint32_t n = 2u << (packed & 0x07u);

                if (!rd_skip(g, (size_t)n * 3u))
                    break;
            }
            if (!rd_u8(g, &mcs))
                break;
            if (!skip_subblocks(g))
                break;
        }

        frames++;
        {
            int32_t d = delay_pending_valid != 0u ? delay_ms : 0;

            if (d <= 0)
                d = ND_GIF_DEFAULT_DELAY_MS;
            else if (d < ND_GIF_MIN_DELAY_MS)
                d = ND_GIF_MIN_DELAY_MS;
            total += d;
        }
        delay_pending_valid = 0u;
        delay_ms = 0;

        if (frames >= ND_GIF_MAX_FRAMES)
            break;
    }

    g->n_frames = frames;
    g->duration_ms = frames > 1u ? total : 0;
    return frames > 0u;
}

/* ------------------------------------------------------------------ *
 * Open / close
 * ------------------------------------------------------------------ */

static bool read_header(nd_gif *g)
{
    uint8_t sig[6];
    uint16_t sw, sh;
    uint8_t packed, bg, aspect;

    if (!rd(g, sig, 6u))
        return false;
    if (memcmp(sig, "GIF87a", 6) != 0 && memcmp(sig, "GIF89a", 6) != 0)
        return false;
    if (!rd_u16(g, &sw) || !rd_u16(g, &sh))
        return false;
    if (!rd_u8(g, &packed) || !rd_u8(g, &bg) || !rd_u8(g, &aspect))
        return false;
    ND_UNUSED(aspect);

    if (sw == 0u || sh == 0u || sw > ND_GIF_MAX_DIM || sh > ND_GIF_MAX_DIM)
        return false;

    g->w = (int32_t)sw;
    g->h = (int32_t)sh;
    g->bg_index = bg;

    if ((packed & 0x80u) != 0u) {
        uint32_t n = 2u << (packed & 0x07u);

        if (!rd(g, g->gct, (size_t)n * 3u))
            return false;
        g->gct_n = (int32_t)n;
    }
    return true;
}

static nd_gif *finish_open(nd_gif *g)
{
    if (!read_header(g))
        goto fail;

    g->first_block = rd_tell(g);
    if (g->first_block < 0)
        goto fail;
    if (!scan_structure(g))
        goto fail;
    if (!rd_seek(g, g->first_block))
        goto fail;

    /* 240x175 RGBA8888 = 168,000 bytes for the canvas and 42,000 for the
     * index scratch. The DISPOSE_PREVIOUS backup is a second 168,000 and is
     * not allocated unless a frame actually asks for it. */
    g->canvas = nd_image_new_filled(g->w, g->h, ND_PIXFMT_RGBA8888, ND_RGBA(0, 0, 0, 0));
    if (g->canvas == NULL)
        goto fail;
    g->indices = calloc((size_t)g->w * (size_t)g->h, 1u);
    if (g->indices == NULL)
        goto fail;

    /* 4096 * 2 + 4096 + 4096 = 16,384 bytes, once per decoder. */
    g->lzw_prefix = malloc(LZW_MAX_CODES * sizeof *g->lzw_prefix);
    g->lzw_suffix = malloc(LZW_MAX_CODES);
    g->lzw_stack = malloc(LZW_MAX_CODES);
    if (g->lzw_prefix == NULL || g->lzw_suffix == NULL || g->lzw_stack == NULL)
        goto fail;

    return g;

fail:
    nd_gif_close(g);
    return NULL;
}

nd_gif *nd_gif_open(const char *path)
{
    char resolved[ND_PATH_MAX];
    nd_gif *g;

    if (path == NULL)
        return NULL;
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return NULL;

    g = calloc(1u, sizeof *g);
    if (g == NULL)
        return NULL;

    g->f = fopen(resolved, "rb");
    if (g->f == NULL) {
        free(g);
        return NULL;
    }
    return finish_open(g);
}

nd_gif *nd_gif_open_mem(const uint8_t *data, size_t len)
{
    nd_gif *g;

    if (data == NULL || len < 13u)
        return NULL;

    g = calloc(1u, sizeof *g);
    if (g == NULL)
        return NULL;

    /* Copied, not borrowed: the decoder outlives the call and a caller's
     * buffer is not promised to. */
    g->buf = malloc(len);
    if (g->buf == NULL) {
        free(g);
        return NULL;
    }
    memcpy(g->buf, data, len);
    g->buf_len = len;
    return finish_open(g);
}

void nd_gif_close(nd_gif *g)
{
    if (g == NULL)
        return;
    if (g->f != NULL)
        (void)fclose(g->f);
    free(g->buf);
    nd_image_free(g->canvas);
    nd_image_free(g->backup);
    free(g->indices);
    free(g->lzw_prefix);
    free(g->lzw_suffix);
    free(g->lzw_stack);
    free(g);
}

/* ------------------------------------------------------------------ *
 * Accessors
 * ------------------------------------------------------------------ */

int32_t nd_gif_width(const nd_gif *g)
{
    return g != NULL ? g->w : 0;
}

int32_t nd_gif_height(const nd_gif *g)
{
    return g != NULL ? g->h : 0;
}

size_t nd_gif_frame_count(const nd_gif *g)
{
    return g != NULL ? g->n_frames : 0u;
}

int32_t nd_gif_duration_ms(const nd_gif *g)
{
    return g != NULL ? g->duration_ms : 0;
}

size_t nd_gif_position(const nd_gif *g)
{
    return g != NULL ? g->pos : 0u;
}

void nd_gif_rewind(nd_gif *g)
{
    if (g == NULL)
        return;
    (void)rd_seek(g, g->first_block);
    if (g->canvas != NULL)
        canvas_clear(g, ND_RECT(0, 0, g->w - 1, g->h - 1));
    g->pending_disposal = DISPOSE_NONE;
    g->pos = 0u;
    g->started = false;
}

/* ------------------------------------------------------------------ *
 * Playback
 * ------------------------------------------------------------------ */

/* Everything a Graphic Control Extension says about the frame that follows
 * it. Reset per frame: a frame with no GCE is opaque and 0 ms, which is what
 * a GIF87a file is made of. */
typedef struct {
    uint8_t disposal;
    bool has_transparent;
    uint8_t transparent;
    int32_t delay_ms;
} frame_ctrl;

static void apply_pending_disposal(nd_gif *g)
{
    switch (g->pending_disposal) {
    case DISPOSE_BACKGROUND:
        canvas_clear(g, g->pending_rect);
        break;
    case DISPOSE_PREVIOUS:
        if (g->backup != NULL)
            (void)nd_image_blit(g->canvas, g->backup, 0, 0);
        break;
    default:
        break;
    }
    g->pending_disposal = DISPOSE_NONE;
}

/* Read the image descriptor, decode it, and paint it onto the canvas.
 * Returns false when the frame could not be used at all, in which case the
 * file position has still been advanced past it. */
static bool decode_frame(nd_gif *g, const frame_ctrl *ctrl)
{
    uint16_t l, t, fw, fh;
    uint8_t packed;
    uint8_t mcs;
    uint8_t lct[256u * 3u];
    const uint8_t *pal;
    int32_t pal_n;
    bool interlaced;
    nd_rect rect;
    size_t got;
    int32_t row;

    if (!rd_u16(g, &l) || !rd_u16(g, &t) || !rd_u16(g, &fw) || !rd_u16(g, &fh) ||
        !rd_u8(g, &packed))
        return false;

    pal = g->gct;
    pal_n = g->gct_n;
    if ((packed & 0x80u) != 0u) {
        uint32_t n = 2u << (packed & 0x07u);

        if (!rd(g, lct, (size_t)n * 3u))
            return false;
        pal = lct;
        pal_n = (int32_t)n;
    }
    interlaced = (packed & 0x40u) != 0u;

    if (!rd_u8(g, &mcs))
        return false;

    if (fw == 0u || fh == 0u || (size_t)fw * (size_t)fh > (size_t)g->w * (size_t)g->h) {
        /* Outside anything this panel could show. Skip the pixels, keep the
         * file position, and say so once rather than once per frame. */
        if (!g->warned_oversize) {
            g->warned_oversize = true;
            nd_log_err(ND_LOG_UI, "gif: frame %ux%u does not fit a %dx%d screen; skipped", fw, fh,
                       g->w, g->h);
        }
        (void)skip_subblocks(g);
        return false;
    }

    /* DISPOSE_PREVIOUS is defined against the canvas as it stands NOW, before
     * this frame is drawn -- so the backup is taken here and not later. */
    if (ctrl->disposal == DISPOSE_PREVIOUS) {
        if (g->backup == NULL)
            g->backup = nd_image_new(g->w, g->h, ND_PIXFMT_RGBA8888);
        if (g->backup != NULL)
            (void)nd_image_blit(g->backup, g->canvas, 0, 0);
    }

    got = lzw_decode(g, mcs, (size_t)fw * (size_t)fh);
    if (got == 0u)
        return false;

    for (row = 0; row < (int32_t)fh; row++) {
        int32_t src_row = interlaced ? interlace_row(row, (int32_t)fh) : row;
        int32_t dst_y = (int32_t)t + src_row;
        const uint8_t *idx;
        int32_t col;

        if (src_row >= (int32_t)fh)
            continue;
        if (dst_y < 0 || dst_y >= g->h)
            continue;
        idx = g->indices + (size_t)row * (size_t)fw;

        for (col = 0; col < (int32_t)fw; col++) {
            int32_t dst_x = (int32_t)l + col;
            uint8_t v;
            uint8_t *px;

            if (dst_x < 0 || dst_x >= g->w)
                continue;
            if ((size_t)row * (size_t)fw + (size_t)col >= got)
                break; /* a truncated stream keeps what it decoded */
            v = idx[col];
            if (ctrl->has_transparent && v == ctrl->transparent)
                continue; /* "leave what was there" -- the whole point */
            if ((int32_t)v >= pal_n)
                continue; /* an index past the palette draws nothing */

            px = nd_img_px(g->canvas, dst_x, dst_y);
            px[0] = pal[(size_t)v * 3u + 0u];
            px[1] = pal[(size_t)v * 3u + 1u];
            px[2] = pal[(size_t)v * 3u + 2u];
            px[3] = 255u;
        }
    }

    rect.x0 = (int32_t)l;
    rect.y0 = (int32_t)t;
    rect.x1 = (int32_t)l + (int32_t)fw - 1;
    rect.y1 = (int32_t)t + (int32_t)fh - 1;
    if (rect.x0 < 0)
        rect.x0 = 0;
    if (rect.y0 < 0)
        rect.y0 = 0;
    if (rect.x1 >= g->w)
        rect.x1 = g->w - 1;
    if (rect.y1 >= g->h)
        rect.y1 = g->h - 1;

    g->pending_disposal = ctrl->disposal;
    g->pending_rect = rect;
    return true;
}

const nd_image *nd_gif_next(nd_gif *g, int32_t *delay_ms)
{
    frame_ctrl ctrl;
    bool rewound = false;

    if (g == NULL || g->canvas == NULL)
        return NULL;

    memset(&ctrl, 0, sizeof ctrl);
    apply_pending_disposal(g);

    for (;;) {
        uint8_t intro;

        if (!rd_u8(g, &intro) || intro == BLK_TRAILER) {
            /* End of the animation. One rewind per call, so a file that is
             * nothing but a trailer cannot spin here. */
            if (rewound)
                return NULL;
            rewound = true;
            nd_gif_rewind(g);
            memset(&ctrl, 0, sizeof ctrl);
            continue;
        }

        if (intro == BLK_EXTENSION) {
            uint8_t label;

            if (!rd_u8(g, &label))
                goto restart;
            if (label == EXT_GRAPHICCTRL) {
                uint8_t size, packed, tidx, term;
                uint16_t cs;

                if (!rd_u8(g, &size) || size != 4u)
                    goto restart;
                if (!rd_u8(g, &packed) || !rd_u16(g, &cs) || !rd_u8(g, &tidx) || !rd_u8(g, &term))
                    goto restart;
                ctrl.disposal = (uint8_t)((packed >> 2) & 0x07u);
                ctrl.has_transparent = (packed & 0x01u) != 0u;
                ctrl.transparent = tidx;
                ctrl.delay_ms = (int32_t)cs * 10;
            } else if (label == EXT_PLAINTEXT || label == EXT_COMMENT || label == EXT_APPLICATION) {
                if (!skip_subblocks(g))
                    goto restart;
            } else if (!skip_subblocks(g)) {
                goto restart;
            }
            continue;
        }

        if (intro != BLK_IMAGE)
            goto restart;

        if (!decode_frame(g, &ctrl)) {
            /* A frame we could not use is not the end of the file: try the
             * next block. If that runs out, the trailer branch rewinds. */
            memset(&ctrl, 0, sizeof ctrl);
            continue;
        }

        if (g->started)
            g->pos++;
        g->started = true;
        if (delay_ms != NULL) {
            int32_t d = ctrl.delay_ms;

            if (d <= 0)
                d = ND_GIF_DEFAULT_DELAY_MS;
            else if (d < ND_GIF_MIN_DELAY_MS)
                d = ND_GIF_MIN_DELAY_MS;
            *delay_ms = d;
        }
        return g->canvas;

    restart:
        if (rewound)
            return NULL;
        rewound = true;
        nd_gif_rewind(g);
        memset(&ctrl, 0, sizeof ctrl);
    }
}

/* ------------------------------------------------------------------ *
 * Stills
 * ------------------------------------------------------------------ */

nd_image *nd_gif_load_first(const char *path)
{
    nd_gif *g = nd_gif_open(path);
    const nd_image *frame;
    nd_image *out = NULL;

    if (g == NULL)
        return NULL;
    frame = nd_gif_next(g, NULL);
    if (frame != NULL)
        out = nd_image_copy(frame);
    nd_gif_close(g);
    return out;
}

/* nd_image.h's third decoder. It is here rather than in nd_image.c so that
 * everything that knows the GIF format lives in one file. */
nd_image *nd_image_load_gif(const char *path)
{
    return nd_gif_load_first(path);
}

nd_image *nd_gif_decode_mem(const uint8_t *data, size_t len)
{
    nd_gif *g = nd_gif_open_mem(data, len);
    const nd_image *frame;
    nd_image *out = NULL;

    if (g == NULL)
        return NULL;
    frame = nd_gif_next(g, NULL);
    if (frame != NULL)
        out = nd_image_copy(frame);
    nd_gif_close(g);
    return out;
}

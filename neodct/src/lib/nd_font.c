/* nd_font.c -- FreeType behind the exact behaviour Pillow gave us.
 *
 * Everything in this file exists to make one sentence true: the letters land
 * on the same pixels they landed on under Python. That is not a matter of
 * "use FreeType too" -- Pillow makes four specific choices on top of
 * FreeType, and getting any of them wrong moves every label on the phone:
 *
 *   1. FT_Set_Pixel_Sizes(face, 0, px). Pillow asks for the size in pixels,
 *      so ppem is exactly the number the UI names (14/18/20/24).
 *   2. FT_LOAD_DEFAULT -- hinting ON, the font's own TrueType bytecode. This
 *      is what makes every advance in this font a whole number of pixels;
 *      FT_LOAD_NO_HINTING gives fractional advances and a different bitmap
 *      for nearly every glyph.
 *   3. FT_RENDER_MODE_NORMAL, 8-bit coverage.
 *   4. The ink box is the tight box of NON-ZERO coverage, because Pillow
 *      measures a drawn string with Image.getbbox() and that is what
 *      getbbox() sees. A hinted outline can leave an all-zero border row in
 *      the bitmap FreeType hands back; Pillow crops it, so we crop it.
 *
 * All four were established by rendering all 95 printable ASCII characters at
 * all four sizes and comparing the SHA-256 of the coverage bytes, the
 * advance, the ink box and the ink offset against
 * neodct/tests/golden/font/fontref.json. 380 of 380 records match, and so do
 * the 84 whole-string records. test/unit/test_font.c re-checks that on every
 * build; if you change a load flag here, that test is what tells you.
 *
 * ============ WHY THE ASCII CACHE IS FILLED EAGERLY ============
 *
 * CODING-STANDARDS.md section 4: no allocation in the render path. The
 * printable-ASCII coverage bitmaps for one size are ~11 KB at 14 px and
 * ~29 KB at 24 px, so all four sizes together cost under 80 KB -- cheaper
 * than the branch it would take to fill them lazily, and it means drawing a
 * label can never fail for want of memory. Everything outside printable
 * ASCII goes in a small fixed side table which is NEVER evicted, because
 * nd_font.h promises a returned nd_glyph stays valid until the font is freed.
 *
 * ============ THREADS ============
 *
 * An FT_Face is not thread-safe and neither is an nd_font. Fonts are created
 * and used on the UI thread only. The FT_Library handle IS shared between
 * every nd_font in the process and its reference count is taken under a
 * mutex, so a service thread creating a font of its own cannot race the UI.
 */

#include <ft2build.h>
#include FT_FREETYPE_H

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "nd_font.h"
#include "nd_log.h"

/* Printable ASCII, 0x20..0x7E. Everything the shipped UI draws. */
#define FONT_ASCII_LO 0x20u
#define FONT_ASCII_HI 0x7Eu
#define FONT_ASCII_N  (FONT_ASCII_HI - FONT_ASCII_LO + 1u)

/* Distinct non-ASCII codepoints one font will cache. The shipped OS is
 * English-only and draws none; this exists so a stray character in a contact
 * name renders instead of crashing, and it is a hard cap rather than an
 * evicting cache because callers are allowed to hold an nd_glyph pointer. */
#define FONT_OVER_N 64u

struct nd_font {
    FT_Face face;
    int32_t px;
    int32_t ascent;  /* getmetrics()[0] -- 14/18/20/24 */
    int32_t descent; /* getmetrics()[1] -- 4/5/5/6      */

    /* One allocation for every printable-ASCII coverage bitmap. Owned here,
     * freed by nd_font_free(). */
    uint8_t *ascii_arena;
    nd_glyph ascii[FONT_ASCII_N];

    /* The side table. over_buf[i] owns over[i].coverage; both are freed by
     * nd_font_free() and by nothing else. */
    nd_glyph over[FONT_OVER_N];
    uint8_t *over_buf[FONT_OVER_N];
    size_t n_over;
    bool over_full_logged;
};

/* ------------------------------------------------------------------ *
 * The shared FT_Library
 * ------------------------------------------------------------------ */

static pthread_mutex_t g_lib_lock = PTHREAD_MUTEX_INITIALIZER;
static FT_Library g_lib;
static size_t g_lib_refs;

static FT_Library lib_acquire(void)
{
    FT_Library out = NULL;

    pthread_mutex_lock(&g_lib_lock);
    if (g_lib_refs == 0) {
        if (FT_Init_FreeType(&g_lib) != 0) {
            g_lib = NULL;
            goto done;
        }
    }
    g_lib_refs++;
    out = g_lib;
done:
    pthread_mutex_unlock(&g_lib_lock);
    return out;
}

static void lib_release(void)
{
    pthread_mutex_lock(&g_lib_lock);
    if (g_lib_refs > 0 && --g_lib_refs == 0) {
        FT_Done_FreeType(g_lib);
        g_lib = NULL;
    }
    pthread_mutex_unlock(&g_lib_lock);
}

/* ------------------------------------------------------------------ *
 * 26.6 arithmetic
 * ------------------------------------------------------------------ */

/* Pillow's PIXEL() macro, verbatim: round a 26.6 fixed-point value to the
 * nearest whole pixel. Ascent, descent and every bounding-box edge Pillow
 * reports come through it, so we use the same arithmetic rather than a
 * lround() that disagrees on the halves. The >> on a negative value is an
 * arithmetic shift on every compiler this project builds with, which is the
 * same assumption Pillow's own C makes. */
static int32_t ft_pixel(FT_Pos v)
{
    return (int32_t)(((v + 32) & ~(FT_Pos)63) >> 6);
}

/* ------------------------------------------------------------------ *
 * UTF-8
 * ------------------------------------------------------------------ */

uint32_t nd_utf8_next(const char **p)
{
    const uint8_t *s;
    uint32_t cp;
    uint32_t min;
    int extra;
    int i;

    if (!p || !*p)
        return 0;

    s = (const uint8_t *)*p;
    if (s[0] == 0)
        return 0;

    if (s[0] < 0x80u) {
        *p += 1;
        return s[0];
    }

    if ((s[0] & 0xE0u) == 0xC0u) {
        cp = s[0] & 0x1Fu;
        extra = 1;
        min = 0x80u;
    } else if ((s[0] & 0xF0u) == 0xE0u) {
        cp = s[0] & 0x0Fu;
        extra = 2;
        min = 0x800u;
    } else if ((s[0] & 0xF8u) == 0xF0u) {
        cp = s[0] & 0x07u;
        extra = 3;
        min = 0x10000u;
    } else {
        /* A continuation byte or 0xFE/0xFF where a lead byte belongs.
         * Advance by one so no decoder can spin on malformed input. */
        *p += 1;
        return 0xFFFDu;
    }

    for (i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0u) != 0x80u) {
            *p += 1;
            return 0xFFFDu;
        }
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }

    /* Overlong encodings, surrogates and out-of-range values are all the same
     * kind of lie about how many bytes follow; refusing to consume them keeps
     * the "never advance by more than the input justifies" property. */
    if (cp < min || (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) {
        *p += 1;
        return 0xFFFDu;
    }

    *p += (size_t)(extra + 1);
    return cp;
}

/* ------------------------------------------------------------------ *
 * Rasterising one glyph
 * ------------------------------------------------------------------ */

/* Load and render cp into the face's slot. False means FreeType refused,
 * which for a missing character it does not do -- FT_Get_Char_Index returns
 * 0 and glyph 0 is .notdef, which in this font is blank with a real advance.
 * That is why U+2026 costs 8 px at 20 px and draws nothing. */
static bool slot_render(FT_Face face, uint32_t cp)
{
    FT_UInt index = FT_Get_Char_Index(face, (FT_ULong)cp);

    if (FT_Load_Glyph(face, index, FT_LOAD_DEFAULT) != 0)
        return false;
    if (face->glyph->format != FT_GLYPH_FORMAT_BITMAP &&
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0)
        return false;
    return true;
}

/* One row of the rendered bitmap. pitch is signed because FreeType is allowed
 * to hand back a bottom-up bitmap; it never does for FT_RENDER_MODE_NORMAL,
 * but honouring the sign costs one multiply. */
static const uint8_t *slot_row(const FT_Bitmap *bm, int32_t y)
{
    return bm->buffer + (ptrdiff_t)y * (ptrdiff_t)bm->pitch;
}

/* The tight box of non-zero coverage, in bitmap coordinates. Returns false
 * when the glyph has no ink at all (space, and .notdef in this font). */
static bool slot_ink_box(const FT_Bitmap *bm, int32_t *bx0, int32_t *by0, int32_t *bx1,
                         int32_t *by1)
{
    int32_t w = (int32_t)bm->width;
    int32_t h = (int32_t)bm->rows;
    int32_t x0 = w;
    int32_t y0 = h;
    int32_t x1 = -1;
    int32_t y1 = -1;
    int32_t x;
    int32_t y;

    /* Anything but 8-bit gray means a load flag changed and the antialiasing
     * Pillow's blend expects is gone; refuse rather than draw a hard edge. */
    if (!bm->buffer || w <= 0 || h <= 0 || bm->pixel_mode != FT_PIXEL_MODE_GRAY)
        return false;

    for (y = 0; y < h; y++) {
        const uint8_t *row = slot_row(bm, y);
        for (x = 0; x < w; x++) {
            if (row[x] == 0)
                continue;
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
    if (x1 < 0)
        return false;

    *bx0 = x0;
    *by0 = y0;
    *bx1 = x1;
    *by1 = y1;
    return true;
}

/* Fill everything in g except coverage, and report where in the slot's bitmap
 * the ink starts so the caller can copy it. */
static void slot_measure(const nd_font *f, uint32_t cp, nd_glyph *g, int32_t *src_x, int32_t *src_y)
{
    const FT_GlyphSlot slot = f->face->glyph;
    int32_t bx0 = 0;
    int32_t by0 = 0;
    int32_t bx1 = 0;
    int32_t by1 = 0;

    memset(g, 0, sizeof *g);
    g->codepoint = cp;
    /* Hinted, so this is already a whole number of pixels for every glyph in
     * this font; ft_pixel() keeps that true for a font where it is not. */
    g->advance = ft_pixel(slot->metrics.horiAdvance);

    *src_x = 0;
    *src_y = 0;

    if (!slot_ink_box(&slot->bitmap, &bx0, &by0, &bx1, &by1))
        return;

    g->ink_w = bx1 - bx0 + 1;
    g->ink_h = by1 - by0 + 1;
    /* bitmap_left is measured from the pen; bitmap_top upward from the
     * baseline, and the baseline sits `ascent` below the "la" anchor Pillow
     * draws at. */
    g->ink_dx = (int32_t)slot->bitmap_left + bx0;
    g->ink_dy = f->ascent - (int32_t)slot->bitmap_top + by0;
    *src_x = bx0;
    *src_y = by0;
}

static void slot_copy_ink(const FT_Bitmap *bm, int32_t src_x, int32_t src_y, int32_t w, int32_t h,
                          uint8_t *dst)
{
    int32_t y;

    for (y = 0; y < h; y++)
        memcpy(dst + (size_t)y * (size_t)w, slot_row(bm, src_y + y) + src_x, (size_t)w);
}

/* ------------------------------------------------------------------ *
 * The ASCII cache
 * ------------------------------------------------------------------ */

static nd_err ascii_fill(nd_font *f)
{
    size_t total = 0;
    size_t used = 0;
    uint32_t cp;
    size_t i;

    /* Pass one sizes the arena exactly. Rendering twice costs about 200
     * microseconds per font and saves carrying a realloc that would have to
     * fix up 95 interior pointers afterwards. */
    for (cp = FONT_ASCII_LO; cp <= FONT_ASCII_HI; cp++) {
        int32_t sx;
        int32_t sy;
        nd_glyph *g = &f->ascii[cp - FONT_ASCII_LO];

        if (!slot_render(f->face, cp))
            return ND_ERR_IO;
        slot_measure(f, cp, g, &sx, &sy);
        total += (size_t)g->ink_w * (size_t)g->ink_h;
    }

    if (total > 0) {
        /* owned by the nd_font; freed by nd_font_free() */
        f->ascii_arena = malloc(total);
        if (!f->ascii_arena)
            return ND_ERR_NOMEM;
    }

    for (i = 0; i < FONT_ASCII_N; i++) {
        nd_glyph *g = &f->ascii[i];
        int32_t sx;
        int32_t sy;
        size_t bytes;

        cp = FONT_ASCII_LO + (uint32_t)i;
        if (!slot_render(f->face, cp))
            return ND_ERR_IO;
        slot_measure(f, cp, g, &sx, &sy);

        bytes = (size_t)g->ink_w * (size_t)g->ink_h;
        if (bytes == 0) {
            g->coverage = NULL;
            continue;
        }
        slot_copy_ink(&f->face->glyph->bitmap, sx, sy, g->ink_w, g->ink_h, f->ascii_arena + used);
        g->coverage = f->ascii_arena + used;
        used += bytes;
    }

    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------ */

nd_font *nd_font_load(const char *path, int32_t px)
{
    nd_font *f = NULL;
    FT_Library lib = NULL;
    bool ok = false;

    if (!path || px <= 0)
        return NULL;

    lib = lib_acquire();
    if (!lib) {
        nd_log_err(ND_LOG_UI, "FreeType init failed");
        return NULL;
    }

    /* owned by the caller; freed with nd_font_free() */
    f = calloc(1, sizeof *f);
    if (!f) {
        nd_log_err(ND_LOG_UI, "out of memory loading %s at %d", path, (int)px);
        goto done;
    }

    if (FT_New_Face(lib, path, 0, &f->face) != 0) {
        nd_log_err(ND_LOG_UI, "cannot open font %s", path);
        goto done;
    }
    if (FT_Set_Pixel_Sizes(f->face, 0, (FT_UInt)px) != 0) {
        nd_log_err(ND_LOG_UI, "font %s has no %d px size", path, (int)px);
        goto done;
    }

    f->px = px;
    f->ascent = ft_pixel(f->face->size->metrics.ascender);
    f->descent = -ft_pixel(f->face->size->metrics.descender);

    if (ascii_fill(f) != ND_OK) {
        nd_log_err(ND_LOG_UI, "cannot rasterise %s at %d px", path, (int)px);
        goto done;
    }

    ok = true;
done:
    if (!ok) {
        /* nd_font_free() drops the library reference this call took; when
         * there is no nd_font to free, drop it here instead. */
        if (f)
            nd_font_free(f);
        else
            lib_release();
        f = NULL;
    }
    return f;
}

void nd_font_free(nd_font *f)
{
    size_t i;

    if (!f)
        return;

    for (i = 0; i < FONT_OVER_N; i++)
        free(f->over_buf[i]);
    free(f->ascii_arena);
    if (f->face)
        FT_Done_Face(f->face);
    free(f);
    lib_release();
}

int32_t nd_font_px(const nd_font *f)
{
    return f ? f->px : 0;
}

void nd_font_metrics(const nd_font *f, int32_t *ascent, int32_t *descent)
{
    if (ascent)
        *ascent = f ? f->ascent : 0;
    if (descent)
        *descent = f ? f->descent : 0;
}

/* ------------------------------------------------------------------ *
 * Glyph lookup
 * ------------------------------------------------------------------ */

const nd_glyph *nd_font_glyph(nd_font *f, uint32_t codepoint)
{
    nd_glyph *g;
    int32_t sx;
    int32_t sy;
    size_t bytes;
    size_t i;

    if (!f)
        return NULL;

    if (codepoint >= FONT_ASCII_LO && codepoint <= FONT_ASCII_HI)
        return &f->ascii[codepoint - FONT_ASCII_LO];

    for (i = 0; i < f->n_over; i++) {
        if (f->over[i].codepoint == codepoint)
            return &f->over[i];
    }

    if (f->n_over >= FONT_OVER_N) {
        /* Once, not once per frame: a screen full of unexpected characters
         * would otherwise write more log than it draws pixels. */
        if (!f->over_full_logged) {
            f->over_full_logged = true;
            nd_log_err(ND_LOG_UI, "glyph cache full at %d px, U+%04X not drawn", (int)f->px,
                       (unsigned)codepoint);
        }
        return NULL;
    }

    if (!slot_render(f->face, codepoint))
        return NULL;

    g = &f->over[f->n_over];
    slot_measure(f, codepoint, g, &sx, &sy);
    bytes = (size_t)g->ink_w * (size_t)g->ink_h;
    if (bytes > 0) {
        /* owned by the nd_font; freed by nd_font_free() */
        uint8_t *buf = malloc(bytes);
        if (!buf)
            return NULL;
        slot_copy_ink(&f->face->glyph->bitmap, sx, sy, g->ink_w, g->ink_h, buf);
        f->over_buf[f->n_over] = buf;
        g->coverage = buf;
    }
    f->n_over++;
    return g;
}

/* The measuring calls take a const nd_font, so they cannot fill the cache.
 * Reading a cached entry covers everything the UI draws; anything else is
 * rasterised into the face's slot and measured there, which mutates the
 * FT_Face but not the nd_font -- the same distinction C's const already
 * makes about a pointer member. */
static bool measure_glyph(const nd_font *f, uint32_t cp, nd_glyph *out)
{
    int32_t sx;
    int32_t sy;
    size_t i;

    if (cp >= FONT_ASCII_LO && cp <= FONT_ASCII_HI) {
        *out = f->ascii[cp - FONT_ASCII_LO];
        return true;
    }
    for (i = 0; i < f->n_over; i++) {
        if (f->over[i].codepoint == cp) {
            *out = f->over[i];
            return true;
        }
    }
    if (!slot_render(f->face, cp))
        return false;
    slot_measure(f, cp, out, &sx, &sy);
    out->coverage = NULL;
    return true;
}

int32_t nd_font_advance(const nd_font *f, uint32_t codepoint)
{
    nd_glyph g;

    if (!f)
        return 0;
    if (!measure_glyph(f, codepoint, &g))
        return 0;
    return g.advance;
}

/* ------------------------------------------------------------------ *
 * Measurement
 * ------------------------------------------------------------------ */

void nd_text_bbox(const nd_font *f, const char *utf8, nd_rect *out)
{
    const char *p;
    int32_t pen = 0;
    int32_t top;
    int32_t bottom;

    if (!out)
        return;
    if (!f) {
        *out = ND_RECT(0, 0, 0, 0);
        return;
    }

    /* The box starts collapsed ON THE BASELINE, not empty and not on the
     * first glyph's ink. That is what makes get_text_size("_") report 3 px at
     * 20 px -- the underscore's ink sits above the baseline and the box is
     * stretched down to meet it. Pillow initialises its y extents to zero in
     * baseline-relative coordinates and this is the same thing said in "la"
     * coordinates. */
    top = f->ascent;
    bottom = f->ascent;

    for (p = utf8; p && *p;) {
        uint32_t cp = nd_utf8_next(&p);
        nd_glyph g;

        if (!measure_glyph(f, cp, &g))
            continue;
        if (g.ink_h > 0) {
            if (g.ink_dy < top)
                top = g.ink_dy;
            if (g.ink_dy + g.ink_h > bottom)
                bottom = g.ink_dy + g.ink_h;
        }
        pen += g.advance;
    }

    /* Right edge is the pen, not the ink: Pillow's textbbox reports the full
     * advance, so "Menu" at 20 px is 68 wide and a trailing space counts.
     * Right and bottom are EXCLUSIVE here, unlike every other nd_rect. */
    out->x0 = 0;
    out->y0 = top;
    out->x1 = pen;
    out->y1 = bottom;
}

void nd_text_size(const nd_font *f, const char *utf8, int32_t *w, int32_t *h)
{
    nd_rect box;

    nd_text_bbox(f, utf8, &box);
    if (w)
        *w = box.x1 - box.x0;
    if (h)
        *h = box.y1 - box.y0;
}

int32_t nd_text_advance(const nd_font *f, const char *utf8)
{
    const char *p;
    int32_t pen = 0;

    if (!f)
        return 0;
    for (p = utf8; p && *p;) {
        uint32_t cp = nd_utf8_next(&p);
        nd_glyph g;

        if (measure_glyph(f, cp, &g))
            pen += g.advance;
    }
    return pen;
}

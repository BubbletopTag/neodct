/* nd_font.h -- FreeType at four sizes on one TTF, and the measurement the
 * whole UI positions itself with.
 *
 * The font is Nokia Cellphone FC (Small), a 16,272-byte TTF at
 * /NeoDCT/System/ui/resources/fonts/font.ttf, loaded at 14, 18, 20 and 24 px.
 * Same renderer, same file, same sizes as Pillow used -- that is how the
 * letters land on the same pixels.
 *
 * ============ WHAT nd_text_size RETURNS ============
 *
 * INK EXTENTS. Not line metrics. The string "_" is 3 pixels tall at 20 px;
 * "Ag" is 21 at the same size. About forty places in the widget code centre
 * and stack things using this number, so text visibly shifts by a few pixels
 * depending on which letters are in it. That is what the current screens look
 * like and the C must reproduce it.
 *
 * ============ TWO FACTS ESTABLISHED BY MEASUREMENT ============
 *
 * Recorded in neodct/tests/golden/font/fontref.json, which is the oracle for
 * this module -- per-glyph coverage hashes, advances, ink boxes and whole-
 * string pen trails for all four sizes:
 *
 *   1. Pillow's BASIC layout engine applies NO KERNING. Pen positions are the
 *      running sum of advances and nothing else. (The phone's Pillow is built
 *      with -Craqm=disable, so BASIC is what the device has always used.)
 *   2. Every advance is an INTEGER number of pixels. There is no fractional
 *      accumulation to get right.
 *
 * Fact 2 contradicts spec-core-loop.md section 7, which describes advances as
 * "n * size / 8" and therefore fractional. fontref.json was captured after
 * that spec was written and with the layout engine forced to BASIC; it wins.
 * Logged in OPEN-QUESTIONS.md.
 */

#ifndef ND_FONT_H_INCLUDED
#define ND_FONT_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The four sizes the OS loads. Nothing else may be loaded at runtime --
 * a fifth size is a new glyph cache and a new set of golden frames. */
#define ND_FONT_PX_S  14
#define ND_FONT_PX_MD 18
#define ND_FONT_PX_N  20
#define ND_FONT_PX_XL 24

/* Opaque: it owns an FT_Face and a per-size glyph cache, and no caller has
 * any business inside either. */
typedef struct nd_font nd_font;

/* One rendered glyph, as FreeType produced it and as fontref.json records it.
 * The bitmap is 8-bit coverage, ink_w * ink_h, row-major, owned by the font's
 * cache -- valid until the font is freed, never freed by the caller. */
typedef struct {
    uint32_t codepoint;
    int32_t advance; /* whole pixels; see fact 2 above           */
    int32_t ink_w;
    int32_t ink_h;
    int32_t ink_dx;          /* pen x + ink_dx == left edge of the ink    */
    int32_t ink_dy;          /* ascender line + ink_dy == top of the ink  */
    const uint8_t *coverage; /* ink_w * ink_h bytes, or NULL when blank */
} nd_glyph;

/* ------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------ */

/* Load path at px pixels. Owned by the caller; free with nd_font_free().
 * NULL on failure -- the caller is expected to log and fall back, exactly as
 * NeoDCT_UI does with "[UI] Font load failed, using default."
 *
 * FreeType's library handle is shared and reference-counted internally, so
 * loading the same file four times costs one mmap. */
nd_font *nd_font_load(const char *path, int32_t px);

void nd_font_free(nd_font *f);

/* Pixel size the font was loaded at. */
int32_t nd_font_px(const nd_font *f);

/* getmetrics() -- (ascent, descent), measured as (14,4) (18,5) (20,5) (24,6).
 * The widgets never read these; the text renderer needs them for the "la"
 * anchor and nd_text_size() needs the ascent as its baseline origin. */
void nd_font_metrics(const nd_font *f, int32_t *ascent, int32_t *descent);

/* ------------------------------------------------------------------ *
 * Measurement -- the single most-used call in the UI
 * ------------------------------------------------------------------ */

/* ui.get_text_size(text, font): the INK width and height of the laid-out
 * string, i.e. (bbox.right - bbox.left, bbox.bottom - bbox.top) of
 * draw.textbbox((0,0), text, font).
 *
 * A string of only spaces reports a positive width and ZERO height. An empty
 * string reports (0, 0). Either output pointer may be NULL. */
void nd_text_size(const nd_font *f, const char *utf8, int32_t *w, int32_t *h);

/* The bounding box itself, origin (0,0), right and bottom EXCLUSIVE, as
 * Pillow returns it. nd_text_size() is this minus the origin. */
void nd_text_bbox(const nd_font *f, const char *utf8, nd_rect *out);

/* Sum of advances for the whole string -- the pen position after drawing it.
 * With no kerning this is a plain sum, and it is NOT the same as the ink
 * width: "j" advances 4 px at 14 but its ink starts left of the pen. */
int32_t nd_text_advance(const nd_font *f, const char *utf8);

/* One glyph's advance, in whole pixels. Zero for a codepoint the font has no
 * glyph for... except U+2026, which advances 8 px at 20 and draws nothing;
 * the missing-glyph advance comes from the font's .notdef, so ask the font,
 * do not assume zero. */
int32_t nd_font_advance(const nd_font *f, uint32_t codepoint);

/* Fetch (and cache) one rendered glyph. Returns NULL only when the font
 * itself is broken; a blank glyph such as space comes back with
 * coverage == NULL and ink_w == ink_h == 0. */
const nd_glyph *nd_font_glyph(nd_font *f, uint32_t codepoint);

/* ------------------------------------------------------------------ *
 * UTF-8
 * ------------------------------------------------------------------ */

/* Decode one codepoint, advancing *p past it. Invalid bytes yield U+FFFD and
 * advance by one, so a decoder can never loop forever on bad input. */
uint32_t nd_utf8_next(const char **p);

#ifdef __cplusplus
}
#endif

#endif /* ND_FONT_H_INCLUDED */

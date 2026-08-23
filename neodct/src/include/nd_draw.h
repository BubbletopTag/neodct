/* nd_draw.h -- the seven ImageDraw primitives, and the two rules that decide
 * whether the port looks identical.
 *
 * Counted across the whole overlay: text 117, rectangle 91, line 29,
 * polygon 9, ellipse 7, textbbox 4, point 2. That is the entire drawing
 * surface of the operating system.
 *
 * ============ RULE 1: RECTANGLES INCLUDE BOTH CORNERS ============
 *
 *   nd_draw_rect_fill(d, ND_RECT(2,3,6,8), c)
 *
 * lights x in [2,6] and y in [3,8] -- six columns and six rows. This is
 * Pillow's behaviour and every coordinate in every spec assumes it. A
 * half-open implementation loses the right column and bottom row of every
 * box on the phone.
 *
 * ============ RULE 2: A WIDE LINE GROWS IN THE MINOR AXIS ONLY ============
 *
 *   nd_draw_line(d, 10, 36, 10, 135, c, 2)
 *
 * lights columns 10 AND 11. A horizontal line at y=10 with width 2 lights rows
 * 10 and 11. It does not grow symmetrically, it does not round outward, and it
 * does not extend the line's length. Three scrollbar tracks depend on this
 * (AppSelector, PagedList, DetailPage) and one does not (VerticalList, which
 * asks for width 1 in grey).
 *
 * ============ FLOATS TRUNCATE ============
 *
 * Where the Python computes a coordinate as a float -- every scrollbar notch
 * does -- Pillow truncates toward zero: 8.5 becomes 8. Compute in double and
 * pass through nd_trunc32(), never round().
 *
 * ============ NO ALLOCATION ============
 *
 * Nothing in this header allocates. Functions that need scratch space take it
 * as an argument. This is the render path; CODING-STANDARDS.md section 4
 * forbids allocating in it.
 */

#ifndef ND_DRAW_H
#define ND_DRAW_H

#include "nd_font.h"
#include "nd_image.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PIL's ImageDraw.Draw(img): a drawing context bound to one surface. Small
 * and caller-owned, so it lives on the stack or inside nd_ui -- never
 * allocated. Fields are public but treat them as read-only. */
typedef struct nd_draw {
    nd_image *img; /* the target. Not owned. Never NULL after bind. */
} nd_draw;

/* Bind a context to a surface. The context is valid for as long as the
 * surface is. Rebinding is cheap and is how the DetailPage's scratch column
 * gets drawn into. */
nd_err nd_draw_bind(nd_draw *d, nd_image *img);

/* ------------------------------------------------------------------ *
 * Shapes
 * ------------------------------------------------------------------ */

/* rectangle(box, fill=c) -- inclusive of both corners. Clipped. */
nd_err nd_draw_rect_fill(nd_draw *d, nd_rect box, nd_color c);

/* rectangle(box, outline=c, width=w) -- a border w pixels thick drawn INSIDE
 * the inclusive rectangle; the interior is untouched. Every caller in the
 * project passes width 1. */
nd_err nd_draw_rect_outline(nd_draw *d, nd_rect box, nd_color c, int32_t width);

/* line((x0,y0,x1,y1), fill=c, width=w). Endpoints inclusive. See RULE 2 for
 * what width does. Bresenham for the diagonal case, which no shipped code
 * currently reaches -- every line in the overlay is axis-aligned. */
nd_err nd_draw_line(nd_draw *d, int32_t x0, int32_t y0, int32_t x1, int32_t y1, nd_color c,
                    int32_t width);

/* point((x,y), fill=c). One pixel, clipped. The T9 pencil plots roughly a
 * hundred of these per indicator, deliberately, because at 15 px a polygon's
 * edges land wherever rounding puts them. */
nd_err nd_draw_point(nd_draw *d, int32_t x, int32_t y, nd_color c);

/* polygon(points, fill=c) -- Pillow's scanline fill, including its parity
 * rule for self-intersecting outlines. Memory's card glyphs include a
 * self-intersecting quadrilateral whose interior depends on it. Used by
 * exactly two things: that, and Koki's music-note icon.
 * n_points is capped by the implementation; ND_ERR_INVAL beyond it. */
nd_err nd_draw_polygon(nd_draw *d, const nd_point *points, size_t n_points, nd_color c);

/* ellipse(box, fill=c) / ellipse(box, outline=c) -- inscribed in the
 * inclusive box, matching Pillow's rasterisation. Pass fill for a solid
 * ellipse; pass outline with width for a ring. Use one or the other. */
nd_err nd_draw_ellipse_fill(nd_draw *d, nd_rect box, nd_color c);
nd_err nd_draw_ellipse_outline(nd_draw *d, nd_rect box, nd_color c, int32_t width);

/* ------------------------------------------------------------------ *
 * Text
 * ------------------------------------------------------------------ */

/* text((x,y), s, font=f, fill=c) with Pillow's default anchor "la".
 *
 * THE Y YOU PASS IS THE ASCENDER LINE, NOT THE TOP OF THE INK. The baseline
 * lands at y + ascent, and the visible ink begins at y + bbox_top, where
 * bbox_top is 1 at 14 px, 3 at 18, 2 at 20 and 3 at 24. Getting this wrong
 * moves every label on the phone by a couple of pixels.
 *
 * Glyphs are rendered antialiased (FreeType FT_RENDER_MODE_NORMAL, 8-bit
 * coverage) and composited with nd_blend8() -- see nd_image.h. A plain white
 * label on black really does contain greys; that is not a bug to remove.
 *
 * The string is UTF-8. A codepoint the font has no glyph for draws nothing and
 * still costs its advance -- U+2026 is 8 px of invisible gap at 20 px, and
 * MessageDialog depends on exactly that. */
nd_err nd_draw_text(nd_draw *d, int32_t x, int32_t y, const char *utf8, const nd_font *f,
                    nd_color c);

/* textbbox((0,0), s, font=f) -- the INK bounding box of the laid-out string,
 * with right and bottom exclusive, as Pillow returns it. This is what
 * ui.get_text_size() is built on; see nd_font.h for the measurement itself. */
nd_err nd_draw_textbbox(const nd_font *f, const char *utf8, nd_rect *out);

#ifdef __cplusplus
}
#endif

#endif /* ND_DRAW_H */

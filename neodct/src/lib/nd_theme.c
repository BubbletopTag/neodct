/* nd_theme.c -- the Frutiger Aero primitives. See nd_theme.h for the four
 * ideas the look is made of; this file is how each one is computed.
 *
 * ============ WHY THIS WRITES PIXELS ITSELF ============
 *
 * Every function here either blends or varies colour down the rectangle, and
 * nd_draw does neither: its ink is resolved once into a machine word and
 * splatted, and its alpha is dropped on the RGB canvas because that is what
 * Pillow does. Both are correct there and neither is usable here, so this
 * file addresses rows through nd_image_priv.h and composites with nd_blend8()
 * -- the same measured formula the glyph rasteriser uses, so a half-covered
 * corner and a half-covered letter agree about what half means.
 *
 * The one exception is text, which goes through nd_draw_text() twice. A
 * shadow that was a blur would cost a scratch surface per label; a shadow
 * that is the same glyphs one pixel lower costs a second rasterisation and
 * looks the same at 14 to 24 px.
 *
 * ============ THE CLIP HAPPENS ONCE ============
 *
 * Every entry point clips its rectangle against the surface before the loops
 * and then addresses rows directly, rather than calling a bounds-checked
 * plot() per pixel. At 240x175 the difference is the whole cost of a full
 * screen gradient: 42,000 branch pairs against none.
 */

#include "nd_theme.h"

#include <string.h>

#include "nd_image_priv.h"

/* ------------------------------------------------------------------ *
 * Blending
 * ------------------------------------------------------------------ */

/* Composite one RGB triple at `cov` coverage. Alpha, where the target has a
 * channel for it, is driven to opaque as coverage rises: a translucent plate
 * drawn onto an RGBA surface should end up opaque where it is solid, not
 * inherit whatever transparency was underneath. On the RGB canvas -- which is
 * every real target -- the branch is not taken. */
static void blend_px(uint8_t *p, nd_pixfmt fmt, nd_color c, uint8_t cov)
{
    uint8_t px[4];

    nd_img_px_read(p, fmt, px);
    px[0] = nd_blend8(px[0], c.r, cov);
    px[1] = nd_blend8(px[1], c.g, cov);
    px[2] = nd_blend8(px[2], c.b, cov);
    if (fmt == ND_PIXFMT_RGBA8888)
        px[3] = nd_blend8(px[3], 255u, cov);
    nd_img_px_write(p, fmt, px);
}

/* The clipped intersection of r with the surface. Returns false when nothing
 * survives, which every caller treats as "draw nothing" rather than an error
 * -- a plate pushed off the edge by a long string is a layout the widget
 * chose, not a fault to report. */
static bool clip(const nd_image *img, nd_rect r, nd_rect *out)
{
    if (img == NULL || img->pixels == NULL)
        return false;
    if (r.x0 > r.x1 || r.y0 > r.y1)
        return false;

    out->x0 = (r.x0 < 0) ? 0 : r.x0;
    out->y0 = (r.y0 < 0) ? 0 : r.y0;
    out->x1 = (r.x1 >= img->w) ? img->w - 1 : r.x1;
    out->y1 = (r.y1 >= img->h) ? img->h - 1 : r.y1;

    return out->x0 <= out->x1 && out->y0 <= out->y1;
}

/* Linear interpolation of one channel across `span` rows, at row `i`.
 * Rounded, not truncated: a 30-row gradient truncated has a visible flat band
 * at the top where the first two rows land on the same value. */
static uint8_t lerp8(uint8_t a, uint8_t b, int32_t i, int32_t span)
{
    int32_t d;

    if (span <= 0)
        return a;
    d = (int32_t)b - (int32_t)a;
    return (uint8_t)((int32_t)a + ((d * i * 2 + (d >= 0 ? span : -span)) / (span * 2)));
}

/* The bottom of a ramp. With gradients off it is the TOP colour, which
 * collapses every gradient in the interface to a flat fill without any of the
 * ramp code having to know -- and keeps "top" as the single colour a flat
 * theme has to name. */
static nd_color ramp_bot(nd_color top, nd_color bot)
{
    return ND_TH_GRADIENTS ? bot : top;
}

static nd_color lerp_colour(nd_color a, nd_color b, int32_t i, int32_t span)
{
    return ND_RGB(lerp8(a.r, b.r, i, span), lerp8(a.g, b.g, i, span), lerp8(a.b, b.b, i, span));
}

static void span_blend(nd_image *img, int32_t x0, int32_t x1, int32_t y, nd_color c, uint8_t cov)
{
    uint8_t *p;
    int32_t x;

    if (cov == 0u)
        return;
    p = nd_img_px(img, x0, y);
    for (x = x0; x <= x1; x++) {
        blend_px(p, img->fmt, c, cov);
        p += img->bpp;
    }
}

/* ------------------------------------------------------------------ *
 * Fills
 * ------------------------------------------------------------------ */

void nd_theme_fill(nd_image *img, nd_rect r, nd_color c, uint8_t alpha)
{
    nd_rect q;
    int32_t y;

    if (alpha == 0u || !clip(img, r, &q))
        return;

    for (y = q.y0; y <= q.y1; y++)
        span_blend(img, q.x0, q.x1, y, c, alpha);
}

void nd_theme_gradient_v_ramped(nd_image *img, nd_rect paint, int32_t ramp_y0, int32_t ramp_y1,
                                nd_color top, nd_color bot, uint8_t alpha)
{
    bot = ramp_bot(top, bot);
    nd_rect q;
    int32_t span = ramp_y1 - ramp_y0;
    int32_t y;

    if (alpha == 0u || !clip(img, paint, &q))
        return;

    for (y = q.y0; y <= q.y1; y++) {
        /* Clamped rather than extrapolated: a row outside the ramp gets the
         * nearer end's colour, which is what "the gradient stops here" means.
         * Extrapolating would run the channels past their endpoints and wrap
         * them through the uint8_t. */
        int32_t i = nd_clamp32(y - ramp_y0, 0, span > 0 ? span : 0);

        span_blend(img, q.x0, q.x1, y, lerp_colour(top, bot, i, span), alpha);
    }
}

void nd_theme_gradient_fade_ramped(nd_image *img, nd_rect paint, int32_t ramp_y0, int32_t ramp_y1,
                                   nd_color c, uint8_t a_top, uint8_t a_bot)
{
    nd_rect q;
    int32_t span = ramp_y1 - ramp_y0;
    int32_t y;

    if (!clip(img, paint, &q))
        return;

    for (y = q.y0; y <= q.y1; y++) {
        int32_t i = nd_clamp32(y - ramp_y0, 0, span > 0 ? span : 0);

        span_blend(img, q.x0, q.x1, y, c, lerp8(a_top, a_bot, i, span));
    }
}

/* The plain forms: ramp and region are the same rectangle. This is what a
 * plate wants -- its gradient is defined by its own height and nothing
 * else. */
void nd_theme_gradient_v(nd_image *img, nd_rect r, nd_color top, nd_color bot, uint8_t alpha)
{
    nd_theme_gradient_v_ramped(img, r, r.y0, r.y1, top, bot, alpha);
}

void nd_theme_gradient_fade(nd_image *img, nd_rect r, nd_color c, uint8_t a_top, uint8_t a_bot)
{
    nd_theme_gradient_fade_ramped(img, r, r.y0, r.y1, c, a_top, a_bot);
}

/* ------------------------------------------------------------------ *
 * Rounded corners
 * ------------------------------------------------------------------ *
 *
 * Coverage for one pixel of one corner, 0..255. `dx` and `dy` are the
 * distances IN PIXEL CENTRES from the corner's circle centre, so the test is
 * against radius - 0.5 with one pixel of feather either side.
 *
 * Fixed point rather than float: this runs for every pixel of every corner of
 * every plate on every frame, and the RV1103's FPU is not the reason to
 * spend a square root here. dist2 is compared against two squared radii and
 * only the band between them interpolates -- which is at most a couple of
 * dozen pixels per corner.
 */
static uint8_t corner_cov(int32_t dx, int32_t dy, int32_t radius)
{
    int32_t d2 = dx * dx + dy * dy;
    int32_t r_in = radius - 1;
    int32_t r_out = radius;
    int32_t in2 = r_in > 0 ? r_in * r_in : 0;
    int32_t out2 = r_out * r_out;
    int32_t band;

    if (d2 <= in2)
        return 255u;
    if (d2 >= out2)
        return 0u;

    /* Interpolate on the SQUARED distance across the one-pixel band. It is not
     * the exact area a scanline integrator would give, but the error is under
     * a level of the 256 at every radius this OS draws, and it costs two
     * multiplies instead of a sqrt. */
    band = out2 - in2;
    if (band <= 0)
        return 255u;
    return (uint8_t)(255 - ((d2 - in2) * 255) / band);
}

/* Coverage of pixel (x,y) inside rectangle r with `radius` corners. Pixels
 * away from the four corner squares are fully covered and cost one compare. */
static uint8_t round_cov(nd_rect r, int32_t x, int32_t y, int32_t radius)
{
    int32_t cx;
    int32_t cy;

    if (radius <= 0)
        return 255u;

    /* The corner circle centres sit `radius - 1` in from each edge, so the
     * curve is tangent to the rectangle rather than inset by a pixel. */
    if (x < r.x0 + radius)
        cx = r.x0 + radius - 1;
    else if (x > r.x1 - radius)
        cx = r.x1 - radius + 1;
    else
        return 255u;

    if (y < r.y0 + radius)
        cy = r.y0 + radius - 1;
    else if (y > r.y1 - radius)
        cy = r.y1 - radius + 1;
    else
        return 255u;

    return corner_cov(x - cx, y - cy, radius);
}

/* The largest radius this rectangle can carry. A caller asking for 8 on a
 * 6 px tall pill gets 3, rather than two corner arcs crossing in the middle
 * and eating a bite out of the shape. */
static int32_t clamp_radius(nd_rect r, int32_t radius)
{
    /* A flat theme squares every corner, and it is done HERE rather than at
     * the seventeen call sites that pass a radius: those numbers are a
     * layout's business and stay as written, so turning the corners back on
     * restores the shape they always described. */
    if (!ND_TH_ROUND)
        return 0;
    int32_t half_w = (r.x1 - r.x0 + 1) / 2;
    int32_t half_h = (r.y1 - r.y0 + 1) / 2;
    int32_t cap = half_w < half_h ? half_w : half_h;

    if (radius < 0)
        return 0;
    return radius > cap ? cap : radius;
}

/* Both rounded fills are the same loop with a different colour per row, so
 * they share one. `top`/`bot` equal is the flat case and costs one lerp. */
static void round_body(nd_image *img, nd_rect r, int32_t radius, nd_color top, nd_color bot,
                       uint8_t alpha)
{
    nd_rect q;
    int32_t span;
    int32_t y;

    if (alpha == 0u || !clip(img, r, &q))
        return;

    radius = clamp_radius(r, radius);
    span = r.y1 - r.y0;

    for (y = q.y0; y <= q.y1; y++) {
        nd_color c = lerp_colour(top, bot, y - r.y0, span);
        int32_t x;
        uint8_t *p;

        /* Rows clear of both corner bands are one straight run. That is most
         * of every plate, so it is worth the branch. */
        if (radius == 0 || (y >= r.y0 + radius && y <= r.y1 - radius)) {
            span_blend(img, q.x0, q.x1, y, c, alpha);
            continue;
        }

        p = nd_img_px(img, q.x0, y);
        for (x = q.x0; x <= q.x1; x++) {
            uint8_t cov = round_cov(r, x, y, radius);

            if (cov != 0u)
                blend_px(p, img->fmt, c, (uint8_t)(((uint32_t)cov * alpha + 127u) / 255u));
            p += img->bpp;
        }
    }
}

void nd_theme_round_fill(nd_image *img, nd_rect r, int32_t radius, nd_color c, uint8_t alpha)
{
    round_body(img, r, radius, c, c, alpha);
}

void nd_theme_round_gradient(nd_image *img, nd_rect r, int32_t radius, nd_color top, nd_color bot,
                             uint8_t alpha)
{
    bot = ramp_bot(top, bot);
    round_body(img, r, radius, top, bot, alpha);
}

void nd_theme_round_outline(nd_image *img, nd_rect r, int32_t radius, nd_color c, uint8_t alpha)
{
    nd_rect q;
    int32_t y;

    if (alpha == 0u || !clip(img, r, &q))
        return;

    radius = clamp_radius(r, radius);

    /* The border is where coverage falls off: full inside, zero outside, and
     * the ring between is the line. Deriving it from the same coverage
     * function the fill uses is what keeps a bordered plate's edge from
     * being a pixel wider on one side than the shape it is bordering. */
    for (y = q.y0; y <= q.y1; y++) {
        uint8_t *p = nd_img_px(img, q.x0, y);
        int32_t x;

        for (x = q.x0; x <= q.x1; x++) {
            bool edge = (x == r.x0 || x == r.x1 || y == r.y0 || y == r.y1);
            uint8_t cov = round_cov(r, x, y, radius);

            /* On a straight edge the border is the edge pixel itself; on a
             * corner it is any pixel the curve only partly covers. */
            if (radius > 0 && cov != 0u && cov != 255u)
                blend_px(p, img->fmt, c, (uint8_t)(((uint32_t)(255u - cov) * alpha + 127u) / 255u));
            else if (edge && cov == 255u)
                blend_px(p, img->fmt, c, alpha);
            p += img->bpp;
        }
    }
}

/* ------------------------------------------------------------------ *
 * The glossy plate
 * ------------------------------------------------------------------ */

/* The decoration every plate shares, applied after its own fields are set so
 * that one theme switch reaches all four constructors. */
static void plate_style(nd_theme_plate *p)
{
    if (!ND_TH_GLOSS)
        p->sheen_a = 0u;
    if (!ND_TH_BEVEL)
        p->bevel = false;
    if (!ND_TH_PLATE_SHADOW)
        p->drop_shadow = false;
}

nd_theme_plate nd_theme_plate_blue(int32_t radius)
{
    nd_theme_plate p;

    memset(&p, 0, sizeof p);
    p.top = ND_TH_BLUE_TOP;
    p.bot = ND_TH_BLUE_BOT;
    p.border = ND_TH_BLUE_DEEP;
    p.border_a = 210u;
    p.sheen_a = 90u;
    p.body_a = 255u;
    p.radius = radius;
    p.bevel = true;
    p.drop_shadow = true;
    plate_style(&p);
    return p;
}

/* The title bar and the softkey strip.
 *
 * Identical to the blue plate in a glass theme -- bar_top and bar_bot ARE the
 * blues there -- and completely different in a flat one, where they are the
 * background and the strip disappears behind its own type. The border goes
 * with them, because a dark cut around a black bar on a black screen is a
 * line the classic look does not have. */
nd_theme_plate nd_theme_plate_bar(int32_t radius)
{
    nd_theme_plate p;

    memset(&p, 0, sizeof p);
    p.top = ND_TH_BAR_TOP;
    p.bot = ND_TH_BAR_BOT;
    p.border = ND_TH_BLUE_DEEP;
    p.border_a = ND_TH_GRADIENTS ? 210u : 0u;
    p.sheen_a = 90u;
    p.body_a = 255u;
    p.radius = radius;
    p.bevel = true;
    p.drop_shadow = true;
    plate_style(&p);
    return p;
}

nd_theme_plate nd_theme_plate_glass(int32_t radius)
{
    nd_theme_plate p;

    memset(&p, 0, sizeof p);
    p.top = ND_TH_GLASS_TOP;
    p.bot = ND_TH_GLASS_BOT;
    p.border = ND_TH_BLUE_DEEP;
    p.border_a = 120u;
    p.sheen_a = 70u;
    /* Not opaque: the panel is glass and the wallpaper belongs under it.
     * 216 is the point at which 20 px navy type is still comfortably legible
     * over the busiest shipped wallpaper -- measured, not chosen. */
    p.body_a = 216u;
    p.radius = radius;
    p.bevel = true;
    p.drop_shadow = true;
    plate_style(&p);
    return p;
}

nd_theme_plate nd_theme_plate_chrome(int32_t radius)
{
    nd_theme_plate p;

    memset(&p, 0, sizeof p);
    p.top = ND_TH_CHROME_TOP;
    p.bot = ND_TH_CHROME_BOT;
    p.border = ND_TH_BLUE_DEEP;
    p.border_a = 150u;
    p.sheen_a = 120u;
    p.body_a = 235u;
    p.radius = radius;
    p.bevel = true;
    p.drop_shadow = false;
    plate_style(&p);
    return p;
}

void nd_theme_plate_draw(nd_image *img, nd_rect r, const nd_theme_plate *p)
{
    int32_t radius;
    int32_t mid;

    if (img == NULL || p == NULL || r.x0 > r.x1 || r.y0 > r.y1)
        return;

    radius = clamp_radius(r, p->radius);

    /* 0. The shadow first, so the plate lands on top of its own edge rather
     *    than the shadow being smeared over the border. Two rows, because one
     *    reads as a misplaced divider and three as a halo. */
    if (p->drop_shadow)
        nd_theme_shadow_band(img, r.x0 + radius, r.x1 - radius, r.y1 + 1, 2, 90u);

    /* 1. The body. */
    round_body(img, r, radius, p->top, p->bot, p->body_a);

    /* 2. The sheen: white, the TOP HALF ONLY, stopping dead at the midpoint.
     *    The hard edge is the effect -- see idea 2 in the header. It fades
     *    from sheen_a at the top to about a third of that at the cut, which
     *    is what stops the stop looking like a printing error. */
    mid = r.y0 + (r.y1 - r.y0) / 2;
    if (p->sheen_a != 0u && mid > r.y0) {
        nd_rect gloss = ND_RECT(r.x0, r.y0, r.x1, mid);
        nd_rect q;
        int32_t y;
        int32_t span = mid - r.y0;

        if (clip(img, gloss, &q)) {
            for (y = q.y0; y <= q.y1; y++) {
                uint8_t a = lerp8(p->sheen_a, (uint8_t)(p->sheen_a / 3u), y - r.y0, span);
                int32_t x;
                uint8_t *px = nd_img_px(img, q.x0, y);

                for (x = q.x0; x <= q.x1; x++) {
                    /* Masked by the plate's own coverage, so the sheen
                     * follows the rounded corner instead of squaring it off. */
                    uint8_t cov = round_cov(r, x, y, radius);

                    if (cov != 0u)
                        blend_px(px, img->fmt, ND_TH_CHROME_HI,
                                 (uint8_t)(((uint32_t)cov * a + 127u) / 255u));
                    px += img->bpp;
                }
            }
        }
    }

    /* 3. The bevel: a white hairline one row inside the top edge, inset by
     *    the radius so it does not run out past the curve. */
    if (p->bevel && r.y1 > r.y0)
        span_blend(img, nd_max32(0, r.x0 + radius), nd_min32(img->w - 1, r.x1 - radius), r.y0 + 1,
                   ND_TH_CHROME_HI, 150u);

    /* 4. The border last, over everything, so nothing bleeds past the edge. */
    if (p->border_a != 0u)
        nd_theme_round_outline(img, r, radius, p->border, p->border_a);
}

/* ------------------------------------------------------------------ *
 * Edges
 * ------------------------------------------------------------------ */

void nd_theme_divider(nd_image *img, int32_t x0, int32_t x1, int32_t y, uint8_t alpha)
{
    nd_rect q;

    if (img == NULL)
        return;

    /* ONE RULE, OR THE CUT-AND-CATCH PAIR.
     *
     * The pair is idea 3: a dark line for the cut and a white one just inside
     * it for the light on the bevel. On a flat black screen the dark half is
     * invisible and the white half is the whole divider -- which is exactly
     * what the classic look draws, a single white pixel row -- so a flat
     * theme skips the cut and paints the catch at full strength rather than
     * at the 130/255 that makes it read as a highlight. */
    if (!ND_TH_BEVEL_DIVIDER) {
        if (clip(img, ND_RECT(x0, y, x1, y), &q))
            span_blend(img, q.x0, q.x1, q.y0, ND_TH_CHROME_HI, alpha);
        return;
    }
    if (clip(img, ND_RECT(x0, y, x1, y), &q))
        span_blend(img, q.x0, q.x1, q.y0, ND_TH_BLUE_DEEP, alpha);
    if (clip(img, ND_RECT(x0, y + 1, x1, y + 1), &q))
        span_blend(img, q.x0, q.x1, q.y0, ND_TH_CHROME_HI,
                   (uint8_t)(((uint32_t)alpha * 130u) / 255u));
}

void nd_theme_shadow_band(nd_image *img, int32_t x0, int32_t x1, int32_t y, int32_t height,
                          uint8_t alpha)
{
    int32_t i;

    if (img == NULL || height <= 0)
        return;

    for (i = 0; i < height; i++) {
        nd_rect q;
        /* Quadratic falloff rather than linear: a linear fade over two or
         * three rows still has a visible last row, and squaring it puts that
         * row under the threshold where the panel notices it. */
        uint32_t a = ((uint32_t)alpha * (uint32_t)(height - i) * (uint32_t)(height - i)) /
                     ((uint32_t)height * (uint32_t)height);

        if (clip(img, ND_RECT(x0, y + i, x1, y + i), &q))
            span_blend(img, q.x0, q.x1, q.y0, ND_TH_BLUE_DEEP, (uint8_t)a);
    }
}

/* ------------------------------------------------------------------ *
 * Text
 * ------------------------------------------------------------------ */

void nd_theme_text(nd_draw *d, int32_t x, int32_t y, const char *utf8, const nd_font *f,
                   nd_color ink, nd_color shadow)
{
    if (d == NULL || utf8 == NULL || utf8[0] == '\0' || f == NULL)
        return;

    /* The shadow is the same glyphs one row down, drawn at reduced coverage by
     * blending the shadow colour toward what is already there. nd_draw_text
     * has no alpha, so the effect is carried by the colour: a dark shadow on
     * a dark ground is already most of the way to invisible, which is the
     * behaviour wanted. Sideways offsets were tried and read as a print
     * registration error; down is the only direction that reads as light from
     * above. */
    /* SKIPPED, not drawn in the ink colour, when the theme is flat: the
     * classic face is a pixel font at small sizes and a second pass one row
     * down thickens every stem enough to close the counters in "e" and "a".
     * It also halves the text cost of a frame, which the phone notices. */
    if (ND_TH_TYPE_SHADOW)
        (void)nd_draw_text(d, x, y + 1, utf8, f, shadow);
    (void)nd_draw_text(d, x, y, utf8, f, ink);
}

void nd_theme_text_light(nd_draw *d, int32_t x, int32_t y, const char *utf8, const nd_font *f)
{
    nd_theme_text(d, x, y, utf8, f, ND_TH_INK_LIGHT, ND_TH_TEXT_SHADOW);
}

void nd_theme_text_sel(nd_draw *d, int32_t x, int32_t y, const char *utf8, const nd_font *f)
{
    nd_theme_text(d, x, y, utf8, f, ND_TH_SEL_INK, ND_TH_TEXT_SHADOW);
}

void nd_theme_text_bar(nd_draw *d, int32_t x, int32_t y, const char *utf8, const nd_font *f)
{
    nd_theme_text(d, x, y, utf8, f, ND_TH_BAR_INK, ND_TH_TEXT_SHADOW);
}

void nd_theme_text_dark(nd_draw *d, int32_t x, int32_t y, const char *utf8, const nd_font *f)
{
    /* The sheen goes UNDER the ink and one row DOWN -- letterpress, not a
     * drop shadow: on a light plate the engraved reading comes from a
     * highlight below the stroke, which is the opposite of what light type
     * over a photograph wants. */
    nd_theme_text(d, x, y, utf8, f, ND_TH_INK_DARK, ND_TH_TEXT_SHEEN);
}

/* ------------------------------------------------------------------ *
 * Composed parts
 * ------------------------------------------------------------------ */

void nd_theme_scrollbar(nd_image *img, int32_t x, int32_t top, int32_t bottom, size_t pos,
                        size_t count)
{
    nd_rect track;
    int32_t track_h;
    int32_t thumb_h;
    int32_t thumb_y;
    nd_theme_plate thumb;

    if (img == NULL || bottom < top)
        return;

    /* The track: 5 px wide, centred on the column the widget nominated, so
     * every layout that computed a position around the old 1 px line still
     * works without moving. Recessed rather than raised -- a dark translucent
     * groove with a light hairline down its right edge. */
    track = ND_RECT(x - 2, top, x + 2, bottom);
    nd_theme_round_fill(img, track, 2, ND_TH_BLUE_DEEP, 90u);
    nd_theme_round_outline(img, track, 2, ND_TH_CHROME_HI, 60u);

    track_h = bottom - top + 1;
    if (track_h < 6)
        return;

    /* Proportional, with a floor: a 40-item list would otherwise get a
     * two-pixel thumb that reads as dirt on the glass. */
    if (count > 1u) {
        thumb_h = (int32_t)((size_t)track_h / count);
        if (thumb_h < 10)
            thumb_h = 10;
        if (thumb_h > track_h)
            thumb_h = track_h;
        {
            /* Truncating, like every other notch in this project -- see
             * nd_widgets.h rule 3. The travel is what is left of the track
             * once the thumb's own height is taken out of it. */
            double step = (double)(track_h - thumb_h) / (double)(count - 1u);

            thumb_y = top + nd_trunc32((double)pos * step);
        }
    } else {
        thumb_h = track_h;
        thumb_y = top;
    }

    thumb = nd_theme_plate_blue(2);
    thumb.drop_shadow = false;
    thumb.top = ND_TH_BLUE_HI;
    thumb.bot = ND_TH_BLUE_MID;
    nd_theme_plate_draw(img, ND_RECT(x - 2, thumb_y, x + 2, thumb_y + thumb_h - 1), &thumb);
}

void nd_theme_panel(nd_image *img, nd_rect r, int32_t radius)
{
    nd_theme_plate p = nd_theme_plate_glass(radius);

    nd_theme_plate_draw(img, r, &p);

    /* A second, tighter hairline just inside the border: the bezel. It is
     * what makes the panel read as a pane of glass in a frame rather than a
     * painted rectangle, and it costs one outline. */
    if (r.x1 - r.x0 > 4 && r.y1 - r.y0 > 4)
        nd_theme_round_outline(img, ND_RECT(r.x0 + 1, r.y0 + 1, r.x1 - 1, r.y1 - 1),
                               nd_max32(0, radius - 1), ND_TH_CHROME_HI, 90u);
}

int32_t nd_theme_titlebar(nd_image *img, nd_draw *d, int32_t w, int32_t bar_h, const char *title,
                          const nd_font *title_font, const char *badge, const nd_font *badge_font)
{
    nd_theme_plate p = nd_theme_plate_bar(0);
    nd_rect bar = ND_RECT(0, 0, w - 1, bar_h - 1);

    if (img == NULL || bar_h <= 0)
        return bar_h > 0 ? bar_h : 0;

    /* Square, and flush to three edges. A bar with rounded top corners would
     * show the wallpaper in two notches at the very top of the screen, which
     * reads as a rendering fault rather than a design -- the plate is the top
     * of the phone, not a card floating on it. */
    p.drop_shadow = false;
    p.radius = 0;
    nd_theme_plate_draw(img, bar, &p);

    /* The shadow it casts DOWN onto the content, which is what makes the bar
     * sit in front rather than beside. Three rows: at two the content still
     * looks pasted on, at four the band itself becomes the thing you see. */
    if (ND_TH_PLATE_SHADOW)
        nd_theme_shadow_band(img, 0, w - 1, bar_h, 3, 130u);

    if (d != NULL && title != NULL && title[0] != '\0' && title_font != NULL) {
        int32_t th = 0;

        /* Centred on the bar by the string's OWN ink height, per rule 2 in
         * nd_widgets.h -- a title of "Tones" and one of "Messages" have
         * different ink boxes and centring on the line height would sit them
         * on different rows. */
        nd_text_size(title_font, title, NULL, &th);
        nd_theme_text_bar(d, 8, (bar_h - th) / 2, title, title_font);
    }

    if (d != NULL && badge != NULL && badge[0] != '\0' && badge_font != NULL) {
        int32_t bw = 0;
        int32_t bh = 0;

        nd_text_size(badge_font, badge, &bw, &bh);
        nd_theme_text_bar(d, w - 6 - bw, (bar_h - bh) / 2, badge, badge_font);
    }

    /* +3 for the shadow band, so a caller laying out from the returned row
     * does not put its first line of text inside the shadow. */
    return bar_h + 3;
}

void nd_theme_reflection(nd_image *dst, const nd_image *src, int32_t x, int32_t y, int32_t height,
                         uint8_t alpha_top)
{
    /* The glossy floor an icon stands on. There is no floor in the classic
     * look; the icon simply ends where it ends. */
    if (!ND_TH_REFLECTION)
        return;

    int32_t row;

    if (dst == NULL || src == NULL || src->pixels == NULL || height <= 0)
        return;
    /* RGBA only. Every caller passes an app icon, which nd_ui_get_image
     * guarantees is RGBA8888, and reflecting an opaque photograph would put a
     * hard-edged rectangle under it rather than a reflection. */
    if (src->fmt != ND_PIXFMT_RGBA8888)
        return;
    if (height > src->h)
        height = src->h;

    for (row = 0; row < height; row++) {
        /* src->h - 1 - row: the source's LAST row lands nearest the icon, so
         * the reflection meets it edge to edge. */
        const uint8_t *sp = nd_img_px(src, 0, src->h - 1 - row);
        int32_t dy = y + row;
        int32_t col;
        uint8_t *dp;
        uint32_t fade;

        if (dy < 0 || dy >= dst->h)
            continue;

        /* Linear in the row, so the reflection is gone by the time it reaches
         * `height`. Squaring it looked more like glass and read as dirt at
         * this size -- 24 rows is not enough travel for a curve. */
        fade = ((uint32_t)alpha_top * (uint32_t)(height - row)) / (uint32_t)height;
        if (fade == 0u)
            continue;

        dp = nd_img_px(dst, 0, dy);
        for (col = 0; col < src->w; col++) {
            int32_t dx = x + col;
            uint8_t a = sp[col * 4 + 3];

            if (dx >= 0 && dx < dst->w && a != 0u) {
                nd_color c = ND_RGB(sp[col * 4], sp[col * 4 + 1], sp[col * 4 + 2]);

                blend_px(dp + (size_t)dx * dst->bpp, dst->fmt, c,
                         (uint8_t)(((uint32_t)a * fade + 127u) / 255u));
            }
        }
    }
}

void nd_theme_glow(nd_image *img, int32_t cx, int32_t cy, int32_t radius, nd_color c,
                   uint8_t alpha_centre)
{
    /* A lit halo behind an icon is the most Frutiger-Aero thing on the screen
     * and the most wrong on a flat one. */
    if (!ND_TH_ICON_GLOW)
        return;

    nd_rect q;
    int32_t r2;
    int32_t y;

    if (img == NULL || radius <= 0 || alpha_centre == 0u)
        return;
    if (!clip(img, ND_RECT(cx - radius, cy - radius, cx + radius, cy + radius), &q))
        return;

    r2 = radius * radius;
    for (y = q.y0; y <= q.y1; y++) {
        int32_t dy = y - cy;
        int32_t dy2 = dy * dy;
        uint8_t *p = nd_img_px(img, q.x0, y);
        int32_t x;

        for (x = q.x0; x <= q.x1; x++) {
            int32_t dx = x - cx;
            int32_t d2 = dx * dx + dy2;

            if (d2 < r2) {
                /* (1 - d^2/r^2), squared. In integer, and computed in 32 bits
                 * throughout: radius is at most half a panel here, so
                 * r2 <= 14400 and the product cannot overflow. */
                uint32_t t = (uint32_t)(r2 - d2);
                uint32_t f = (t * t) / ((uint32_t)r2 * (uint32_t)r2 / 255u + 1u);

                if (f > 255u)
                    f = 255u;
                blend_px(p, img->fmt, c, (uint8_t)((f * alpha_centre) / 255u));
            }
            p += img->bpp;
        }
    }
}

void nd_theme_scrim(nd_image *img, nd_rect paint, int32_t ramp_y0, int32_t ramp_y1,
                    uint8_t alpha_top, uint8_t alpha_bot)
{
    /* The readability wash under type on a wallpaper. A flat theme does not
     * need it -- its type sits on a solid background, not a photograph -- and
     * a grey band across a black screen is a smear with nothing behind it. */
    if (!ND_TH_SCRIM)
        return;

    /* Not black: a neutral wash over a blue-green sky greys it, and the whole
     * palette is trying to stay in one family. A very dark blue darkens
     * without desaturating. */
    nd_theme_gradient_fade_ramped(img, paint, ramp_y0, ramp_y1, ND_TH_SCRIM_INK, alpha_top,
                                  alpha_bot);
}

/* nd_theme.h -- the Frutiger Aero look, as eleven primitives and one palette.
 *
 * The OS used to be white ink on black: a 3310 in a 240x175 window. This is
 * the same OS wearing glass. Every screen is now built from glossy plates,
 * bevelled dividers and text that carries a shadow, and all of that comes
 * from here so that thirteen widgets cannot drift apart.
 *
 * ============ WHY THIS IS NOT IN nd_draw.h ============
 *
 * nd_draw.h is a port of seven Pillow primitives and its contract is
 * "identical pixels to the Python". Nothing in this file has a Pillow
 * equivalent, and changing nd_draw would put the port's guarantee at risk for
 * the sake of decoration. So the seven stay frozen and the look is a layer
 * ABOVE them: nd_theme calls nd_draw for text, and writes pixels itself for
 * everything with a gradient or an alpha in it.
 *
 * ============ THE FOUR IDEAS THE LOOK IS MADE OF ============
 *
 * 1. NOTHING IS FLAT. A surface is a vertical gradient, light at the top,
 *    because that is what a lit convex object does. ND_THEME_* pairs below
 *    are always given top-first for that reason.
 *
 * 2. THE TOP HALF IS GLASS. iOS 6's signature is a white sheen filling the
 *    upper half of a control and stopping dead at the midpoint. It is not a
 *    smooth fade across the whole height; the hard edge is the effect.
 *
 * 3. EVERY EDGE IS TWO PIXELS. One dark line for the cut, one white line just
 *    inside it for the light catching the bevel. A single grey line reads as
 *    a scratch; the pair reads as depth.
 *
 * 4. TEXT CARRIES A SHADOW, ALWAYS, AND IT IS OFFSET DOWN, NEVER SIDEWAYS.
 *    Light ink gets a dark shadow below; dark ink on a light plate gets a
 *    white one. This is the whole reason white type stays legible over a
 *    photographic wallpaper, and it is why nd_theme_text() exists rather than
 *    a rule saying "call nd_draw_text twice".
 *
 * ============ ALPHA, AND WHY IT IS NOT nd_color's ============
 *
 * nd_color carries an alpha byte and nd_draw DROPS IT on the RGB canvas --
 * see nd_image.h. Reproducing that was correct for the port and is useless
 * here, so every function below that blends takes its coverage as an explicit
 * 0..255 argument and composites with nd_blend8(), the same measured formula
 * the glyph rasteriser uses. A translucent plate and an antialiased letter
 * therefore agree about what half-covered means.
 *
 * ============ NO ALLOCATION ============
 *
 * CODING-STANDARDS.md section 4 forbids it in the render path and every
 * function here honours it: the gradients and the rounded corners are
 * computed per scanline into automatic scalars, never a scratch surface.
 */

#ifndef ND_THEME_H_INCLUDED
#define ND_THEME_H_INCLUDED

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * The palette
 * ------------------------------------------------------------------ *
 *
 * Sampled from the icon set this theme was built around, so a drawn plate and
 * a shipped PNG sit in the same family rather than merely near each other.
 * Pairs are TOP first, BOTTOM second -- see idea 1.
 */

/* The signature blue. Title bars, the selection lozenge, the softkey. */
#define ND_TH_BLUE_HI   ND_RGB(0x5C, 0xC3, 0xF5) /* lit top edge      */
#define ND_TH_BLUE_TOP  ND_RGB(0x2A, 0x9B, 0xE8)
#define ND_TH_BLUE_MID  ND_RGB(0x0F, 0x6C, 0xC8) /* where the gloss stops */
#define ND_TH_BLUE_BOT  ND_RGB(0x0A, 0x4A, 0x9B)
#define ND_TH_BLUE_DEEP ND_RGB(0x06, 0x2E, 0x63) /* the cut under a plate */

/* Glass: the frosted panel content sits on. Light, cool, barely there. */
#define ND_TH_GLASS_TOP ND_RGB(0xF2, 0xF9, 0xFF)
#define ND_TH_GLASS_BOT ND_RGB(0xC6, 0xDF, 0xF2)

/* Chrome, for the bezel around a panel and the scrollbar track. */
#define ND_TH_CHROME_HI  ND_RGB(0xFF, 0xFF, 0xFF)
#define ND_TH_CHROME_TOP ND_RGB(0xDA, 0xE7, 0xF2)
#define ND_TH_CHROME_BOT ND_RGB(0x8E, 0xA8, 0xBE)

/* The sky. What a screen with no wallpaper stands on -- the OS used to fill
 * black there, and black is the one colour this theme has nothing to say in:
 * a glass plate over black reads as a grey box rather than a pane. */
#define ND_TH_SKY_TOP ND_RGB(0x9E, 0xDC, 0xF7)
#define ND_TH_SKY_BOT ND_RGB(0x14, 0x4E, 0x8F)

/* Aero green, for a confirmation and the battery when it is healthy. */
#define ND_TH_GREEN_TOP ND_RGB(0x9E, 0xE8, 0x4A)
#define ND_TH_GREEN_BOT ND_RGB(0x3D, 0x9A, 0x14)

/* Amber and red, for a warning and a fault. Same construction, other hues. */
#define ND_TH_AMBER_TOP ND_RGB(0xFF, 0xD9, 0x5C)
#define ND_TH_AMBER_BOT ND_RGB(0xD8, 0x88, 0x0A)
#define ND_TH_RED_TOP   ND_RGB(0xFF, 0x8A, 0x7A)
#define ND_TH_RED_BOT   ND_RGB(0xB4, 0x1C, 0x14)

/* Ink. Dark type on a light plate is navy rather than black, because pure
 * black against a blue-white gradient reads as a hole punched in it. */
#define ND_TH_INK_DARK  ND_RGB(0x0C, 0x2A, 0x47)
#define ND_TH_INK_LIGHT ND_RGB(0xFF, 0xFF, 0xFF)
#define ND_TH_INK_MUTED ND_RGB(0x5B, 0x7C, 0x99)

/* How hard the scrim leans on the picture, at the top of the region it is
 * painted over and at the bottom. 96 is where 20 px white type with its
 * shadow stays legible over the busiest shipped wallpaper -- measured with
 * nd-shoot against Classroom.gif, which is the brightest of the six. 0 at the
 * bottom is not an approximation: the scrim has to reach zero somewhere
 * inside the content area, or the softkey strip below shows a step where it
 * stopped.
 *
 * Lives here rather than in nd_ui.c because DetailPage needs it too -- it
 * paints its scrolling column into a scratch surface and has to apply the
 * same wash by hand, shifted into the column's own coordinates. Two files
 * agreeing by copying a number is how a seam appears halfway down a screen.
 */
#define ND_TH_SCRIM_TOP_A 96u
#define ND_TH_SCRIM_BOT_A 0u

/* How hard the app selector leans on its wallpaper. Lighter than a list
 * screen's, because the only things standing on it are a title plate, one
 * icon and a softkey -- and the icon is the point of the screen. */
#define ND_TH_APPSEL_SCRIM_A 70u

/* The shadow under light type, and the highlight under dark type. Coverage,
 * not colour: both are composited at ND_TH_SHADOW_A. */
#define ND_TH_SHADOW_A 150u
#define ND_TH_SHEEN_A  110u

/* ------------------------------------------------------------------ *
 * Fills
 * ------------------------------------------------------------------ */

/* A flat rectangle at `alpha` coverage. alpha 255 is an opaque fill and takes
 * the fast path; 0 draws nothing. Inclusive of both corners, like everything
 * else in this project. */
void nd_theme_fill(nd_image *img, nd_rect r, nd_color c, uint8_t alpha);

/* A vertical linear gradient, `top` at r.y0 and `bot` at r.y1 inclusive, at
 * a constant coverage. A one-row rectangle is `top` and does not divide by
 * zero. This is the workhorse: every plate in the OS is one of these plus
 * decoration. */
void nd_theme_gradient_v(nd_image *img, nd_rect r, nd_color top, nd_color bot, uint8_t alpha);

/* The same, but the coverage itself ramps from a_top to a_bot. Used for the
 * scrim that fades a wallpaper out behind the status bar, where a constant
 * alpha would leave a visible edge where it stopped. */
void nd_theme_gradient_fade(nd_image *img, nd_rect r, nd_color c, uint8_t a_top, uint8_t a_bot);

/* ============ RAMP DOMAIN vs PAINTED REGION ============
 *
 * The two above take one rectangle and use it for both: the gradient runs from
 * its top to its bottom, and those are the rows that get painted. That is
 * wrong for a background, and the reason is nd_widgets.h rule 1.
 *
 * Most widgets clear rows 0..145 and leave the softkey strip alone; a few
 * clear 0..175. If the background's gradient is defined over whatever was
 * asked for, then row 145 is a different colour depending on which of those
 * two a screen did -- so the strip and the content above it stop matching,
 * and the seam moves around as you navigate. The fix is to define the ramp
 * over the PANEL and paint only the REGION.
 *
 * `paint` is what is written. `ramp_y0`/`ramp_y1` are where the gradient's
 * ends live, in the same coordinate space, and may lie outside `paint`
 * entirely. */
void nd_theme_gradient_v_ramped(nd_image *img, nd_rect paint, int32_t ramp_y0, int32_t ramp_y1,
                                nd_color top, nd_color bot, uint8_t alpha);
void nd_theme_gradient_fade_ramped(nd_image *img, nd_rect paint, int32_t ramp_y0, int32_t ramp_y1,
                                   nd_color c, uint8_t a_top, uint8_t a_bot);

/* ------------------------------------------------------------------ *
 * Rounded rectangles
 * ------------------------------------------------------------------ *
 *
 * The corners are ANTIALIASED, by area coverage against the quarter-circle,
 * and that is not a luxury at this size: a 4 px radius drawn by a hard
 * distance test has three visible steps in it and looks like a mistake rather
 * than a curve. Coverage is computed per pixel from the distance to the
 * corner centre, clamped to one pixel of feather.
 */

/* Fill, with `radius` corners, at `alpha`. radius <= 0 is a plain rectangle. */
void nd_theme_round_fill(nd_image *img, nd_rect r, int32_t radius, nd_color c, uint8_t alpha);

/* Gradient fill with rounded corners -- the shape everything visible is made
 * of. */
void nd_theme_round_gradient(nd_image *img, nd_rect r, int32_t radius, nd_color top, nd_color bot,
                             uint8_t alpha);

/* A 1 px rounded border, drawn ON the rectangle's own edge (not inside it,
 * unlike nd_draw_rect_outline -- a border that sat inside would leave the
 * gradient's own corner pixels showing outside the line). */
void nd_theme_round_outline(nd_image *img, nd_rect r, int32_t radius, nd_color c, uint8_t alpha);

/* ------------------------------------------------------------------ *
 * The glossy plate
 * ------------------------------------------------------------------ *
 *
 * This is the control the whole interface is made of: title bars, softkeys,
 * the selected row of a list, a dialog's button, the battery. One call so
 * that all of them are the same object at different sizes.
 *
 * Construction, in order, which is also why it cannot be assembled from the
 * pieces at each call site without somebody getting it wrong:
 *
 *   1. the body, a top-to-bottom gradient with rounded corners
 *   2. the sheen, white, filling the TOP HALF only, stopping dead (idea 2)
 *   3. the bevel, a white hairline just inside the top edge
 *   4. the border, one dark line around the whole shape
 */
typedef struct {
    nd_color top;    /* body gradient, top    */
    nd_color bot;    /* body gradient, bottom */
    nd_color border; /* the dark cut around it; pass with alpha via border_a */
    uint8_t border_a;
    uint8_t sheen_a;  /* 0 disables the glass highlight entirely */
    uint8_t body_a;   /* the plate's own coverage; 255 for opaque */
    int32_t radius;   /* corner radius in pixels */
    bool bevel;       /* the white hairline under the top edge   */
    bool drop_shadow; /* one soft dark row under the bottom edge */
} nd_theme_plate;

/* The three plates that cover almost every call site. Take one, adjust a
 * field, pass it: this is the house style rather than filling in nine fields
 * at each of forty call sites. */
nd_theme_plate nd_theme_plate_blue(int32_t radius);
nd_theme_plate nd_theme_plate_glass(int32_t radius);
nd_theme_plate nd_theme_plate_chrome(int32_t radius);

void nd_theme_plate_draw(nd_image *img, nd_rect r, const nd_theme_plate *p);

/* ------------------------------------------------------------------ *
 * Edges
 * ------------------------------------------------------------------ */

/* The two-pixel divider of idea 3: a dark row at y and a white row at y+1.
 * Every horizontal rule in the OS is one of these. */
void nd_theme_divider(nd_image *img, int32_t x0, int32_t x1, int32_t y, uint8_t alpha);

/* A soft dark band `height` rows tall fading downward from y -- the shadow a
 * title bar casts onto the content under it. Distinct from a divider: this is
 * depth, that is a cut. */
void nd_theme_shadow_band(nd_image *img, int32_t x0, int32_t x1, int32_t y, int32_t height,
                          uint8_t alpha);

/* ------------------------------------------------------------------ *
 * Text
 * ------------------------------------------------------------------ */

/* Text with the shadow of idea 4. `shadow` is composited at ND_TH_SHADOW_A
 * one pixel BELOW the glyph, then the ink is drawn on top.
 *
 * Both draws go through nd_draw_text(), so the ink is antialiased by FreeType
 * exactly as it always was -- the shadow is a second pass of the same glyphs,
 * not a blur, which is what keeps this affordable at 30 fps on a Cortex-A7. */
void nd_theme_text(nd_draw *d, int32_t x, int32_t y, const char *utf8, const nd_font *f,
                   nd_color ink, nd_color shadow);

/* White type over a photograph or a dark plate. The overwhelmingly common
 * case, so it gets a name rather than two colour arguments. */
void nd_theme_text_light(nd_draw *d, int32_t x, int32_t y, const char *utf8, const nd_font *f);

/* Navy type on a glass plate, with a white sheen under it -- the letterpress
 * effect. Reads as engraved rather than printed, which is the whole of iOS 6's
 * type treatment on a light surface. */
void nd_theme_text_dark(nd_draw *d, int32_t x, int32_t y, const char *utf8, const nd_font *f);

/* ------------------------------------------------------------------ *
 * Composed parts
 * ------------------------------------------------------------------ */

/* The scrollbar: a recessed translucent track with a glossy blue thumb.
 * `pos` is 0-based and `count` is the number of stops; count <= 1 parks the
 * thumb at the top. The thumb is sized to the list rather than fixed, so a
 * long list reads as long.
 *
 * The old track was a 1 px grey line with a 7 px notch on it. That geometry
 * is kept -- x is the track's centre column, and the widget's own
 * track_top/track_bottom still decide the extent -- so nothing that computed
 * a layout around it has to move. */
void nd_theme_scrollbar(nd_image *img, int32_t x, int32_t top, int32_t bottom, size_t pos,
                        size_t count);

/* The frosted panel content sits on: a glass plate with a chrome bezel and a
 * drop shadow. One call, because a panel that is a plate here and a plate
 * plus an outline there is how two screens stop matching. */
void nd_theme_panel(nd_image *img, nd_rect r, int32_t radius);

/* The title bar: a glossy plate across the top of the screen carrying the
 * screen's name on the left and an optional badge (the "1-4" breadcrumb, a
 * page number) on the right.
 *
 * Eight widgets draw one and they used to draw it eight times: a 24 px string
 * at y=0, a right-aligned counter at y=5, a one-pixel white rule at y=30. The
 * plate has more parts than that -- a gradient, a sheen, a bevel, a shadow
 * onto the content below -- and eight copies of it would have drifted within
 * a week.
 *
 * `title` is drawn as given: trimming it against the badge is the caller's
 * job, because only the caller knows whether the right answer is to ellipsize
 * or to step down a font size (nd_text.h). Returns the y of the first content
 * row below the bar, which is what every caller then lays out against.
 *
 * `d` must already be bound to `img`. */
int32_t nd_theme_titlebar(nd_image *img, nd_draw *d, int32_t w, int32_t bar_h, const char *title,
                          const nd_font *title_font, const char *badge, const nd_font *badge_font);

/* THE REFLECTION. An icon standing on a glossy floor, which is the single
 * most recognisable thing about this whole visual period -- every Web 2.0
 * logo and every Frutiger Aero dock had one.
 *
 * `src`'s rows are read BOTTOM-UP into `height` rows starting at (x, y), at a
 * coverage that ramps from alpha_top down to zero, multiplied by the icon's
 * own alpha so a transparent corner stays transparent in the reflection too.
 *
 * Bottom-up rather than nd_image_flip_h plus a transpose because there is no
 * vertical flip in nd_image.h -- deliberately, per its header -- and adding
 * one to get a decoration would mean a scratch surface in the render path.
 * Reading the source backwards costs nothing and allocates nothing. */
void nd_theme_reflection(nd_image *dst, const nd_image *src, int32_t x, int32_t y, int32_t height,
                         uint8_t alpha_top);

/* A soft radial glow centred on (cx, cy). What lifts an icon off the
 * wallpaper: without it a 72 px picture on a photograph is a sticker, and
 * with it the icon looks lit.
 *
 * Falloff is quadratic in the radius, which is close enough to the Gaussian
 * anyone would reach for and costs one multiply per pixel. */
void nd_theme_glow(nd_image *img, int32_t cx, int32_t cy, int32_t radius, nd_color c,
                   uint8_t alpha_centre);

/* A readability wash. Over a bright photographic wallpaper white type does
 * not survive on its own, and dimming the whole picture (which is what the OS
 * used to do, at 30% brightness) throws away the reason for having a
 * photograph. This darkens only the rows given.
 *
 * RAMPED OVER ramp_y0..ramp_y1, not over `paint` -- see the note above; the
 * background is the case that note exists for. */
void nd_theme_scrim(nd_image *img, nd_rect paint, int32_t ramp_y0, int32_t ramp_y1,
                    uint8_t alpha_top, uint8_t alpha_bot);

#ifdef __cplusplus
}
#endif

#endif /* ND_THEME_H_INCLUDED */

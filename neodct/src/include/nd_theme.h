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
#include "nd_paths.h"
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
 *
 * ============ WHY THESE ARE NO LONGER #defines ============
 *
 * They were twenty-six constants and the look was therefore a property of the
 * BINARY: changing a colour meant a cross build, an update package and a
 * reflash. A theme the owner installs cannot work that way, so the constants
 * became FIELDS of a struct that is loaded at startup and can be replaced
 * while the phone is running.
 *
 * The names did not change, and that is the point. `ND_TH_BLUE_TOP` still
 * spells the same thing at all twenty-seven call sites across the OS; it now
 * reads the active palette instead of a literal the compiler folded in. So a
 * widget written against the old header keeps working, an app compiled last
 * month keeps working, and nothing had to be touched to make the whole
 * interface themeable. The alternative -- passing a palette pointer into
 * every draw function -- would have been a hundred-file diff for the same
 * pixels.
 *
 * WHAT THIS COSTS: one load from a global pointer where there used to be an
 * immediate. It is a macro over an extern pointer rather than a function call
 * precisely because some of these are read PER PIXEL -- the sheen loop in
 * nd_theme_plate_draw() is the hot one -- and a call there would be felt on a
 * Cortex-A7. Where a colour is read inside a pixel loop this file hoists it
 * into a local first; that is a rule for new code in here, not an accident.
 *
 * WHAT THIS DOES NOT CHANGE: the default values below are byte-for-byte the
 * ones that were compiled in, so a phone with no theme installed renders the
 * frames it always did. The golden set is the proof and it did not move.
 */

/* Every colour the interface is made of, in one replaceable object.
 *
 * Field order is the order the old #defines were written in, because
 * nd_theme.c's parser walks a table keyed by these names and a reader
 * comparing the two should not have to hunt. */
typedef struct {
    /* ============ THREE ROLES THAT WERE ONE COLOUR ============
     *
     * blue_* used to paint the title bar, the softkey strip AND the selected
     * row, because in a glass theme all three are the same object at
     * different sizes. They are not the same object in a FLAT theme: the
     * classic look has white type on a black bar and inverts to black type
     * on a white lozenge when a row is selected, so a palette that spells
     * them with one colour cannot say it.
     *
     * So the bars have their own pair and the selection has its own ink.
     * Frutiger Aero sets bar_* to the blues and sel_ink to white, which is
     * what it always drew; the classic look sets bar_* to the background and
     * sel_ink to black, and the same widgets draw both. */

    /* The signature colour. The selection lozenge, a progress fill, an
     * accent. */
    nd_color blue_hi;   /* lit top edge          */
    nd_color blue_top;
    nd_color blue_mid;  /* where the gloss stops */
    nd_color blue_bot;
    nd_color blue_deep; /* the cut under a plate */

    /* Glass: the frosted panel content sits on. Light, cool, barely there. */
    nd_color glass_top;
    nd_color glass_bot;

    /* Chrome, for the bezel around a panel and the scrollbar track. */
    nd_color chrome_hi;
    nd_color chrome_top;
    nd_color chrome_bot;

    /* The sky. What a screen with no wallpaper stands on -- the OS used to
     * fill black there, and black is the one colour this theme has nothing to
     * say in: a glass plate over black reads as a grey box rather than a
     * pane. */
    nd_color sky_top;
    nd_color sky_bot;

    /* Aero green, for a confirmation and the battery when it is healthy. */
    nd_color green_top;
    nd_color green_bot;

    /* Amber and red, for a warning and a fault. Same construction, other
     * hues. */
    nd_color amber_top;
    nd_color amber_bot;
    nd_color red_top;
    nd_color red_bot;

    /* The title bar and the softkey strip. */
    nd_color bar_top;
    nd_color bar_bot;
    nd_color bar_ink;

    /* Type standing on the signature colour -- the selected row's label.
     * White in a glass theme, black in an inverting one. */
    nd_color sel_ink;

    /* A warning IN TYPE -- the home screen's "Eng. Mode" line, and anything
     * else ui_home.json marks "red".
     *
     * Its own field rather than red_bot, which it briefly shared. Those are
     * two unrelated jobs: red_top/red_bot are the gradient of a fault PLATE
     * (and of Snake's food pellet), while this is a colour a string is drawn
     * in and has to be legible against the background rather than pretty
     * against a plate. Sharing them meant a theme could not make its warning
     * pure red without making its fault plates flat. */
    nd_color warn_ink;

    /* Ink. Dark type on a light plate is navy rather than black, because pure
     * black against a blue-white gradient reads as a hole punched in it. */
    nd_color ink_dark;
    nd_color ink_light;
    nd_color ink_muted;

    /* The two colours idea 4 is drawn with. They were literals inside
     * nd_theme_text_light() and _dark(), which made them the only part of the
     * look a theme could not reach -- and the shadow under white type is
     * exactly what a pink theme needs to restate, because navy under pink
     * reads as a bruise. */
    nd_color text_shadow; /* under light ink */
    nd_color text_sheen;  /* under dark ink  */

    /* What the readability wash is made of. A very dark blue darkens without
     * desaturating, which is the whole reason the scrim is not black -- and
     * it is a per-theme decision for the same reason: a pink interface washed
     * with navy reads as a bruise. */
    nd_color scrim_ink;

    /* How hard the scrim leans on the picture, at the top of the region it is
     * painted over and at the bottom. 96 is where 20 px white type with its
     * shadow stays legible over the busiest shipped wallpaper -- measured
     * with nd-shoot against Classroom.gif, which is the brightest of the six.
     * 0 at the bottom is not an approximation: the scrim has to reach zero
     * somewhere inside the content area, or the softkey strip below shows a
     * step where it stopped. */
    uint8_t scrim_top_a;
    uint8_t scrim_bot_a;

    /* How hard the app selector leans on its wallpaper. Lighter than a list
     * screen's, because the only things standing on it are a title plate, one
     * icon and a softkey -- and the icon is the point of the screen. */
    uint8_t appsel_scrim_a;

    /* The shadow under light type, and the highlight under dark type.
     * Coverage, not colour: both are composited at shadow_a. */
    uint8_t shadow_a;
    uint8_t sheen_a;
} nd_theme_palette;

/* ============ STRUCTURE, WHICH IS NOT COLOUR ============
 *
 * A palette can make the interface pink. It cannot make it FLAT, and flat is
 * what the phone's own look is: the classic face is white type on black with
 * a one-pixel rule under the title, no gradient anywhere, square corners and
 * no shadow under anything. Recolouring a glossy plate to black leaves a
 * glossy black plate.
 *
 * So a theme carries structure as well, and these are the switches. Every one
 * of them turns something OFF, and the built-in has them all off: the stock
 * phone is the plain one, and Frutiger Aero is the theme that turns the
 * decoration on. That direction matters -- it means a theme file that says
 * nothing gets the honest, cheap look rather than accidentally inheriting
 * somebody else's gloss.
 *
 * They are read in the render path exactly like the colours, through the same
 * pointer, so nothing has to be passed down. */
typedef struct {
    bool gloss;         /* the white sheen filling a plate's top half   */
    bool bevel;         /* the white hairline just inside a top edge    */
    bool gradients;     /* off collapses every ramp to its top colour   */
    bool round;         /* off squares every corner                     */
    bool type_shadow;   /* the shadow under light type, sheen under dark */
    bool plate_shadow;  /* the soft band a plate casts onto what is below */
    bool bevel_divider; /* a dark line plus a white one, not just a rule */
    bool icon_glow;     /* the radial glow behind a selector icon       */
    bool reflection;    /* the icon standing on a glossy floor          */
    bool scrim;         /* the readability wash over a wallpaper        */

    /* The pixel face rather than the UI face. Not a decoration: the classic
     * look IS Nokia Cellphone FC, and drawing it in a rounded sans is the one
     * thing that would stop it reading as the phone it is imitating. A theme
     * that ships its own fonts/ui.ttf overrides this either way. */
    bool pixel_font;

    /* Colour in the games, and it is a STRUCTURAL switch rather than a
     * palette entry because monochrome and colour distinguish things
     * differently. Snake's food and its body are told apart by SHAPE when
     * there is one ink -- an outlined cell against a filled one, which is how
     * the phone has always drawn it -- and by HUE when there is a palette to
     * spend, red against green. A theme cannot express that by naming
     * colours: given one ink, a filled apple and a filled snake are the same
     * square.
     *
     * Off in the built-in, so the stock phone plays the game it always
     * played. */
    bool game_colour;

    /* How far the wallpaper is dimmed, 0-100, before anything is drawn on it.
     *
     * THE OTHER HALF OF THE SCRIM DECISION, and it has to move with it. A
     * glass theme darkens only the rows that hold type (nd_theme_scrim) and
     * can therefore leave the picture at 88 and let it actually look like a
     * photograph. A flat theme has no scrim -- white type sits straight on
     * the picture with nothing behind it -- so the only way to keep "Memory
     * card" legible is to dim the whole thing, which is what the phone did
     * before any of this and why 30 is the built-in.
     *
     * Shipping one number for both looks means one of them is wrong: at 88
     * with no scrim the menu is unreadable over a bright wallpaper, and at 30
     * with a scrim the photograph is a dark smudge for no reason. */
    uint8_t wallpaper_dim;

    /* The same question asked INSIDE an app, where there is far more type to
     * read than on the home screen, so the picture gives way further. 75 is
     * the phone's own number and 68 is the glass theme's.
     *
     * A DEFAULT and not a decree: system.ui.wpeverywhere_dim still overrides
     * it, so an owner who has tuned the background keeps their value when
     * they change theme. What a theme sets is what an owner who has never
     * touched it gets. */
    uint8_t app_wallpaper_dim;
} nd_theme_style;

/* The active structure, alongside the active palette and swapped with it. */
extern const nd_theme_style *nd_theme_style_of;

#define ND_TH_GLOSS         (nd_theme_style_of->gloss)
#define ND_TH_BEVEL         (nd_theme_style_of->bevel)
#define ND_TH_GRADIENTS     (nd_theme_style_of->gradients)
#define ND_TH_ROUND         (nd_theme_style_of->round)
#define ND_TH_TYPE_SHADOW   (nd_theme_style_of->type_shadow)
#define ND_TH_PLATE_SHADOW  (nd_theme_style_of->plate_shadow)
#define ND_TH_BEVEL_DIVIDER (nd_theme_style_of->bevel_divider)
#define ND_TH_ICON_GLOW     (nd_theme_style_of->icon_glow)
#define ND_TH_REFLECTION    (nd_theme_style_of->reflection)
#define ND_TH_SCRIM         (nd_theme_style_of->scrim)
#define ND_TH_PIXEL_FONT    (nd_theme_style_of->pixel_font)
#define ND_TH_GAME_COLOUR   (nd_theme_style_of->game_colour)
#define ND_TH_WALLPAPER_DIM (nd_theme_style_of->wallpaper_dim)
#define ND_TH_APP_WALLPAPER_DIM (nd_theme_style_of->app_wallpaper_dim)

/* The active palette. Never NULL -- it points at the built-in Frutiger Aero
 * values until nd_theme_load() replaces it, and back at them when a theme is
 * removed. Read it through the ND_TH_* names below rather than directly; the
 * indirection is what lets a future palette gain a field without every caller
 * learning about it.
 *
 * OWNED BY nd_theme.c. A caller must not free it and must not keep the
 * pointer across an nd_theme_apply(), which is why nothing in the tree stores
 * it in a struct. */
extern const nd_theme_palette *nd_theme_pal;

/* The built-in look, for a caller that needs the defaults regardless of what
 * is installed -- the theme picker's "Frutiger Aero" entry, and the reset
 * path when a theme file turns out to be unreadable. */
const nd_theme_palette *nd_theme_palette_builtin(void);
const nd_theme_style *nd_theme_style_builtin(void);

/* The signature blue. Title bars, the selection lozenge, the softkey. */
#define ND_TH_BLUE_HI   (nd_theme_pal->blue_hi)   /* lit top edge      */
#define ND_TH_BLUE_TOP  (nd_theme_pal->blue_top)
#define ND_TH_BLUE_MID  (nd_theme_pal->blue_mid)  /* where the gloss stops */
#define ND_TH_BLUE_BOT  (nd_theme_pal->blue_bot)
#define ND_TH_BLUE_DEEP (nd_theme_pal->blue_deep) /* the cut under a plate */

/* Glass: the frosted panel content sits on. Light, cool, barely there. */
#define ND_TH_GLASS_TOP (nd_theme_pal->glass_top)
#define ND_TH_GLASS_BOT (nd_theme_pal->glass_bot)

/* Chrome, for the bezel around a panel and the scrollbar track. */
#define ND_TH_CHROME_HI  (nd_theme_pal->chrome_hi)
#define ND_TH_CHROME_TOP (nd_theme_pal->chrome_top)
#define ND_TH_CHROME_BOT (nd_theme_pal->chrome_bot)

/* The sky. What a screen with no wallpaper stands on. */
#define ND_TH_SKY_TOP (nd_theme_pal->sky_top)
#define ND_TH_SKY_BOT (nd_theme_pal->sky_bot)

/* Aero green, for a confirmation and the battery when it is healthy. */
#define ND_TH_GREEN_TOP (nd_theme_pal->green_top)
#define ND_TH_GREEN_BOT (nd_theme_pal->green_bot)

/* Amber and red, for a warning and a fault. */
#define ND_TH_AMBER_TOP (nd_theme_pal->amber_top)
#define ND_TH_AMBER_BOT (nd_theme_pal->amber_bot)
#define ND_TH_RED_TOP   (nd_theme_pal->red_top)
#define ND_TH_RED_BOT   (nd_theme_pal->red_bot)

/* Ink. */
#define ND_TH_BAR_TOP (nd_theme_pal->bar_top)
#define ND_TH_BAR_BOT (nd_theme_pal->bar_bot)
#define ND_TH_BAR_INK (nd_theme_pal->bar_ink)
#define ND_TH_SEL_INK (nd_theme_pal->sel_ink)
#define ND_TH_WARN_INK (nd_theme_pal->warn_ink)

/* Ink. */
#define ND_TH_INK_DARK  (nd_theme_pal->ink_dark)
#define ND_TH_INK_LIGHT (nd_theme_pal->ink_light)
#define ND_TH_INK_MUTED (nd_theme_pal->ink_muted)

/* The two colours idea 4 is drawn with. */
#define ND_TH_TEXT_SHADOW (nd_theme_pal->text_shadow)
#define ND_TH_TEXT_SHEEN  (nd_theme_pal->text_sheen)
#define ND_TH_SCRIM_INK   (nd_theme_pal->scrim_ink)

/* The scrim, the selector's lighter scrim, and the two type coverages. */
#define ND_TH_SCRIM_TOP_A    (nd_theme_pal->scrim_top_a)
#define ND_TH_SCRIM_BOT_A    (nd_theme_pal->scrim_bot_a)
#define ND_TH_APPSEL_SCRIM_A (nd_theme_pal->appsel_scrim_a)
#define ND_TH_SHADOW_A       (nd_theme_pal->shadow_a)
#define ND_TH_SHEEN_A        (nd_theme_pal->sheen_a)

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

/* The title bar and the softkey strip. Its own constructor because those two
 * are the strips that FRAME the screen rather than things standing on it, and
 * a flat theme paints them in the background colour so that only the type and
 * the rule under it show. */
nd_theme_plate nd_theme_plate_bar(int32_t radius);
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

/* The y to hand a text call so that a string's INK sits centred in a band
 * `height` tall.
 *
 * nd_widgets.h rule 2 says centre by the ink extents, and the half of it that
 * is easy to forget is that nd_draw_text's y is the ASCENDER LINE: the ink
 * begins bbox.y0 rows lower. Centring the ink HEIGHT and passing that as the
 * y puts every string that far too low.
 *
 * It read as a slight offset while the phone had two faces with bearings of 2
 * and 4 rows. A theme may bring any face it likes, and one with a bearing of
 * 8 pushes a 30-row title bar's own title off the bottom of it -- so anything
 * centring text in a BAND, where there is an edge to clip against, uses this.
 * The forty-odd places that centre inside the open content area are left as
 * they are, deliberately; see the note in nd_softkey.c. */
int32_t nd_theme_ink_centre_y(const nd_font *f, const char *utf8, int32_t height);

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

/* Type standing ON the signature colour -- the selected row's label. White in
 * a glass theme and therefore identical to nd_theme_text_light() there; black
 * in a theme whose selection is a white lozenge, which is the whole reason it
 * is a separate call rather than a second argument nobody would pass. */
void nd_theme_text_sel(nd_draw *d, int32_t x, int32_t y, const char *utf8, const nd_font *f);

/* Type on a title bar or a softkey. Follows bar_ink for the same reason. */
void nd_theme_text_bar(nd_draw *d, int32_t x, int32_t y, const char *utf8, const nd_font *f);

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

/* ================================================================== *
 * Themes -- the look as an object the owner can install
 * ================================================================== *
 *
 * Everything above draws. This part decides WHAT it draws with, and it is the
 * whole of "the framework is patchable": a theme is a directory, the palette
 * above is loaded out of it, and the resources the OS opens by name are
 * looked for inside it first.
 *
 * ============ A THEME IS A DIRECTORY, NOT A FORMAT ============
 *
 *     <theme>/theme.json          the palette, and what else is present
 *     <theme>/fonts/ui.ttf        optional, replaces the UI face
 *     <theme>/fonts/ui-bold.ttf   optional
 *     <theme>/icons/<App>.png     optional, one per app directory NAME
 *     <theme>/img/...             optional, mirrors ui/resources/img
 *     <theme>/wallpaper.jpg       optional
 *     <theme>/preview.png         optional, what the picker shows
 *
 * EVERY PART IS OPTIONAL EXCEPT theme.json, and a missing part means "keep
 * what the system has". That is what makes a theme small: a recolour is nine
 * lines of JSON and nothing else, and it still themes an app installed from a
 * card last week, because the lookup is by name at open time rather than a
 * table baked at build time.
 *
 * ============ WHERE THEY LIVE ============
 *
 * Built-in themes are under ND_PATH_THEMES_DIR on the read-only image.
 * Installed ones are under ND_PATH_USER_THEMES_DIR on the card, beside apps/
 * -- the same partition, the same removability, and the same reasoning as
 * nd_paths.h gives for apps living there rather than on the user partition.
 *
 * A theme is DATA and never code: no .so, nothing executed, nothing that
 * cares which arch the phone is. That is why installing one needs no
 * confinement argument of its own, and why a .nap holding a theme carries no
 * "arch" -- see nd_nap.h.
 *
 * ============ AND WHY THE ACTIVE ONE IS A PROCESS-LOCAL ============
 *
 * Process-per-app: the core and every running app each hold their own copy of
 * the palette, loaded by nd_ui_init() from the same setting. There is no
 * shared page and no IPC, because there does not need to be -- a theme change
 * is rare, the setting is the single source of truth, and an app that started
 * before the change reads the old value until it exits. The alternative, a
 * signal to every process, buys a repaint of screens nobody is looking at.
 *
 * The one process where it must change WITHOUT a restart is the one doing the
 * changing, which is why nd_theme_apply() exists at all: the picker previews
 * a theme by applying it and drawing a frame.
 */

#define ND_THEME_ID_MAX      32
#define ND_THEME_NAME_MAX    48
#define ND_THEME_AUTHOR_MAX  48
#define ND_THEME_VERSION_MAX 24
#define ND_THEME_DESC_MAX    256

/* Enough for the built-in plus everything a card can sensibly hold. The
 * selector is a list a person scrolls with two keys; a phone with thirty-two
 * themes on it has a different problem. */
#define ND_THEME_MAX_FOUND 32

/* The built-in look's id. Not a directory -- there is no theme.json for it,
 * because its values are the compiled-in defaults and a file that merely
 * restated them would be a second place to get them wrong.
 *
 * "classic", because the built-in IS the phone's own face: white type on
 * black in the pixel typeface. It was briefly "aero" while the glass look was
 * compiled in, which then collided with the theme file that took the glass
 * look over -- the id a theme claims and the id the built-in answers to are
 * one namespace, and the built-in wins, so the collision presented as the
 * Frutiger Aero theme silently never loading. */
#define ND_THEME_ID_BUILTIN   "classic"
#define ND_THEME_NAME_BUILTIN "Classic"

/* What theme.json is called inside a theme directory. */
#define ND_THEME_MANIFEST "theme.json"
#define ND_THEME_PREVIEW  "preview.png"

/* One theme, as read off the disk. Flat and copyable: the picker holds an
 * array of these and a preview must not depend on a file still being open. */
typedef struct {
    char id[ND_THEME_ID_MAX];
    char name[ND_THEME_NAME_MAX];
    char author[ND_THEME_AUTHOR_MAX];
    char version[ND_THEME_VERSION_MAX];
    char desc[ND_THEME_DESC_MAX];

    /* The directory this was read from, ND_ROOT-relative like every other
     * path in the tree. Empty for the built-in, which has none. */
    char dir[ND_PATH_MAX];

    bool builtin;

    /* The palette and the structure, both fully resolved: every field the
     * file did not mention has already been filled in from the built-in, so a
     * caller never has to ask whether something was specified. */
    nd_theme_palette palette;
    nd_theme_style style;

    /* Which optional parts this theme actually ships. Recorded at read time
     * so the picker can say "icons and a wallpaper" without stat()ing seven
     * paths per keypress. */
    bool has_font;
    bool has_font_bold;
    bool has_icons;
    bool has_img;
    bool has_wallpaper;
    bool has_preview;
} nd_theme_info;

/* Reads <dir>/theme.json. Fields the file omits keep their built-in values,
 * so a nine-line theme is legal and a malformed one is REJECTED rather than
 * half-applied -- a palette with three of its colours parsed is worse than no
 * theme at all.
 *
 * ND_ERR_NOTFOUND when there is no theme.json, ND_ERR_INVAL when it is not
 * usable. `out` is untouched on failure. */
nd_err nd_theme_read(const char *dir, nd_theme_info *out);

/* Every theme the phone can offer, built-in first and then the card, sorted
 * by name within each. Returns how many were written, at most `max`.
 *
 * A directory that does not parse is SKIPPED with a log line rather than
 * failing the walk: one bad theme on a card must not cost the owner the
 * picker. */
size_t nd_theme_list(nd_theme_info *out, size_t max);

/* Finds one by id. False when no such theme is installed. */
bool nd_theme_find(const char *id, nd_theme_info *out);

/* Makes `t` the active look OF THIS PROCESS, immediately: the palette pointer
 * above starts answering `t`'s values and every subsequent draw uses them.
 * Does NOT persist and does NOT reload fonts -- a caller that wants the new
 * face has to ask nd_ui for it, because fonts are the UI's to own.
 *
 * Copies what it needs; `t` may be a stack temporary. Passing NULL restores
 * the built-in. */
void nd_theme_apply(const nd_theme_info *t);

/* The active theme. Never NULL; the built-in until something replaces it. */
const nd_theme_info *nd_theme_active(void);

/* Reads the persisted choice (ND_SET_UI_THEME) and applies it. What every
 * process calls at startup -- the core through nd_ui_init(), an app through
 * the same path -- so that one setting is the only thing deciding the look.
 *
 * An id naming a theme that is no longer installed falls back to the built-in
 * rather than failing: a card pulled out must not leave the phone unable to
 * draw. */
void nd_theme_load_active(void);

/* Persists `id` as the choice and applies it here. The setting is written
 * FIRST, because a process that applied a theme it failed to record would
 * show the owner a change that vanishes at the next boot with nothing to say
 * why. */
nd_err nd_theme_select(const char *id);

/* ------------------------------------------------------------------ *
 * Resource override
 * ------------------------------------------------------------------ *
 *
 * The one call every resource open goes through. Given the SYSTEM path of a
 * resource -- the constant in nd_paths.h, or an app's icon.png -- it writes
 * the path that should actually be opened: the active theme's version if that
 * theme ships one, and otherwise the path it was given, unchanged.
 *
 * So a call site becomes one line longer and stops caring that themes exist:
 *
 *     char path[ND_PATH_MAX];
 *     nd_theme_resource(ND_PATH_UI_FONT, path, sizeof path);
 *
 * The mapping, which is deliberately small and by PREFIX rather than a table
 * of every file:
 *
 *   .../ui/resources/fonts/aero.ttf       -> <theme>/fonts/ui.ttf
 *   .../ui/resources/fonts/aero-bold.ttf  -> <theme>/fonts/ui-bold.ttf
 *   .../ui/resources/img/<rest>           -> <theme>/img/<rest>
 *   <anything>/apps/<Name>/icon.png       -> <theme>/icons/<Name>.png
 *
 * The icon rule is by shape and not by root on purpose: it catches
 * /NeoDCT/System/apps, the engineering apps, AND the card's apps, so a theme
 * covers an app the owner installed without knowing it exists.
 *
 * Returns true when the path was overridden. The output is always a usable
 * path -- on any failure it is the input -- so a caller that ignores the
 * return value is still correct. */
bool nd_theme_resource(const char *system_path, char *out, size_t out_sz);

/* The active theme's wallpaper, if it ships one. False leaves `out` empty.
 *
 * nd_theme_select() writes this into the wallpaper setting, so choosing a
 * theme DOES replace the background -- but only when the theme ships one, so
 * a pure recolour leaves the owner's photograph alone. The reasoning, and
 * what it costs, is at the call site in nd_themeload.c. */
bool nd_theme_wallpaper(char *out, size_t out_sz);

/* <dir>/preview.png, for the picker. False when the theme has none. */
bool nd_theme_preview_path(const nd_theme_info *t, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* ND_THEME_H_INCLUDED */

/*
 * NeoDCT browser chrome: the phone's theme, read by the browser.
 *
 * The phone's look is data -- a theme is a folder with a theme.json of
 * colours and structural switches, see nd_theme.h in neodct/src -- and
 * the core tells every app which one is on in NEODCT_UI_THEME. The
 * browser is netsurf, not a libneodct app, so it cannot call
 * nd_theme_load_active(); this is the same file read the same way,
 * for the subset of the look the browser's chrome is made of: the
 * bars, the ground, the selection, the rules and the panels.
 *
 * The built-in is the classic face, exactly the colours the chrome
 * was drawn in before it could be themed, so a phone with no theme
 * looks as it did. A field a theme leaves out keeps its built-in
 * value, as it does on the phone.
 *
 * Pure logic and plain file reads, no netsurf dependencies, unit
 * tested in test/. Colours are 0xRRGGBB.
 */

#ifndef NEODCT_THEME_H
#define NEODCT_THEME_H

#include <stdbool.h>
#include <stdint.h>

#define NEODCT_THEME_ID_MAX 64
#define NEODCT_THEME_PATH_MAX 512

/** the id the phone gives its own look; never a folder */
#define NEODCT_THEME_ID_BUILTIN "classic"

/** where the phone keeps its themes: in the image, then on the card */
#define NEODCT_THEME_DIR_SYSTEM "/NeoDCT/System/themes"
#define NEODCT_THEME_DIR_USER "/NeoDCT/User/sdcard/themes"

/** the core's word on which theme is on (nd_proc.h ND_ENV_UI_THEME) */
#define NEODCT_THEME_ENV "NEODCT_UI_THEME"

/** host development: a themes folder to search instead of the phone's */
#define NEODCT_THEME_DIR_ENV "NEODCT_THEMES_DIR"

/* The phone's faces, for a theme that brings none of its own */
#define NEODCT_FONT_PIXEL "/NeoDCT/System/ui/resources/fonts/font.ttf"
#define NEODCT_FONT_UI "/NeoDCT/System/ui/resources/fonts/aero.ttf"

struct neodct_theme {
	char id[NEODCT_THEME_ID_MAX];
	char dir[NEODCT_THEME_PATH_MAX]; /**< "" for the built-in */

	/* palette -- the same names as theme.json and nd_theme.h */
	uint32_t blue_hi, blue_top, blue_mid, blue_bot, blue_deep;
	uint32_t glass_top, glass_bot;
	uint32_t chrome_hi, chrome_top, chrome_bot;
	uint32_t sky_top, sky_bot;
	uint32_t bar_top, bar_bot, bar_ink;
	uint32_t sel_ink;
	uint32_t ink_dark, ink_light, ink_muted;
	uint32_t text_shadow, text_sheen;
	uint8_t shadow_a, sheen_a;

	/* structure */
	bool gloss, bevel, gradients, round;
	bool type_shadow, plate_shadow, bevel_divider;
	bool pixel_font;

	bool has_font; /**< the theme ships fonts/ui.ttf */
};

/** the classic face */
void neodct_theme_builtin(struct neodct_theme *t);

/**
 * Lay a theme.json's text over *t. Unknown keys are ignored and a
 * malformed value keeps what was there, so a half-written theme lands
 * on the built-in rather than on nonsense.
 *
 * \return the theme's "id", or NULL when the text has none
 */
const char *neodct_theme_parse(struct neodct_theme *t, const char *json,
			       char *id_buf, int id_sz);

/**
 * Load theme `id` from the folders in `roots` (NULL-terminated,
 * searched in order, first id wins -- the phone's rule, so a card
 * cannot shadow a system theme). NULL, "" or the built-in id, or a
 * theme that cannot be found or read, gives the built-in.
 *
 * \return true when a theme other than the built-in was loaded
 */
bool neodct_theme_load(struct neodct_theme *t, const char *id,
		       const char *const *roots);

/**
 * The face the chrome is drawn in: the theme's own, else the pixel
 * face or the UI face as the theme's style asks. Written as a path
 * into out; the caller checks it exists.
 */
void neodct_theme_font(const struct neodct_theme *t, char *out, int out_sz);

/* ------------------------------------------------------------------
 * the questions the renderer asks (nd_theme.c answers the same ones)
 */

/** a bar drawn in the ground's own colours is left out, not painted */
bool neodct_theme_bars_painted(const struct neodct_theme *t);
bool neodct_theme_panels_painted(const struct neodct_theme *t);

/** a + (b - a) * num / den, per channel; the ramp of a gradient */
uint32_t neodct_theme_lerp(uint32_t a, uint32_t b, int num, int den);

/** b composited over a at coverage alpha (0..255) */
uint32_t neodct_theme_blend(uint32_t a, uint32_t b, int alpha);

/** the process-wide theme, loaded once from the environment */
const struct neodct_theme *neodct_theme_active(void);

#endif

/*
 * Tests for neodct_theme_css: the theme as the stylesheet pages link.
 * The classic face must come out as the black-and-white the home page
 * always hard-coded, and a theme's colours must reach the classes.
 */

#include "test_util.h"
#include "../neodct_theme_css.h"

#define SHIPPED "../../../../../../neodct/overlay/NeoDCT/System/themes"

int main(void)
{
	static char css[NEODCT_THEME_CSS_MAX];
	struct neodct_theme t;
	const char *roots[] = { SHIPPED, NULL };
	size_t n;

	/* classic: black ground, white ink, the selection a white plate
	 * with black type, fields a white rule on black */
	neodct_theme_builtin(&t);
	n = neodct_theme_css(&t, css, sizeof(css));
	CHECK(n > 0 && n == strlen(css));
	CHECK(strstr(css, "html, body { background-color: #000000; color: #ffffff; }") != NULL);
	CHECK(strstr(css, ".nd-selected { background-color: #ffffff; color: #000000; }") != NULL);
	CHECK(strstr(css, ".nd-bar { background-color: #000000; color: #ffffff; border-bottom: 2px solid #ffffff; }") != NULL);
	CHECK(strstr(css, "border: 2px solid #ffffff; font-family: fantasy; }") != NULL);
	/* and netsurf's own pages are restyled, the heading a size down in
	 * the wide pixel face so "Error occurred fetching page" is one line */
	CHECK(strstr(css, "body.ns-border h1 { font-size: 12px !important;") != NULL);
	CHECK(strstr(css, "input.default-action") != NULL);
	/* nothing unfilled */
	CHECK(strstr(css, "%") == NULL || strstr(css, "100%") != NULL);

	/* Frutiger Aero: the middle of each ramp, painted bars and panels */
	CHECK(neodct_theme_load(&t, "aero", roots));
	n = neodct_theme_css(&t, css, sizeof(css));
	CHECK(n > 0);
	CHECK(strstr(css, "\"aero\"") != NULL);
	/* bar: middle of #2A9BE8..#0A4A9B, rule in blue_deep */
	CHECK(strstr(css, ".nd-bar { background-color: #1a73c2; color: #ffffff; border-bottom: 2px solid #062e63; }") != NULL);
	/* a theme with its own face keeps the full-size heading */
	CHECK(strstr(css, "body.ns-border h1 { font-size: 16px !important;") != NULL);
	/* panels are glass with the dark ink */
	CHECK(strstr(css, "color: #0c2a47; border: 2px solid #062e63; }") != NULL);

	/* too small a buffer is refused, not truncated */
	CHECK_INT(neodct_theme_css(&t, css, 64), 0);

	TEST_EXIT();
}

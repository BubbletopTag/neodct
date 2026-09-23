/*
 * Tests for neodct_theme: the browser reads the phone's theme.json the
 * way nd_themeload.c does -- built-in underneath, a theme's fields on
 * top, a malformed value ignored -- and finds a theme by its id.
 */

#include "test_util.h"
#include "../neodct_theme.h"

/* the themes shipped in the image, from this checkout */
#define SHIPPED "../../../../../../neodct/overlay/NeoDCT/System/themes"

int main(void)
{
	struct neodct_theme t;
	char id[NEODCT_THEME_ID_MAX];
	const char *roots[] = { SHIPPED, NULL };
	const char *nowhere[] = { "/nonexistent-neodct-themes", NULL };

	/* the built-in is the classic face the chrome was always drawn in */
	neodct_theme_builtin(&t);
	CHECK_STR(t.id, "classic");
	CHECK_INT(t.sky_top, 0x000000);
	CHECK_INT(t.bar_ink, 0xFFFFFF);
	CHECK_INT(t.blue_top, 0xFFFFFF);
	CHECK_INT(t.sel_ink, 0x000000);
	CHECK_INT(t.chrome_bot, 0x808080);
	CHECK(!t.gloss && !t.gradients && !t.round && t.pixel_font);
	/* ...whose bars and panels are the ground, so are not painted */
	CHECK(!neodct_theme_bars_painted(&t));
	CHECK(!neodct_theme_panels_painted(&t));

	/* a theme's fields land on top; what it leaves out stays */
	neodct_theme_builtin(&t);
	CHECK_STR(neodct_theme_parse(&t,
		"{ \"id\": \"pinkish\", \"description\": \"no \\\"gloss\\\": true here\",\n"
		"  \"style\": { \"gloss\": true, \"round\" : true, \"pixel_font\": false },\n"
		"  \"palette\": { \"bar_top\": \"#E84285\", \"sky_top\": \"#e4608e\",\n"
		"                 \"sel_ink\": \"#FFF\", \"blue_top\": \"E84285\",\n"
		"                 \"ink_dark\": \"#12345G\" },\n"
		"  \"alpha\": { \"shadow\": 300, \"sheen\": -4 } }",
		id, sizeof(id)), "pinkish");
	CHECK(t.gloss && t.round && !t.pixel_font);
	CHECK(!t.bevel); /* not mentioned: stays off */
	CHECK_INT(t.bar_top, 0xE84285);
	CHECK_INT(t.sky_top, 0xE4608E);
	CHECK_INT(t.bar_bot, 0x000000);
	/* malformed colours keep the built-in */
	CHECK_INT(t.sel_ink, 0x000000);
	CHECK_INT(t.blue_top, 0xFFFFFF);
	CHECK_INT(t.ink_dark, 0xFFFFFF);
	/* coverages clamp rather than refuse */
	CHECK_INT(t.shadow_a, 255);
	CHECK_INT(t.sheen_a, 0);
	CHECK(neodct_theme_bars_painted(&t));

	/* no id, no id */
	neodct_theme_builtin(&t);
	CHECK(neodct_theme_parse(&t, "{ \"palette\": {} }", id,
				 sizeof(id)) == NULL);

	/* colour arithmetic */
	CHECK_INT(neodct_theme_lerp(0x000000, 0xFFFFFF, 0, 10), 0x000000);
	CHECK_INT(neodct_theme_lerp(0x000000, 0xFFFFFF, 10, 10), 0xFFFFFF);
	CHECK_INT(neodct_theme_lerp(0x000000, 0xC8640A, 1, 2), 0x643205);
	CHECK_INT(neodct_theme_blend(0x102030, 0x102030, 99), 0x102030);

	/* loading by id: the built-in id, nothing, and an unknown id all
	 * give the classic face */
	CHECK(!neodct_theme_load(&t, NULL, roots));
	CHECK_STR(t.id, "classic");
	CHECK(!neodct_theme_load(&t, "classic", roots));
	CHECK(!neodct_theme_load(&t, "no-such-theme", roots));
	CHECK_STR(t.id, "classic");
	CHECK(!neodct_theme_load(&t, "aero", nowhere));
	CHECK_STR(t.id, "classic");

	/* the shipped themes, found by id rather than by folder name */
	CHECK(neodct_theme_load(&t, "aero", roots));
	CHECK_STR(t.id, "aero");
	CHECK(strstr(t.dir, "/FrutigerAero") != NULL);
	CHECK_INT(t.bar_top, 0x2A9BE8);
	CHECK_INT(t.sky_bot, 0x144E8F);
	CHECK(t.gloss && t.gradients && t.round && !t.pixel_font);
	CHECK(t.has_font);
	CHECK(neodct_theme_bars_painted(&t));
	CHECK(neodct_theme_panels_painted(&t));

	CHECK(neodct_theme_load(&t, "blossom", roots));
	CHECK_INT(t.bar_top, 0xE84285);
	CHECK_INT(t.text_shadow, 0x5C1535);

	/* the face: the theme's own, else the pixel or the UI face */
	{
		char font[NEODCT_THEME_PATH_MAX + 32];

		neodct_theme_font(&t, font, sizeof(font));
		CHECK(strstr(font, "/Blossom/fonts/ui.ttf") != NULL);

		neodct_theme_builtin(&t);
		neodct_theme_font(&t, font, sizeof(font));
		CHECK_STR(font, NEODCT_FONT_PIXEL);
		t.pixel_font = false;
		neodct_theme_font(&t, font, sizeof(font));
		CHECK_STR(font, NEODCT_FONT_UI);
	}

	TEST_EXIT();
}

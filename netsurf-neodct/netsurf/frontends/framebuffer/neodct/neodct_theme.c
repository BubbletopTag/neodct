/*
 * NeoDCT browser chrome: the phone's theme, read by the browser.
 */

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "neodct_theme.h"

/* A theme.json is a page of text; anything much larger is not one. */
#define THEME_JSON_MAX (64 * 1024)

void neodct_theme_builtin(struct neodct_theme *t)
{
	memset(t, 0, sizeof(*t));
	snprintf(t->id, sizeof(t->id), "%s", NEODCT_THEME_ID_BUILTIN);

	/* nd_themeload.c's builtin_palette, field for field: white type on
	 * black, a white rule, a white lozenge with black type for the
	 * selection. The glass is the ground, so a panel is a hollow rule. */
	t->blue_hi = t->blue_top = t->blue_mid = t->blue_bot =
		t->blue_deep = 0xFFFFFF;
	t->glass_top = t->glass_bot = 0x000000;
	t->chrome_hi = 0xFFFFFF;
	t->chrome_top = 0xFFFFFF;
	t->chrome_bot = 0x808080;
	t->sky_top = t->sky_bot = 0x000000;
	t->bar_top = t->bar_bot = 0x000000;
	t->bar_ink = 0xFFFFFF;
	t->sel_ink = 0x000000;
	t->ink_dark = 0xFFFFFF;
	t->ink_light = 0xFFFFFF;
	t->ink_muted = 0xA0A0A0;
	t->text_shadow = 0x000000;
	t->text_sheen = 0xFFFFFF;
	t->shadow_a = 150;
	t->sheen_a = 110;

	/* and nothing on but the pixel face */
	t->pixel_font = true;
}

/* ------------------------------------------------------------------
 * just enough JSON
 *
 * theme.json is flat: three objects of scalars under a few top-level
 * strings, and every key in it is unique across the whole file. So a
 * value is found by its quoted key rather than by walking the tree --
 * a parser here would be several hundred lines for the same answers.
 * A key is only a key when it is followed by a colon, which is what
 * stops "gloss" in a description's prose being read as the setting.
 */

static const char *find_value(const char *json, const char *key)
{
	char pat[64];
	const char *p = json;
	int n;

	n = snprintf(pat, sizeof(pat), "\"%s\"", key);
	if (n < 0 || n >= (int)sizeof(pat))
		return NULL;

	while ((p = strstr(p, pat)) != NULL) {
		const char *v = p + n;

		/* \"key\" inside a string value is escaped, not a key */
		if (p > json && p[-1] == '\\') {
			p = v;
			continue;
		}
		while (isspace((unsigned char)*v))
			v++;
		if (*v == ':') {
			v++;
			while (isspace((unsigned char)*v))
				v++;
			return v;
		}
		p = v;
	}
	return NULL;
}

static int hex_nib(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* "#RRGGBB" and nothing else, as on the phone */
static void read_colour(const char *json, const char *key, uint32_t *out)
{
	const char *v = find_value(json, key);
	uint32_t c = 0;
	int i;

	if (v == NULL || v[0] != '"' || v[1] != '#')
		return;
	/* the digits before the closing quote, so a short value stops at
	 * its own quote or the terminator rather than reading past it */
	for (i = 0; i < 6; i++) {
		int h = hex_nib(v[2 + i]);

		if (h < 0)
			return;
		c = (c << 4) | (uint32_t)h;
	}
	if (v[8] != '"')
		return;
	*out = c;
}

static void read_bool(const char *json, const char *key, bool *out)
{
	const char *v = find_value(json, key);

	if (v == NULL)
		return;
	if (strncmp(v, "true", 4) == 0)
		*out = true;
	else if (strncmp(v, "false", 5) == 0)
		*out = false;
}

/* a coverage, clamped rather than refused, as on the phone */
static void read_alpha(const char *json, const char *key, uint8_t *out)
{
	const char *v = find_value(json, key);
	long n;
	char *end;

	if (v == NULL)
		return;
	n = strtol(v, &end, 10);
	if (end == v)
		return;
	*out = (uint8_t)(n < 0 ? 0 : n > 255 ? 255 : n);
}

const char *neodct_theme_parse(struct neodct_theme *t, const char *json,
			       char *id_buf, int id_sz)
{
	const char *id = NULL;
	const char *v;

	read_colour(json, "blue_hi", &t->blue_hi);
	read_colour(json, "blue_top", &t->blue_top);
	read_colour(json, "blue_mid", &t->blue_mid);
	read_colour(json, "blue_bot", &t->blue_bot);
	read_colour(json, "blue_deep", &t->blue_deep);
	read_colour(json, "glass_top", &t->glass_top);
	read_colour(json, "glass_bot", &t->glass_bot);
	read_colour(json, "chrome_hi", &t->chrome_hi);
	read_colour(json, "chrome_top", &t->chrome_top);
	read_colour(json, "chrome_bot", &t->chrome_bot);
	read_colour(json, "sky_top", &t->sky_top);
	read_colour(json, "sky_bot", &t->sky_bot);
	read_colour(json, "bar_top", &t->bar_top);
	read_colour(json, "bar_bot", &t->bar_bot);
	read_colour(json, "bar_ink", &t->bar_ink);
	read_colour(json, "sel_ink", &t->sel_ink);
	read_colour(json, "ink_dark", &t->ink_dark);
	read_colour(json, "ink_light", &t->ink_light);
	read_colour(json, "ink_muted", &t->ink_muted);
	read_colour(json, "text_shadow", &t->text_shadow);
	read_colour(json, "text_sheen", &t->text_sheen);
	read_alpha(json, "shadow", &t->shadow_a);
	read_alpha(json, "sheen", &t->sheen_a);

	read_bool(json, "gloss", &t->gloss);
	read_bool(json, "bevel", &t->bevel);
	read_bool(json, "gradients", &t->gradients);
	read_bool(json, "round", &t->round);
	read_bool(json, "type_shadow", &t->type_shadow);
	read_bool(json, "plate_shadow", &t->plate_shadow);
	read_bool(json, "bevel_divider", &t->bevel_divider);
	read_bool(json, "pixel_font", &t->pixel_font);

	v = find_value(json, "id");
	if (v != NULL && *v == '"' && id_buf != NULL && id_sz > 0) {
		const char *end = strchr(v + 1, '"');
		int len;

		if (end != NULL && end > v + 1 && end - v - 1 < id_sz) {
			len = (int)(end - v - 1);
			memcpy(id_buf, v + 1, (size_t)len);
			id_buf[len] = '\0';
			if (memchr(id_buf, '\\', (size_t)len) == NULL)
				id = id_buf;
		}
	}
	return id;
}

/* owned by the caller; free() */
static char *read_file(const char *path)
{
	FILE *f = fopen(path, "r");
	char *buf;
	size_t n;

	if (f == NULL)
		return NULL;
	buf = malloc(THEME_JSON_MAX + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	n = fread(buf, 1, THEME_JSON_MAX, f);
	fclose(f);
	buf[n] = '\0';
	return buf;
}

static bool load_from(struct neodct_theme *t, const char *id,
		      const char *root)
{
	DIR *d = opendir(root);
	struct dirent *e;
	bool found = false;

	if (d == NULL)
		return false;

	while (!found && (e = readdir(d)) != NULL) {
		char dir[NEODCT_THEME_PATH_MAX];
		char path[NEODCT_THEME_PATH_MAX + 32];
		char got[NEODCT_THEME_ID_MAX];
		struct neodct_theme cand;
		char *json;
		int n;

		if (e->d_name[0] == '.')
			continue;
		n = snprintf(dir, sizeof(dir), "%s/%s", root, e->d_name);
		if (n < 0 || n >= (int)sizeof(dir))
			continue;
		snprintf(path, sizeof(path), "%s/theme.json", dir);
		json = read_file(path);
		if (json == NULL)
			continue;

		neodct_theme_builtin(&cand);
		if (neodct_theme_parse(&cand, json, got, sizeof(got)) != NULL &&
		    strcmp(got, id) == 0) {
			snprintf(cand.id, sizeof(cand.id), "%s", got);
			snprintf(cand.dir, sizeof(cand.dir), "%s", dir);
			snprintf(path, sizeof(path), "%s/fonts/ui.ttf", dir);
			cand.has_font = (access(path, R_OK) == 0);
			*t = cand;
			found = true;
		}
		free(json);
	}
	closedir(d);
	return found;
}

bool neodct_theme_load(struct neodct_theme *t, const char *id,
		       const char *const *roots)
{
	int i;

	neodct_theme_builtin(t);
	if (id == NULL || id[0] == '\0' ||
	    strcmp(id, NEODCT_THEME_ID_BUILTIN) == 0 || roots == NULL)
		return false;

	/* A theme on a card the browser cannot read, or one since
	 * removed, is the built-in -- the phone does the same, and a
	 * confined app like this one can only ever wear the themes that
	 * ship in the image. */
	for (i = 0; roots[i] != NULL; i++) {
		if (load_from(t, id, roots[i]))
			return true;
	}
	return false;
}

void neodct_theme_font(const struct neodct_theme *t, char *out, int out_sz)
{
	/* nd_ui.c's ui_font_paths(): the theme's own face wins, then the
	 * pixel face if the theme asks for it, else the UI face. The
	 * caller falls back to the pixel face if the UI face is missing. */
	if (t->has_font)
		snprintf(out, (size_t)out_sz, "%s/fonts/ui.ttf", t->dir);
	else if (t->pixel_font)
		snprintf(out, (size_t)out_sz, "%s", NEODCT_FONT_PIXEL);
	else
		snprintf(out, (size_t)out_sz, "%s", NEODCT_FONT_UI);
}

/* ------------------------------------------------------------------
 * colour arithmetic
 */

#define CH(c, s) ((int)(((c) >> (s)) & 0xFF))

/* nd_theme.c's tolerance: a theme may spell its bar and its sky a few
 * levels apart and still mean "no bar" */
static bool same_colour(uint32_t a, uint32_t b)
{
	int s;

	for (s = 0; s <= 16; s += 8) {
		int d = CH(a, s) - CH(b, s);

		if (d < -6 || d > 6)
			return false;
	}
	return true;
}

bool neodct_theme_bars_painted(const struct neodct_theme *t)
{
	return !same_colour(t->bar_top, t->sky_top) ||
		!same_colour(t->bar_bot, t->sky_bot);
}

bool neodct_theme_panels_painted(const struct neodct_theme *t)
{
	return !same_colour(t->glass_top, t->sky_top) ||
		!same_colour(t->glass_bot, t->sky_bot);
}

uint32_t neodct_theme_lerp(uint32_t a, uint32_t b, int num, int den)
{
	uint32_t out = 0;
	int s;

	if (den <= 0 || num <= 0)
		return a;
	if (num >= den)
		return b;
	for (s = 0; s <= 16; s += 8) {
		int ca = CH(a, s);
		int cb = CH(b, s);

		out |= (uint32_t)(ca + (cb - ca) * num / den) << s;
	}
	return out;
}

uint32_t neodct_theme_blend(uint32_t a, uint32_t b, int alpha)
{
	return neodct_theme_lerp(a, b, alpha, 255);
}

const struct neodct_theme *neodct_theme_active(void)
{
	static struct neodct_theme active;
	static bool loaded;

	if (!loaded) {
		const char *dev = getenv(NEODCT_THEME_DIR_ENV);
		const char *phone[] = { NEODCT_THEME_DIR_SYSTEM,
					NEODCT_THEME_DIR_USER, NULL };
		const char *host[] = { dev, NULL };

		neodct_theme_load(&active, getenv(NEODCT_THEME_ENV),
				  (dev != NULL && dev[0] != '\0') ? host : phone);
		loaded = true;
	}
	return &active;
}

/*
 * NeoDCT browser chrome: the phone's theme as a stylesheet.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "neodct_theme_css.h"

struct sink {
	char *buf;
	size_t sz;
	size_t len;
	bool full;
};

static void put(struct sink *s, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (s->full)
		return;
	va_start(ap, fmt);
	n = vsnprintf(s->buf + s->len, s->sz - s->len, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= s->sz - s->len) {
		s->full = true;
		return;
	}
	s->len += (size_t)n;
}

/* the colour at the middle of a ramp, which is what a gradient becomes
 * in a stylesheet that cannot draw one */
static unsigned mid(const struct neodct_theme *t, unsigned top, unsigned bot)
{
	return t->gradients ? neodct_theme_lerp(top, bot, 1, 2) : top;
}

size_t neodct_theme_css(const struct neodct_theme *t, char *out,
			size_t out_sz)
{
	struct sink s = { out, out_sz, 0, false };
	bool bars = neodct_theme_bars_painted(t);
	bool panels = neodct_theme_panels_painted(t);
	unsigned ground = mid(t, t->sky_top, t->sky_bot);
	unsigned ink = t->ink_light;
	unsigned bar = bars ? mid(t, t->bar_top, t->bar_bot) : ground;
	unsigned bar_rule = bars ? t->blue_deep : t->chrome_hi;
	unsigned panel = panels ? mid(t, t->glass_top, t->glass_bot) : ground;
	unsigned panel_ink = panels ? t->ink_dark : t->ink_light;
	unsigned panel_edge = panels ? t->blue_deep : t->chrome_hi;
	unsigned sel = mid(t, t->blue_top, t->blue_bot);
	unsigned rule = t->bevel_divider ? t->blue_deep : t->chrome_hi;

	if (out == NULL || out_sz == 0)
		return 0;
	out[0] = '\0';

	put(&s, "/* NeoDCT theme \"%s\", from the phone's theme.json */\n\n",
	    t->id);

	/* ---- the API ---------------------------------------------- */
	put(&s,
	    "html, body { background-color: #%06x; color: #%06x; }\n"
	    "body { font-family: fantasy; }\n"
	    "a { color: #%06x; }\n",
	    ground, ink, ink);
	put(&s,
	    ".nd-bar { background-color: #%06x; color: #%06x;"
	    " border-bottom: 2px solid #%06x; }\n",
	    bar, t->bar_ink, bar_rule);
	put(&s,
	    ".nd-panel, .nd-tile { background-color: #%06x; color: #%06x;"
	    " border: 2px solid #%06x; }\n"
	    ".nd-tile { text-decoration: none; }\n",
	    panel, panel_ink, panel_edge);
	put(&s,
	    ".nd-selected { background-color: #%06x; color: #%06x; }\n",
	    sel, t->sel_ink);
	put(&s,
	    ".nd-button, button, input[type=\"submit\"] {"
	    " background-color: #%06x; color: #%06x;"
	    " border: 2px solid #%06x; font-family: fantasy; }\n",
	    sel, t->sel_ink, t->blue_deep);
	put(&s,
	    ".nd-field, input[type=\"text\"], input[type=\"password\"] {"
	    " background-color: #%06x; color: #%06x;"
	    " border: 2px solid #%06x; font-family: fantasy; }\n",
	    panel, panel_ink, panel_edge);
	put(&s,
	    ".nd-muted { color: #%06x; }\n"
	    ".nd-ink { color: #%06x; }\n"
	    ".nd-rule { border-top: 2px solid #%06x; }\n",
	    t->ink_muted, ink, rule);

	/* ---- netsurf's own pages, as phone screens ---------------- */
	put(&s,
	    "\n/* netsurf's internal pages (internal.css imports this) */\n"
	    "html { padding: 0 !important;"
	    " background-color: #%06x !important; }\n"
	    "body.ns-border { border: none !important; margin: 0 !important;"
	    " max-width: none !important;"
	    " background-color: #%06x !important; color: #%06x !important;"
	    " font-family: fantasy !important; font-size: 12px !important; }\n",
	    ground, ground, ink);
	put(&s,
	    "body.ns-border h1 { font-size: 16px !important;"
	    " font-weight: normal !important; margin: 0 !important;"
	    " padding: 4px 6px !important;"
	    " background-color: #%06x !important; color: #%06x !important;"
	    " border-bottom: 2px solid #%06x !important; }\n",
	    bar, t->bar_ink, bar_rule);
	put(&s,
	    "body.ns-border form { padding: 4px 0 !important; }\n"
	    "body.ns-border p { margin: 4px 6px !important; }\n"
	    "body.ns-border div#buttons { text-align: center !important;"
	    " margin: 8px 0 !important; }\n"
	    "body.ns-border table { margin: 4px 0 !important;"
	    " width: 100%% !important; }\n"
	    "body.ns-border table input { width: 100%% !important;"
	    " margin: 0 !important; }\n");
	put(&s,
	    "body.ns-border input { font-family: fantasy !important;"
	    " font-size: 12px !important; margin: 0 3px !important;"
	    " padding: 2px 6px !important;"
	    " background-color: #%06x !important; color: #%06x !important;"
	    " border: 2px solid #%06x !important; }\n",
	    panel, panel_ink, panel_edge);
	put(&s,
	    "body.ns-border input.default-action {"
	    " background-color: #%06x !important; color: #%06x !important;"
	    " border-color: #%06x !important; }\n",
	    sel, t->sel_ink, t->blue_deep);

	return s.full ? 0 : s.len;
}

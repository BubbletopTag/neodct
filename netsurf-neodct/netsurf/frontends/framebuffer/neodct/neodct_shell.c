/*
 * NeoDCT browser chrome: fbtk shell.
 *
 * The overlay screens (Options menu, Go to URL, Input Text) replicate
 * the NeoDCT python framework widgets (VerticalList, TextInput,
 * TextInputLong, SoftKeyBar) pixel-for-pixel: same layout constants,
 * same font sizes, drawn imperatively in one full-screen widget.
 */

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include <libnsfb.h>
#include <libnsfb_event.h>
#include <libnsfb_plot.h>
#include <nsutils/time.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "content/llcache.h"
#include "netsurf/browser_window.h"
#include "netsurf/keypress.h"
#include "netsurf/layout.h"
#include "netsurf/plotters.h"
#include "desktop/browser_history.h"

#include "framebuffer/gui.h"
#include "framebuffer/fbtk.h"
#include "framebuffer/font.h"
#include "framebuffer/framebuffer.h"
#include "framebuffer/schedule.h"
#include "framebuffer/fbtk/widget.h"

#include "framebuffer/neodct/neodct_ui.h"
#include "framebuffer/neodct/neodct_history.h"
#include "framebuffer/neodct/neodct_theme.h"
#include "framebuffer/neodct/neodct_status.h"
#include "framebuffer/neodct/neodct_wrap.h"
#include "framebuffer/neodct/neodct_mem.h"
#include "framebuffer/neodct/neodct_media.h"
#include "framebuffer/neodct/neodct_shell.h"

/* framework.py layout constants (240x175) */
#define FRAME_SOFTKEY_H 30
#define FRAME_HEADER_Y 30       /* max(30, H * 0.11) */
#define BROWSER_APP_ID "11"

/* framework.py font pixel sizes */
#define FONT_S_PX 14
#define FONT_MD_PX 18
#define FONT_N_PX 20
#define FONT_XL_PX 24

/* the browse chrome's own type, the sizes fbtk's text widget used to
 * derive from the bars' heights */
#define URLBAR_FONT_PX 13
#define STATUS_FONT_PX 10

/* nd_vlist.c / nd_softkey.c: the corner of a selection lozenge and of
 * the softkey plate, when the theme rounds corners at all */
#define PLATE_RADIUS 6

#define MEM_LOG_INTERVAL_MS 5000
#define BLINK_INTERVAL_MS 500

/* In $HOME, which the Browser app points at /NeoDCT/User/browser: the
 * one directory the untrusted browser user may write. A dot file so
 * that a developer running netsurf on a desktop does not find it
 * littering their home directory. */
#define HISTORY_FILE ".neodct_history"

/* pixels to points at the toolkit's 90 DPI */
#define px_to_pt(x) (((x) * 72) / FBTK_DPI)

struct neodct_shell {
	struct gui_window *gw;
	struct neodct_ui ui;
	struct neodct_status status;
	const struct neodct_theme *theme;

	char cur_url[NEODCT_TEXT_MAX + 1];
	char cur_title[NEODCT_HISTORY_TITLE_MAX + 1];
	char homepage[NEODCT_TEXT_MAX + 1];
	/** homepage as the core spells it, for recognising it in set_url */
	char homepage_norm[NEODCT_TEXT_MAX + 1];

	struct neodct_history history;
	char history_path[NEODCT_TEXT_MAX + 1]; /**< "" when not persisted */

	/* browse-mode chrome */
	fbtk_widget_t *url_bar;
	fbtk_widget_t *status_bar;

	/* full-screen NeoDCT overlay (menu / url / input screens) */
	fbtk_widget_t *screen;

	bool blink;          /**< text cursor blink state */
	bool blink_running;  /**< blink timer scheduled */
	bool tick_scheduled;

	/* dev harness: scripted input from NEODCT_SCRIPT */
	char *script;
	char *script_pos;
};

/* single window on the phone */
static struct neodct_shell the_shell;

static struct neodct_shell *shell_of(struct gui_window *gw)
{
	(void)gw;
	return &the_shell;
}

static long now_ms(void)
{
	uint64_t ms;
	nsu_getmonotonic_ms(&ms);
	return (long)ms;
}

/* ------------------------------------------------------------------
 * drawing primitives over the raw framebuffer
 *
 * The chrome is drawn from the phone's theme (neodct_theme.h), with
 * the same construction nd_theme.c uses -- a plate is a gradient, a
 * sheen over its top half, a bevel and a border; type on a light
 * ground carries a shadow -- cut down to what these few screens need.
 * Every primitive draws exactly the old flat black and white when the
 * theme is the classic one, whose switches are all off.
 *
 * nd_theme.c blends against the pixels already there. Here the
 * colour underneath is always known (the ground's ramp, or the plate
 * being drawn on), so every blend is worked out as a colour first and
 * painted opaque: no read-back from the framebuffer, and no alpha
 * support needed from libnsfb's plotters.
 */

/* 0xRRGGBB to the toolkit's 0xAABBGGRR */
static colour fbc(uint32_t rgb)
{
	return 0xFF000000u | ((rgb & 0xFFu) << 16) | (rgb & 0xFF00u) |
		((rgb >> 16) & 0xFFu);
}

static void mkstyle(plot_font_style_t *fs, int px, uint32_t fg, uint32_t bg)
{
	memset(fs, 0, sizeof(*fs));
	/* the fantasy face carries the NeoDCT system font; sans-serif
	 * stays a regular web font for page content */
	fs->family = PLOT_FONT_FAMILY_FANTASY;
	fs->size = px_to_pt(px * PLOT_STYLE_SCALE);
	fs->weight = 400;
	fs->flags = FONTF_NONE;
	fs->foreground = fbc(fg);
	fs->background = fbc(bg);
}

static int text_width(const plot_font_style_t *fs, const char *s, size_t len)
{
	int w = 0;

	if (framebuffer_layout_table->width(fs, s, len, &w) != NSERROR_OK)
		return (int)len * 8;
	return w;
}

/* draw text with (x, y) as the glyph box top-left, like PIL draw.text */
static void draw_text_plain(struct redraw_context *ctx,
			    const plot_font_style_t *fs, int px,
			    int x, int y, const char *s)
{
	if (s == NULL || *s == '\0')
		return;
	/* baseline sits 3/4 down the font box (matches fbtk/text.c) */
	ctx->plot->text(ctx, fs, x, y + ((px * 3 + 2) / 4) + 1,
			s, strlen(s));
}

/* nd_theme_text(): the shadow one row below, then the ink. `bg` is
 * what the type stands on, which the shadow is composited over. */
static void draw_text(struct neodct_shell *sh, struct redraw_context *ctx,
		      int px, int x, int y, const char *s,
		      uint32_t ink, uint32_t shadow, uint32_t bg)
{
	const struct neodct_theme *th = sh->theme;
	plot_font_style_t fs;

	if (th->type_shadow) {
		mkstyle(&fs, px, neodct_theme_blend(bg, shadow, th->shadow_a),
			bg);
		draw_text_plain(ctx, &fs, px, x, y + 1, s);
	}
	mkstyle(&fs, px, ink, bg);
	draw_text_plain(ctx, &fs, px, x, y, s);
}

/* the half-open rectangle [x0,x1) x [y0,y1) */
static void fill_rect(nsfb_t *nsfb, int x0, int y0, int x1, int y1,
		      uint32_t rgb)
{
	nsfb_bbox_t box = { x0, y0, x1, y1 };

	if (x1 > x0 && y1 > y0)
		nsfb_plot_rectangle_fill(nsfb, &box, fbc(rgb));
}

/* A gradient's colour at row y of a ramp running ramp_y0..ramp_y1 --
 * the ramp is defined over the panel and only part of it painted, so
 * the softkey strip and the content above it agree where they meet
 * (nd_theme_gradient_v_ramped). Flat themes take the top colour. */
static uint32_t ramp(const struct neodct_theme *th, uint32_t top, uint32_t bot,
		     int y, int ramp_y0, int ramp_y1)
{
	if (!th->gradients)
		return top;
	if (y <= ramp_y0)
		return top;
	if (y >= ramp_y1)
		return bot;
	return neodct_theme_lerp(top, bot, y - ramp_y0, ramp_y1 - ramp_y0);
}

/* the ground: what a screen with no wallpaper stands on */
static uint32_t ground(const struct neodct_shell *sh, int y)
{
	int h = fbtk_get_height(sh->gw->window);

	return ramp(sh->theme, sh->theme->sky_top, sh->theme->sky_bot,
		    y, 0, h - 1);
}

static void fill_ground(struct neodct_shell *sh, nsfb_t *nsfb,
			int x0, int y0, int x1, int y1)
{
	int y;

	if (!sh->theme->gradients) {
		fill_rect(nsfb, x0, y0, x1, y1, sh->theme->sky_top);
		return;
	}
	for (y = y0; y < y1; y++)
		fill_rect(nsfb, x0, y, x1, y + 1, ground(sh, y));
}

/* How far a row `i` rows in from a rounded edge starts in from the
 * side: the quarter circle of radius r, sampled at the row's centre.
 * Integer so the chrome does not pull in libm for a corner. */
static int corner_inset(int r, int i)
{
	int k = 0;
	int d = 2 * r - 2 * i - 1; /* twice the distance to the centre row */

	if (i >= r || r <= 0)
		return 0;
	/* largest k with (2k)^2 + d^2 <= (2r)^2 */
	while (4 * (k + 1) * (k + 1) + d * d <= 4 * r * r)
		k++;
	return r - k;
}

/* nd_theme_plate, the fields this chrome uses */
struct plate {
	uint32_t top, bot;  /* body gradient */
	uint32_t border;
	int border_a;       /* 0: no border */
	int sheen_a;        /* 0: no gloss */
	bool bevel;
	bool painted;       /* false: the body is the ground, left out */
	int radius;
};

/* nd_theme_plate_blue(): the selection, and anything else in the
 * signature colour */
static struct plate plate_blue(const struct neodct_theme *th, int radius)
{
	struct plate p = { th->blue_top, th->blue_bot, th->blue_deep, 210,
			   th->gloss ? 90 : 0, th->bevel, true,
			   th->round ? radius : 0 };
	return p;
}

/* nd_theme_plate_bar(): the strips that frame the screen, not painted
 * when the theme's bar is its ground -- the classic face */
static struct plate plate_bar(const struct neodct_theme *th, int radius)
{
	struct plate p = { th->bar_top, th->bar_bot, th->blue_deep,
			   th->gradients ? 210 : 0, th->gloss ? 90 : 0,
			   th->bevel, neodct_theme_bars_painted(th),
			   th->round ? radius : 0 };
	return p;
}

/* nd_theme_plate_glass(): a text field. The phone composites it at
 * 216 over the ground; that is folded into the colours here. */
static struct plate plate_glass(struct neodct_shell *sh, int radius, int y0,
				int y1)
{
	const struct neodct_theme *th = sh->theme;
	struct plate p = { neodct_theme_blend(ground(sh, y0), th->glass_top, 216),
			   neodct_theme_blend(ground(sh, y1), th->glass_bot, 216),
			   th->blue_deep, 120, th->gloss ? 70 : 0, th->bevel,
			   neodct_theme_panels_painted(th),
			   th->round ? radius : 0 };
	return p;
}

/* nd_theme_plate_draw() on [x0,x1) x [y0,y1), over the ground */
static void plate_draw(struct neodct_shell *sh, nsfb_t *nsfb,
		       int x0, int y0, int x1, int y1, const struct plate *p)
{
	const struct neodct_theme *th = sh->theme;
	int h = y1 - y0;
	int mid = y0 + (h - 1) / 2;
	int r = p->radius;
	int y;

	if (x1 <= x0 || h <= 0)
		return;
	if (r > h / 2)
		r = h / 2;
	if (r > (x1 - x0) / 2)
		r = (x1 - x0) / 2;

	for (y = y0; y < y1; y++) {
		int from_edge = (y - y0 < y1 - 1 - y) ? y - y0 : y1 - 1 - y;
		int ins = corner_inset(r, from_edge);
		int lx = x0 + ins, rx = x1 - ins;
		uint32_t c;

		if (!p->painted) {
			if (p->border_a == 0)
				continue;
			c = ground(sh, y);
		} else {
			c = ramp(th, p->top, p->bot, y, y0, y1 - 1);
			/* the sheen: the top half only, stopping dead at the
			 * midpoint, fading to a third of itself on the way */
			if (p->sheen_a > 0 && y <= mid && mid > y0)
				c = neodct_theme_blend(c, th->chrome_hi,
					p->sheen_a - (p->sheen_a * 2 / 3) *
					(y - y0) / (mid - y0));
			/* the bevel: a hairline one row inside the top */
			if (p->bevel && y == y0 + 1)
				c = neodct_theme_blend(c, th->chrome_hi, 150);
			fill_rect(nsfb, lx, y, rx, y + 1, c);
		}

		/* the border last, over everything */
		if (p->border_a > 0) {
			uint32_t b = neodct_theme_blend(c, p->border,
							p->border_a);

			if (y == y0 || y == y1 - 1) {
				fill_rect(nsfb, lx, y, rx, y + 1, b);
			} else {
				fill_rect(nsfb, lx, y, lx + 1, y + 1, b);
				fill_rect(nsfb, rx - 1, y, rx, y + 1, b);
			}
		}
	}
}

/* the colour a plate's type stands on, for its shadow */
static uint32_t plate_mid(const struct neodct_shell *sh, const struct plate *p,
			  int y0, int y1)
{
	if (!p->painted)
		return ground(sh, (y0 + y1) / 2);
	return neodct_theme_lerp(p->top, p->bot, 1, 2);
}

/* The rule under a title on a screen whose bars are the ground: one
 * white row in the classic face, the cut-and-catch pair in a theme
 * that asks for it (nd_theme_divider). */
static void divider(struct neodct_shell *sh, nsfb_t *nsfb, int x0, int x1,
		    int y)
{
	const struct neodct_theme *th = sh->theme;

	if (!th->bevel_divider) {
		fill_rect(nsfb, x0, y, x1, y + 1, th->chrome_hi);
		return;
	}
	fill_rect(nsfb, x0, y, x1, y + 1, th->blue_deep);
	fill_rect(nsfb, x0, y + 1, x1, y + 2,
		  neodct_theme_blend(ground(sh, y + 1), th->chrome_hi, 130));
}

/* the soft band a plate casts onto the ground under it */
static void shadow_band(struct neodct_shell *sh, nsfb_t *nsfb, int x0, int x1,
			int y, int rows, int alpha)
{
	int i;

	if (!sh->theme->plate_shadow)
		return;
	for (i = 0; i < rows; i++) {
		int a = alpha * (rows - i) * (rows - i) / (rows * rows);

		fill_rect(nsfb, x0, y + i, x1, y + i + 1,
			  neodct_theme_blend(ground(sh, y + i),
					     sh->theme->blue_deep, a));
	}
}

/* ------------------------------------------------------------------
 * NeoDCT framework screens
 */

/* The title strip: a bar plate carrying the title and the badge when
 * the theme paints its bars, and otherwise the classic title on the
 * ground with a rule under it. title_y is where the classic face puts
 * the title, which differs between the list and the text screens. */
static void render_title(struct neodct_shell *sh, nsfb_t *nsfb,
			 struct redraw_context *ctx, const char *title,
			 int title_y, const char *badge)
{
	const struct neodct_theme *th = sh->theme;
	int w = fbtk_get_width(sh->gw->window);
	struct plate p = plate_bar(th, 0);
	uint32_t under = plate_mid(sh, &p, 0, FRAME_HEADER_Y);
	plot_font_style_t fs;

	if (p.painted) {
		/* square and flush, like nd_theme_titlebar(): the top of
		 * the phone, not a card floating on it */
		p.border_a = 0;
		plate_draw(sh, nsfb, 0, 0, w, FRAME_HEADER_Y, &p);
		shadow_band(sh, nsfb, 0, w, FRAME_HEADER_Y, 3, 130);
		title_y = (FRAME_HEADER_Y - FONT_XL_PX) / 2;
	} else {
		divider(sh, nsfb, 0, w, FRAME_HEADER_Y);
	}

	draw_text(sh, ctx, FONT_XL_PX, 5, title_y, title, th->bar_ink,
		  th->text_shadow, under);

	if (badge != NULL && badge[0] != '\0') {
		mkstyle(&fs, FONT_N_PX, th->bar_ink, under);
		draw_text(sh, ctx, FONT_N_PX,
			  w - 5 - text_width(&fs, badge, strlen(badge)), 5,
			  badge, th->bar_ink, th->text_shadow, under);
	}
}

/* SoftKeyBar.update(): the strip, and a centred font_n label on a bar
 * plate (nd_softkey.c) */
static void render_softkey(struct neodct_shell *sh, nsfb_t *nsfb,
			   struct redraw_context *ctx, const char *label)
{
	const struct neodct_theme *th = sh->theme;
	int w = fbtk_get_width(sh->gw->window);
	int h = fbtk_get_height(sh->gw->window);
	int y = h - FRAME_SOFTKEY_H;
	struct plate p = plate_bar(th, PLATE_RADIUS);
	plot_font_style_t fs;
	int tw;

	fill_ground(sh, nsfb, 0, y, w, h);
	plate_draw(sh, nsfb, 2, y + 2, w - 2, h - 2, &p);

	mkstyle(&fs, FONT_N_PX, th->bar_ink, th->bar_top);
	tw = text_width(&fs, label, strlen(label));
	draw_text(sh, ctx, FONT_N_PX, (w - tw) / 2,
		  y + (FRAME_SOFTKEY_H - FONT_N_PX) / 2, label, th->bar_ink,
		  th->text_shadow, plate_mid(sh, &p, y, h));
}

/* Cut s to fit max_w pixels, ending in "..." when anything was cut.
 * Page titles and urls in the history list are routinely wider than
 * the screen, and a row that runs under the scrollbar reads as a
 * rendering fault. */
static void fit_text(const plot_font_style_t *fs, const char *s,
		     int max_w, char *out, size_t out_sz)
{
	size_t len = strlen(s);

	if (len >= out_sz)
		len = out_sz - 1;
	memcpy(out, s, len);
	out[len] = '\0';
	if (text_width(fs, out, len) <= max_w)
		return;

	while (len > 0) {
		len--;
		if (len + 4 > out_sz)
			continue;
		memcpy(out + len, "...", 4);
		if (text_width(fs, out, len + 3) <= max_w)
			return;
	}
	out[0] = '\0';
}

/* The scrollbar. The classic face's 1 px grey track with a white notch;
 * a theme with gradients gets nd_theme_scrollbar()'s recessed groove
 * with a glossy thumb sized to the list. */
static void render_scrollbar(struct neodct_shell *sh, nsfb_t *nsfb, int x,
			     int top, int bottom, int pos, int count)
{
	const struct neodct_theme *th = sh->theme;
	int notch_y, y;

	if (!th->gradients) {
		fill_rect(nsfb, x, top, x + 1, bottom, th->chrome_bot);
		if (count > 1)
			notch_y = top + pos * (bottom - top) / (count - 1);
		else
			notch_y = top;
		fill_rect(nsfb, x - 2, notch_y - 3, x + 2, notch_y + 3,
			  th->chrome_hi);
		return;
	}

	for (y = top; y <= bottom; y++)
		fill_rect(nsfb, x - 2, y, x + 3, y + 1,
			  neodct_theme_blend(ground(sh, y), th->blue_deep, 90));
	{
		int track_h = bottom - top + 1;
		int thumb_h = count > 1 ? track_h / count : track_h;
		struct plate p = plate_blue(th, 2);

		if (thumb_h < 10)
			thumb_h = 10;
		if (thumb_h > track_h)
			thumb_h = track_h;
		notch_y = top;
		if (count > 1)
			notch_y += pos * (track_h - thumb_h) / (count - 1);
		p.top = th->blue_hi;
		p.bot = th->blue_mid;
		plate_draw(sh, nsfb, x - 2, notch_y, x + 3, notch_y + thumb_h,
			   &p);
	}
}

/* VerticalList.draw(): title, breadcrumb, divider, 3 rows, scrollbar */
static void render_list(struct neodct_shell *sh, nsfb_t *nsfb,
			struct redraw_context *ctx, const struct neodct_menu *m,
			const char *title, const char *crumb_prefix,
			const char *empty_text)
{
	const struct neodct_theme *th = sh->theme;
	int w = fbtk_get_width(sh->gw->window);
	int h = fbtk_get_height(sh->gw->window);
	int content_bottom = h - FRAME_SOFTKEY_H;
	int y_start = FRAME_HEADER_Y + 10;
	int content_height = content_bottom - y_start - 4;
	int line_height = content_height / 3;
	int item_height;
	int bar_x = w - 5;
	int selected_right = bar_x - 10;
	/* a rounded lozenge flush to the edge has nowhere to put its
	 * corner, so it starts 4 px in (nd_vlist.c) */
	int sel_left = th->round ? 4 : 0;
	plot_font_style_t fs;
	char crumb[24];
	char label[NEODCT_TEXT_MAX + 1];
	int i;

	if (line_height < 28)
		line_height = 28;
	item_height = line_height - 4;
	if (item_height < 24)
		item_height = 24;

	fill_ground(sh, nsfb, 0, 0, w, content_bottom);

	if (m->count > 0)
		snprintf(crumb, sizeof(crumb), "%s-%d", crumb_prefix,
			 m->selected + 1);
	else
		snprintf(crumb, sizeof(crumb), "%s", crumb_prefix);
	render_title(sh, nsfb, ctx, title, 0, crumb);

	/* list rows */
	for (i = 0; i < m->max_lines; i++) {
		int item = m->window_start + i;
		int y = y_start + i * line_height;
		int text_y = y + (item_height - FONT_MD_PX) / 2;

		if (item >= m->count)
			break;

		mkstyle(&fs, FONT_MD_PX, th->ink_light, th->sky_top);
		fit_text(&fs, m->items[item], selected_right - 10 - 4,
			 label, sizeof(label));

		if (item == m->selected) {
			struct plate p = plate_blue(th, PLATE_RADIUS);

			plate_draw(sh, nsfb, sel_left, y, selected_right,
				   y + item_height, &p);
			draw_text(sh, ctx, FONT_MD_PX, 10, text_y, label,
				  th->sel_ink, th->text_shadow,
				  plate_mid(sh, &p, y, y + item_height));
		} else {
			draw_text(sh, ctx, FONT_MD_PX, 10, text_y, label,
				  th->ink_light, th->text_shadow,
				  ground(sh, text_y));
		}
	}

	if (m->count == 0) {
		draw_text(sh, ctx, FONT_MD_PX, 10,
			  y_start + (item_height - FONT_MD_PX) / 2, empty_text,
			  th->ink_light, th->text_shadow, ground(sh, y_start));
		render_softkey(sh, nsfb, ctx, "Back");
		return;
	}

	render_scrollbar(sh, nsfb, bar_x, y_start, content_bottom - 5,
			 m->selected, m->count);
	render_softkey(sh, nsfb, ctx, "Select");
}

static void render_menu(struct neodct_shell *sh, nsfb_t *nsfb,
			struct redraw_context *ctx)
{
	render_list(sh, nsfb, ctx, &sh->ui.menu, "Options", BROWSER_APP_ID,
		    "");
}

/* History sits third in Options, so its breadcrumb is 11-3-n */
static void render_history(struct neodct_shell *sh, nsfb_t *nsfb,
			   struct redraw_context *ctx)
{
	render_list(sh, nsfb, ctx, &sh->ui.history_menu, "History",
		    BROWSER_APP_ID "-3", "No history yet");
}

/* TextInput.draw(): title, divider, prompt, outlined box, blink text */
static void render_urlbar(struct neodct_shell *sh, nsfb_t *nsfb,
			  struct redraw_context *ctx)
{
	const struct neodct_theme *th = sh->theme;
	int w = fbtk_get_width(sh->gw->window);
	int h = fbtk_get_height(sh->gw->window);
	int content_bottom = h - FRAME_SOFTKEY_H;
	int prompt_y = FRAME_HEADER_Y + 20;
	int box_y = prompt_y + 30;
	int box_h = content_bottom - box_y - 10;
	int box_right = w - 10;
	struct plate field;
	uint32_t field_ink, field_under;
	plot_font_style_t fs;
	char buf[NEODCT_TEXT_MAX + 2];
	int tw;

	if (box_h > 40)
		box_h = 40;
	if (box_h < 24)
		box_h = 24;

	fill_ground(sh, nsfb, 0, 0, w, content_bottom);
	render_title(sh, nsfb, ctx, "Go to URL", 5, NULL);

	draw_text(sh, ctx, FONT_N_PX, 10, prompt_y, "URL:", th->ink_light,
		  th->text_shadow, ground(sh, prompt_y));

	/* The field: a glass well in a theme that paints panels, and the
	 * classic face's hollow white rule where the glass is the ground. */
	field = plate_glass(sh, PLATE_RADIUS, box_y, box_y + box_h);
	if (!field.painted) {
		field.border = th->chrome_hi;
		field.border_a = 255;
		field.radius = 0;
		field_ink = th->ink_light;
		field_under = ground(sh, box_y + box_h / 2);
	} else {
		field_ink = th->ink_dark;
		field_under = plate_mid(sh, &field, box_y, box_y + box_h);
	}
	plate_draw(sh, nsfb, 10, box_y, box_right, box_y + box_h, &field);

	/* A selected url is drawn highlighted, like a selected list row,
	 * with no cursor: what happens next replaces it, not extends it. */
	if (sh->ui.text_selected)
		snprintf(buf, sizeof(buf), "%s", sh->ui.textbuf);
	else
		snprintf(buf, sizeof(buf), "%s%s", sh->ui.textbuf,
			 sh->blink ? "_" : "");

	/* keep the tail visible when the text outgrows the box */
	mkstyle(&fs, FONT_N_PX, field_ink, field_under);
	tw = text_width(&fs, buf, strlen(buf));
	{
		int text_x = 15;
		int max_w = box_right - 15 - 5;
		int text_y = box_y + (box_h - FONT_N_PX) / 2;
		struct rect clip = { 12, box_y + 1,
				     box_right - 2, box_y + box_h - 1 };
		struct rect full = { 0, 0, w, h };

		if (tw > max_w)
			text_x = 15 - (tw - max_w);
		ctx->plot->clip(ctx, &clip);
		if (sh->ui.text_selected) {
			struct plate sel = plate_blue(th, 2);

			sel.border_a = 0;
			plate_draw(sh, nsfb, text_x - 2, text_y - 1,
				   text_x + tw + 2, text_y + FONT_N_PX + 2,
				   &sel);
			draw_text(sh, ctx, FONT_N_PX, text_x, text_y, buf,
				  th->sel_ink, th->text_shadow,
				  plate_mid(sh, &sel, text_y, text_y + FONT_N_PX));
		} else {
			draw_text(sh, ctx, FONT_N_PX, text_x, text_y, buf,
				  field_ink,
				  field.painted ? th->text_sheen : th->text_shadow,
				  field_under);
		}

		/* Unclip before the softkey. Left clipped to the text box,
		 * the bar was never drawn on this screen and the page
		 * showed through where "OK" belongs. */
		ctx->plot->clip(ctx, &full);
	}
	render_softkey(sh, nsfb, ctx, "OK");
}

/* TextInputLong.draw(): title, char count, divider, wrapped text */
static int measure_cb(void *pw, const char *s, size_t len)
{
	return text_width(pw, s, len);
}

static void render_input(struct neodct_shell *sh, nsfb_t *nsfb,
			 struct redraw_context *ctx)
{
	const struct neodct_theme *th = sh->theme;
	int w = fbtk_get_width(sh->gw->window);
	int h = fbtk_get_height(sh->gw->window);
	int content_bottom = h - FRAME_SOFTKEY_H;
	int area_top = FRAME_HEADER_Y + 10;
	int area_bottom = content_bottom - 4;
	int line_h = FONT_S_PX + 3;
	int max_lines = (area_bottom - area_top) / line_h;
	struct neodct_wrap_line lines[32];
	plot_font_style_t fs_small;
	char buf[NEODCT_TEXT_MAX + 2];
	char count[16];
	int n, start, i;

	if (max_lines < 1)
		max_lines = 1;
	if (max_lines > 32)
		max_lines = 32;

	fill_ground(sh, nsfb, 0, 0, w, content_bottom);

	snprintf(count, sizeof(count), "%d",
		 (int)strlen(sh->ui.textbuf));
	render_title(sh, nsfb, ctx, "Input Text", 5, count);

	snprintf(buf, sizeof(buf), "%s%s", sh->ui.textbuf,
		 sh->blink ? "_" : "");

	mkstyle(&fs_small, FONT_S_PX, th->ink_light, th->sky_top);
	n = neodct_wrap_text(buf, w - 20, measure_cb, &fs_small,
			     lines, 32);

	/* show the tail of the text like TextInputLong does */
	start = (n > max_lines) ? n - max_lines : 0;
	for (i = start; i < n; i++) {
		char line[NEODCT_TEXT_MAX + 2];
		int len = lines[i].len;
		int y = area_top + (i - start) * line_h;

		if (len > (int)sizeof(line) - 1)
			len = (int)sizeof(line) - 1;
		memcpy(line, lines[i].start, len);
		line[len] = '\0';
		draw_text(sh, ctx, FONT_S_PX, 10, y, line, th->ink_light,
			  th->text_shadow, ground(sh, y));
	}

	render_softkey(sh, nsfb, ctx, "OK");
}

/* claim, clip and flush around a chrome widget's drawing */
static void begin_widget(fbtk_widget_t *widget, struct redraw_context *ctx,
			 nsfb_bbox_t *bbox)
{
	nsfb_t *nsfb = fbtk_get_nsfb(widget);
	struct rect clip;

	fbtk_get_bbox(widget, bbox);
	nsfb_claim(nsfb, bbox);

	clip.x0 = bbox->x0;
	clip.y0 = bbox->y0;
	clip.x1 = bbox->x1;
	clip.y1 = bbox->y1;
	ctx->plot->clip(ctx, &clip);
}

/* redraw callback of the full-screen overlay widget */
static int screen_redraw(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct neodct_shell *sh = cbi->context;
	nsfb_t *nsfb = fbtk_get_nsfb(widget);
	nsfb_bbox_t bbox;
	struct redraw_context ctx = {
		.interactive = true,
		.background_images = true,
		.plot = &fb_plotters
	};

	begin_widget(widget, &ctx, &bbox);

	switch (sh->ui.mode) {
	case NEODCT_MODE_MENU:
		render_menu(sh, nsfb, &ctx);
		break;
	case NEODCT_MODE_URLBAR:
		render_urlbar(sh, nsfb, &ctx);
		break;
	case NEODCT_MODE_INPUT:
		render_input(sh, nsfb, &ctx);
		break;
	case NEODCT_MODE_HISTORY:
		render_history(sh, nsfb, &ctx);
		break;
	default:
		break;
	}

	nsfb_update(nsfb, &bbox);
	return 0;
}

/* The address bar across the top of the page: a bar plate in a theme
 * that paints its bars, and the classic black strip with a white rule
 * under it otherwise. It was two fbtk fills and a text widget, which
 * can only be one flat colour each. */
static int url_bar_redraw(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct neodct_shell *sh = cbi->context;
	const struct neodct_theme *th = sh->theme;
	nsfb_t *nsfb = fbtk_get_nsfb(widget);
	int w = fbtk_get_width(widget);
	struct plate p = plate_bar(th, 0);
	nsfb_bbox_t bbox;
	struct redraw_context ctx = {
		.interactive = true,
		.background_images = true,
		.plot = &fb_plotters
	};

	begin_widget(widget, &ctx, &bbox);

	fill_ground(sh, nsfb, 0, 0, w, NEODCT_URLBAR_H);
	p.border_a = 0;
	plate_draw(sh, nsfb, 0, 0, w, NEODCT_URLBAR_H - 1, &p);
	if (p.painted)
		fill_rect(nsfb, 0, NEODCT_URLBAR_H - 1, w, NEODCT_URLBAR_H,
			  th->blue_deep);
	else
		fill_rect(nsfb, 0, NEODCT_URLBAR_H - 1, w, NEODCT_URLBAR_H,
			  th->chrome_hi);

	/* where fbtk's text widget put it: 2 px padding, 13 px type */
	{
		struct rect clip = { 1, 1, w - 1, NEODCT_URLBAR_H - 2 };

		ctx.plot->clip(&ctx, &clip);
		draw_text(sh, &ctx, URLBAR_FONT_PX, 3, 3, sh->cur_url,
			  th->bar_ink, th->text_shadow,
			  plate_mid(sh, &p, 0, NEODCT_URLBAR_H));
	}

	nsfb_update(nsfb, &bbox);
	return 0;
}

/* the status line along the bottom, drawn over the page */
static int status_bar_redraw(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct neodct_shell *sh = cbi->context;
	const struct neodct_theme *th = sh->theme;
	nsfb_t *nsfb = fbtk_get_nsfb(widget);
	int w = fbtk_get_width(widget);
	int y0 = fbtk_get_absy(widget);
	int y1 = y0 + fbtk_get_height(widget);
	struct plate p = plate_bar(th, 0);
	nsfb_bbox_t bbox;
	struct redraw_context ctx = {
		.interactive = true,
		.background_images = true,
		.plot = &fb_plotters
	};

	begin_widget(widget, &ctx, &bbox);

	fill_ground(sh, nsfb, 0, y0, w, y1);
	p.border_a = 0;
	plate_draw(sh, nsfb, 0, y0 + 1, w, y1, &p);
	fill_rect(nsfb, 0, y0, w, y0 + 1, p.painted ? th->blue_deep :
		  ground(sh, y0));

	draw_text(sh, &ctx, STATUS_FONT_PX, 3, y0 + 2, sh->status.text,
		  th->bar_ink, th->text_shadow, plate_mid(sh, &p, y0, y1));

	nsfb_update(nsfb, &bbox);
	return 0;
}

/* ------------------------------------------------------------------
 * chrome state -> widgets
 */

static void status_tick_cb(void *ctx);
static void blink_cb(void *ctx);

static void shell_sync(struct neodct_shell *sh)
{
	struct neodct_ui *ui = &sh->ui;
	bool overlay = (ui->mode != NEODCT_MODE_BROWSE);

	fbtk_request_redraw(sh->url_bar);

	fbtk_set_mapping(sh->screen, overlay);
	if (overlay) {
		fbtk_request_redraw(sh->screen);
		if (!sh->blink_running) {
			sh->blink_running = true;
			sh->blink = true;
			framebuffer_schedule(BLINK_INTERVAL_MS, blink_cb, sh);
		}
	}

	/* status bar, only while browsing */
	neodct_status_tick(&sh->status, now_ms());
	if (sh->status.visible && !overlay) {
		fbtk_set_mapping(sh->status_bar, true);
		fbtk_request_redraw(sh->status_bar);
		if (sh->status.hide_at_ms >= 0 && !sh->tick_scheduled) {
			sh->tick_scheduled = true;
			framebuffer_schedule(250, status_tick_cb, sh);
		}
		/* the bar is drawn over the page, so a scroll that copies
		 * the page up copies the bar with it: tell the pan */
		fb_browser_set_obscured_bottom(sh->gw, NEODCT_STATUS_H);
	} else {
		fbtk_set_mapping(sh->status_bar, false);
		fb_browser_set_obscured_bottom(sh->gw, 0);
	}

	fbtk_request_redraw(sh->gw->window);
}

static void status_tick_cb(void *ctx)
{
	struct neodct_shell *sh = ctx;

	sh->tick_scheduled = false;
	shell_sync(sh);
}

static void blink_cb(void *ctx)
{
	struct neodct_shell *sh = ctx;

	if (sh->ui.mode == NEODCT_MODE_BROWSE) {
		sh->blink_running = false;
		return;
	}
	sh->blink = !sh->blink;
	fbtk_request_redraw(sh->screen);
	framebuffer_schedule(BLINK_INTERVAL_MS, blink_cb, sh);
}

/* last sampled RSS, for the async-signal-safe crash report */
static long last_rss_kb = -1;

static long proc_field(const char *path, const char *field)
{
	char text[4096];
	ssize_t rd = -1;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		rd = read(fd, text, sizeof(text) - 1);
		close(fd);
	}
	if (rd <= 0)
		return -1;
	text[rd] = '\0';
	return neodct_mem_parse_field(text, field);
}

/* periodic report of browser and whole-system memory on stderr
 * (serial console): the sys columns show the kernel's real headroom */
static void mem_log_cb(void *ctx)
{
	struct neodct_shell *sh = ctx;

	last_rss_kb = proc_field("/proc/self/status", "VmRSS");

	fprintf(stderr,
		"neodct-mem: rss=%ldkB hwm=%ldkB | "
		"sys avail=%ldkB free=%ldkB swapfree=%ldkB\n",
		last_rss_kb,
		proc_field("/proc/self/status", "VmHWM"),
		proc_field("/proc/meminfo", "MemAvailable"),
		proc_field("/proc/meminfo", "MemFree"),
		proc_field("/proc/meminfo", "SwapFree"));

	framebuffer_schedule(MEM_LOG_INTERVAL_MS, mem_log_cb, sh);
}

/* fatal-signal report: async-signal-safe (write + manual formatting
 * only), then the default action so the exit code shows the signal */
static void crash_handler(int sig)
{
	char buf[96];
	char num[24];
	int len = 0;

	memcpy(buf, "neodct-crash: signal=", 21);
	len = 21;
	if (neodct_mem_format_long(num, sizeof(num), sig) > 0) {
		strcpy(buf + len, num);
		len += strlen(num);
	}
	memcpy(buf + len, " last-rss-kb=", 13);
	len += 13;
	if (neodct_mem_format_long(num, sizeof(num), last_rss_kb) > 0) {
		strcpy(buf + len, num);
		len += strlen(num);
	}
	buf[len++] = '\n';
	if (write(STDERR_FILENO, buf, len) < 0) {
		/* nothing more we can do */
	}

	signal(sig, SIG_DFL);
	raise(sig);
}

static void install_crash_handler(void)
{
	struct sigaction sa;
	int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL };
	unsigned i;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = crash_handler;
	sigemptyset(&sa.sa_mask);

	for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
		sigaction(sigs[i], &sa, NULL);
}

/* ------------------------------------------------------------------
 * home page and history
 */

static bool shell_is_home(const struct neodct_shell *sh, const char *url)
{
	return sh->homepage_norm[0] != '\0' &&
		strcmp(url, sh->homepage_norm) == 0;
}

/* The core spells urls its own way (a trailing slash on a bare host,
 * a lower-case scheme), so compare against the homepage as it will
 * come back through set_url rather than as it was typed. */
static void shell_init_homepage(struct neodct_shell *sh, const char *homepage)
{
	nsurl *url;

	strncpy(sh->homepage, homepage, NEODCT_TEXT_MAX);
	sh->homepage[NEODCT_TEXT_MAX] = '\0';

	if (nsurl_create(homepage, &url) == NSERROR_OK) {
		strncpy(sh->homepage_norm, nsurl_access(url),
			NEODCT_TEXT_MAX);
		sh->homepage_norm[NEODCT_TEXT_MAX] = '\0';
		nsurl_unref(url);
	} else {
		strncpy(sh->homepage_norm, homepage, NEODCT_TEXT_MAX);
		sh->homepage_norm[NEODCT_TEXT_MAX] = '\0';
	}
}

static void shell_init_history(struct neodct_shell *sh)
{
	const char *home = getenv("HOME");
	int n;

	neodct_history_init(&sh->history);
	neodct_ui_set_history(&sh->ui, &sh->history);

	sh->history_path[0] = '\0';
	if (home == NULL || home[0] == '\0')
		return; /* kept for this session only */
	n = snprintf(sh->history_path, sizeof(sh->history_path), "%s/%s",
		     home, HISTORY_FILE);
	if (n < 0 || (size_t)n >= sizeof(sh->history_path)) {
		sh->history_path[0] = '\0';
		return;
	}
	neodct_history_load(&sh->history, sh->history_path);
}

/* Recorded when a page finishes rather than when it starts, so that a
 * mistyped address that never loaded does not sit at the top of the
 * list. The home page and netsurf's own about: pages (error pages
 * among them) are not places anyone goes back to. */
static void shell_record_visit(struct neodct_shell *sh)
{
	if (sh->cur_url[0] == '\0' || shell_is_home(sh, sh->cur_url) ||
	    strncmp(sh->cur_url, "about:", 6) == 0)
		return;
	if (!neodct_history_add(&sh->history, sh->cur_url, sh->cur_title))
		return;
	if (sh->history_path[0] != '\0' &&
	    !neodct_history_save(&sh->history, sh->history_path))
		NSLOG(netsurf, INFO, "neodct: cannot save history to %s",
		      sh->history_path);
}

/* ------------------------------------------------------------------
 * browser actions
 */

static void shell_navigate(struct neodct_shell *sh, const char *url_text)
{
	nsurl *url;

	if (nsurl_create(url_text, &url) != NSERROR_OK) {
		neodct_status_error(&sh->status, "Bad URL");
		return;
	}
	browser_window_navigate(sh->gw->bw, url, NULL, BW_NAVIGATE_HISTORY,
				NULL, NULL, NULL);
	nsurl_unref(url);
}

static void shell_commit_text(struct neodct_shell *sh, const char *text)
{
	const char *p;

	for (p = text; *p != '\0'; p++) {
		browser_window_key_press(sh->gw->bw, (uint32_t)*p);
	}
	/* submit, like the old browser's synthetic Enter */
	browser_window_key_press(sh->gw->bw, NS_KEY_NL);
}



/**
 * NeoDCT: throw away input that arrived while something else had the screen.
 *
 * The event device keeps delivering to our queue while we are stopped, so
 * every seek and volume press aimed at mpv is still sitting there when we
 * come back. Replayed as browser input they scroll the page and open menus.
 */
static void shell_drain_input(struct neodct_shell *sh)
{
	nsfb_t *nsfb = fbtk_get_nsfb(sh->gw->window);
	nsfb_event_t event;
	int guard;

	if (nsfb == NULL) {
		return;
	}

	/* Bounded rather than "until empty": a device that always has an
	 * event ready would hold the browser here forever, and a stray
	 * keypress is the smaller problem. */
	for (guard = 0; guard < 256; guard++) {
		if (nsfb_event(nsfb, &event, 0) == false) {
			break;
		}
	}
}


/**
 * NeoDCT: hand a url to the media player and wait for it to finish.
 *
 * The player suspends us for the duration -- it stops its own parent, and
 * that is this process. On a single core, NetSurf's redraw loop competing
 * with a software video decoder means neither of them keeps up, and the
 * page we are holding is the largest allocation on the phone.
 *
 * Blocking here is the point: there is one screen and mpv is on it.
 */
static void shell_play(struct neodct_shell *sh, const char *url)
{
	const char *argv[8];
	pid_t pid;
	int status;

	if (!neodct_media_argv(url, argv, 8)) {
		neodct_status_error(&sh->status, "Cannot play that");
		shell_sync(sh);
		return;
	}

	/* Say so NOW, on the screen, before anything else.
	 *
	 * shell_sync() only asks for a redraw; the request is serviced from
	 * the event loop, and this function does not return to the event
	 * loop until the player has finished with the screen. Over a mobile
	 * link the player can take several seconds to open the url, and
	 * without this paint those seconds were the page, unchanged, with
	 * nothing to say the press had registered -- so the natural thing
	 * was to press again, into a browser that was already stopped. */
	neodct_status_loading(&sh->status);
	shell_sync(sh);
	fbtk_redraw(sh->gw->window);

	/* Give the memory back before mpv asks for it.
	 *
	 * Being stopped is not the same as being small: SIGSTOP parks the
	 * process but every page it holds stays resident, and on a 56 MB
	 * device the browser and the player together do not fit. What the
	 * kernel does about that is compress pages into zram, and zram
	 * costs the one core the decoder needs -- so memory pressure here
	 * arrives disguised as a slow decoder.
	 *
	 * Purging the source cache is safe at any point: it holds fetched
	 * data that can be fetched again, and the page on screen has
	 * already been laid out from it. */
	llcache_clean(true);

	pid = fork();
	if (pid < 0) {
		neodct_status_error(&sh->status, "Out of memory");
		shell_sync(sh);
		return;
	}

	if (pid == 0) {
		execv(argv[0], (char *const *)argv);
		/* Only reachable when the player is missing. _exit, not
		 * exit: this is a forked copy of a browser and running its
		 * atexit handlers would flush its buffers twice. */
		_exit(127);
	}

	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		/* SIGCONT on the way back in interrupts the wait */
	}

	/* The helper's exit status is the one word it gets to send back
	 * about what happened -- see NEODCT_MEDIA_EXIT_*. It played, or
	 * the status bar says why not, and says it until the next thing
	 * the user does. */
	{
		int code = WIFEXITED(status) ? WEXITSTATUS(status)
					     : NEODCT_MEDIA_EXIT_LOST;
		const char *why = neodct_media_exit_text(code);

		if (why == NULL) {
			neodct_status_done(&sh->status, now_ms());
		} else {
			NSLOG(netsurf, INFO, "neodct-play exit %d: %s",
			      code, why);
			neodct_status_error(&sh->status, why);
		}
	}

	/* mpv owned the framebuffer and left a video frame on it; and every
	 * key pressed at mpv was also queued on the event device we share
	 * with it, so it has to be thrown away before it is replayed as
	 * browser input. */
	shell_drain_input(sh);
	fbtk_request_redraw(sh->gw->window);
	shell_sync(sh);
}


static void shell_click(struct neodct_shell *sh, int x, int y)
{
	if (y < NEODCT_URLBAR_H) {
		/* Exit lives in Options, so the whole bar is the url */
		neodct_ui_open_urlbar(&sh->ui, sh->ui.page_url);
		return;
	}

	/* A <video> placeholder, or a plain link to something mpv can
	 * play: hand it over rather than asking the fetcher for it. */
	{
		const char *href = fb_browser_link_at(sh->gw, x, y);

		if (href != NULL && neodct_media_is_media(href)) {
			shell_play(sh, href);
			return;
		}
	}

	fb_browser_click_at(sh->gw, x, y);

	if (sh->ui.hover_editable) {
		/* clicked a form field: bring up the input popup */
		neodct_ui_open_input(&sh->ui, "");
	}
}

static void shell_action(struct neodct_shell *sh,
			 const struct neodct_action *act,
			 int keycode, uint32_t chr)
{
	switch (act->type) {
	case NEODCT_ACT_NONE:
		break;
	case NEODCT_ACT_CLICK:
		shell_click(sh, act->click.x, act->click.y);
		break;
	case NEODCT_ACT_SCROLL:
		if (act->scroll.dx != 0)
			widget_scroll_x(sh->gw, act->scroll.dx, false);
		if (act->scroll.dy != 0)
			widget_scroll_y(sh->gw, act->scroll.dy, false);
		break;
	case NEODCT_ACT_PASS_KEY:
		if (keycode == NSFB_KEY_BACKSPACE)
			browser_window_key_press(sh->gw->bw,
						 NS_KEY_DELETE_LEFT);
		else if (chr != 0)
			browser_window_key_press(sh->gw->bw, chr);
		break;
	case NEODCT_ACT_NAV_BACK:
		if (browser_window_history_back_available(sh->gw->bw))
			browser_window_history_back(sh->gw->bw, false);
		break;
	case NEODCT_ACT_NAV_FORWARD:
		if (browser_window_history_forward_available(sh->gw->bw))
			browser_window_history_forward(sh->gw->bw, false);
		break;
	case NEODCT_ACT_NAV_HOME:
		shell_navigate(sh, sh->homepage);
		break;
	case NEODCT_ACT_NAV_RELOAD:
		browser_window_reload(sh->gw->bw, true);
		break;
	case NEODCT_ACT_NAVIGATE:
		shell_navigate(sh, act->text);
		break;
	case NEODCT_ACT_COMMIT_TEXT:
		shell_commit_text(sh, act->text);
		break;
	case NEODCT_ACT_EXIT:
		fb_complete = true;
		break;
	}
}

static void shell_key(struct neodct_shell *sh, enum neodct_key key,
		      uint32_t chr, int keycode)
{
	struct neodct_action act;
	int old_x = sh->ui.cursor.x;
	int old_y = sh->ui.cursor.y;

	neodct_ui_key(&sh->ui, key, chr, &act);

	/* reflect cursor moves on screen and let the core track hover */
	if (sh->ui.mode == NEODCT_MODE_BROWSE &&
	    (sh->ui.cursor.x != old_x || sh->ui.cursor.y != old_y ||
	     act.type == NEODCT_ACT_SCROLL)) {
		fbtk_warp_pointer(sh->gw->window, sh->ui.cursor.x,
				  sh->ui.cursor.y, false);
		fb_browser_track_at(sh->gw, sh->ui.cursor.x,
				    sh->ui.cursor.y);
	}

	shell_action(sh, &act, keycode, chr);
	shell_sync(sh);
}

/* ------------------------------------------------------------------
 * input mapping
 */

int neodct_shell_input(struct gui_window *gw, struct fbtk_callback_info *cbi)
{
	struct neodct_shell *sh = shell_of(gw);
	static fbtk_modifier_type mods = FBTK_MOD_CLEAR;
	int keycode = cbi->event->value.keycode;
	int ucs4;

	if (cbi->event->type == NSFB_EVENT_KEY_UP) {
		switch (keycode) {
		case NSFB_KEY_RSHIFT: mods &= ~FBTK_MOD_RSHIFT; break;
		case NSFB_KEY_LSHIFT: mods &= ~FBTK_MOD_LSHIFT; break;
		case NSFB_KEY_RCTRL: mods &= ~FBTK_MOD_RCTRL; break;
		case NSFB_KEY_LCTRL: mods &= ~FBTK_MOD_LCTRL; break;
		default: break;
		}
		return 1;
	}

	if (cbi->event->type != NSFB_EVENT_KEY_DOWN)
		return 0;

	switch (keycode) {
	case NSFB_KEY_RSHIFT: mods |= FBTK_MOD_RSHIFT; return 1;
	case NSFB_KEY_LSHIFT: mods |= FBTK_MOD_LSHIFT; return 1;
	case NSFB_KEY_RCTRL: mods |= FBTK_MOD_RCTRL; return 1;
	case NSFB_KEY_LCTRL: mods |= FBTK_MOD_LCTRL; return 1;

	case NSFB_KEY_UP:
		shell_key(sh, NEODCT_KEY_UP, 0, keycode);
		return 1;
	case NSFB_KEY_DOWN:
		shell_key(sh, NEODCT_KEY_DOWN, 0, keycode);
		return 1;
	case NSFB_KEY_LEFT:
		shell_key(sh, NEODCT_KEY_LEFT, 0, keycode);
		return 1;
	case NSFB_KEY_RIGHT:
		shell_key(sh, NEODCT_KEY_RIGHT, 0, keycode);
		return 1;
	case NSFB_KEY_RETURN:
	case NSFB_KEY_KP_ENTER:
		shell_key(sh, NEODCT_KEY_SELECT, 0, keycode);
		return 1;
	case NSFB_KEY_BACKSPACE:
	case NSFB_KEY_ESCAPE:
		shell_key(sh, NEODCT_KEY_BACK, 0, NSFB_KEY_BACKSPACE);
		return 1;

	case NSFB_KEY_q:
		if (mods & (FBTK_MOD_LCTRL | FBTK_MOD_RCTRL)) {
			fb_complete = true;
			return 1;
		}
		/* fall through */
	default:
		ucs4 = fbtk_keycode_to_ucs4(keycode, mods);
		if (ucs4 != -1) {
			shell_key(sh, NEODCT_KEY_CHAR, (uint32_t)ucs4,
				  keycode);
			return 1;
		}
		return 1;
	}
}

/* ------------------------------------------------------------------
 * core notifications
 */

void neodct_shell_set_url(struct gui_window *gw, const char *url)
{
	struct neodct_shell *sh = shell_of(gw);

	if (url == NULL)
		return;
	if (strcmp(sh->cur_url, url) != 0)
		sh->cur_title[0] = '\0'; /* the title belonged to the last page */
	strncpy(sh->cur_url, url, NEODCT_TEXT_MAX);
	sh->cur_url[NEODCT_TEXT_MAX] = '\0';

	/* "Go to URL" starts from the page you are on, except on the home
	 * page, whose file:// address is nowhere anyone wants to type
	 * from */
	neodct_ui_set_page_url(&sh->ui, shell_is_home(sh, url) ? NULL : url);
	shell_sync(sh);
}

void neodct_shell_set_title(struct gui_window *gw, const char *title)
{
	struct neodct_shell *sh = shell_of(gw);

	if (title == NULL)
		return;
	strncpy(sh->cur_title, title, NEODCT_HISTORY_TITLE_MAX);
	sh->cur_title[NEODCT_HISTORY_TITLE_MAX] = '\0';
}

void neodct_shell_set_hover(struct gui_window *gw, bool editable)
{
	struct neodct_shell *sh = shell_of(gw);

	neodct_ui_set_hover_editable(&sh->ui, editable);
}

void neodct_shell_load_start(struct gui_window *gw)
{
	struct neodct_shell *sh = shell_of(gw);

	/* The remembered urls belong to the page being replaced; keeping
	 * them would let a video on the last page hijack a link on this
	 * one that happens to share its url. */
	neodct_media_reset();

	neodct_status_waiting(&sh->status, sh->cur_url);
	shell_sync(sh);
}

void neodct_shell_load_stop(struct gui_window *gw)
{
	struct neodct_shell *sh = shell_of(gw);

	neodct_status_done(&sh->status, now_ms());
	shell_record_visit(sh);
	shell_sync(sh);
}

/* ------------------------------------------------------------------
 * dev harness: scripted input and screenshots (NEODCT_SCRIPT)
 *
 * The script is a text file of lines "<delay_ms> <command>" where
 * command is up|down|left|right|select|back|char:<c>|text:<s>|
 * shot:<path>|quit. Delays are relative to the previous command.
 * This drives the real input path so the whole chrome can be
 * exercised and captured headlessly (e.g. on the ram surface).
 */

static void script_step(void *ctx);

static void script_schedule_next(struct neodct_shell *sh)
{
	int delay;
	char *line = sh->script_pos;

	if (line == NULL || *line == '\0')
		return;

	delay = atoi(line);
	framebuffer_schedule(delay, script_step, sh);
}

static void script_step(void *ctx)
{
	struct neodct_shell *sh = ctx;
	char *line = sh->script_pos;
	char *nl, *cmd;

	if (line == NULL || *line == '\0')
		return;

	nl = strchr(line, '\n');
	if (nl != NULL) {
		*nl = '\0';
		sh->script_pos = nl + 1;
	} else {
		sh->script_pos = line + strlen(line);
	}

	cmd = strchr(line, ' ');
	if (cmd == NULL) {
		script_schedule_next(sh);
		return;
	}
	cmd++;

	NSLOG(netsurf, INFO, "neodct script: %s", cmd);

	if (strcmp(cmd, "up") == 0) {
		shell_key(sh, NEODCT_KEY_UP, 0, 0);
	} else if (strcmp(cmd, "down") == 0) {
		shell_key(sh, NEODCT_KEY_DOWN, 0, 0);
	} else if (strcmp(cmd, "left") == 0) {
		shell_key(sh, NEODCT_KEY_LEFT, 0, 0);
	} else if (strcmp(cmd, "right") == 0) {
		shell_key(sh, NEODCT_KEY_RIGHT, 0, 0);
	} else if (strcmp(cmd, "select") == 0) {
		shell_key(sh, NEODCT_KEY_SELECT, 0, 0);
	} else if (strcmp(cmd, "back") == 0) {
		shell_key(sh, NEODCT_KEY_BACK, 0, NSFB_KEY_BACKSPACE);
	} else if (strncmp(cmd, "char:", 5) == 0) {
		shell_key(sh, NEODCT_KEY_CHAR, (uint32_t)cmd[5], 0);
	} else if (strncmp(cmd, "text:", 5) == 0) {
		const char *p;
		for (p = cmd + 5; *p != '\0'; p++)
			shell_key(sh, NEODCT_KEY_CHAR, (uint32_t)*p, 0);
	} else if (strncmp(cmd, "shot:", 5) == 0) {
		nsfb_t *nsfb = fbtk_get_nsfb(sh->gw->window);
		int fd;

		fbtk_redraw(sh->gw->window);

		/* mark the cursor position (ram surface plots no
		 * pointer sprite); repaired by a full redraw after */
		if (sh->ui.mode == NEODCT_MODE_BROWSE) {
			nsfb_bbox_t hl = { sh->ui.cursor.x - 4,
					   sh->ui.cursor.y,
					   sh->ui.cursor.x + 5,
					   sh->ui.cursor.y + 1 };
			nsfb_bbox_t vl = { sh->ui.cursor.x,
					   sh->ui.cursor.y - 4,
					   sh->ui.cursor.x + 1,
					   sh->ui.cursor.y + 5 };
			nsfb_claim(nsfb, &hl);
			nsfb_plot_rectangle_fill(nsfb, &hl, 0xff0000ff);
			nsfb_update(nsfb, &hl);
			nsfb_claim(nsfb, &vl);
			nsfb_plot_rectangle_fill(nsfb, &vl, 0xff0000ff);
			nsfb_update(nsfb, &vl);
		}

		fd = open(cmd + 5, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			nsfb_dump(nsfb, fd);
			close(fd);
		}
		fbtk_request_redraw(sh->gw->window);
	} else if (strcmp(cmd, "quit") == 0) {
		fb_complete = true;
		return;
	}

	script_schedule_next(sh);
}

static void script_init(struct neodct_shell *sh)
{
	const char *path = getenv("NEODCT_SCRIPT");
	FILE *f;
	long size;

	if (path == NULL)
		return;

	f = fopen(path, "r");
	if (f == NULL)
		return;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	sh->script = calloc(1, size + 1);
	if (sh->script != NULL) {
		if (fread(sh->script, 1, size, f) != (size_t)size) {
			free(sh->script);
			sh->script = NULL;
		}
	}
	fclose(f);

	sh->script_pos = sh->script;
	script_schedule_next(sh);
}

/* ------------------------------------------------------------------
 * chrome construction
 */

void neodct_shell_create(struct gui_window *gw, const char *homepage)
{
	struct neodct_shell *sh = &the_shell;
	fbtk_widget_t *win = gw->window;
	int width = fbtk_get_width(win);
	int height = fbtk_get_height(win);

	memset(sh, 0, sizeof(*sh));
	sh->gw = gw;
	gw->neodct = sh;

	sh->theme = neodct_theme_active();
	neodct_ui_init(&sh->ui, width, height);
	neodct_status_init(&sh->status);
	shell_init_homepage(sh, homepage);
	shell_init_history(sh);

	/* browse chrome: the address bar, with no close box -- Exit is the
	 * first entry in Options, so the whole bar is the url and a click
	 * anywhere on it opens Go to URL */
	sh->url_bar = fbtk_create_user(win, 0, 0, width, NEODCT_URLBAR_H,
				       NULL);
	fbtk_set_handler(sh->url_bar, FBTK_CBT_REDRAW, url_bar_redraw, sh);

	/* status bar overlay */
	sh->status_bar = fbtk_create_user(win, 0, height - NEODCT_STATUS_H,
					  width, NEODCT_STATUS_H, NULL);
	fbtk_set_handler(sh->status_bar, FBTK_CBT_REDRAW, status_bar_redraw,
			 sh);
	fbtk_set_mapping(sh->status_bar, false);

	/* full-screen NeoDCT overlay for menu / url / input screens */
	sh->screen = fbtk_create_user(win, 0, 0, width, height, NULL);
	fbtk_set_handler(sh->screen, FBTK_CBT_REDRAW, screen_redraw, sh);
	fbtk_set_mapping(sh->screen, false);

	/* park the pointer at the cursor start position */
	fbtk_warp_pointer(win, sh->ui.cursor.x, sh->ui.cursor.y, false);

	shell_sync(sh);
	script_init(sh);

	install_crash_handler();
	framebuffer_schedule(MEM_LOG_INTERVAL_MS, mem_log_cb, sh);
}

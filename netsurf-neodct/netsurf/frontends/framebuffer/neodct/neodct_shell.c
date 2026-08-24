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
#include "framebuffer/neodct/neodct_status.h"
#include "framebuffer/neodct/neodct_wrap.h"
#include "framebuffer/neodct/neodct_mem.h"
#include "framebuffer/neodct/neodct_media.h"
#include "framebuffer/neodct/neodct_shell.h"

#define NEODCT_BLACK FB_COLOUR_BLACK
#define NEODCT_WHITE FB_COLOUR_WHITE
#define NEODCT_GRAY  0xFF808080

/* framework.py layout constants (240x175) */
#define FRAME_SOFTKEY_H 30
#define FRAME_HEADER_Y 30       /* max(30, H * 0.11) */
#define BROWSER_APP_ID "11"

/* framework.py font pixel sizes */
#define FONT_S_PX 14
#define FONT_MD_PX 18
#define FONT_N_PX 20
#define FONT_XL_PX 24

#define URL_CLOSE_W 16
#define MEM_LOG_INTERVAL_MS 5000
#define BLINK_INTERVAL_MS 500

/* pixels to points at the toolkit's 90 DPI */
#define px_to_pt(x) (((x) * 72) / FBTK_DPI)

struct neodct_shell {
	struct gui_window *gw;
	struct neodct_ui ui;
	struct neodct_status status;

	char cur_url[NEODCT_TEXT_MAX + 1];
	char homepage[NEODCT_TEXT_MAX + 1];

	/* browse-mode chrome */
	fbtk_widget_t *url_text;
	fbtk_widget_t *close_text;
	fbtk_widget_t *status_text;

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
 */

static void mkstyle(plot_font_style_t *fs, int px, colour fg, colour bg)
{
	memset(fs, 0, sizeof(*fs));
	/* the fantasy face carries the NeoDCT system font; sans-serif
	 * stays a regular web font for page content */
	fs->family = PLOT_FONT_FAMILY_FANTASY;
	fs->size = px_to_pt(px * PLOT_STYLE_SCALE);
	fs->weight = 400;
	fs->flags = FONTF_NONE;
	fs->foreground = fg;
	fs->background = bg;
}

static int text_width(const plot_font_style_t *fs, const char *s, size_t len)
{
	int w = 0;

	if (framebuffer_layout_table->width(fs, s, len, &w) != NSERROR_OK)
		return (int)len * 8;
	return w;
}

/* draw text with (x, y) as the glyph box top-left, like PIL draw.text */
static void draw_text(struct redraw_context *ctx,
		      const plot_font_style_t *fs, int px,
		      int x, int y, const char *s)
{
	if (s == NULL || *s == '\0')
		return;
	/* baseline sits 3/4 down the font box (matches fbtk/text.c) */
	ctx->plot->text(ctx, fs, x, y + ((px * 3 + 2) / 4) + 1,
			s, strlen(s));
}

static void fill_rect(nsfb_t *nsfb, int x0, int y0, int x1, int y1,
		      colour c)
{
	nsfb_bbox_t box = { x0, y0, x1, y1 };

	nsfb_plot_rectangle_fill(nsfb, &box, c);
}

static void outline_rect(nsfb_t *nsfb, int x0, int y0, int x1, int y1,
			 colour c)
{
	/* four 1px edges; nsfb_plot_rectangle dashes on some plotters */
	fill_rect(nsfb, x0, y0, x1, y0 + 1, c);
	fill_rect(nsfb, x0, y1 - 1, x1, y1, c);
	fill_rect(nsfb, x0, y0, x0 + 1, y1, c);
	fill_rect(nsfb, x1 - 1, y0, x1, y1, c);
}

/* ------------------------------------------------------------------
 * NeoDCT framework screens
 */

/* SoftKeyBar.update(): black bar, centred font_n label */
static void render_softkey(struct neodct_shell *sh, nsfb_t *nsfb,
			   struct redraw_context *ctx, const char *label)
{
	int w = fbtk_get_width(sh->gw->window);
	int h = fbtk_get_height(sh->gw->window);
	int y = h - FRAME_SOFTKEY_H;
	plot_font_style_t fs;
	int tw;

	fill_rect(nsfb, 0, y, w, h, NEODCT_BLACK);

	mkstyle(&fs, FONT_N_PX, NEODCT_WHITE, NEODCT_BLACK);
	tw = text_width(&fs, label, strlen(label));
	draw_text(ctx, &fs, FONT_N_PX, (w - tw) / 2,
		  y + (FRAME_SOFTKEY_H - FONT_N_PX) / 2, label);
}

/* VerticalList.draw(): title, breadcrumb, divider, 3 rows, scrollbar */
static void render_menu(struct neodct_shell *sh, nsfb_t *nsfb,
			struct redraw_context *ctx)
{
	struct neodct_menu *m = &sh->ui.menu;
	int w = fbtk_get_width(sh->gw->window);
	int h = fbtk_get_height(sh->gw->window);
	int content_bottom = h - FRAME_SOFTKEY_H;
	int y_start = FRAME_HEADER_Y + 10;
	int content_height = content_bottom - y_start - 4;
	int line_height = content_height / 3;
	int item_height;
	int bar_x = w - 5;
	int selected_right = bar_x - 10;
	int track_top, track_bottom, notch_y;
	plot_font_style_t fs;
	char crumb[16];
	int i, tw;

	if (line_height < 28)
		line_height = 28;
	item_height = line_height - 4;
	if (item_height < 24)
		item_height = 24;

	fill_rect(nsfb, 0, 0, w, content_bottom, NEODCT_BLACK);

	/* title and breadcrumb header */
	mkstyle(&fs, FONT_XL_PX, NEODCT_WHITE, NEODCT_BLACK);
	draw_text(ctx, &fs, FONT_XL_PX, 5, 0, "Options");

	snprintf(crumb, sizeof(crumb), "%s-%d", BROWSER_APP_ID,
		 m->selected + 1);
	mkstyle(&fs, FONT_N_PX, NEODCT_WHITE, NEODCT_BLACK);
	tw = text_width(&fs, crumb, strlen(crumb));
	draw_text(ctx, &fs, FONT_N_PX, w - 5 - tw, 5, crumb);

	fill_rect(nsfb, 0, FRAME_HEADER_Y, w, FRAME_HEADER_Y + 1,
		  NEODCT_WHITE);

	/* list rows */
	for (i = 0; i < m->max_lines; i++) {
		int item = m->window_start + i;
		int y = y_start + i * line_height;
		int text_y = y + (item_height - FONT_MD_PX) / 2;

		if (item >= m->count)
			break;

		if (item == m->selected) {
			fill_rect(nsfb, 0, y, selected_right,
				  y + item_height, NEODCT_WHITE);
			mkstyle(&fs, FONT_MD_PX, NEODCT_BLACK, NEODCT_WHITE);
		} else {
			mkstyle(&fs, FONT_MD_PX, NEODCT_WHITE, NEODCT_BLACK);
		}
		draw_text(ctx, &fs, FONT_MD_PX, 10, text_y, m->items[item]);
	}

	/* scrollbar track and notch */
	track_top = y_start;
	track_bottom = content_bottom - 5;
	fill_rect(nsfb, bar_x, track_top, bar_x + 1, track_bottom,
		  NEODCT_GRAY);

	if (m->count > 1)
		notch_y = track_top + m->selected *
			(track_bottom - track_top) / (m->count - 1);
	else
		notch_y = track_top;
	fill_rect(nsfb, bar_x - 2, notch_y - 3, bar_x + 2, notch_y + 3,
		  NEODCT_WHITE);

	render_softkey(sh, nsfb, ctx, "Select");
}

/* TextInput.draw(): title, divider, prompt, outlined box, blink text */
static void render_urlbar(struct neodct_shell *sh, nsfb_t *nsfb,
			  struct redraw_context *ctx)
{
	int w = fbtk_get_width(sh->gw->window);
	int h = fbtk_get_height(sh->gw->window);
	int content_bottom = h - FRAME_SOFTKEY_H;
	int prompt_y = FRAME_HEADER_Y + 20;
	int box_y = prompt_y + 30;
	int box_h = content_bottom - box_y - 10;
	int box_right = w - 10;
	plot_font_style_t fs;
	char buf[NEODCT_TEXT_MAX + 2];
	int tw;

	if (box_h > 40)
		box_h = 40;
	if (box_h < 24)
		box_h = 24;

	fill_rect(nsfb, 0, 0, w, content_bottom, NEODCT_BLACK);

	mkstyle(&fs, FONT_XL_PX, NEODCT_WHITE, NEODCT_BLACK);
	draw_text(ctx, &fs, FONT_XL_PX, 5, 5, "Go to URL");
	fill_rect(nsfb, 0, FRAME_HEADER_Y, w, FRAME_HEADER_Y + 1,
		  NEODCT_WHITE);

	mkstyle(&fs, FONT_N_PX, NEODCT_WHITE, NEODCT_BLACK);
	draw_text(ctx, &fs, FONT_N_PX, 10, prompt_y, "URL:");

	outline_rect(nsfb, 10, box_y, box_right, box_y + box_h,
		     NEODCT_WHITE);

	snprintf(buf, sizeof(buf), "%s%s", sh->ui.textbuf,
		 sh->blink ? "_" : "");

	/* keep the tail visible when the text outgrows the box */
	tw = text_width(&fs, buf, strlen(buf));
	{
		int text_x = 15;
		int max_w = box_right - 15 - 5;
		struct rect clip = { 12, box_y + 1,
				     box_right - 2, box_y + box_h - 1 };

		if (tw > max_w)
			text_x = 15 - (tw - max_w);
		ctx->plot->clip(ctx, &clip);
		draw_text(ctx, &fs, FONT_N_PX, text_x,
			  box_y + (box_h - FONT_N_PX) / 2, buf);
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
	int w = fbtk_get_width(sh->gw->window);
	int h = fbtk_get_height(sh->gw->window);
	int content_bottom = h - FRAME_SOFTKEY_H;
	int area_top = FRAME_HEADER_Y + 10;
	int area_bottom = content_bottom - 4;
	int line_h = FONT_S_PX + 3;
	int max_lines = (area_bottom - area_top) / line_h;
	struct neodct_wrap_line lines[32];
	plot_font_style_t fs, fs_small;
	char buf[NEODCT_TEXT_MAX + 2];
	char count[16];
	int n, start, i, tw;

	if (max_lines < 1)
		max_lines = 1;
	if (max_lines > 32)
		max_lines = 32;

	fill_rect(nsfb, 0, 0, w, content_bottom, NEODCT_BLACK);

	mkstyle(&fs, FONT_XL_PX, NEODCT_WHITE, NEODCT_BLACK);
	draw_text(ctx, &fs, FONT_XL_PX, 5, 5, "Input Text");

	snprintf(count, sizeof(count), "%d",
		 (int)strlen(sh->ui.textbuf));
	mkstyle(&fs, FONT_N_PX, NEODCT_WHITE, NEODCT_BLACK);
	tw = text_width(&fs, count, strlen(count));
	draw_text(ctx, &fs, FONT_N_PX, w - 5 - tw, 5, count);

	fill_rect(nsfb, 0, FRAME_HEADER_Y, w, FRAME_HEADER_Y + 1,
		  NEODCT_WHITE);

	snprintf(buf, sizeof(buf), "%s%s", sh->ui.textbuf,
		 sh->blink ? "_" : "");

	mkstyle(&fs_small, FONT_S_PX, NEODCT_WHITE, NEODCT_BLACK);
	n = neodct_wrap_text(buf, w - 20, measure_cb, &fs_small,
			     lines, 32);

	/* show the tail of the text like TextInputLong does */
	start = (n > max_lines) ? n - max_lines : 0;
	for (i = start; i < n; i++) {
		char line[NEODCT_TEXT_MAX + 2];
		int len = lines[i].len;

		if (len > (int)sizeof(line) - 1)
			len = (int)sizeof(line) - 1;
		memcpy(line, lines[i].start, len);
		line[len] = '\0';
		draw_text(ctx, &fs_small, FONT_S_PX, 10,
			  area_top + (i - start) * line_h, line);
	}

	render_softkey(sh, nsfb, ctx, "OK");
}

/* redraw callback of the full-screen overlay widget */
static int screen_redraw(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct neodct_shell *sh = cbi->context;
	nsfb_t *nsfb = fbtk_get_nsfb(widget);
	nsfb_bbox_t bbox;
	struct rect fullclip;
	struct redraw_context ctx = {
		.interactive = true,
		.background_images = true,
		.plot = &fb_plotters
	};

	fbtk_get_bbox(widget, &bbox);
	nsfb_claim(nsfb, &bbox);

	fullclip.x0 = bbox.x0;
	fullclip.y0 = bbox.y0;
	fullclip.x1 = bbox.x1;
	fullclip.y1 = bbox.y1;
	ctx.plot->clip(&ctx, &fullclip);

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
	default:
		break;
	}

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

	fbtk_set_text(sh->url_text, sh->cur_url);

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
		fbtk_set_text(sh->status_text, sh->status.text);
		fbtk_set_mapping(sh->status_text, true);
		if (sh->status.hide_at_ms >= 0 && !sh->tick_scheduled) {
			sh->tick_scheduled = true;
			framebuffer_schedule(250, status_tick_cb, sh);
		}
	} else {
		fbtk_set_mapping(sh->status_text, false);
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
 * browser actions
 */

static void shell_navigate(struct neodct_shell *sh, const char *url_text)
{
	nsurl *url;

	if (nsurl_create(url_text, &url) != NSERROR_OK) {
		neodct_status_transferring(&sh->status, -1);
		snprintf(sh->status.text, sizeof(sh->status.text),
			 "Bad URL");
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
		snprintf(sh->status.text, sizeof(sh->status.text),
			 "Cannot play that");
		shell_sync(sh);
		return;
	}

	snprintf(sh->status.text, sizeof(sh->status.text), "Playing...");
	shell_sync(sh);

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
		snprintf(sh->status.text, sizeof(sh->status.text),
			 "Out of memory");
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

	if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
		snprintf(sh->status.text, sizeof(sh->status.text),
			 "No media player");
	} else {
		neodct_status_done(&sh->status, now_ms());
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
		if (x >= sh->ui.cursor.width - URL_CLOSE_W) {
			fb_complete = true;
		} else {
			neodct_ui_open_urlbar(&sh->ui, sh->cur_url);
		}
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
	strncpy(sh->cur_url, url, NEODCT_TEXT_MAX);
	sh->cur_url[NEODCT_TEXT_MAX] = '\0';
	shell_sync(sh);
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

	neodct_ui_init(&sh->ui, width, height);
	neodct_status_init(&sh->status);
	strncpy(sh->homepage, homepage, NEODCT_TEXT_MAX);

	/* browse chrome: url bar with 1px white underline, close X */
	fbtk_create_fill(win, 0, 0, width, NEODCT_URLBAR_H, NEODCT_BLACK);
	fbtk_create_fill(win, 0, NEODCT_URLBAR_H - 1, width, 1,
			 NEODCT_WHITE);
	sh->url_text = fbtk_create_text(win, 1, 1,
					width - URL_CLOSE_W - 4,
					NEODCT_URLBAR_H - 3,
					NEODCT_BLACK, NEODCT_WHITE, false);
	sh->close_text = fbtk_create_text(win, width - URL_CLOSE_W - 1, 1,
					  URL_CLOSE_W, NEODCT_URLBAR_H - 3,
					  NEODCT_BLACK, NEODCT_WHITE, true);
	fbtk_set_text(sh->close_text, "X");

	/* status bar overlay */
	sh->status_text = fbtk_create_text(win, 0, height - NEODCT_STATUS_H,
					   width, NEODCT_STATUS_H,
					   NEODCT_BLACK, NEODCT_WHITE, true);
	fbtk_set_mapping(sh->status_text, false);

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

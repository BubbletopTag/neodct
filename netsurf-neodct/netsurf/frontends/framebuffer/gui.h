/*
 * Copyright 2008 Vincent Sanders <vince@simtec.co.uk>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef NETSURF_FB_GUI_H
#define NETSURF_FB_GUI_H

#include <stdbool.h>


struct fbtk_widget_s;

typedef struct fb_cursor_s fb_cursor_t;

/* bounding box */
typedef struct nsfb_bbox_s bbox_t;

struct gui_window {
	struct browser_window *bw;

	void *neodct; /**< NeoDCT chrome shell state */

	struct fbtk_widget_s *window;
	struct fbtk_widget_s *back;
	struct fbtk_widget_s *forward;
	struct fbtk_widget_s *history;
	struct fbtk_widget_s *stop;
	struct fbtk_widget_s *reload;
	struct fbtk_widget_s *close;
	struct fbtk_widget_s *url;
	struct fbtk_widget_s *status;
	struct fbtk_widget_s *throbber;
	struct fbtk_widget_s *hscroll;
	struct fbtk_widget_s *vscroll;
	struct fbtk_widget_s *browser;
	struct fbtk_widget_s *toolbar;
	struct fbtk_widget_s *bottom_right;

	int throbber_index;

	struct gui_window *next;
	struct gui_window *prev;
};


extern struct gui_window *window_list;

/** set when the main loop should terminate */
extern bool fb_complete;

void gui_resize(struct fbtk_widget_s *root, int width, int height);

/* exported for the NeoDCT chrome shell */
void widget_scroll_y(struct gui_window *gw, int y, bool abs);
void widget_scroll_x(struct gui_window *gw, int x, bool abs);
/** send a synthetic left click to the page at screen coordinates */
void fb_browser_click_at(struct gui_window *gw, int sx, int sy);

/**
 * NeoDCT: the link url under a screen position, or NULL if there is none.
 *
 * Lives here rather than in the shell because turning a screen position
 * into a page position needs the browser widget's scroll offsets, and
 * those are private to gui.c. The returned string belongs to the core and
 * is only valid until the page changes.
 */
const char *fb_browser_link_at(struct gui_window *gw, int sx, int sy);
/** send synthetic pointer movement to the page at screen coordinates */
void fb_browser_track_at(struct gui_window *gw, int sx, int sy);

#endif /* NETSURF_FB_GUI_H */

/*
 * Local Variables:
 * c-basic-offset:8
 * End:
 */

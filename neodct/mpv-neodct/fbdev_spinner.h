/*
 * NeoDCT: the loading ring the framebuffer output draws while it has
 * nothing else to show.
 *
 * Pressing play on a phone hands the screen to mpv, and on a mobile link
 * mpv can then spend several seconds opening the url before it has a frame
 * to draw. During those seconds the screen was black, which on a device
 * with no other indicator reads as "it did not work" -- and the natural
 * response, pressing play again, queues a second keypress for a browser
 * that is stopped. The ring is the answer to that: eight dots, one bright,
 * the brightness trailing round behind it, stepping once every tenth of a
 * second. It is also what shows over the last frame when a stream stalls
 * mid-way, which on a bad connection is most of the time.
 *
 * This file is the geometry and nothing else -- where the dots go, how
 * bright each one is at a given moment, which pixels a dot covers -- so
 * that all of it can be unit tested on a host without a framebuffer or an
 * mpv build. vo_fbdev.c turns the answers into pixels. Integer throughout:
 * the ring is a few dozen pixels across and a rounded edge is a ragged one.
 */

#ifndef NEODCT_FBDEV_SPINNER_H
#define NEODCT_FBDEV_SPINNER_H

#include <stdbool.h>
#include <stdint.h>

#include "fbdev_format.h"

/** dots on the ring */
#define NEODCT_SPINNER_DOTS 8

/** how long the bright dot rests on each position: a turn takes 0.8 s */
#define NEODCT_SPINNER_TICK_US 100000

/**
 * How long a stall has to last before the ring appears over a playing
 * video. Playback restarts after every seek, and a seek served from the
 * cache is over in well under this; flashing the ring for it would make
 * every keypress look like a failure. The initial load does not wait --
 * there the screen is already black and the user is waiting for exactly
 * this.
 */
#define NEODCT_SPINNER_GRACE_US 300000

struct neodct_spinner_geom {
	int cx, cy;   /**< centre of the ring */
	int ring_r;   /**< radius the dot centres sit on */
	int dot_r;    /**< radius of each dot */
};

/** size and place the ring for a screen; a fraction of the shorter side */
void neodct_spinner_geometry(int screen_w, int screen_h,
			     struct neodct_spinner_geom *g);

/** centre of dot `dot` (0..NEODCT_SPINNER_DOTS-1), clockwise from 3 o'clock */
void neodct_spinner_dot_centre(const struct neodct_spinner_geom *g, int dot,
			       int *x, int *y);

/**
 * Brightness of dot `dot` when the bright one is at position `phase`,
 * 0..255. The dot just behind the head is the next brightest, and so on
 * round the ring; nothing is ever fully dark, so the ring reads as a ring.
 */
int neodct_spinner_dot_level(int dot, int phase);

/** which dot is the bright one, `elapsed_us` after the ring first showed */
int neodct_spinner_phase(int64_t elapsed_us);

/** true if the pixel at offset (dx, dy) from a dot's centre is inside it */
bool neodct_spinner_covers(int dx, int dy, int dot_r);

/**
 * The rectangle the ring touches, clamped to the screen, half-open
 * ([x0, x1) by [y0, y1)) so a caller can loop over it directly. This is
 * what has to be repainted from the clean frame before each step, so that
 * the previous step's dots do not accumulate.
 */
void neodct_spinner_bbox(const struct neodct_spinner_geom *g,
			 int screen_w, int screen_h,
			 int *x0, int *y0, int *x1, int *y1);

/**
 * A grey of brightness `level` (0..255) packed for the framebuffer's
 * pixel layout. 16 bits of the result are meaningful for RGB565, 32 for
 * the others; a grey has the same bytes in either 32-bit order, which is
 * the whole reason the ring is grey.
 */
uint32_t neodct_spinner_pixel(enum neodct_fb_pixfmt fmt, int level);

#endif

/*
 * NeoDCT: the loading ring -- geometry only. See fbdev_spinner.h.
 */

#include "fbdev_spinner.h"

/* cos and sin of the eight positions, times 1000, clockwise on a screen
 * whose y axis points down: 0 is 3 o'clock, 2 is 6 o'clock. */
static const int COS8[NEODCT_SPINNER_DOTS] = {
	1000, 707, 0, -707, -1000, -707, 0, 707
};
static const int SIN8[NEODCT_SPINNER_DOTS] = {
	0, 707, 1000, 707, 0, -707, -1000, -707
};

/* Brightness by distance behind the head: each step three quarters of the
 * last, floored where it still shows against black. */
static const int LEVELS[NEODCT_SPINNER_DOTS] = {
	255, 192, 144, 108, 81, 61, 46, 35
};

/* Divide with rounding towards the nearest integer, for negatives too. */
static int scale(int value, int by)
{
	long v = (long)value * by;

	if (v >= 0)
		return (int)((v + 500) / 1000);
	return -(int)((-v + 500) / 1000);
}

static int clamp(int v, int lo, int hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

void neodct_spinner_geometry(int screen_w, int screen_h,
			     struct neodct_spinner_geom *g)
{
	int side = screen_w < screen_h ? screen_w : screen_h;

	if (side < 1)
		side = 1;

	g->cx = screen_w / 2;
	g->cy = screen_h / 2;

	/* An eighth of the shorter side: on the 240x175 panel a ring 42
	 * pixels across, which is unmistakable without covering the frame
	 * it sits over when a stream stalls. The floor keeps it a ring on
	 * any screen that could still show one. */
	g->ring_r = clamp(side / 8, 4, 40);
	g->dot_r = clamp(g->ring_r / 5, 1, 6);
}

void neodct_spinner_dot_centre(const struct neodct_spinner_geom *g, int dot,
			       int *x, int *y)
{
	dot %= NEODCT_SPINNER_DOTS;
	if (dot < 0)
		dot += NEODCT_SPINNER_DOTS;

	*x = g->cx + scale(g->ring_r, COS8[dot]);
	*y = g->cy + scale(g->ring_r, SIN8[dot]);
}

int neodct_spinner_dot_level(int dot, int phase)
{
	int behind;

	dot %= NEODCT_SPINNER_DOTS;
	if (dot < 0)
		dot += NEODCT_SPINNER_DOTS;
	phase %= NEODCT_SPINNER_DOTS;
	if (phase < 0)
		phase += NEODCT_SPINNER_DOTS;

	/* The head moves clockwise, so the dot it just left is the one
	 * before it in index order. */
	behind = (phase - dot + NEODCT_SPINNER_DOTS) % NEODCT_SPINNER_DOTS;
	return LEVELS[behind];
}

int neodct_spinner_phase(int64_t elapsed_us)
{
	if (elapsed_us < 0)
		elapsed_us = 0;
	return (int)((elapsed_us / NEODCT_SPINNER_TICK_US) % NEODCT_SPINNER_DOTS);
}

bool neodct_spinner_covers(int dx, int dy, int dot_r)
{
	if (dot_r < 0)
		return false;
	/* r*r + r rather than r*r: the extra rounds the diagonals out, which
	 * at a radius of four is the difference between a dot and a diamond. */
	return dx * dx + dy * dy <= dot_r * dot_r + dot_r;
}

void neodct_spinner_bbox(const struct neodct_spinner_geom *g,
			 int screen_w, int screen_h,
			 int *x0, int *y0, int *x1, int *y1)
{
	int reach = g->ring_r + g->dot_r + 1;

	*x0 = clamp(g->cx - reach, 0, screen_w);
	*y0 = clamp(g->cy - reach, 0, screen_h);
	*x1 = clamp(g->cx + reach + 1, 0, screen_w);
	*y1 = clamp(g->cy + reach + 1, 0, screen_h);
}

uint32_t neodct_spinner_pixel(enum neodct_fb_pixfmt fmt, int level)
{
	uint32_t v = (uint32_t)clamp(level, 0, 255);

	switch (fmt) {
	case NEODCT_FB_RGB565:
		return ((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3);
	case NEODCT_FB_BGR0:
	case NEODCT_FB_RGB0:
		return v | (v << 8) | (v << 16);
	case NEODCT_FB_UNSUPPORTED:
		break;
	}
	return 0;
}

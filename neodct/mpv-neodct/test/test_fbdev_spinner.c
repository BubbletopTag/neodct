/*
 * Tests for fbdev_spinner: the loading ring's geometry.
 *
 * The ring is what the user sees for the several seconds mpv spends
 * opening a url over a mobile link, and what tells a stalled stream apart
 * from a crashed player. It has to be visible on the 240x175 panel, stay
 * inside it, keep turning, and never paint a pixel it cannot later repaint
 * from the clean frame -- which is what the bounding box is for.
 */

#include "test_util.h"
#include "../fbdev_spinner.h"

static void test_geometry_fits_the_panel(void)
{
	struct neodct_spinner_geom g;
	int d;

	neodct_spinner_geometry(240, 175, &g);
	CHECK_INT(g.cx, 120);
	CHECK_INT(g.cy, 87);
	/* big enough to see, small enough to sit over a frame */
	CHECK(g.ring_r >= 16);
	CHECK(g.ring_r <= 30);
	CHECK(g.dot_r >= 3);
	CHECK(g.dot_r <= 5);

	/* every dot, dot radius included, is inside the screen */
	for (d = 0; d < NEODCT_SPINNER_DOTS; d++) {
		int x, y;
		neodct_spinner_dot_centre(&g, d, &x, &y);
		CHECK(x - g.dot_r >= 0);
		CHECK(y - g.dot_r >= 0);
		CHECK(x + g.dot_r < 240);
		CHECK(y + g.dot_r < 175);
	}

	/* the square 240x240 hardware panel gets a ring centred on it */
	neodct_spinner_geometry(240, 240, &g);
	CHECK_INT(g.cx, 120);
	CHECK_INT(g.cy, 120);

	/* and a screen too small for a ring still gets a valid one */
	neodct_spinner_geometry(8, 8, &g);
	CHECK(g.ring_r >= 1);
	CHECK(g.dot_r >= 1);
	neodct_spinner_geometry(0, 0, &g);
	CHECK(g.ring_r >= 1);
}

static void test_dots_go_round_clockwise(void)
{
	struct neodct_spinner_geom g;
	int x, y, i, j;

	neodct_spinner_geometry(240, 175, &g);

	/* dot 0 is at 3 o'clock, 2 at the bottom, 4 at 9 o'clock, 6 at the
	 * top: clockwise on a screen whose y grows downwards */
	neodct_spinner_dot_centre(&g, 0, &x, &y);
	CHECK_INT(x, g.cx + g.ring_r);
	CHECK_INT(y, g.cy);
	neodct_spinner_dot_centre(&g, 2, &x, &y);
	CHECK_INT(x, g.cx);
	CHECK_INT(y, g.cy + g.ring_r);
	neodct_spinner_dot_centre(&g, 4, &x, &y);
	CHECK_INT(x, g.cx - g.ring_r);
	CHECK_INT(y, g.cy);
	neodct_spinner_dot_centre(&g, 6, &x, &y);
	CHECK_INT(x, g.cx);
	CHECK_INT(y, g.cy - g.ring_r);

	/* the diagonals sit on the ring too, to within a pixel */
	neodct_spinner_dot_centre(&g, 1, &x, &y);
	CHECK(x > g.cx && y > g.cy);
	{
		int dx = x - g.cx, dy = y - g.cy;
		int rr = dx * dx + dy * dy;
		CHECK(rr >= (g.ring_r - 1) * (g.ring_r - 1));
		CHECK(rr <= (g.ring_r + 1) * (g.ring_r + 1));
	}

	/* no two dots share a centre, and none overlap */
	for (i = 0; i < NEODCT_SPINNER_DOTS; i++) {
		for (j = i + 1; j < NEODCT_SPINNER_DOTS; j++) {
			int xi, yi, xj, yj, dx, dy;
			neodct_spinner_dot_centre(&g, i, &xi, &yi);
			neodct_spinner_dot_centre(&g, j, &xj, &yj);
			dx = xi - xj;
			dy = yi - yj;
			CHECK(dx * dx + dy * dy >
			      (2 * g.dot_r) * (2 * g.dot_r));
		}
	}

	/* an index off the end wraps rather than reading off the table */
	neodct_spinner_dot_centre(&g, NEODCT_SPINNER_DOTS, &x, &y);
	CHECK_INT(x, g.cx + g.ring_r);
	{
		int x7, y7;
		neodct_spinner_dot_centre(&g, 7, &x7, &y7);
		neodct_spinner_dot_centre(&g, -1, &x, &y);
		CHECK_INT(x, x7);
		CHECK_INT(y, y7);
	}
}

static void test_brightness_trails_the_head(void)
{
	int phase, d;

	for (phase = 0; phase < NEODCT_SPINNER_DOTS; phase++) {
		int prev = 256;

		/* the head is fully bright */
		CHECK_INT(neodct_spinner_dot_level(phase, phase), 255);

		/* walking backwards from the head, each dot is dimmer than
		 * the last, and none goes dark */
		for (d = 0; d < NEODCT_SPINNER_DOTS; d++) {
			int dot = (phase - d + NEODCT_SPINNER_DOTS) %
				  NEODCT_SPINNER_DOTS;
			int level = neodct_spinner_dot_level(dot, phase);
			CHECK(level < prev || d == 0);
			CHECK(level >= 32);
			CHECK(level <= 255);
			prev = level;
		}

		/* the dot just AHEAD of the head is the dimmest: it is the
		 * one the head reached longest ago */
		CHECK_INT(neodct_spinner_dot_level(phase + 1, phase),
			  neodct_spinner_dot_level(phase - 7, phase));
	}

	/* indices wrap, so the caller can pass a raw phase counter */
	CHECK_INT(neodct_spinner_dot_level(0, 8), 255);
	CHECK_INT(neodct_spinner_dot_level(8, 0), 255);
	CHECK_INT(neodct_spinner_dot_level(-1, 7), 255);
}

static void test_phase_advances_with_time(void)
{
	/* one step per tick, round the ring, and never off it */
	CHECK_INT(neodct_spinner_phase(0), 0);
	CHECK_INT(neodct_spinner_phase(NEODCT_SPINNER_TICK_US - 1), 0);
	CHECK_INT(neodct_spinner_phase(NEODCT_SPINNER_TICK_US), 1);
	CHECK_INT(neodct_spinner_phase(7 * NEODCT_SPINNER_TICK_US), 7);
	CHECK_INT(neodct_spinner_phase(8 * NEODCT_SPINNER_TICK_US), 0);
	CHECK_INT(neodct_spinner_phase(9 * NEODCT_SPINNER_TICK_US), 1);

	/* a full turn is under a second: slow enough to see, fast enough to
	 * look alive */
	CHECK(NEODCT_SPINNER_DOTS * NEODCT_SPINNER_TICK_US <= 1000000);
	CHECK(NEODCT_SPINNER_DOTS * NEODCT_SPINNER_TICK_US >= 500000);

	/* a clock that went backwards is not a crash */
	CHECK_INT(neodct_spinner_phase(-5), 0);

	/* the grace before a stall shows the ring is shorter than a stall
	 * anyone would notice, and longer than a cached seek */
	CHECK(NEODCT_SPINNER_GRACE_US >= 200000);
	CHECK(NEODCT_SPINNER_GRACE_US <= 500000);
}

static void test_dot_coverage_is_round(void)
{
	int r = 4;
	int dx, dy, count = 0;

	/* centre and axes are in; the far corners are out */
	CHECK(neodct_spinner_covers(0, 0, r));
	CHECK(neodct_spinner_covers(r, 0, r));
	CHECK(neodct_spinner_covers(0, -r, r));
	CHECK(!neodct_spinner_covers(r, r, r));
	CHECK(!neodct_spinner_covers(r + 1, 0, r));

	/* symmetric in all four quadrants */
	for (dy = -r; dy <= r; dy++) {
		for (dx = -r; dx <= r; dx++) {
			bool c = neodct_spinner_covers(dx, dy, r);
			CHECK(c == neodct_spinner_covers(-dx, dy, r));
			CHECK(c == neodct_spinner_covers(dx, -dy, r));
			CHECK(c == neodct_spinner_covers(dy, dx, r));
			if (c)
				count++;
		}
	}
	/* somewhere between a diamond (41) and the full square (81) */
	CHECK(count > 41);
	CHECK(count < 81);

	/* a one-pixel dot is exactly one pixel */
	CHECK(neodct_spinner_covers(0, 0, 0));
	CHECK(!neodct_spinner_covers(1, 0, 0));
	CHECK(!neodct_spinner_covers(0, 0, -1));
}

static void test_bbox_contains_every_dot_and_stays_on_screen(void)
{
	struct neodct_spinner_geom g;
	int x0, y0, x1, y1, d;

	neodct_spinner_geometry(240, 175, &g);
	neodct_spinner_bbox(&g, 240, 175, &x0, &y0, &x1, &y1);
	CHECK(x0 >= 0 && y0 >= 0);
	CHECK(x1 <= 240 && y1 <= 175);
	CHECK(x1 > x0 && y1 > y0);

	for (d = 0; d < NEODCT_SPINNER_DOTS; d++) {
		int x, y;
		neodct_spinner_dot_centre(&g, d, &x, &y);
		CHECK(x - g.dot_r >= x0);
		CHECK(y - g.dot_r >= y0);
		CHECK(x + g.dot_r < x1);
		CHECK(y + g.dot_r < y1);
	}

	/* a ring wider than its screen is clamped, not drawn off the edge */
	g.cx = 4;
	g.cy = 4;
	neodct_spinner_bbox(&g, 240, 175, &x0, &y0, &x1, &y1);
	CHECK_INT(x0, 0);
	CHECK_INT(y0, 0);
	CHECK(x1 <= 240);
}

static void test_grey_packs_for_every_layout(void)
{
	/* 565: white is all ones, black all zeros, mid grey lands mid range */
	CHECK_INT(neodct_spinner_pixel(NEODCT_FB_RGB565, 255), 0xffff);
	CHECK_INT(neodct_spinner_pixel(NEODCT_FB_RGB565, 0), 0);
	CHECK_INT(neodct_spinner_pixel(NEODCT_FB_RGB565, 128),
		  (16 << 11) | (32 << 5) | 16);

	/* 32-bit: one byte per channel, the same in either byte order,
	 * which is why the ring is grey and not NeoDCT green */
	CHECK_INT(neodct_spinner_pixel(NEODCT_FB_BGR0, 255), 0x00ffffff);
	CHECK_INT(neodct_spinner_pixel(NEODCT_FB_RGB0, 255), 0x00ffffff);
	CHECK_INT(neodct_spinner_pixel(NEODCT_FB_BGR0, 0x40), 0x00404040);
	CHECK_INT(neodct_spinner_pixel(NEODCT_FB_RGB0, 0x40), 0x00404040);

	/* out of range levels clamp rather than wrap into a colour */
	CHECK_INT(neodct_spinner_pixel(NEODCT_FB_BGR0, 300), 0x00ffffff);
	CHECK_INT(neodct_spinner_pixel(NEODCT_FB_BGR0, -3), 0);

	/* a layout the output refused draws nothing */
	CHECK_INT(neodct_spinner_pixel(NEODCT_FB_UNSUPPORTED, 255), 0);
}

int main(void)
{
	test_geometry_fits_the_panel();
	test_dots_go_round_clockwise();
	test_brightness_trails_the_head();
	test_phase_advances_with_time();
	test_dot_coverage_is_round();
	test_bbox_contains_every_dot_and_stays_on_screen();
	test_grey_packs_for_every_layout();
	TEST_EXIT();
}

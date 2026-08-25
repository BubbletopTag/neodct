/*
 * Tests for fbdev_format: what a Linux framebuffer's pixel layout is.
 *
 * Split out of vo_fbdev.c because getting this wrong is silent -- the
 * picture appears, in the wrong colours, and looks like a decoder bug.
 * The values come from struct fb_var_screeninfo on the two configurations
 * NeoDCT actually boots: the ST7789 panel at 16bpp, and the same panel
 * after neodct_displayd switches it to 32bpp.
 */

#include "test_util.h"
#include "../fbdev_format.h"

int main(void)
{
	/* the panel as the kernel brings it up */
	CHECK_INT(neodct_fb_pixfmt_of(16, 11, 5, 0), NEODCT_FB_RGB565);

	/* and after displayd switches it: 32bpp, blue in the low byte */
	CHECK_INT(neodct_fb_pixfmt_of(32, 16, 8, 0), NEODCT_FB_BGR0);

	/* the other 32bpp byte order, as QEMU's virtio-gpu can report */
	CHECK_INT(neodct_fb_pixfmt_of(32, 0, 8, 16), NEODCT_FB_RGB0);

	/* 24bpp has no mpv format that matches without a repack, and no
	 * NeoDCT panel produces it; refusing is better than a smear */
	CHECK_INT(neodct_fb_pixfmt_of(24, 16, 8, 0), NEODCT_FB_UNSUPPORTED);

	/* 16bpp that is not 565 -- 555 with a spare bit -- is not ours */
	CHECK_INT(neodct_fb_pixfmt_of(16, 10, 5, 0), NEODCT_FB_UNSUPPORTED);

	/* nonsense from a driver that did not fill the struct in */
	CHECK_INT(neodct_fb_pixfmt_of(0, 0, 0, 0), NEODCT_FB_UNSUPPORTED);
	CHECK_INT(neodct_fb_pixfmt_of(8, 0, 0, 0), NEODCT_FB_UNSUPPORTED);
	CHECK_INT(neodct_fb_pixfmt_of(-1, 0, 0, 0), NEODCT_FB_UNSUPPORTED);

	/* bytes per pixel, so the blit strides are right */
	CHECK_INT(neodct_fb_bytes_per_pixel(NEODCT_FB_RGB565), 2);
	CHECK_INT(neodct_fb_bytes_per_pixel(NEODCT_FB_BGR0), 4);
	CHECK_INT(neodct_fb_bytes_per_pixel(NEODCT_FB_RGB0), 4);
	CHECK_INT(neodct_fb_bytes_per_pixel(NEODCT_FB_UNSUPPORTED), 0);

	/* a name for the log, so a bad panel is diagnosable from serial */
	CHECK_STR(neodct_fb_pixfmt_name(NEODCT_FB_RGB565), "rgb565");
	CHECK_STR(neodct_fb_pixfmt_name(NEODCT_FB_BGR0), "bgr0");
	CHECK_STR(neodct_fb_pixfmt_name(NEODCT_FB_RGB0), "rgb0");
	CHECK_STR(neodct_fb_pixfmt_name(NEODCT_FB_UNSUPPORTED), "unsupported");

	TEST_EXIT();
}

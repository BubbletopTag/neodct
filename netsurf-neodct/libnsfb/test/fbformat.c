/*
 * Test for the fb_var_screeninfo -> nsfb format decision used by the linux
 * framebuffer surface.
 *
 * Standalone logic test: needs no surface, exits non-zero on failure.
 *
 * The two configurations below are the ones NeoDCT actually boots, and they
 * disagree about where red is. Getting this wrong does not fail, it renders:
 * every page comes out with red and blue exchanged, which reads as a decoder
 * fault rather than as one line in a surface handler.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "libnsfb.h"

#include "../src/surface/linux_format.h"

static int failures = 0;

static void check(const char *what, int bpp, int red, int blue, int transp,
		  enum nsfb_format_e want)
{
	enum nsfb_format_e got = nsfb_linux_format_of(bpp, red, blue, transp);

	if (got != want) {
		printf("FAIL %s: got %d want %d\n", what, (int)got, (int)want);
		failures++;
	}
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	/* QEMU: virtio-gpu through DRM's fbdev emulation. depth 24 in a
	 * 32bpp mode, so no alpha; bytes B G R x.
	 * (drm_fb_helper_fill_pixel_fmt, the depth 24 case.) */
	check("drm xrgb8888", 32, 16, 0, 0, NSFB_FMT_XRGB8888);
	check("drm argb8888", 32, 16, 0, 8, NSFB_FMT_ARGB8888);

	/* The phone: the kernel's vfb, which neodct_displayd mirrors to the
	 * ST7789 over SPI. Bytes R G B a, alpha and all.
	 * (drivers/video/fbdev/vfb.c, the 32bpp case of vfb_check_var.) */
	check("vfb abgr8888", 32, 0, 16, 8, NSFB_FMT_ABGR8888);
	check("vfb xbgr8888", 32, 0, 16, 0, NSFB_FMT_XBGR8888);

	/* A driver that filled none of the masks in has said nothing, and
	 * gets the answer it always used to get rather than a coin toss. */
	check("masks unset", 32, 0, 0, 0, NSFB_FMT_XRGB8888);

	/* Everything below 32 is unchanged: 16bpp is assumed 565 with red at
	 * the top because there is no other 16bpp plotter to pick. */
	check("24bpp", 24, 16, 0, 0, NSFB_FMT_RGB888);
	check("16bpp 565", 16, 11, 0, 0, NSFB_FMT_RGB565);
	check("16bpp blue first", 16, 0, 11, 0, NSFB_FMT_RGB565);
	check("8bpp", 8, 0, 0, 0, NSFB_FMT_I8);
	check("1bpp", 1, 0, 0, 0, NSFB_FMT_RGB565);

	/* A depth with no plotters behind it must not name a format: the
	 * caller checks for ANY and refuses the surface. */
	check("4bpp", 4, 0, 0, 0, NSFB_FMT_ANY);
	check("0bpp", 0, 0, 0, 0, NSFB_FMT_ANY);

	if (failures != 0) {
		printf("fbformat: %d failures\n", failures);
		return 1;
	}
	printf("fbformat: all checks passed\n");
	return 0;
}

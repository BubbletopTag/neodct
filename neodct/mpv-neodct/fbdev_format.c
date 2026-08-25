/*
 * NeoDCT: what pixel layout a Linux framebuffer is using.
 */

#include "fbdev_format.h"

enum neodct_fb_pixfmt neodct_fb_pixfmt_of(int bpp, int r_off, int g_off,
					  int b_off)
{
	if (bpp == 16) {
		/* Only 565. A 555 panel would need its own conversion and
		 * no NeoDCT hardware produces one. */
		if (r_off == 11 && g_off == 5 && b_off == 0)
			return NEODCT_FB_RGB565;
		return NEODCT_FB_UNSUPPORTED;
	}

	if (bpp == 32) {
		if (r_off == 16 && g_off == 8 && b_off == 0)
			return NEODCT_FB_BGR0;
		if (r_off == 0 && g_off == 8 && b_off == 16)
			return NEODCT_FB_RGB0;
		return NEODCT_FB_UNSUPPORTED;
	}

	/* 24bpp packed has no mpv format that blits without a repack, and
	 * 8bpp means a palette this has no way to set. */
	return NEODCT_FB_UNSUPPORTED;
}

int neodct_fb_bytes_per_pixel(enum neodct_fb_pixfmt fmt)
{
	switch (fmt) {
	case NEODCT_FB_RGB565:
		return 2;
	case NEODCT_FB_BGR0:
	case NEODCT_FB_RGB0:
		return 4;
	case NEODCT_FB_UNSUPPORTED:
		break;
	}
	return 0;
}

const char *neodct_fb_pixfmt_name(enum neodct_fb_pixfmt fmt)
{
	switch (fmt) {
	case NEODCT_FB_RGB565:
		return "rgb565";
	case NEODCT_FB_BGR0:
		return "bgr0";
	case NEODCT_FB_RGB0:
		return "rgb0";
	case NEODCT_FB_UNSUPPORTED:
		break;
	}
	return "unsupported";
}

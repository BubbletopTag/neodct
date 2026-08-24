/*
 * NeoDCT: what pixel layout a Linux framebuffer is using.
 *
 * Split out of vo_fbdev.c so it can be tested without a framebuffer. The
 * failure mode this guards against is silent: guess the layout wrong and
 * the video plays with red and blue swapped, which looks like a decoder
 * fault rather than a two-line mistake in a VO.
 *
 * Deliberately free of mpv headers -- the caller maps these onto IMGFMT_*.
 */

#ifndef NEODCT_FBDEV_FORMAT_H
#define NEODCT_FBDEV_FORMAT_H

enum neodct_fb_pixfmt {
	NEODCT_FB_UNSUPPORTED = 0,
	NEODCT_FB_RGB565,  /**< 16bpp, 5r 6g 5b in one little-endian word */
	NEODCT_FB_BGR0,    /**< 32bpp, bytes B G R x */
	NEODCT_FB_RGB0     /**< 32bpp, bytes R G B x */
};

/**
 * Classify a framebuffer from its fb_var_screeninfo fields.
 *
 * \param bpp    bits_per_pixel
 * \param r_off  red.offset, in bits from the least significant
 * \param g_off  green.offset
 * \param b_off  blue.offset
 */
enum neodct_fb_pixfmt neodct_fb_pixfmt_of(int bpp, int r_off, int g_off,
					  int b_off);

/** bytes one pixel occupies, or 0 when the format is not supported */
int neodct_fb_bytes_per_pixel(enum neodct_fb_pixfmt fmt);

/** a short name, for the serial log */
const char *neodct_fb_pixfmt_name(enum neodct_fb_pixfmt fmt);

#endif

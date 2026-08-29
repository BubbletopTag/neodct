/*
 * What pixel format a Linux framebuffer is in, from what its driver says.
 *
 * Split out of linux.c so it can be tested without a framebuffer -- the same
 * split, for the same reason, as linux_evdev.[ch] next door. The failure this
 * guards against is silent: guess the channel order wrong and pages render in
 * the wrong colours, which looks like a decoding fault rather than one line
 * in a surface handler.
 */

#ifndef LIBNSFB_LINUX_FORMAT_H
#define LIBNSFB_LINUX_FORMAT_H

#include "libnsfb.h"

/**
 * Choose a surface format from fb_var_screeninfo.
 *
 * A bit depth says how wide a pixel is and nothing about where red sits
 * inside it, so the offsets decide that and the depth decides the rest.
 * Red BELOW blue means red comes first in memory; equal offsets mean the
 * driver filled in nothing and the historic blue-first answer stands.
 *
 * \param bpp           bits_per_pixel
 * \param red_offset    red.offset, in bits from the least significant
 * \param blue_offset   blue.offset
 * \param transp_length transp.length; nonzero picks the alpha-bearing format
 * \return the surface format, or NSFB_FMT_ANY for a depth with no plotters
 */
enum nsfb_format_e nsfb_linux_format_of(int bpp, int red_offset, int blue_offset,
					int transp_length);

#endif

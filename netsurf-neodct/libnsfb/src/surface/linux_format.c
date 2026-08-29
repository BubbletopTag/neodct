/*
 * What pixel format a Linux framebuffer is in, from what its driver says.
 */

#include <stdbool.h>

#include "libnsfb.h"

#include "linux_format.h"

enum nsfb_format_e nsfb_linux_format_of(int bpp, int red_offset, int blue_offset,
					int transp_length)
{
	bool red_first = (red_offset < blue_offset);

	switch (bpp) {
	case 32:
		if (transp_length == 0)
			return red_first ? NSFB_FMT_XBGR8888 : NSFB_FMT_XRGB8888;
		return red_first ? NSFB_FMT_ABGR8888 : NSFB_FMT_ARGB8888;

	case 24:
		return NSFB_FMT_RGB888;

	case 16:
		/* Assumed to be 565 with red at the top whatever the offsets
		 * say: libnsfb has no blue-first 16bpp plotter to select even
		 * when a driver asks for one. Naming the assumption is as far
		 * as this can honestly go. */
		return NSFB_FMT_RGB565;

	case 8:
		return NSFB_FMT_I8;

	case 1:
		return NSFB_FMT_RGB565;
	}

	return NSFB_FMT_ANY;
}

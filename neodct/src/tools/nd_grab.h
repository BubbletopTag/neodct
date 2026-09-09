/* nd_grab.h -- the framebuffer-to-image half of nd-grab.
 *
 * Split from main() for the same reason nd_key.h is: the conversion is where
 * the bug would be, and a bug here does not crash. It produces a picture that
 * looks plausible and has red and blue the wrong way round.
 *
 * ============ THE TRAP THIS FILE EXISTS TO AVOID ============
 *
 * The phone's fb0 is the kernel's vfb, which at 32 bpp declares
 * red.offset 0 -- bytes R G B x. Almost everything else on earth declares
 * red.offset 16, i.e. B G R x. At 16 bpp vfb is likewise blue-last.
 *
 * Both halves of NeoDCT once assumed B G R x. Being wrong TOGETHER they
 * looked right to each other, and every program that believed the driver --
 * mpv, NetSurf through libnsfb, the framebuffer console -- came out with red
 * and blue swapped. It cost a release.
 *
 * So the channel order is never assumed. It is read from fb_var_screeninfo
 * and reduced to one flag, exactly as neodct_displayd does it:
 *
 *     swap_rb = (vinfo.red.offset < vinfo.blue.offset)
 *
 * and --swap-rb INVERTS that detection rather than selecting an order, so the
 * flag is for a driver that fills the struct in wrongly and not for guessing.
 */

#ifndef ND_GRAB_H_INCLUDED
#define ND_GRAB_H_INCLUDED

#include "nd_image.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Everything needed to read a framebuffer, and nothing about where it came
 * from -- so a test can describe a fixture without a device. */
typedef struct {
    int32_t w;
    int32_t h;
    uint32_t bytespp; /* 2 or 4; anything else is refused before we get here */
    size_t stride;    /* finfo.line_length. NEVER w * bytespp: rows pad.     */
    bool swap_rb;     /* already resolved, including any --swap-rb inversion */
} nd_grab_fmt;

/* True when bytespp is one this tool understands. 16 and 32 bpp only: those
 * are what the phone and QEMU produce, and rendering anything else would be
 * guessing at a layout nobody has seen. */
bool nd_grab_fmt_supported(uint32_t bytespp);

/* Convert a raw framebuffer into a fresh RGB888 image, or NULL.
 *
 * `fb` must hold at least fmt->stride * fmt->h bytes. The result is tightly
 * packed regardless of the source stride, which is what makes the digest
 * comparable between a padded framebuffer and an unpadded one.
 *
 * Owned by the caller; free with nd_image_free(). */
nd_image *nd_grab_to_image(const uint8_t *fb, const nd_grab_fmt *fmt);

#ifdef __cplusplus
}
#endif

#endif /* ND_GRAB_H_INCLUDED */

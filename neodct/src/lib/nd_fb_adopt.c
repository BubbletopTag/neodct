/* nd_fb_adopt.c -- wrap a framebuffer descriptor somebody else opened.
 *
 * nd_fb_open() opens /dev/fb0. An app process must not need permission to do
 * that (nd_app.h, SECURITY.md): the core opens the device once and passes the
 * descriptor down as NEODCT_FB_FD, and this turns that number back into an
 * nd_fb without a second open().
 *
 * TWO DELIBERATE DIFFERENCES FROM nd_fb_open():
 *
 *   - the mapping is NOT zeroed. nd_fb.h explains that the device path zeroes
 *     once at open so that later partial-band writes leave the letterbox rows
 *     black. That already happened, in the core, before this process existed.
 *     Zeroing again would blank the panel between the app starting and its
 *     first frame, which is a visible flash the Python never had.
 *   - the geometry is re-queried rather than assumed, because the descriptor
 *     is the only thing that crossed the boundary and a driver may have been
 *     reconfigured since.
 *
 * Kept out of nd_fb.c on purpose: that file is the device driver and this is
 * the process-boundary helper that happens to build the same struct.
 */

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/fb.h>

#include "nd_app.h"
#include "nd_fb_priv.h"
#include "nd_log.h"
#include "nd_types.h"

#include <stdlib.h>

nd_err nd_fb_adopt_fd(nd_fb **out, int fd)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    struct nd_fb *fb = NULL;
    nd_err rc;

    if (out == NULL || fd < 0)
        return ND_ERR_INVAL;
    *out = NULL;

    memset(&var, 0, sizeof var);
    memset(&fix, 0, sizeof fix);

    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0) {
        nd_log_err(ND_LOG_FB, "inherited fd %d: FBIOGET_VSCREENINFO: %s", fd, strerror(errno));
        return ND_ERR_IO;
    }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
        nd_log_err(ND_LOG_FB, "inherited fd %d: FBIOGET_FSCREENINFO: %s", fd, strerror(errno));
        return ND_ERR_IO;
    }

    fb = calloc(1u, sizeof *fb);
    if (fb == NULL)
        return ND_ERR_NOMEM;

    fb->backend = ND_FB_BACKEND_DEVICE;
    fb->fd = fd;

    rc = nd_fb_derive_geometry(fb, (int32_t)var.xres, (int32_t)var.yres,
                               (int32_t)var.bits_per_pixel, (size_t)fix.line_length);
    if (rc != ND_OK) {
        free(fb);
        return rc;
    }

    fb->mem = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb->mem == MAP_FAILED) {
        nd_log_err(ND_LOG_FB, "inherited fd %d: mmap %zu bytes: %s", fd, fb->size,
                   strerror(errno));
        fb->mem = NULL;
        free(fb);
        return ND_ERR_IO;
    }

    nd_log(ND_LOG_FB, "inherited %dx%d @ %dbpp from the core", fb->xres, fb->yres, fb->bpp);
    *out = fb;
    return ND_OK;
}

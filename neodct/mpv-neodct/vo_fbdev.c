/*
 * NeoDCT: video output straight to a Linux framebuffer.
 *
 * mpv dropped its fbdev output long before 0.35, leaving --vo=drm as the
 * only way onto a screen with no window system. The Luckfox Pico Mini B
 * has no DRM driver at all: its panel is an ST7789 hanging off SPI, and
 * the kernel exposes it as a plain /dev/fb0 and nothing else. So the
 * output has to come back.
 *
 * It is deliberately the simplest thing that can work. One buffer, written
 * straight into the mapping the kernel handed us -- no page flipping,
 * because there is no second buffer to flip to and no vblank to flip on.
 * Tearing is possible in principle; at 240x175 on a panel refreshed over
 * SPI it is not the thing anyone notices.
 *
 * Scaling and colour conversion go through mp_sws, the same as vo_drm.
 * There is no hardware on this board to do either.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/fb.h>

#include "common/msg.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "sub/osd.h"
#include <libswscale/swscale.h>

#include "video/fmt-conversion.h"
#include "video/mp_image.h"
#include "video/sws_utils.h"
#include "vo.h"

#include "fbdev_format.h"

#define DEFAULT_DEVICE "/dev/fb0"

struct fbdev_opts {
    char *device;
};

#define OPT_BASE_STRUCT struct fbdev_opts

static const struct m_sub_options vo_fbdev_conf = {
    .opts = (const struct m_option[]) {
        {"fbdev-device", OPT_STRING(device), .flags = M_OPT_FILE},
        {0},
    },
    .size = sizeof(struct fbdev_opts),
    .defaults = &(const struct fbdev_opts) {
        .device = DEFAULT_DEVICE,
    },
};

struct priv {
    struct fbdev_opts *opts;

    int fd;
    uint8_t *map;
    size_t map_size;

    int screen_w, screen_h;
    size_t stride;              // bytes per line, from the driver
    enum neodct_fb_pixfmt pixfmt;
    int imgfmt;                 // the matching IMGFMT_*
    int bytes_per_pixel;

    struct mp_sws_context *sws;
    struct mp_image *cur_frame;         // whole screen
    struct mp_image *cur_frame_cropped; // just the video rectangle
    struct mp_image *last_input;

    struct mp_rect src, dst;
    struct mp_osd_res osd;

    bool active;
};

static int imgfmt_for(enum neodct_fb_pixfmt fmt)
{
    switch (fmt) {
    case NEODCT_FB_RGB565:
        return IMGFMT_RGB565;
    case NEODCT_FB_BGR0:
        return IMGFMT_BGR0;
    case NEODCT_FB_RGB0:
        return IMGFMT_RGB0;
    case NEODCT_FB_UNSUPPORTED:
        break;
    }
    return 0;
}

static void unmap_fb(struct priv *p)
{
    if (p->map && p->map != MAP_FAILED)
        munmap(p->map, p->map_size);
    p->map = NULL;
    if (p->fd >= 0)
        close(p->fd);
    p->fd = -1;
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;

    p->fd = -1;
    p->opts = mp_get_config_group(vo, vo->global, &vo_fbdev_conf);

    const char *device = p->opts->device ? p->opts->device : DEFAULT_DEVICE;

    p->fd = open(device, O_RDWR | O_CLOEXEC);
    if (p->fd < 0) {
        MP_ERR(vo, "Cannot open %s: %s\n", device, mp_strerror(errno));
        return -1;
    }

    if (ioctl(p->fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(p->fd, FBIOGET_FSCREENINFO, &fix) < 0)
    {
        MP_ERR(vo, "%s does not answer the framebuffer ioctls: %s\n",
               device, mp_strerror(errno));
        goto fail;
    }

    p->pixfmt = neodct_fb_pixfmt_of(var.bits_per_pixel, var.red.offset,
                                    var.green.offset, var.blue.offset);
    p->imgfmt = imgfmt_for(p->pixfmt);
    if (p->imgfmt == 0) {
        MP_ERR(vo, "%s: unsupported pixel layout "
                   "(%dbpp r=%d g=%d b=%d)\n",
               device, var.bits_per_pixel, var.red.offset,
               var.green.offset, var.blue.offset);
        goto fail;
    }
    p->bytes_per_pixel = neodct_fb_bytes_per_pixel(p->pixfmt);

    p->screen_w = var.xres;
    p->screen_h = var.yres;
    p->stride = fix.line_length;
    if (p->stride == 0)
        p->stride = (size_t)p->screen_w * p->bytes_per_pixel;

    if (p->screen_w <= 0 || p->screen_h <= 0) {
        MP_ERR(vo, "%s reports a %dx%d screen\n", device,
               p->screen_w, p->screen_h);
        goto fail;
    }

    p->map_size = p->stride * (size_t)p->screen_h;
    p->map = mmap(NULL, p->map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  p->fd, 0);
    if (p->map == MAP_FAILED) {
        MP_ERR(vo, "Cannot map %s: %s\n", device, mp_strerror(errno));
        p->map = NULL;
        goto fail;
    }

    MP_INFO(vo, "%s: %dx%d %s, %zu bytes per line\n", device,
            p->screen_w, p->screen_h,
            neodct_fb_pixfmt_name(p->pixfmt), p->stride);

    p->sws = mp_sws_alloc(vo);
    p->sws->log = vo->log;
    mp_sws_enable_cmdline_opts(p->sws, vo->global);

    p->active = true;
    return 0;

fail:
    unmap_fb(p);
    return -1;
}

static int query_format(struct vo *vo, int format)
{
    (void)vo;
    return sws_isSupportedInput(imgfmt2pixfmt(format));
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *p = vo->priv;

    vo->dwidth = p->screen_w;
    vo->dheight = p->screen_h;
    vo_get_src_dst_rects(vo, &p->src, &p->dst, &p->osd);

    p->sws->src = *params;
    p->sws->dst = (struct mp_image_params) {
        .imgfmt = p->imgfmt,
        .w = p->dst.x1 - p->dst.x0,
        .h = p->dst.y1 - p->dst.y0,
        .p_w = 1,
        .p_h = 1,
    };

    talloc_free(p->cur_frame);
    p->cur_frame = mp_image_alloc(p->imgfmt, p->screen_w, p->screen_h);
    if (!p->cur_frame)
        return -1;
    mp_image_params_guess_csp(&p->sws->dst);
    mp_image_set_params(p->cur_frame, &p->sws->dst);
    mp_image_set_size(p->cur_frame, p->screen_w, p->screen_h);

    talloc_free(p->cur_frame_cropped);
    p->cur_frame_cropped = mp_image_new_dummy_ref(p->cur_frame);
    mp_image_crop_rc(p->cur_frame_cropped, p->dst);

    talloc_free(p->last_input);
    p->last_input = NULL;

    if (mp_sws_reinit(p->sws) < 0)
        return -1;

    return 0;
}

static void draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;

    if (!p->active)
        return;

    struct mp_image *mpi = frame->current ?
        mp_image_new_ref(frame->current) : NULL;

    if (mpi) {
        struct mp_image src = *mpi;
        struct mp_rect src_rc = p->src;
        src_rc.x0 = MP_ALIGN_DOWN(src_rc.x0, mpi->fmt.align_x);
        src_rc.y0 = MP_ALIGN_DOWN(src_rc.y0, mpi->fmt.align_y);
        mp_image_crop_rc(&src, src_rc);

        // The letterbox. Cleared every frame rather than once, because the
        // rectangle moves when the window is reconfigured and a stale band
        // of the previous video is worse than the cost of clearing it.
        mp_image_clear(p->cur_frame, 0, 0, p->cur_frame->w, p->dst.y0);
        mp_image_clear(p->cur_frame, 0, p->dst.y1,
                       p->cur_frame->w, p->cur_frame->h);
        mp_image_clear(p->cur_frame, 0, p->dst.y0, p->dst.x0, p->dst.y1);
        mp_image_clear(p->cur_frame, p->dst.x1, p->dst.y0,
                       p->cur_frame->w, p->dst.y1);

        mp_sws_scale(p->sws, p->cur_frame_cropped, &src);
        osd_draw_on_image(vo->osd, p->osd, src.pts, 0, p->cur_frame);
    } else {
        mp_image_clear(p->cur_frame, 0, 0, p->cur_frame->w, p->cur_frame->h);
        osd_draw_on_image(vo->osd, p->osd, 0, 0, p->cur_frame);
    }

    memcpy_pic(p->map, p->cur_frame->planes[0],
               (size_t)p->screen_w * p->bytes_per_pixel, p->screen_h,
               p->stride, p->cur_frame->stride[0]);

    if (mpi != p->last_input) {
        talloc_free(p->last_input);
        p->last_input = mpi;
    }
}

static void flip_page(struct vo *vo)
{
    // Nothing to flip: draw_frame wrote into the visible mapping. Kept
    // because the vo interface expects it to exist.
    (void)vo;
}

static int control(struct vo *vo, uint32_t request, void *arg)
{
    (void)arg;

    switch (request) {
    case VOCTRL_SET_PANSCAN:
        // The video rectangle is recomputed on the next reconfig.
        if (vo->config_ok)
            reconfig(vo, vo->params);
        return VO_TRUE;
    }

    return VO_NOTIMPL;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;

    p->active = false;

    // Leave a black screen rather than the last frame: the caller is about
    // to draw its own interface over this, and a frozen video frame
    // showing through while it does looks like a crash.
    if (p->map)
        memset(p->map, 0, p->map_size);

    talloc_free(p->last_input);
    p->last_input = NULL;
    talloc_free(p->cur_frame_cropped);
    p->cur_frame_cropped = NULL;
    talloc_free(p->cur_frame);
    p->cur_frame = NULL;

    unmap_fb(p);
}

const struct vo_driver video_out_fbdev = {
    .name = "fbdev",
    .description = "Linux framebuffer (software scaling)",
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
    .global_opts = &vo_fbdev_conf,
};

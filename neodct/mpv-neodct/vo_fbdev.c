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
 *
 * ============ THE LOADING RING ============
 *
 * The one thing this output does that vo_drm does not is show that
 * something is happening when there is no frame to show. On the phone the
 * screen is handed to mpv the moment play is pressed, and over a mobile
 * link mpv can then take several seconds to open the url -- seconds of
 * black, on a device with no other indicator, which reads as "it did not
 * work". The ring (fbdev_spinner.h) turns during that wait, and again over
 * the last frame whenever the stream stalls, which on a bad connection is
 * what most of the waiting looks like.
 *
 * For the ring to show before the file has loaded, the output must exist
 * before the file has loaded: neodct-play passes --force-window=immediate,
 * which creates the VO at startup with a dummy configuration. Without that
 * option the first thing this driver hears is the first decoded frame, and
 * everything below about "loading" simply never triggers -- there is no
 * harm in it, just no ring.
 *
 * Knowing WHEN to show it is the part that needs care. The VO interface
 * says nothing about buffering: a stalled stream and a paused one both
 * arrive here as "no new frames". So this driver opens an ordinary client
 * handle on its own core -- the same thing a Lua script gets -- and watches
 * the paused-for-cache and seeking properties through it. The handle is
 * weak (it cannot keep the core alive), it is drained from the VO thread
 * where every other VO callback already runs, and it is closed the moment
 * the core says it is shutting down, because the core waits for exactly
 * that before it will let this output be torn down.
 *
 * The ring is painted by this thread, on its own timer, straight into the
 * framebuffer over whatever the clean frame holds -- never into the frame
 * itself. The core's redraws (draw_frame) therefore cannot smear it and it
 * cannot leak into a frame the core keeps; and stopping it is a matter of
 * copying the clean pixels back.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/fb.h>

#include "common/global.h"
#include "common/msg.h"
#include "libmpv/client.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "osdep/timer.h"
#include "player/client.h"
#include "sub/osd.h"
#include <libswscale/swscale.h>

#include "video/fmt-conversion.h"
#include "video/mp_image.h"
#include "video/sws_utils.h"
#include "vo.h"

#include "fbdev_format.h"
#include "fbdev_spinner.h"

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

    // ---- the loading ring ----
    mpv_handle *client;         // our window onto the core's state, or NULL
    bool loading;               // nothing has played yet
    bool buffering;             // the core is paused waiting for the cache
    bool seeking;               // a seek is in flight
    bool spin_wanted;           // any of the three
    bool spin_visible;          // ...and it has been painted
    int64_t spin_since_us;      // when spin_wanted last became true
    int64_t next_tick_us;       // when the ring next moves (or first shows)
    struct neodct_spinner_geom geom;
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

// ---------------------------------------------------------------------
// the loading ring

// Copy the ring's rectangle back from the clean frame -- or paint it black
// when there is no frame yet -- so that the dots of the previous step (or
// the ring itself, once it is done) leave nothing behind.
static void spinner_restore(struct priv *p)
{
    int x0, y0, x1, y1;

    neodct_spinner_bbox(&p->geom, p->screen_w, p->screen_h, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0)
        return;

    size_t bpp = (size_t)p->bytes_per_pixel;
    size_t width = (size_t)(x1 - x0) * bpp;

    for (int y = y0; y < y1; y++) {
        uint8_t *dst = p->map + (size_t)y * p->stride + (size_t)x0 * bpp;
        if (p->cur_frame) {
            const uint8_t *src = p->cur_frame->planes[0] +
                                 (ptrdiff_t)y * p->cur_frame->stride[0] +
                                 (ptrdiff_t)x0 * (ptrdiff_t)bpp;
            memcpy(dst, src, width);
        } else {
            memset(dst, 0, width);
        }
    }
}

static void put_pixel(struct priv *p, int x, int y, uint32_t pixel)
{
    if (x < 0 || y < 0 || x >= p->screen_w || y >= p->screen_h)
        return;

    uint8_t *dst = p->map + (size_t)y * p->stride +
                   (size_t)x * (size_t)p->bytes_per_pixel;
    if (p->bytes_per_pixel == 2) {
        uint16_t v = (uint16_t)pixel;
        memcpy(dst, &v, sizeof(v));
    } else {
        memcpy(dst, &pixel, sizeof(pixel));
    }
}

// Paint the ring for the current moment over the clean frame.
static void spinner_paint(struct priv *p, int64_t now)
{
    int phase = neodct_spinner_phase(now - p->spin_since_us);

    spinner_restore(p);

    for (int d = 0; d < NEODCT_SPINNER_DOTS; d++) {
        int cx, cy;
        neodct_spinner_dot_centre(&p->geom, d, &cx, &cy);
        uint32_t pixel = neodct_spinner_pixel(p->pixfmt,
                                              neodct_spinner_dot_level(d, phase));
        for (int dy = -p->geom.dot_r; dy <= p->geom.dot_r; dy++) {
            for (int dx = -p->geom.dot_r; dx <= p->geom.dot_r; dx++) {
                if (neodct_spinner_covers(dx, dy, p->geom.dot_r))
                    put_pixel(p, cx + dx, cy + dy, pixel);
            }
        }
    }
}

// Decide from the three states whether the ring should be turning, and
// arrange for it to start (now, or after the grace) or stop.
static void spinner_update(struct vo *vo)
{
    struct priv *p = vo->priv;
    bool want = p->loading || p->buffering || p->seeking;
    int64_t now = mp_time_us();

    if (want && !p->spin_wanted) {
        p->spin_wanted = true;
        p->spin_since_us = now;
        // While loading, the screen is already black and the user is
        // waiting for exactly this: show it at once. A stall in a playing
        // stream waits out the grace, so a seek answered from the cache
        // does not flash a ring at every keypress.
        p->next_tick_us = p->loading ? now : now + NEODCT_SPINNER_GRACE_US;
    } else if (!want && p->spin_wanted) {
        p->spin_wanted = false;
        if (p->spin_visible) {
            p->spin_visible = false;
            if (p->active)
                spinner_restore(p);
        }
    }
}

// One step of the ring, if it is time. Called from the VO thread's wait,
// so this is what keeps the ring moving while the core has nothing to
// say to us at all.
static void spinner_tick(struct vo *vo)
{
    struct priv *p = vo->priv;
    int64_t now = mp_time_us();

    if (!p->spin_wanted || !p->active || now < p->next_tick_us)
        return;

    p->spin_visible = true;
    spinner_paint(p, now);
    p->next_tick_us = now + NEODCT_SPINNER_TICK_US;
}

// The client handle's wakeup: an event is waiting. Runs on whichever
// thread produced the event, so it may do nothing but nudge the VO thread.
static void client_wakeup(void *ctx)
{
    vo_wakeup(ctx);
}

// Everything the core has told us since the last look. Runs on the VO
// thread, from VOCTRL_CHECK_EVENTS, which the thread issues on every pass
// of its loop -- including the pass client_wakeup() provokes.
static void drain_events(struct vo *vo)
{
    struct priv *p = vo->priv;

    while (p->client) {
        mpv_event *ev = mpv_wait_event(p->client, 0);
        if (ev->event_id == MPV_EVENT_NONE)
            break;

        switch (ev->event_id) {
        case MPV_EVENT_SHUTDOWN:
            // mp_shutdown_clients() will not return until every handle
            // is gone, and uninit() below runs only after it has.
            mpv_destroy(p->client);
            p->client = NULL;
            break;
        case MPV_EVENT_START_FILE:
            p->loading = true;
            break;
        case MPV_EVENT_PLAYBACK_RESTART:
            // The core has started (or restarted) output: the file is
            // open, decoded and moving. For an audio-only file this is
            // the only signal that loading is over.
            p->loading = false;
            p->seeking = false;
            break;
        case MPV_EVENT_END_FILE:
            p->loading = false;
            break;
        case MPV_EVENT_PROPERTY_CHANGE: {
            mpv_event_property *prop = ev->data;
            if (prop->format != MPV_FORMAT_FLAG)
                break;
            int flag = *(int *)prop->data;
            if (strcmp(prop->name, "paused-for-cache") == 0)
                p->buffering = flag;
            else if (strcmp(prop->name, "seeking") == 0)
                p->seeking = flag;
            break;
        }
        default:
            break;
        }
    }

    spinner_update(vo);
}

static void client_open(struct vo *vo)
{
    struct priv *p = vo->priv;

    p->client = mp_new_client(vo->global->client_api, "vo_fbdev");
    if (!p->client) {
        MP_WARN(vo, "no client handle; the stall indicator is off\n");
        return;
    }

    // Weak: the core must never stay alive because this driver is.
    mp_client_set_weak(p->client);
    mpv_set_wakeup_callback(p->client, client_wakeup, vo);
    mpv_observe_property(p->client, 0, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(p->client, 0, "seeking", MPV_FORMAT_FLAG);

    // mp_play_files() waits until every client has called mpv_wait_event()
    // once before it will load anything. Do so now rather than on the
    // thread's first pass, so the file open is not held up on a race with
    // ourselves.
    drain_events(vo);
}

static void client_close(struct priv *p)
{
    if (p->client) {
        mpv_destroy(p->client);
        p->client = NULL;
    }
}

// ---------------------------------------------------------------------

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

    // Take the screen now. The application that started us left its last
    // frame on it, and a frozen copy of that is what "nothing happened"
    // looks like; black with the ring turning is what "loading" looks like.
    memset(p->map, 0, p->map_size);
    neodct_spinner_geometry(p->screen_w, p->screen_h, &p->geom);
    p->loading = true;
    p->active = true;

    client_open(vo);
    spinner_tick(vo);
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
    // Black, not whatever the allocator handed back: the ring restores
    // its background from this frame, and it may need to before the first
    // draw_frame() has filled it.
    mp_image_clear(p->cur_frame, 0, 0, p->cur_frame->w, p->cur_frame->h);

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

        // A real frame: whatever else is going on, the file has loaded.
        p->loading = false;
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

    // The frame just went underneath the ring; put the ring back on top.
    spinner_update(vo);
    if (p->spin_visible)
        spinner_paint(p, mp_time_us());
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
    case VOCTRL_CHECK_EVENTS:
        drain_events(vo);
        return VO_TRUE;
    case VOCTRL_SET_PANSCAN:
        // The video rectangle is recomputed on the next reconfig.
        if (vo->config_ok)
            reconfig(vo, vo->params);
        return VO_TRUE;
    }

    return VO_NOTIMPL;
}

// The thread's idle wait, cut short when the ring is due to move.
static void wait_events(struct vo *vo, int64_t until_time_us)
{
    struct priv *p = vo->priv;

    if (p->spin_wanted && p->next_tick_us < until_time_us)
        until_time_us = p->next_tick_us;
    vo_wait_default(vo, until_time_us);
    spinner_tick(vo);
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;

    p->active = false;
    client_close(p);

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
    .wait_events = wait_events,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
    .global_opts = &vo_fbdev_conf,
};

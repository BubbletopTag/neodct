/* nd_fb_priv.h -- the nd_fb struct, shared between nd_fb.c and nd_capture.c.
 *
 * Private on purpose: nd_fb.h keeps the type opaque so that widget code
 * cannot reach past nd_fb_update(). The capture backend is the one thing that
 * genuinely has to build an nd_fb of its own, and it lives inside libneodct,
 * so it gets the definition and nobody else does.
 */

#ifndef ND_FB_PRIV_H_INCLUDED
#define ND_FB_PRIV_H_INCLUDED

#include "nd_fb.h"
#include "nd_image.h"
#include "nd_types.h"

/* A backend that takes the RGB band whole, instead of packing it into a
 * mapping. Returning anything but ND_OK is passed straight back out of
 * nd_fb_update(). */
typedef nd_err (*nd_fb_sink_fn)(void *ctx, const nd_image *src);

typedef enum {
    ND_FB_BACKEND_DEVICE = 0, /* mmap of /dev/fb0; munmap + close on the way out */
    ND_FB_BACKEND_MEM,        /* calloc'd mapping; free on the way out           */
    ND_FB_BACKEND_SINK        /* no mapping at all; hands the source over        */
} nd_fb_backend;

struct nd_fb {
    nd_fb_backend backend;
    int fd; /* -1 unless ND_FB_BACKEND_DEVICE */

    /* The mapping. NULL for a sink. */
    uint8_t *mem;
    size_t size; /* line_length * yres */

    /* Geometry, exactly as the Python's __init__ derives it. */
    int32_t xres;
    int32_t yres;
    int32_t bpp;
    size_t line_length;
    int32_t bytes_per_pixel; /* max(1, bpp / 8)                */
    int32_t stride_pixels;   /* line_length / bytes_per_pixel  */

    nd_fb_path path;

    nd_fb_sink_fn sink;
    void *sink_ctx;
};

/* Fill in the derived geometry and choose the pixel path. line_length of 0
 * takes the "== 0 means compute it" fallback that the Python depends on --
 * see the warning in nd_fb.h. Returns ND_ERR_INVAL for geometry that cannot
 * describe a framebuffer, ND_ERR_HARDWARE for a stride too short to hold a
 * row. */
nd_err nd_fb_derive_geometry(struct nd_fb *fb, int32_t xres, int32_t yres, int32_t bpp,
                             size_t line_length);

/* Re-pick the pixel path from the red and blue offsets fb_var_screeninfo
 * reported, having already derived the geometry. Red BELOW blue means red
 * comes first in memory; anything else, equal offsets included, keeps the
 * blue-first order nd_fb_derive_geometry() chose -- a driver that left the
 * masks zeroed has told us nothing, and the answer it used to get is the
 * safer thing to give it. Only the two paths with a real driver to ask call
 * this: nd_fb_open() and nd_fb_adopt_fd(). A memory framebuffer and the
 * capture sink have no driver and keep B G R A. Depths other than 16 and 32
 * have no order to choose and are left alone. */
void nd_fb_set_channel_order(struct nd_fb *fb, int32_t red_offset, int32_t blue_offset);

/* Allocate a bare nd_fb with a sink attached and no mapping. Owned by the
 * caller; release with nd_fb_close(). */
nd_err nd_fb_open_sink(struct nd_fb **out, int32_t xres, int32_t yres, int32_t bpp,
                       nd_fb_sink_fn sink, void *ctx);

#endif /* ND_FB_PRIV_H_INCLUDED */

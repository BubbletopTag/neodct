/* nd_fb.c -- /dev/fb0: two ioctls, an mmap, and the centred-band blit.
 *
 * A one-to-one port of the Framebuffer class in System/core/main.py, which is
 * 148 lines and has more corners in it than that suggests. What each of them
 * is for, since the Python does not always say:
 *
 *   THE MAPPING IS ZEROED ONCE, at open. Every later write touches only the
 *   band, so the letterbox rows above and below it are black from boot and
 *   are never written again. Zeroing per frame would cost 230 KB of memory
 *   traffic for no visible difference.
 *
 *   THE FAST PATH exists because on both QEMU and the hardware the panel has
 *   already been reconfigured to 240x175 @ 32bpp by neodct_displayd, so the
 *   source fits the framebuffer exactly and the whole frame is one contiguous
 *   168,000-byte write. The generic row-by-row path is for a genuine 240x240
 *   framebuffer, for side padding, and for a driver that pads its rows.
 *
 *   THE SLOW PATH composes into a full-stride image first. It is reached only
 *   when the source has to be cropped, or when the bit depth is neither 16 nor
 *   32. It writes the WHOLE mapping, and its black is not the same black: the
 *   composed image goes through convert("RGBA"), so on the 32bpp path its
 *   letterbox is 00 00 00 ff, where the fast path leaves the 00 00 00 00 the
 *   initial clear put there. That is a visible-to-a-checksum difference and it
 *   is reproduced rather than tidied.
 *
 * ============ WHAT IS DELIBERATELY NOT PORTED ============
 *
 * The Python's pure-Python RGB565 packer and the three 256-entry lookup
 * tables it needs. Those exist because Pillow >= 11 dropped the "BGR;16"
 * packer and the fallback ran at ~350 ms/frame; the tables bought back some
 * of it and cost ~350 KB. In C the packer is a loop, so 16bpp is a fast path
 * like any other and there are no tables to hold. The bytes it produces are
 * identical -- test_nd_fb.c checks the C output against that very Python
 * packer's output.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "nd_capture.h"
#include "nd_fb.h"
#include "nd_fb_priv.h"
#include "nd_image.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_types.h"

/* ------------------------------------------------------------------ *
 * Packing
 * ------------------------------------------------------------------ */

/* Bytes the composed/packed image spends per pixel, which is NOT always
 * fb->bytes_per_pixel: on the raw path the Python writes RGB triples whatever
 * the bit depth claims, and at 8bpp that overruns the mapping and gets
 * truncated. Reproduced, because a truncated frame is what the phone would
 * show. */
static size_t out_bytes(nd_fb_path path)
{
    switch (path) {
    case ND_FB_PATH_BGRA32:
        return 4u;
    case ND_FB_PATH_RGB565:
        return 2u;
    case ND_FB_PATH_RGB888:
        break;
    }
    return 3u;
}

/* One pixel, into at most 4 bytes. Separated out only so the partial-pixel
 * tail of a truncated row can reuse it. */
static void pack_px(uint8_t out[4], const uint8_t *src, nd_fb_path path)
{
    uint32_t v;

    switch (path) {
    case ND_FB_PATH_BGRA32:
        out[0] = src[2];
        out[1] = src[1];
        out[2] = src[0];
        /* Always opaque: update() converts the source to RGB first, so any
         * alpha it arrived with is already gone. */
        out[3] = 255u;
        return;
    case ND_FB_PATH_RGB565:
        v = (uint32_t)((src[0] & 0xF8u) << 8) | (uint32_t)((src[1] & 0xFCu) << 3) |
            (uint32_t)(src[2] >> 3);
        /* Low byte first: little-endian, which is both ARM and x86 here. */
        out[0] = (uint8_t)(v & 0xFFu);
        out[1] = (uint8_t)((v >> 8) & 0xFFu);
        return;
    case ND_FB_PATH_RGB888:
        break;
    }
    out[0] = src[0];
    out[1] = src[1];
    out[2] = src[2];
}

static void pack_row(uint8_t *dst, const uint8_t *src, size_t n, size_t src_bpp, nd_fb_path path)
{
    size_t i;

    switch (path) {
    case ND_FB_PATH_BGRA32:
        for (i = 0u; i < n; i++) {
            const uint8_t *p = src + i * src_bpp;
            dst[i * 4u + 0u] = p[2];
            dst[i * 4u + 1u] = p[1];
            dst[i * 4u + 2u] = p[0];
            dst[i * 4u + 3u] = 255u;
        }
        return;
    case ND_FB_PATH_RGB565:
        for (i = 0u; i < n; i++) {
            const uint8_t *p = src + i * src_bpp;
            uint32_t v = (uint32_t)((p[0] & 0xF8u) << 8) | (uint32_t)((p[1] & 0xFCu) << 3) |
                         (uint32_t)(p[2] >> 3);
            dst[i * 2u + 0u] = (uint8_t)(v & 0xFFu);
            dst[i * 2u + 1u] = (uint8_t)((v >> 8) & 0xFFu);
        }
        return;
    case ND_FB_PATH_RGB888:
        break;
    }
    for (i = 0u; i < n; i++) {
        const uint8_t *p = src + i * src_bpp;
        dst[i * 3u + 0u] = p[0];
        dst[i * 3u + 1u] = p[1];
        dst[i * 3u + 2u] = p[2];
    }
}

/* The source must be something pack_row can read three channels out of. */
static bool packable(const nd_image *src)
{
    return src != NULL && src->pixels != NULL &&
           (src->fmt == ND_PIXFMT_RGB888 || src->fmt == ND_PIXFMT_RGBA8888);
}

nd_err nd_fb_pack_bgra(const nd_image *src, uint8_t *out, size_t out_sz)
{
    size_t need;
    int32_t y;

    if (!packable(src) || out == NULL)
        return ND_ERR_INVAL;

    need = (size_t)src->w * (size_t)src->h * 4u;
    if (out_sz < need)
        return ND_ERR_TOOLONG;

    for (y = 0; y < src->h; y++)
        pack_row(out + (size_t)y * (size_t)src->w * 4u, src->pixels + (size_t)y * src->stride,
                 (size_t)src->w, src->bpp, ND_FB_PATH_BGRA32);

    return ND_OK;
}

nd_err nd_fb_pack_rgb565(const nd_image *src, uint8_t *out, size_t out_sz)
{
    size_t need;
    int32_t y;

    if (!packable(src) || out == NULL)
        return ND_ERR_INVAL;

    need = (size_t)src->w * (size_t)src->h * 2u;
    if (out_sz < need)
        return ND_ERR_TOOLONG;

    for (y = 0; y < src->h; y++)
        pack_row(out + (size_t)y * (size_t)src->w * 2u, src->pixels + (size_t)y * src->stride,
                 (size_t)src->w, src->bpp, ND_FB_PATH_RGB565);

    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * Geometry
 * ------------------------------------------------------------------ */

nd_err nd_fb_derive_geometry(struct nd_fb *fb, int32_t xres, int32_t yres, int32_t bpp,
                             size_t line_length)
{
    if (xres <= 0 || yres <= 0 || bpp <= 0 || bpp > 32)
        return ND_ERR_INVAL;

    fb->xres = xres;
    fb->yres = yres;
    fb->bpp = bpp;

    /* Load-bearing, see nd_fb.h: the Python reads line_length from the wrong
     * struct offset on 32-bit and is saved by this fallback, so the fallback
     * is the value the phone has always actually run with. */
    fb->line_length = line_length != 0u ? line_length : (size_t)xres * (size_t)(bpp / 8);
    if (fb->line_length == 0u)
        return ND_ERR_INVAL;

    fb->bytes_per_pixel = bpp / 8 > 0 ? bpp / 8 : 1;
    fb->stride_pixels = (int32_t)(fb->line_length / (size_t)fb->bytes_per_pixel);
    fb->size = fb->line_length * (size_t)yres;

    /* The Python would fail here with an mmap write error partway through a
     * frame. Refuse at open instead: a stride shorter than a row of pixels is
     * a driver bug and we want it named, not discovered as tearing. */
    if (fb->line_length < (size_t)xres * (size_t)fb->bytes_per_pixel)
        return ND_ERR_HARDWARE;

    if (bpp == 32)
        fb->path = ND_FB_PATH_BGRA32;
    else if (bpp == 16)
        fb->path = ND_FB_PATH_RGB565;
    else
        fb->path = ND_FB_PATH_RGB888;

    return ND_OK;
}

static const char *path_name(nd_fb_path path)
{
    switch (path) {
    case ND_FB_PATH_BGRA32:
        return "BGRA 32bpp (C, fast)";
    case ND_FB_PATH_RGB565:
        /* The Python prints this only when Pillow still has the "BGR;16"
         * packer, and otherwise a warning about 350 ms/frame. C has no slow
         * variant to warn about, so 16bpp is always the fast line. */
        return "BGR;16 16bpp (C, fast)";
    case ND_FB_PATH_RGB888:
        break;
    }
    return "RGB888 (C, raw)";
}

/* ------------------------------------------------------------------ *
 * Opening
 * ------------------------------------------------------------------ */

static nd_err fb_alloc(struct nd_fb **out)
{
    /* owned by the caller of nd_fb_open()/nd_fb_open_mem(); freed by
     * nd_fb_close() */
    struct nd_fb *fb = calloc(1u, sizeof *fb);

    if (fb == NULL)
        return ND_ERR_NOMEM;
    fb->fd = -1;
    *out = fb;
    return ND_OK;
}

nd_err nd_fb_open(nd_fb **out, const char *path)
{
    char resolved[ND_PATH_MAX];
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    struct nd_fb *fb = NULL;
    void *mapped = MAP_FAILED;
    nd_err rc;

    if (out == NULL)
        return ND_ERR_INVAL;
    *out = NULL;

    /* Resolved like every other opened path: with NEODCT_ROOT unset this is a
     * copy, and with it set a host test cannot reach a developer's real
     * panel. */
    rc = nd_path_resolve(resolved, sizeof resolved, path != NULL ? path : ND_PATH_FB);
    if (rc != ND_OK) {
        nd_log_err(ND_LOG_FB, "framebuffer path too long");
        return rc;
    }

    rc = fb_alloc(&fb);
    if (rc != ND_OK)
        return rc;

    fb->backend = ND_FB_BACKEND_DEVICE;
    fb->fd = open(resolved, O_RDWR);
    if (fb->fd < 0) {
        nd_log_err(ND_LOG_FB, "cannot open %s: %s", resolved, strerror(errno));
        rc = ND_ERR_IO;
        goto done;
    }

    /* FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO -- 0x4600 and 0x4602, read
     * through the real structs so the 32-bit and 64-bit layouts both come out
     * right. See the warning in nd_fb.h about offset 48. */
    memset(&vinfo, 0, sizeof vinfo);
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        nd_log_err(ND_LOG_FB, "FBIOGET_VSCREENINFO on %s: %s", resolved, strerror(errno));
        rc = ND_ERR_HARDWARE;
        goto done;
    }

    memset(&finfo, 0, sizeof finfo);
    if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        nd_log_err(ND_LOG_FB, "FBIOGET_FSCREENINFO on %s: %s", resolved, strerror(errno));
        rc = ND_ERR_HARDWARE;
        goto done;
    }

    rc = nd_fb_derive_geometry(fb, (int32_t)vinfo.xres, (int32_t)vinfo.yres,
                               (int32_t)vinfo.bits_per_pixel, (size_t)finfo.line_length);
    if (rc != ND_OK) {
        nd_log_err(ND_LOG_FB, "%s reports %ux%u @ %ubpp, stride %u -- unusable", resolved,
                   vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, finfo.line_length);
        goto done;
    }

    mapped = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (mapped == MAP_FAILED) {
        nd_log_err(ND_LOG_FB, "mmap %zu bytes of %s: %s", fb->size, resolved, strerror(errno));
        rc = ND_ERR_IO;
        goto done;
    }
    fb->mem = mapped;

    /* Once, here. Everything after this writes only the band. */
    memset(fb->mem, 0, fb->size);

    nd_log(ND_LOG_FB, "%dx%d @ %dbpp, pixel path: %s", fb->xres, fb->yres, fb->bpp,
           path_name(fb->path));

    *out = fb;
    fb = NULL;
    rc = ND_OK;

done:
    if (fb != NULL) {
        if (fb->mem != NULL)
            (void)munmap(fb->mem, fb->size);
        if (fb->fd >= 0)
            (void)close(fb->fd);
        free(fb);
    }
    return rc;
}

nd_err nd_fb_open_mem(nd_fb **out, int32_t xres, int32_t yres, int32_t bpp, size_t line_length)
{
    struct nd_fb *fb = NULL;
    nd_err rc;

    if (out == NULL)
        return ND_ERR_INVAL;
    *out = NULL;

    rc = fb_alloc(&fb);
    if (rc != ND_OK)
        return rc;

    fb->backend = ND_FB_BACKEND_MEM;

    rc = nd_fb_derive_geometry(fb, xres, yres, bpp, line_length);
    if (rc != ND_OK)
        goto done;

    /* calloc, not malloc: the real mapping is zeroed at open and every
     * partial-band write since relies on it. Owned by this nd_fb; freed by
     * nd_fb_close(). */
    fb->mem = calloc(1u, fb->size);
    if (fb->mem == NULL) {
        rc = ND_ERR_NOMEM;
        goto done;
    }

    *out = fb;
    fb = NULL;
    rc = ND_OK;

done:
    if (fb != NULL) {
        free(fb->mem);
        free(fb);
    }
    return rc;
}

nd_err nd_fb_open_sink(struct nd_fb **out, int32_t xres, int32_t yres, int32_t bpp,
                       nd_fb_sink_fn sink, void *ctx)
{
    struct nd_fb *fb = NULL;
    nd_err rc;

    if (out == NULL || sink == NULL)
        return ND_ERR_INVAL;
    *out = NULL;

    rc = fb_alloc(&fb);
    if (rc != ND_OK)
        return rc;

    fb->backend = ND_FB_BACKEND_SINK;

    rc = nd_fb_derive_geometry(fb, xres, yres, bpp, 0u);
    if (rc != ND_OK) {
        free(fb);
        return rc;
    }

    /* A sink never packs, so it has no mapping and its size is advisory --
     * callers that ask read it for the geometry, not for a buffer. */
    fb->sink = sink;
    fb->sink_ctx = ctx;
    *out = fb;
    return ND_OK;
}

void nd_fb_close(nd_fb *fb)
{
    if (fb == NULL)
        return;

    switch (fb->backend) {
    case ND_FB_BACKEND_DEVICE:
        if (fb->mem != NULL)
            (void)munmap(fb->mem, fb->size);
        if (fb->fd >= 0)
            (void)close(fb->fd);
        break;
    case ND_FB_BACKEND_MEM:
        free(fb->mem);
        break;
    case ND_FB_BACKEND_SINK:
        break;
    }
    free(fb);
}

/* ------------------------------------------------------------------ *
 * Geometry accessors
 * ------------------------------------------------------------------ */

int32_t nd_fb_xres(const nd_fb *fb)
{
    return fb != NULL ? fb->xres : 0;
}

int32_t nd_fb_yres(const nd_fb *fb)
{
    return fb != NULL ? fb->yres : 0;
}

int32_t nd_fb_bpp(const nd_fb *fb)
{
    return fb != NULL ? fb->bpp : 0;
}

size_t nd_fb_line_length(const nd_fb *fb)
{
    return fb != NULL ? fb->line_length : 0u;
}

nd_fb_path nd_fb_pixel_path(const nd_fb *fb)
{
    return fb != NULL ? fb->path : ND_FB_PATH_RGB888;
}

const uint8_t *nd_fb_mem_bytes(const nd_fb *fb, size_t *size_out)
{
    if (fb == NULL || fb->backend != ND_FB_BACKEND_MEM)
        return NULL;
    if (size_out != NULL)
        *size_out = fb->size;
    return fb->mem;
}

/* ------------------------------------------------------------------ *
 * Presenting a frame
 * ------------------------------------------------------------------ */

/* _write_center_band(). The Python branches between one contiguous write and
 * a row loop; packing straight into the mapping makes them the same code, and
 * when dst_x is 0 and the row fills the stride the loop writes one unbroken
 * run of bytes anyway. */
static nd_err write_center_band(struct nd_fb *fb, const nd_image *src, int32_t copy_w,
                                int32_t copy_h, int32_t dst_x, int32_t dst_y)
{
    size_t out_bpp = out_bytes(fb->path);
    size_t row_bytes = (size_t)copy_w * out_bpp;
    size_t dst_off = (size_t)dst_y * fb->line_length + (size_t)dst_x * (size_t)fb->bytes_per_pixel;
    int32_t r;

    for (r = 0; r < copy_h; r++) {
        if (dst_off > fb->size || row_bytes > fb->size - dst_off)
            return ND_ERR_HARDWARE;
        pack_row(fb->mem + dst_off, src->pixels + (size_t)r * src->stride, (size_t)copy_w, src->bpp,
                 fb->path);
        dst_off += fb->line_length;
    }
    return ND_OK;
}

/* One row of the composed full-stride image: black everywhere the band does
 * not cover. On the 32bpp path that black carries alpha 255, because the
 * Python's native_img goes through convert("RGBA") and PIL fills the alpha
 * channel with 255 -- unlike the zeroed mapping the fast path leaves alone. */
static void fill_black_row(uint8_t *dst, size_t n, nd_fb_path path)
{
    memset(dst, 0, n);
    if (path == ND_FB_PATH_BGRA32) {
        size_t i;
        for (i = 3u; i < n; i += 4u)
            dst[i] = 255u;
    }
}

/* The Python's slow path: compose into an RGB image stride_pixels wide, pack
 * the whole thing, and write it from byte 0, truncated to the mapping size.
 * Done row at a time straight into the mapping so nothing is allocated. */
static nd_err write_composed(struct nd_fb *fb, const nd_image *src, int32_t copy_w, int32_t copy_h,
                             int32_t src_x, int32_t src_y, int32_t dst_x, int32_t dst_y)
{
    size_t out_bpp = out_bytes(fb->path);
    size_t native_row = (size_t)fb->stride_pixels * out_bpp;
    int32_t y;

    if (native_row == 0u)
        return ND_ERR_HARDWARE;

    for (y = 0; y < fb->yres; y++) {
        size_t row_off = (size_t)y * native_row;
        size_t n;
        size_t band_off;
        size_t avail;
        size_t want;
        size_t nwrite;
        size_t full;
        size_t tail;
        const uint8_t *srow;

        /* data[:size] -- the composed image can be longer than the mapping
         * when bytes_per_pixel is less than three, and then the tail is
         * simply dropped. */
        if (row_off >= fb->size)
            break;
        n = fb->size - row_off;
        if (n > native_row)
            n = native_row;

        fill_black_row(fb->mem + row_off, n, fb->path);

        if (y < dst_y || y >= dst_y + copy_h)
            continue;

        band_off = (size_t)dst_x * out_bpp;
        if (band_off >= n)
            continue;

        avail = n - band_off;
        want = (size_t)copy_w * out_bpp;
        nwrite = want < avail ? want : avail;
        full = nwrite / out_bpp;
        tail = nwrite - full * out_bpp;

        srow = src->pixels + (size_t)(src_y + (y - dst_y)) * src->stride + (size_t)src_x * src->bpp;

        pack_row(fb->mem + row_off + band_off, srow, full, src->bpp, fb->path);

        if (tail != 0u) {
            uint8_t px[4];
            pack_px(px, srow + full * src->bpp, fb->path);
            memcpy(fb->mem + row_off + band_off + full * out_bpp, px, tail);
        }
    }
    return ND_OK;
}

nd_err nd_fb_update(nd_fb *fb, const nd_image *src)
{
    int32_t copy_w;
    int32_t copy_h;
    int32_t src_x;
    int32_t src_y;
    int32_t dst_x;
    int32_t dst_y;
    bool fits;

    if (fb == NULL || src == NULL)
        return ND_ERR_INVAL;

    /* The capture backend takes the band before any of this: it wants the RGB
     * the UI drew, not a packed panel. */
    if (fb->sink != NULL)
        return fb->sink(fb->sink_ctx, src);

    if (!packable(src))
        return ND_ERR_INVAL;
    if (fb->mem == NULL)
        return ND_ERR_INVAL;

    copy_w = nd_min32(src->w, fb->xres);
    copy_h = nd_min32(src->h, fb->yres);
    if (copy_w <= 0 || copy_h <= 0)
        return ND_ERR_INVAL;

    /* Both differences are non-negative, so C's truncating division and
     * Python's floor division agree. */
    src_x = nd_max32(0, (src->w - copy_w) / 2);
    src_y = nd_max32(0, (src->h - copy_h) / 2);
    dst_x = nd_max32(0, (fb->xres - copy_w) / 2);
    dst_y = nd_max32(0, (fb->yres - copy_h) / 2);

    fits = (src_x == 0 && src_y == 0 && copy_w == src->w && copy_h == src->h);

    /* The live case, and the only one that runs per frame on the phone. Note
     * the raw path is excluded exactly as the Python excludes it: at any
     * depth other than 16 or 32 it falls through and composes. */
    if (fits && (fb->bpp == 16 || fb->bpp == 32))
        return write_center_band(fb, src, copy_w, copy_h, dst_x, dst_y);

    return write_composed(fb, src, copy_w, copy_h, src_x, src_y, dst_x, dst_y);
}

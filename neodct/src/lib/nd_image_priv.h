/* nd_image_priv.h -- shared internals of the rasterizer.
 *
 * NOT a public header and deliberately not under include/: the frozen header
 * contract describes what widgets and apps may call, and none of this is it.
 * It exists so nd_image.c, nd_draw.c, nd_resample.c and the codecs agree
 * about row addressing without exporting a symbol for it.
 */

#ifndef ND_IMAGE_PRIV_H_INCLUDED
#define ND_IMAGE_PRIV_H_INCLUDED

#include "nd_image.h"

/* A frame is 240x175 and an app icon is 82x82. This exists only so a corrupt
 * file header cannot ask for a terabyte before a pixel has been read. */
#define ND_IMAGE_MAX_DIM 16384

/* Rows are addressed through stride, never w*bpp: a borrowed view onto the
 * framebuffer mmap has padding and a crop of another image may not. */
static inline uint8_t *nd_img_row(const nd_image *img, int32_t y) ND_UNUSED_FN;
static inline uint8_t *nd_img_row(const nd_image *img, int32_t y)
{
    return img->pixels + (size_t)y * img->stride;
}

static inline uint8_t *nd_img_px(const nd_image *img, int32_t x, int32_t y) ND_UNUSED_FN;
static inline uint8_t *nd_img_px(const nd_image *img, int32_t x, int32_t y)
{
    return nd_img_row(img, y) + (size_t)x * img->bpp;
}

static inline bool nd_img_fmt_valid(nd_pixfmt fmt) ND_UNUSED_FN;
static inline bool nd_img_fmt_valid(nd_pixfmt fmt)
{
    return fmt == ND_PIXFMT_RGB888 || fmt == ND_PIXFMT_RGBA8888 || fmt == ND_PIXFMT_L8;
}

/* Pillow's ITU-R 601-2 luma, verbatim from Convert.c: the 16.16 constants
 * with a +0x8000 bias. A float 0.299/0.587/0.114 dot product disagrees on a
 * scattering of values. */
static inline uint8_t nd_img_rgb_to_l(uint8_t r, uint8_t g, uint8_t b) ND_UNUSED_FN;
static inline uint8_t nd_img_rgb_to_l(uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t v = (uint32_t)r * 19595u + (uint32_t)g * 38470u + (uint32_t)b * 7471u + 0x8000u;
    return (uint8_t)(v >> 16);
}

/* Reads one pixel as RGBA. L8 reports opaque grey and RGB888 reports opaque,
 * which is what PIL's convert() to a wider mode produces. */
static inline void nd_img_px_read(const uint8_t *p, nd_pixfmt fmt, uint8_t out[4]) ND_UNUSED_FN;
static inline void nd_img_px_read(const uint8_t *p, nd_pixfmt fmt, uint8_t out[4])
{
    switch (fmt) {
    case ND_PIXFMT_L8:
        out[0] = out[1] = out[2] = p[0];
        out[3] = 255u;
        break;
    case ND_PIXFMT_RGBA8888:
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
        out[3] = p[3];
        break;
    case ND_PIXFMT_RGB888:
    default:
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
        out[3] = 255u;
        break;
    }
}

static inline void nd_img_px_write(uint8_t *p, nd_pixfmt fmt, const uint8_t in[4]) ND_UNUSED_FN;
static inline void nd_img_px_write(uint8_t *p, nd_pixfmt fmt, const uint8_t in[4])
{
    switch (fmt) {
    case ND_PIXFMT_L8:
        p[0] = nd_img_rgb_to_l(in[0], in[1], in[2]);
        break;
    case ND_PIXFMT_RGBA8888:
        p[0] = in[0];
        p[1] = in[1];
        p[2] = in[2];
        p[3] = in[3];
        break;
    case ND_PIXFMT_RGB888:
    default:
        p[0] = in[0];
        p[1] = in[1];
        p[2] = in[2];
        break;
    }
}

/* nd_color carries alpha even for RGB surfaces; drawing onto RGB drops it. */
static inline void nd_img_colour_quad(nd_color c, uint8_t out[4]) ND_UNUSED_FN;
static inline void nd_img_colour_quad(nd_color c, uint8_t out[4])
{
    out[0] = c.r;
    out[1] = c.g;
    out[2] = c.b;
    out[3] = c.a;
}

/* How many leading bytes of a pixel are colour bands the pointwise operators
 * touch: 1 for L8, 3 for RGB888, 4 for RGBA8888. */
static inline uint8_t nd_img_bands(nd_pixfmt fmt) ND_UNUSED_FN;
static inline uint8_t nd_img_bands(nd_pixfmt fmt)
{
    return fmt == ND_PIXFMT_RGBA8888 ? (uint8_t)4 : (fmt == ND_PIXFMT_L8 ? (uint8_t)1 : (uint8_t)3);
}

/* Implemented in nd_jpeg.c; nd_image_open_mem() in nd_png.c dispatches to it
 * after sniffing the SOI marker, so the two codecs need not know about each
 * other's file handling. */
nd_image *nd_jpeg_decode_mem(const uint8_t *data, size_t len);

/* Implemented in nd_gif.c, and reached the same way: the first frame of an
 * in-memory GIF, for the update package's thumbnail and Koki's bundles. */
nd_image *nd_gif_decode_mem(const uint8_t *data, size_t len);

#endif /* ND_IMAGE_PRIV_H_INCLUDED */

/* nd_grab_ops.c -- framebuffer bytes into an nd_image.
 *
 * The channel-order reasoning is in nd_grab.h and is the reason this is a
 * separate translation unit with its own test.
 */

#include "nd_grab.h"

#include <string.h>

#include "nd_image.h"
#include "nd_types.h"

bool nd_grab_fmt_supported(uint32_t bytespp)
{
    return bytespp == 2u || bytespp == 4u;
}

nd_image *nd_grab_to_image(const uint8_t *fb, const nd_grab_fmt *fmt)
{
    nd_image *img;
    int32_t y;

    if (fb == NULL || fmt == NULL)
        return NULL;
    if (fmt->w <= 0 || fmt->h <= 0)
        return NULL;
    if (!nd_grab_fmt_supported(fmt->bytespp))
        return NULL;
    /* A stride that cannot hold a row would walk off the end of every line. */
    if (fmt->stride < (size_t)fmt->w * fmt->bytespp)
        return NULL;

    /* owned by the caller; free with nd_image_free() */
    img = nd_image_new(fmt->w, fmt->h, ND_PIXFMT_RGB888);
    if (img == NULL)
        return NULL;

    for (y = 0; y < fmt->h; y++) {
        const uint8_t *src = fb + (size_t)y * fmt->stride;
        uint8_t *dst = img->pixels + (size_t)y * img->stride;
        int32_t x;

        if (fmt->bytespp == 4u) {
            for (x = 0; x < fmt->w; x++) {
                const uint8_t *p = src + (size_t)x * 4u;
                /* Read the three channels in memory order, then let the flag
                 * decide which end red was on. Naming them b,g,r first and
                 * swapping is displayd's shape and keeps the two readable
                 * side by side. */
                uint8_t b = p[0];
                uint8_t g = p[1];
                uint8_t r = p[2];

                if (fmt->swap_rb) {
                    uint8_t t = r;

                    r = b;
                    b = t;
                }
                *dst++ = r;
                *dst++ = g;
                *dst++ = b;
            }
        } else {
            for (x = 0; x < fmt->w; x++) {
                const uint8_t *p = src + (size_t)x * 2u;
                uint16_t px = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
                uint8_t r5;
                uint8_t g6;
                uint8_t b5;

                /* RGB565, little-endian in memory. The swap exchanges the two
                 * five-bit fields and leaves green where it is. */
                if (fmt->swap_rb)
                    px = (uint16_t)(((px & 0xF800u) >> 11) | (px & 0x07E0u) |
                                    ((px & 0x001Fu) << 11));

                r5 = (uint8_t)((px >> 11) & 0x1Fu);
                g6 = (uint8_t)((px >> 5) & 0x3Fu);
                b5 = (uint8_t)(px & 0x1Fu);

                /* 5/6 bits to 8, replicating the high bits downwards so that
                 * full-scale stays full-scale: 0x1F must become 0xFF, not
                 * 0xF8, or every white pixel comes out slightly grey. */
                *dst++ = (uint8_t)((r5 << 3) | (r5 >> 2));
                *dst++ = (uint8_t)((g6 << 2) | (g6 >> 4));
                *dst++ = (uint8_t)((b5 << 3) | (b5 >> 2));
            }
        }
    }
    return img;
}

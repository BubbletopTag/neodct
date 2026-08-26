/* nd_png.c -- PNG decode and encode through libpng.
 *
 * The decoder is deliberately incurious. All 235 Koki costumes carry a bKGD
 * chunk and most carry cHRM; honouring either changes the pixels, and the
 * Python's Pillow build ignores both, so libpng is configured to hand back
 * the raw 8-bit samples and nothing else. Specifically NOT applied:
 * bKGD (no background composite), cHRM, gAMA (no gamma correction), tEXt and
 * tIME. What IS applied is palette expansion, 1/2/4-bit grey expansion, 16 ->
 * 8 bit stripping and tRNS -> alpha, because those change the storage format
 * rather than the colour, and Pillow does them too.
 *
 * Interlaced files are read through libpng's own interlace handling rather
 * than rejected; nothing shipped is interlaced, but a user wallpaper might be.
 *
 * The encoder exists for nd-shoot and the host tests. Nothing on the phone
 * saves an image.
 */

#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_image.h"
#include "nd_image_priv.h"
#include "nd_log.h"
#include "nd_paths.h"

/* Decodes from either a FILE or a memory block; exactly one is set. */
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} mem_reader;

static void mem_read_fn(png_structp png, png_bytep out, png_size_t want)
{
    mem_reader *r = png_get_io_ptr(png);

    if (!r || r->pos + want > r->len) {
        png_error(png, "read past end of buffer");
        return;
    }
    memcpy(out, r->data + r->pos, want);
    r->pos += want;
}

/* libpng calls this on a fatal error and must not return; the longjmp target
 * is set up by every caller before any libpng call that can fail. */
static void png_err_fn(png_structp png, png_const_charp msg)
{
    nd_log_err("UI", "png: %s", msg);
    longjmp(png_jmpbuf(png), 1);
}

static void png_warn_fn(png_structp png, png_const_charp msg)
{
    ND_UNUSED(png);
    ND_UNUSED(msg);
}

static nd_image *png_decode(FILE *f, mem_reader *mem)
{
    png_structp png = NULL;
    png_infop info = NULL;
    /* volatile across the setjmp() below: libpng longjmps out of any of the
     * read calls, and these two are what the failure path has to release. */
    nd_image *volatile img = NULL;
    png_bytep *volatile rows = NULL;
    png_uint_32 w = 0, h = 0;
    int bit_depth = 0, colour_type = 0;
    nd_pixfmt fmt;
    int32_t y;

    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, png_err_fn, png_warn_fn);
    if (!png)
        return NULL;
    info = png_create_info_struct(png);
    if (!info)
        goto fail;

    if (setjmp(png_jmpbuf(png)))
        goto fail;

    if (mem)
        png_set_read_fn(png, mem, mem_read_fn);
    else
        png_init_io(png, f);

    png_read_info(png, info);
    png_get_IHDR(png, info, &w, &h, &bit_depth, &colour_type, NULL, NULL, NULL);

    if (w == 0u || h == 0u || w > (png_uint_32)ND_IMAGE_MAX_DIM ||
        h > (png_uint_32)ND_IMAGE_MAX_DIM)
        goto fail;

    /* Storage-format normalisation only. No gamma, no background. */
    if (colour_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (colour_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (bit_depth == 16)
        png_set_strip_16(png);
    if (colour_type == PNG_COLOR_TYPE_GRAY || colour_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    (void)png_set_interlace_handling(png);
    png_read_update_info(png, info);

    colour_type = png_get_color_type(png, info);
    fmt = (colour_type & PNG_COLOR_MASK_ALPHA) ? ND_PIXFMT_RGBA8888 : ND_PIXFMT_RGB888;

    /* owned by the caller; free with nd_image_free() */
    img = nd_image_new((int32_t)w, (int32_t)h, fmt);
    if (!img)
        goto fail;
    if (png_get_rowbytes(png, info) != img->stride)
        goto fail;

    /* one pointer per row, freed before this function returns */
    rows = malloc(sizeof(png_bytep) * (size_t)h);
    if (!rows)
        goto fail;
    for (y = 0; y < (int32_t)h; y++)
        rows[y] = nd_img_row(img, y);

    png_read_image(png, rows);

    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    return img;

fail:
    free(rows);
    nd_image_free(img);
    png_destroy_read_struct(&png, info ? &info : NULL, NULL);
    return NULL;
}

nd_image *nd_image_load_png(const char *path)
{
    char resolved[ND_PATH_MAX];
    FILE *f = NULL;
    nd_image *img = NULL;

    if (!path)
        return NULL;
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return NULL;

    f = fopen(resolved, "rb");
    if (!f)
        return NULL;
    img = png_decode(f, NULL);
    fclose(f);
    return img;
}

nd_image *nd_image_open_mem(const uint8_t *data, size_t len)
{
    mem_reader r;

    if (!data || len < 8u)
        return NULL;

    r.data = data;
    r.len = len;
    r.pos = 0u;

    if (memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0)
        return png_decode(NULL, &r);
    if (data[0] == 0xFFu && data[1] == 0xD8u)
        return nd_jpeg_decode_mem(data, len);
    return NULL;
}

nd_err nd_image_save_png(const nd_image *img, const char *path)
{
    char resolved[ND_PATH_MAX];
    /* volatile across the setjmp() below; see png_decode(). */
    FILE *volatile f = NULL;
    png_structp png = NULL;
    png_infop info = NULL;
    png_bytep *volatile rows = NULL;
    volatile nd_err rc = ND_OK;
    int32_t y;
    int colour_type;

    if (!img || !path)
        return ND_ERR_INVAL;
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return ND_ERR_TOOLONG;

    switch (img->fmt) {
    case ND_PIXFMT_RGB888:
        colour_type = PNG_COLOR_TYPE_RGB;
        break;
    case ND_PIXFMT_RGBA8888:
        colour_type = PNG_COLOR_TYPE_RGB_ALPHA;
        break;
    case ND_PIXFMT_L8:
        colour_type = PNG_COLOR_TYPE_GRAY;
        break;
    default:
        return ND_ERR_INVAL;
    }

    f = fopen(resolved, "wb");
    if (!f) {
        nd_log_err("UI", "png: cannot write %s", resolved);
        return ND_ERR_IO;
    }

    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, png_err_fn, png_warn_fn);
    if (!png) {
        rc = ND_ERR_NOMEM;
        goto done;
    }
    info = png_create_info_struct(png);
    if (!info) {
        rc = ND_ERR_NOMEM;
        goto done;
    }
    if (setjmp(png_jmpbuf(png))) {
        rc = ND_ERR_IO;
        goto done;
    }

    /* one pointer per row, freed at done: */
    rows = malloc(sizeof(png_bytep) * (size_t)img->h);
    if (!rows) {
        rc = ND_ERR_NOMEM;
        goto done;
    }
    for (y = 0; y < img->h; y++)
        rows[y] = nd_img_row(img, y);

    png_init_io(png, f);
    png_set_IHDR(png, info, (png_uint_32)img->w, (png_uint_32)img->h, 8, colour_type,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    png_write_image(png, rows);
    png_write_end(png, NULL);

done:
    free(rows);
    if (png)
        png_destroy_write_struct(&png, info ? &info : NULL);
    fclose(f);
    return rc;
}

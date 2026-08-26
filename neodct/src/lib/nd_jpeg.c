/* nd_jpeg.c -- JPEG decode through libjpeg.
 *
 * WHY THIS TOLERATES BROKEN FILES: the Python sets
 * ImageFile.LOAD_TRUNCATED_IMAGES = True globally before opening the
 * wallpaper, and at least one shipped asset relies on it. A truncated file
 * therefore decodes as far as it got and the remaining rows stay black --
 * which is what Pillow produces, because its own buffer arrives zeroed. It
 * does not become an error, and it does not become a missing wallpaper.
 *
 * Output is always three-channel RGB, including for greyscale JPEGs: libjpeg
 * replicates the luma into all three, exactly as PIL's later convert("RGB")
 * would have. Nothing downstream ever sees a one-channel JPEG.
 */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

#include "nd_image.h"
#include "nd_image_priv.h"
#include "nd_log.h"
#include "nd_paths.h"

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf escape;
} jpeg_err;

static void jpeg_error_exit(j_common_ptr cinfo)
{
    jpeg_err *e = (jpeg_err *)cinfo->err;
    longjmp(e->escape, 1);
}

/* libjpeg reports a truncated stream as a warning, not an error, and then
 * synthesises an EOI. Swallowing the message keeps the boot log readable;
 * the partial image is the intended outcome. */
static void jpeg_emit_message(j_common_ptr cinfo, int msg_level)
{
    ND_UNUSED(cinfo);
    ND_UNUSED(msg_level);
}

static nd_image *jpeg_decode(const uint8_t *data, size_t len)
{
    struct jpeg_decompress_struct cinfo;
    jpeg_err jerr;
    /* volatile because setjmp() is below them: libjpeg's error path longjmps
     * out of the decoder and these three must survive it, or the partial
     * image is leaked and the decoder handle is never destroyed. */
    nd_image *volatile img = NULL;
    volatile bool started = false;
    volatile int32_t y = 0;

    memset(&cinfo, 0, sizeof cinfo);
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;
    jerr.pub.emit_message = jpeg_emit_message;

    if (setjmp(jerr.escape)) {
        /* Whatever was decoded before the failure is kept: the rest of the
         * surface is already zeroed, so a half-read photo shows its top half
         * over black rather than vanishing. */
        goto done;
    }

    jpeg_create_decompress(&cinfo);
    started = true;
    jpeg_mem_src(&cinfo, data, (unsigned long)len);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK)
        goto done;

    cinfo.out_color_space = JCS_RGB;
    if (!jpeg_start_decompress(&cinfo))
        goto done;

    if (cinfo.output_width == 0u || cinfo.output_height == 0u ||
        cinfo.output_width > (unsigned)ND_IMAGE_MAX_DIM ||
        cinfo.output_height > (unsigned)ND_IMAGE_MAX_DIM || cinfo.output_components != 3)
        goto done;

    /* owned by the caller; free with nd_image_free() */
    img = nd_image_new((int32_t)cinfo.output_width, (int32_t)cinfo.output_height, ND_PIXFMT_RGB888);
    if (!img)
        goto done;
    memset(img->pixels, 0, img->stride * (size_t)img->h);

    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = nd_img_row(img, y);
        if (jpeg_read_scanlines(&cinfo, &row, 1u) != 1u)
            break;
        y++;
    }

    (void)jpeg_finish_decompress(&cinfo);

done:
    if (started)
        jpeg_destroy_decompress(&cinfo);
    return img;
}

nd_image *nd_jpeg_decode_mem(const uint8_t *data, size_t len)
{
    if (!data || len < 2u)
        return NULL;
    return jpeg_decode(data, len);
}

nd_image *nd_image_load_jpeg(const char *path)
{
    char resolved[ND_PATH_MAX];
    FILE *f = NULL;
    uint8_t *buf = NULL;
    long size = 0;
    size_t got;
    nd_image *img = NULL;

    if (!path)
        return NULL;
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return NULL;

    f = fopen(resolved, "rb");
    if (!f)
        return NULL;

    /* Slurped whole rather than streamed: jpeg_mem_src is the only source
     * manager that reports a short buffer as a warning instead of blocking,
     * and the truncation tolerance above depends on that. The largest file
     * this ever sees is a user wallpaper. */
    if (fseek(f, 0, SEEK_END) != 0)
        goto done;
    size = ftell(f);
    if (size <= 0 || size > (long)(32 * 1024 * 1024))
        goto done;
    if (fseek(f, 0, SEEK_SET) != 0)
        goto done;

    /* freed at done: before returning */
    buf = malloc((size_t)size);
    if (!buf)
        goto done;
    got = fread(buf, 1u, (size_t)size, f);
    if (got == 0u)
        goto done;

    img = jpeg_decode(buf, got);

done:
    free(buf);
    fclose(f);
    return img;
}

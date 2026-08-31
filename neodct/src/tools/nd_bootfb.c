/* nd_bootfb.c -- the one-bit initramfs framebuffer. See nd_bootfb.h for what
 * this is for and why every one of its guarantees is written the way it is.
 *
 * The only thing worth adding here is the shape of the failure handling: no
 * function in this file has a return value that reports trouble, because the
 * caller is halfway through writing an operating system onto flash. Every
 * failure sets fb->usable to false or is simply dropped, and the install
 * carries on with a dark screen -- which is precisely where the phone is
 * today.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "nd_bootfb.h"
#include "nd_bootfont.h"

static const nd_bootfont *font_for(nd_bootfb_size size)
{
    switch (size) {
    case ND_BOOTFB_STEP:
        return &nd_bootfont_20;
    case ND_BOOTFB_MID:
        return &nd_bootfont_18;
    case ND_BOOTFB_SMALL:
        break;
    }
    return &nd_bootfont_14;
}

static const nd_bootglyph *glyph_for(const nd_bootfont *f, unsigned char c)
{
    if (c < ND_BOOTFONT_FIRST || c > ND_BOOTFONT_LAST)
        return NULL;
    return &f->glyphs[c - ND_BOOTFONT_FIRST];
}

/* ------------------------------------------------------------------ *
 * Open and close
 * ------------------------------------------------------------------ */

static bool finish_open(nd_bootfb *fb, int32_t xres, int32_t yres, int32_t bpp, size_t line_length)
{
    size_t bytes_per_px;

    /* 16 and 32 only. Those are the two neodct_displayd's init_framebuffer()
     * accepts and the two QEMU produces; anything else would need a packer
     * whose colour order this file has deliberately refused to know about. */
    if (bpp != 16 && bpp != 32)
        return false;
    if (xres <= 0 || yres <= 0)
        return false;

    bytes_per_px = (size_t)bpp / 8u;

    /* nd_fb.h: the Python read line_length at an offset that is only right on
     * 64-bit, and it worked solely because the Rockchip driver reports zero
     * there and this fallback then produced the right answer. Keep it. */
    if (line_length == 0u)
        line_length = (size_t)xres * bytes_per_px;
    if (line_length < (size_t)xres * bytes_per_px)
        return false;

    fb->xres = xres;
    fb->yres = yres;
    fb->bpp = bpp;
    fb->line_length = line_length;
    fb->copy_w = (xres < ND_BOOTFB_W) ? xres : ND_BOOTFB_W;
    fb->copy_h = (yres < ND_BOOTFB_H) ? yres : ND_BOOTFB_H;
    fb->row_bytes = (size_t)fb->copy_w * bytes_per_px;

    /* Allocated once, here, and never in the render path -- CODING-STANDARDS
     * section 4. At most 240 * 175 * 4 = 168,000 bytes.
     * Owned by this struct; freed by nd_bootfb_close(). */
    fb->frame = malloc(fb->row_bytes * (size_t)fb->copy_h);
    if (fb->frame == NULL)
        return false;

    fb->usable = true;
    return true;
}

static bool open_fd(nd_bootfb *fb, const char *path)
{
    memset(fb, 0, sizeof *fb);
    fb->fd = -1;
    if (path == NULL)
        return false;
    fb->fd = open(path, O_RDWR | O_CLOEXEC);
    return fb->fd >= 0;
}

bool nd_bootfb_open(nd_bootfb *fb, const char *path)
{
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    if (fb == NULL)
        return false;
    if (!open_fd(fb, path))
        return false;

    memset(&vinfo, 0, sizeof vinfo);
    memset(&finfo, 0, sizeof finfo);
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        nd_bootfb_close(fb);
        return false;
    }
    /* A driver that cannot answer this still has a usable xres and bpp, and
     * the zero it leaves behind is exactly what the fallback above is for. */
    (void)ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo);

    if (!finish_open(fb, (int32_t)vinfo.xres, (int32_t)vinfo.yres, (int32_t)vinfo.bits_per_pixel,
                     (size_t)finfo.line_length)) {
        nd_bootfb_close(fb);
        return false;
    }
    return true;
}

bool nd_bootfb_open_at(nd_bootfb *fb, const char *path, int32_t xres, int32_t yres, int32_t bpp,
                       size_t line_length)
{
    if (fb == NULL)
        return false;
    if (!open_fd(fb, path))
        return false;
    if (!finish_open(fb, xres, yres, bpp, line_length)) {
        nd_bootfb_close(fb);
        return false;
    }
    return true;
}

void nd_bootfb_close(nd_bootfb *fb)
{
    if (fb == NULL)
        return;
    free(fb->frame);
    fb->frame = NULL;
    if (fb->fd >= 0)
        (void)close(fb->fd);
    fb->fd = -1;
    fb->usable = false;
}

/* ------------------------------------------------------------------ *
 * Drawing, all of it into the shadow
 * ------------------------------------------------------------------ */

static void plot(nd_bootfb *fb, int32_t x, int32_t y, uint8_t v)
{
    if (x < 0 || x >= ND_BOOTFB_W || y < 0 || y >= ND_BOOTFB_H)
        return;
    fb->shadow[(size_t)y * ND_BOOTFB_W + (size_t)x] = v;
}

void nd_bootfb_clear(nd_bootfb *fb)
{
    if (fb == NULL || !fb->usable)
        return;
    memset(fb->shadow, 0, sizeof fb->shadow);
}

void nd_bootfb_hline(nd_bootfb *fb, int32_t x0, int32_t x1, int32_t y)
{
    int32_t x;

    if (fb == NULL || !fb->usable)
        return;
    if (y < 0 || y >= ND_BOOTFB_H)
        return;
    if (x0 < 0)
        x0 = 0;
    if (x1 > ND_BOOTFB_W - 1)
        x1 = ND_BOOTFB_W - 1;
    for (x = x0; x <= x1; x++)
        fb->shadow[(size_t)y * ND_BOOTFB_W + (size_t)x] = 1u;
}

void nd_bootfb_fill(nd_bootfb *fb, nd_brect r, bool white)
{
    int32_t x, y;
    uint8_t v = white ? 1u : 0u;

    if (fb == NULL || !fb->usable)
        return;
    /* Inclusive both ends, like Pillow's rectangle(); and like nd_draw's
     * hline(), a run whose x1 is left of its x0 draws nothing at all. */
    for (y = r.y0; y <= r.y1; y++)
        for (x = r.x0; x <= r.x1; x++)
            plot(fb, x, y, v);
}

void nd_bootfb_outline(nd_bootfb *fb, nd_brect r)
{
    int32_t y;

    if (fb == NULL || !fb->usable)
        return;
    nd_bootfb_hline(fb, r.x0, r.x1, r.y0);
    nd_bootfb_hline(fb, r.x0, r.x1, r.y1);
    for (y = r.y0 + 1; y <= r.y1 - 1; y++) {
        plot(fb, r.x0, y, 1u);
        plot(fb, r.x1, y, 1u);
    }
}

int32_t nd_bootfb_text_w(const char *s, nd_bootfb_size size)
{
    const nd_bootfont *f = font_for(size);
    int32_t pen = 0;
    const unsigned char *p;

    if (s == NULL)
        return 0;
    /* The sum of the advances: nd_text_bbox() sets x0 = 0 and x1 = pen, so
     * this is the width nd_text_size() reports and the number the centring
     * arithmetic in nd_progress_draw() divides. */
    for (p = (const unsigned char *)s; *p != '\0'; p++) {
        const nd_bootglyph *g = glyph_for(f, *p);

        if (g != NULL)
            pen += (int32_t)g->advance;
    }
    return pen;
}

nd_bootfb_size nd_bootfb_fit(const char *s, int32_t max_w)
{
    /* nd_font_ladder(ui, "font_n", "font_md", "font_s") -- 20, 18, 14, in
     * that order -- then nd_fit_font()'s "the last one when none fits". */
    static const nd_bootfb_size ladder[3] = {ND_BOOTFB_STEP, ND_BOOTFB_MID, ND_BOOTFB_SMALL};
    size_t i;

    for (i = 0u; i < 3u; i++) {
        if (nd_bootfb_text_w(s, ladder[i]) <= max_w)
            return ladder[i];
    }
    return ladder[2];
}

void nd_bootfb_ellipsize(char *out, size_t out_sz, const char *s, nd_bootfb_size size,
                         int32_t max_w)
{
    char cand[ND_BOOTFB_LINE_MAX];
    size_t len;
    size_t best = 0u;
    size_t off;
    bool found = false;

    if (out == NULL || out_sz == 0u)
        return;
    out[0] = '\0';
    if (s == NULL)
        return;

    len = strlen(s);
    if (nd_bootfb_text_w(s, size) <= max_w) {
        if (len >= out_sz)
            len = out_sz - 1u;
        memcpy(out, s, len);
        out[len] = '\0';
        return;
    }

    /* One byte at a time rather than one codepoint: everything this draws is
     * ASCII, and the glyph table has nothing else in it. */
    for (off = 1u; off <= len; off++) {
        if (off + 4u > sizeof cand)
            break;
        memcpy(cand, s, off);
        memcpy(cand + off, "...", 4u);
        if (nd_bootfb_text_w(cand, size) > max_w)
            break;
        best = off;
        found = true;
    }

    if (!found) {
        /* THE ASYMMETRY: an empty trim returns the ORIGINAL over-wide text,
         * not "". nd_text_ellipsize() says two screens rely on it. */
        if (len >= out_sz)
            len = out_sz - 1u;
        memcpy(out, s, len);
        out[len] = '\0';
        return;
    }

    if (best + 3u >= out_sz)
        best = (out_sz > 4u) ? out_sz - 4u : 0u;
    memcpy(out, s, best);
    memcpy(out + best, "...", 4u);
}

void nd_bootfb_text(nd_bootfb *fb, int32_t x, int32_t y, const char *s, nd_bootfb_size size)
{
    const nd_bootfont *f = font_for(size);
    const unsigned char *p;
    int32_t pen = x;

    if (fb == NULL || !fb->usable || s == NULL)
        return;

    for (p = (const unsigned char *)s; *p != '\0'; p++) {
        const nd_bootglyph *g = glyph_for(f, *p);
        uint32_t row_bytes;
        int32_t gy;

        if (g == NULL)
            continue;
        if (g->ink_w == 0u || g->ink_h == 0u) {
            /* Space, and any glyph the face draws nothing for, still costs
             * its advance -- nd_draw_text() is explicit about this. */
            pen += (int32_t)g->advance;
            continue;
        }

        row_bytes = ((uint32_t)g->ink_w + 7u) / 8u;
        for (gy = 0; gy < (int32_t)g->ink_h; gy++) {
            const uint8_t *row = f->bits + g->offset + (uint32_t)gy * row_bytes;
            int32_t gx;

            for (gx = 0; gx < (int32_t)g->ink_w; gx++) {
                if ((row[gx / 8] & (uint8_t)(0x80u >> (gx % 8))) == 0u)
                    continue;
                plot(fb, pen + g->ink_dx + gx, y + g->ink_dy + gy, 1u);
            }
        }
        pen += (int32_t)g->advance;
    }
}

/* ------------------------------------------------------------------ *
 * Present
 * ------------------------------------------------------------------ */

static bool write_all(int fd, const uint8_t *buf, size_t n)
{
    while (n > 0u) {
        ssize_t got = write(fd, buf, n);

        if (got < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (got == 0)
            return false;
        buf += (size_t)got;
        n -= (size_t)got;
    }
    return true;
}

void nd_bootfb_present(nd_bootfb *fb)
{
    size_t bytes_per_px;
    int32_t y;

    if (fb == NULL || !fb->usable || fb->frame == NULL)
        return;

    bytes_per_px = (size_t)fb->bpp / 8u;

    /* White is all-ones and black is all-zeroes in every packing a 16- or
     * 32-bit framebuffer can have, which is the entire reason this layer is
     * monochrome. No channel order is consulted, because none exists that
     * would change the answer. */
    for (y = 0; y < fb->copy_h; y++) {
        uint8_t *dst = fb->frame + (size_t)y * fb->row_bytes;
        const uint8_t *src = fb->shadow + (size_t)y * ND_BOOTFB_W;
        int32_t x;

        for (x = 0; x < fb->copy_w; x++)
            memset(dst + (size_t)x * bytes_per_px, src[x] != 0u ? 0xFF : 0x00, bytes_per_px);
    }

    if (lseek(fb->fd, 0, SEEK_SET) == (off_t)-1) {
        fb->usable = false;
        return;
    }

    if (fb->line_length == fb->row_bytes && fb->xres == fb->copy_w) {
        /* The live case on both targets: 240x175 @ 32bpp, so the band is the
         * whole framebuffer and this is one contiguous 168,000-byte write --
         * byte for byte what `cat bootlogo.raw > /dev/fb0` does. */
        if (!write_all(fb->fd, fb->frame, fb->row_bytes * (size_t)fb->copy_h))
            fb->usable = false;
        return;
    }

    /* A wider or row-padded framebuffer: the band goes in the top-left
     * corner, one row at a time, and the pixels to the right of it are left
     * exactly as whatever put them there left them. */
    for (y = 0; y < fb->copy_h; y++) {
        if (lseek(fb->fd, (off_t)((size_t)y * fb->line_length), SEEK_SET) == (off_t)-1 ||
            !write_all(fb->fd, fb->frame + (size_t)y * fb->row_bytes, fb->row_bytes)) {
            fb->usable = false;
            return;
        }
    }
}

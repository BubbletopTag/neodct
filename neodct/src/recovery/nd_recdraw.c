/* nd_recdraw.c -- /dev/fb0, three colours, a bitmap font and the two screens
 * recovery needs.
 *
 * ============ ONE CODE PATH, BOTH TARGETS, NOTHING TO DETECT ============
 *
 * There is no output difference between QEMU and the phone to detect.
 *
 *   QEMU     video=Virtual-1:240x175M on the cmdline, so /dev/fb0 already IS
 *            240x175 and IS the screen.
 *   phone    /dev/fb0 is vfb. panel_start() in ndsys-recovery.sh has already
 *            launched neodct_displayd, which forces 240x175 @ 32bpp (16bpp
 *            fallback) and mirrors fb0 to the ST7789 over SPI at 30 fps.
 *
 * So this opens fb0, honours line_length and bits_per_pixel, mmaps and draws
 * -- the same interface panel_show()'s `cat > /dev/fb0` already uses.
 *
 * nd_fb.h's "line_length == 0 means compute it" fallback is copied because it
 * is load-bearing on the Rockchip driver, where the Python read the field at
 * a 64-bit offset that is mmio_start on 32-bit ARM and got away with it only
 * because that field reads zero.
 *
 * ============ THE RENDER PATH ALLOCATES NOTHING ============
 *
 * One mmap at open and static scratch for the two labels. The blit reads the
 * raw file in 8 KB chunks rather than slurping 168 KB, because this runs on a
 * 64 MB device whose cpio is already unpacked into RAM.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/fb.h>
#include <linux/kd.h>

#include "nd_recfont.h"
#include "nd_recui.h"

/* glibc types ioctl's request as unsigned long, musl as int, and this is
 * built under both. Casting at the call site is the only spelling that is
 * warning-clean on each -- nd_pcf8575.c makes the same move. */
#ifdef __GLIBC__
#define IOCTL_REQ(x) ((unsigned long)(x))
#else
#define IOCTL_REQ(x) ((int)(unsigned int)(x))
#endif

static int32_t max32(int32_t a, int32_t b)
{
    return (a > b) ? a : b;
}

static int32_t min32(int32_t a, int32_t b)
{
    return (a < b) ? a : b;
}

/* Python's int() and every ported coordinate: truncate toward zero, never
 * round. nd_draw.h explains why -- rounding moves the scrollbar notch a pixel
 * on most list lengths. */
static int32_t trunc32(double v)
{
    return (int32_t)v;
}

/* ------------------------------------------------------------------ *
 * Pixels
 * ------------------------------------------------------------------ */

/* Black, white and grey are all channel-order-safe: the first two have every
 * component equal to 0 or 255, and grey is three equal 0x80s. That is the
 * whole reason the recovery UI is one bit deep -- see nd_recui.h. */
static uint32_t colour32(nd_reccolour c)
{
    switch (c) {
    case ND_RECCOL_WHITE:
        return 0x00FFFFFFu;
    case ND_RECCOL_GREY:
        return 0x00808080u;
    case ND_RECCOL_BLACK:
    default:
        return 0x00000000u;
    }
}

/* RGB565, low byte first, matching nd_fb_pack_rgb565(). 0x8410 is (16,32,16)
 * in 5-6-5, which is 0x80 in all three channels. */
static uint16_t colour16(nd_reccolour c)
{
    switch (c) {
    case ND_RECCOL_WHITE:
        return 0xFFFFu;
    case ND_RECCOL_GREY:
        return 0x8410u;
    case ND_RECCOL_BLACK:
    default:
        return 0x0000u;
    }
}

static void put_px(nd_recfb *fb, int32_t x, int32_t y, nd_reccolour c)
{
    uint8_t *p;

    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h)
        return;

    p = fb->px + ((size_t)y * fb->stride) + ((size_t)x * (size_t)(fb->bpp / 8));
    if (fb->bpp == 32) {
        uint32_t v = colour32(c);

        p[0] = (uint8_t)(v & 0xFFu);
        p[1] = (uint8_t)((v >> 8) & 0xFFu);
        p[2] = (uint8_t)((v >> 16) & 0xFFu);
        p[3] = 0xFFu;
    } else {
        uint16_t v = colour16(c);

        p[0] = (uint8_t)(v & 0xFFu);
        p[1] = (uint8_t)((v >> 8) & 0xFFu);
    }
}

nd_reccolour nd_recfb_get(const nd_recfb *fb, int32_t x, int32_t y)
{
    const uint8_t *p;

    if (fb == NULL || fb->px == NULL || x < 0 || y < 0 || x >= fb->w || y >= fb->h)
        return ND_RECCOL_BLACK;

    p = fb->px + ((size_t)y * fb->stride) + ((size_t)x * (size_t)(fb->bpp / 8));
    if (fb->bpp == 32) {
        if (p[0] == 0xFFu && p[1] == 0xFFu && p[2] == 0xFFu)
            return ND_RECCOL_WHITE;
        if (p[0] == 0x80u && p[1] == 0x80u && p[2] == 0x80u)
            return ND_RECCOL_GREY;
        return ND_RECCOL_BLACK;
    }
    if (p[0] == 0xFFu && p[1] == 0xFFu)
        return ND_RECCOL_WHITE;
    if (p[0] == 0x10u && p[1] == 0x84u)
        return ND_RECCOL_GREY;
    return ND_RECCOL_BLACK;
}

/* ------------------------------------------------------------------ *
 * Open and close
 * ------------------------------------------------------------------ */

int nd_recfb_open(nd_recfb *fb, const char *path)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;

    if (fb == NULL || path == NULL)
        return -1;

    memset(fb, 0, sizeof *fb);
    fb->fd = open(path, O_RDWR | O_CLOEXEC);
    if (fb->fd < 0) {
        fprintf(stderr, "nd-recui: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    memset(&var, 0, sizeof var);
    memset(&fix, 0, sizeof fix);
    if (ioctl(fb->fd, IOCTL_REQ(FBIOGET_VSCREENINFO), &var) < 0 ||
        ioctl(fb->fd, IOCTL_REQ(FBIOGET_FSCREENINFO), &fix) < 0) {
        fprintf(stderr, "nd-recui: %s is not a framebuffer: %s\n", path, strerror(errno));
        goto fail;
    }

    fb->w = (int32_t)var.xres;
    fb->h = (int32_t)var.yres;
    fb->bpp = (int32_t)var.bits_per_pixel;
    fb->stride = fix.line_length;

    /* nd_fb.h: load-bearing on the Rockchip driver. A driver that pads its
     * rows would change the rendering, and finding that out deliberately is
     * better than a screen full of diagonal stripes. */
    if (fb->stride == 0u)
        fb->stride = (size_t)fb->w * (size_t)(fb->bpp / 8);

    /* Refusing an unexpected depth rather than guessing at it: a wrong guess
     * paints garbage over a phone somebody is trying to rescue, and the tty
     * menu this falls back to genuinely works. */
    if ((fb->bpp != 32 && fb->bpp != 16) || fb->w <= 0 || fb->h <= 0) {
        fprintf(stderr, "nd-recui: %s is %dx%d @ %dbpp, which this cannot draw\n", path, fb->w,
                fb->h, fb->bpp);
        goto fail;
    }

    fb->px_len = fb->stride * (size_t)fb->h;
    fb->px = mmap(NULL, fb->px_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->px == MAP_FAILED) {
        fb->px = NULL;
        fprintf(stderr, "nd-recui: cannot map %s: %s\n", path, strerror(errno));
        goto fail;
    }
    fb->owns_mem = false;
    return 0;

fail:
    (void)close(fb->fd);
    fb->fd = -1;
    return -1;
}

int nd_recfb_open_mem(nd_recfb *fb, int32_t w, int32_t h, int32_t bpp)
{
    if (fb == NULL || w <= 0 || h <= 0 || (bpp != 32 && bpp != 16))
        return -1;

    memset(fb, 0, sizeof *fb);
    fb->fd = -1;
    fb->w = w;
    fb->h = h;
    fb->bpp = bpp;
    fb->stride = (size_t)w * (size_t)(bpp / 8);
    fb->px_len = fb->stride * (size_t)h;
    /* owned here; released by nd_recfb_close() */
    fb->px = calloc(1u, fb->px_len);
    if (fb->px == NULL)
        return -1;
    fb->owns_mem = true;
    return 0;
}

void nd_recfb_close(nd_recfb *fb)
{
    if (fb == NULL)
        return;
    if (fb->px != NULL) {
        if (fb->owns_mem)
            free(fb->px);
        else
            (void)munmap(fb->px, fb->px_len);
        fb->px = NULL;
    }
    if (fb->fd >= 0) {
        (void)close(fb->fd);
        fb->fd = -1;
    }
}

/* ------------------------------------------------------------------ *
 * Shapes
 * ------------------------------------------------------------------ */

void nd_recdraw_rect(nd_recfb *fb, int32_t x0, int32_t y0, int32_t x1, int32_t y1, nd_reccolour c)
{
    int32_t x;
    int32_t y;

    if (fb == NULL || fb->px == NULL)
        return;

    /* Inclusive of both corners, and clipped rather than refused -- the
     * Python names x=240 on a 240-wide canvas in more than one place and
     * Pillow clips it. Port the coordinates, not a fix for them. */
    x0 = max32(0, x0);
    y0 = max32(0, y0);
    x1 = min32(fb->w - 1, x1);
    y1 = min32(fb->h - 1, y1);

    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++)
            put_px(fb, x, y, c);
    }
}

void nd_recdraw_clear(nd_recfb *fb, int32_t y0, int32_t y1, nd_reccolour c)
{
    if (fb == NULL)
        return;
    nd_recdraw_rect(fb, 0, y0, fb->w - 1, y1, c);
}

int nd_recdraw_blit_raw(nd_recfb *fb, const char *path)
{
    /* 8 KB at a time. One 240x175 XRGB8888 frame is 168,000 bytes and this
     * device has 64 MB with the whole cpio already resident. */
    uint8_t chunk[8192];
    int fd;
    int32_t x = 0;
    int32_t y = 0;

    if (fb == NULL || fb->px == NULL || path == NULL)
        return -1;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    for (;;) {
        ssize_t got = read(fd, chunk, sizeof chunk);
        ssize_t i;

        if (got < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (got == 0)
            break;

        /* The blobs mkinitramfs.py builds are XRGB8888 -- bytes B, G, R, X --
         * and are exactly two colours. Anything not black is white, which is
         * the whole conversion and is why this works at 16bpp too. */
        for (i = 0; i + 3 < got; i += 4) {
            nd_reccolour c = (chunk[i] != 0u || chunk[i + 1] != 0u || chunk[i + 2] != 0u)
                                 ? ND_RECCOL_WHITE
                                 : ND_RECCOL_BLACK;

            put_px(fb, x, y, c);
            x++;
            if (x >= fb->w) {
                x = 0;
                y++;
                if (y >= fb->h) {
                    (void)close(fd);
                    return 0;
                }
            }
        }
    }
    (void)close(fd);
    return 0;
}

/* ------------------------------------------------------------------ *
 * Text
 * ------------------------------------------------------------------ */

static const nd_recfont *font_of(nd_recfontsize f)
{
    return (f == ND_RECFONT_SMALL) ? &nd_recfont_14 : &nd_recfont_18;
}

/* Anything outside printable ASCII renders as '?'. A package name comes off a
 * FAT card and can hold whatever somebody typed; dropping the character
 * silently would make two different files look like the same one. */
static const nd_recglyph *glyph_of(const nd_recfont *f, unsigned char ch)
{
    if (ch < ND_RECFONT_FIRST || ch > ND_RECFONT_LAST)
        ch = (unsigned char)'?';
    return &f->glyphs[ch - ND_RECFONT_FIRST];
}

void nd_recdraw_text_size(nd_recfontsize fs, const char *s, int32_t *w, int32_t *h)
{
    const nd_recfont *f = font_of(fs);
    const unsigned char *p;
    int32_t pen = 0;
    /* nd_text_bbox(): the box starts collapsed ON THE BASELINE, not empty
     * and not on the first glyph's ink. That is what makes the height of "_"
     * come out at 3 px rather than 1 -- the underscore sits above the
     * baseline and the box is stretched down to meet it. */
    int32_t top = f->ascent;
    int32_t bottom = f->ascent;

    if (s == NULL)
        s = "";
    for (p = (const unsigned char *)s; *p != '\0'; p++) {
        const nd_recglyph *g = glyph_of(f, *p);

        if (g->h > 0) {
            int32_t dy = g->dy;

            if (dy < top)
                top = dy;
            if (dy + g->h > bottom)
                bottom = dy + g->h;
        }
        pen += g->advance;
    }

    /* The width is the PEN, not the ink: Pillow's textbbox reports the full
     * advance, so a trailing space counts. */
    if (w != NULL)
        *w = pen;
    if (h != NULL)
        *h = bottom - top;
}

void nd_recdraw_text(nd_recfb *fb, int32_t x, int32_t y, const char *s, nd_recfontsize fs,
                     nd_reccolour c)
{
    const nd_recfont *f = font_of(fs);
    const unsigned char *p;
    int32_t pen = x;

    if (fb == NULL || fb->px == NULL || s == NULL)
        return;

    for (p = (const unsigned char *)s; *p != '\0'; p++) {
        const nd_recglyph *g = glyph_of(f, *p);
        int32_t gy;
        size_t row_bytes = ((size_t)g->w + 7u) / 8u;

        if (g->w == 0 || g->h == 0) {
            /* A blank glyph still costs its advance -- a space is 5 px of
             * nothing and every centred string measures it. */
            pen += g->advance;
            continue;
        }
        for (gy = 0; gy < (int32_t)g->h; gy++) {
            const uint8_t *row = f->bits + g->off + ((size_t)gy * row_bytes);
            int32_t gx;

            for (gx = 0; gx < (int32_t)g->w; gx++) {
                if ((row[gx / 8] & (0x80u >> (gx % 8))) != 0u)
                    put_px(fb, pen + gx, y + g->dy + gy, c);
            }
        }
        pen += g->advance;
    }
}

int32_t nd_recdraw_text_fit(char *out, size_t out_sz, const char *s, nd_recfontsize fs,
                            int32_t room)
{
    const nd_recfont *f = font_of(fs);
    const unsigned char *p;
    int32_t pen = 0;
    size_t n = 0u;

    if (out == NULL || out_sz == 0u)
        return 0;
    out[0] = '\0';
    if (s == NULL)
        return 0;

    for (p = (const unsigned char *)s; *p != '\0'; p++) {
        const nd_recglyph *g = glyph_of(f, *p);

        if (pen + g->advance > room)
            break;
        if (n + 1u >= out_sz)
            break;
        out[n++] = (char)*p;
        pen += g->advance;
    }
    out[n] = '\0';
    return pen;
}

/* ------------------------------------------------------------------ *
 * The vertical list
 * ------------------------------------------------------------------ */

void nd_reclist_metrics_of(int32_t width, int32_t content_bottom, nd_reclist_metrics *m)
{
    int32_t content_height;

    if (m == NULL)
        return;

    /* nd_ui_header_divider_y(): max(30, (int)(H * 0.11)), which is 30 on this
     * panel only because the floor wins. */
    m->header_y = max32(30, trunc32((double)ND_RECUI_H * 0.11));
    m->y_start = m->header_y + 10;
    content_height = max32(1, content_bottom - m->y_start - 4);
    m->line_height = max32(28, content_height / 3);
    m->item_height = max32(24, m->line_height - 4);
    m->max_lines = min32(3, max32(1, content_height / m->line_height));
    m->bar_x = width - 5;
    m->selected_right = max32(20, m->bar_x - 10);
    m->track_top = m->y_start;
    m->track_bottom = max32(m->track_top, content_bottom - 5);
}

size_t nd_reclist_window(size_t selected, size_t window_start, size_t n_items, size_t max_lines)
{
    size_t max_start;

    if (max_lines == 0u)
        return 0u;
    if (selected < window_start)
        window_start = selected;
    else if (selected >= window_start + max_lines)
        window_start = selected - max_lines + 1u;

    max_start = (n_items > max_lines) ? (n_items - max_lines) : 0u;
    if (window_start > max_start)
        window_start = max_start;
    return window_start;
}

int32_t nd_reclist_notch_y(const nd_reclist_metrics *m, size_t selected, size_t n_items)
{
    double notch;

    if (m == NULL)
        return 0;
    if (n_items > 1u) {
        double step = (double)(m->track_bottom - m->track_top) / (double)(n_items - 1u);

        notch = (double)m->track_top + ((double)selected * step);
    } else {
        notch = (double)m->track_top;
    }
    /* A FLOAT that Pillow truncates. Rounding it instead moves the notch a
     * pixel on most list lengths -- nd_vlist.c says so and means it. */
    return trunc32(notch);
}

void nd_reclist_draw(nd_recfb *fb, const char *title, const char *const *items, size_t n_items,
                     size_t selected, size_t *window_start)
{
    nd_reclist_metrics m;
    char label[ND_RECUI_ITEM_MAX];
    size_t start;
    size_t i;
    int32_t notch;

    if (fb == NULL || fb->px == NULL)
        return;

    nd_reclist_metrics_of(fb->w, ND_RECUI_CONTENT_BOTTOM, &m);

    /* Rows 0..content_bottom only, exactly as nd_vlist_draw() clears. The
     * strip below is never written by recovery, so it stays the black the
     * mapping was zeroed to at open. */
    nd_recdraw_clear(fb, 0, ND_RECUI_CONTENT_BOTTOM, ND_RECCOL_BLACK);

    /* The title sits at y=0, unlike every other widget's y=5 -- that is what
     * the phone's lists look like. There is no breadcrumb here to reserve
     * width for: recovery is not an app and has no app id. */
    if (title != NULL && title[0] != '\0') {
        (void)nd_recdraw_text_fit(label, sizeof label, title, ND_RECFONT_LARGE, fb->w - 10);
        nd_recdraw_text(fb, 5, 0, label, ND_RECFONT_LARGE, ND_RECCOL_WHITE);
    }
    nd_recdraw_rect(fb, 0, m.header_y, fb->w - 1, m.header_y, ND_RECCOL_WHITE);

    start = nd_reclist_window(selected, (window_start != NULL) ? *window_start : 0u, n_items,
                              (size_t)m.max_lines);
    if (window_start != NULL)
        *window_start = start;

    for (i = 0u; i < (size_t)m.max_lines; i++) {
        size_t idx = start + i;
        int32_t y;
        int32_t text_h = 0;
        int32_t text_y;
        const char *text;

        if (idx >= n_items)
            break;
        text = (items[idx] != NULL) ? items[idx] : "";
        y = m.y_start + (int32_t)i * m.line_height;

        /* The INK height of THIS string, so a row of "reboot" and a row of
         * "wipe user data" do not sit on the same pixel. That is the house
         * look, and it is why the generated font stores a per-glyph h. */
        (void)nd_recdraw_text_fit(label, sizeof label, text, ND_RECFONT_LARGE,
                                  m.selected_right - 10 - 4);
        nd_recdraw_text_size(ND_RECFONT_LARGE, label, NULL, &text_h);
        text_y = y + max32(0, (m.item_height - text_h) / 2);

        if (idx == selected) {
            nd_recdraw_rect(fb, 0, y, m.selected_right, y + m.item_height, ND_RECCOL_WHITE);
            nd_recdraw_text(fb, 10, text_y, label, ND_RECFONT_LARGE, ND_RECCOL_BLACK);
        } else {
            nd_recdraw_text(fb, 10, text_y, label, ND_RECFONT_LARGE, ND_RECCOL_WHITE);
        }
    }

    /* Grey, width 1. nd_vlist.c: "the only grey pixels in the entire
     * framework". Every other track in the OS is white and width 2. */
    nd_recdraw_rect(fb, m.bar_x, m.track_top, m.bar_x, m.track_bottom, ND_RECCOL_GREY);
    notch = nd_reclist_notch_y(&m, selected, n_items);
    nd_recdraw_rect(fb, m.bar_x - 2, notch - 3, m.bar_x + 2, notch + 3, ND_RECCOL_WHITE);
}

int32_t nd_reclist_key(int32_t key, size_t n_items, size_t *selected)
{
    if (selected == NULL || n_items == 0u)
        return ND_RECLIST_BACK;

    if (key == ND_RECKEY_DOWN) {
        if (*selected < n_items - 1u)
            (*selected)++;
        return ND_RECLIST_CONTINUE;
    }
    if (key == ND_RECKEY_UP) {
        if (*selected > 0u)
            (*selected)--;
        return ND_RECLIST_CONTINUE;
    }
    /* Digits 1..9 only -- codes 2..10. Code 11 is '0' and is NOT a shortcut,
     * matching nd_vlist_handle_key(). A shortcut past the end of the list is
     * ignored rather than clamped. */
    if (key >= ND_RECKEY_1 && key <= ND_RECKEY_9) {
        size_t idx = (size_t)(key - ND_RECKEY_1);

        if (idx < n_items)
            return (int32_t)idx;
        return ND_RECLIST_CONTINUE;
    }
    if (key == ND_RECKEY_ENTER)
        return (int32_t)*selected;
    if (key == ND_RECKEY_CLEAR)
        return ND_RECLIST_BACK;
    return ND_RECLIST_CONTINUE;
}

/* ------------------------------------------------------------------ *
 * The progress screen
 * ------------------------------------------------------------------ */

void nd_recprogress_metrics_of(int32_t width, int32_t content_bottom, nd_recprogress_metrics *m)
{
    int32_t bar_top;
    int32_t step_h = 0;

    if (m == NULL)
        return;

    /* nd_progress_init(): int(content_bottom * 0.55) = 79 on this panel, and
     * every other box hangs off it. */
    bar_top = trunc32((double)content_bottom * 0.55);
    m->bar_x0 = ND_RECUI_BAR_MARGIN;
    m->bar_y0 = bar_top;
    m->bar_x1 = width - ND_RECUI_BAR_MARGIN;
    m->bar_y1 = bar_top + ND_RECUI_BAR_HEIGHT;

    /* The label's box is the font's "Ag" height, not the step string's, so
     * the label does not jump between steps. 14 px of air above the bar is
     * what keeps a two-line-worth label off it. */
    nd_recdraw_text_size(ND_RECFONT_LARGE, "Ag", NULL, &step_h);
    m->label_y = bar_top - 14 - step_h;
    m->status_y = m->bar_y1 + 9;
}

int32_t nd_recprogress_percent(int64_t done, int64_t total)
{
    int32_t percent;

    /* int(done * 100 / total): Python's true division then int(), which
     * truncates toward zero. A double reproduces it exactly. total == 0
     * means "done", not a divide by zero. */
    percent = (total != 0) ? trunc32((double)done * 100.0 / (double)total) : 100;
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    return percent;
}

int32_t nd_recprogress_filled(int32_t span, int32_t percent)
{
    if (span <= 0)
        return 0;
    return trunc32((double)span * (double)percent / 100.0);
}

void nd_recprogress_human(char *out, size_t out_sz, int64_t bytes)
{
    static const char units[] = {'B', 'K', 'M', 'G'};
    double v = (double)bytes;
    size_t u = 0u;

    if (out == NULL || out_sz == 0u)
        return;
    while (v >= 1024.0 && u + 1u < sizeof units) {
        v /= 1024.0;
        u++;
    }
    /* Whole bytes read as bytes; anything scaled gets one decimal, which is
     * as much as 220 px of bar has room for beside a percentage. */
    if (u == 0u)
        (void)snprintf(out, out_sz, "%lldB", (long long)bytes);
    else
        (void)snprintf(out, out_sz, "%.1f%c", v, units[u]);
}

void nd_recprogress_init(nd_recprogress *p, nd_recfb *fb, const char *step, const char *header)
{
    if (p == NULL)
        return;
    memset(p, 0, sizeof *p);
    p->fb = fb;
    p->step = (step != NULL) ? step : "";
    p->header = header;
    p->percent = -1; /* Python's None: nothing drawn yet */
    if (fb != NULL)
        nd_recprogress_metrics_of(fb->w, ND_RECUI_CONTENT_BOTTOM, &p->m);
}

bool nd_recprogress_draw(nd_recprogress *p, int64_t done, int64_t total)
{
    char label[ND_RECUI_ITEM_MAX];
    char reading[16];
    char detail[48];
    char human_done[16];
    char human_total[16];
    int32_t percent;
    int32_t span;
    int32_t filled;
    int32_t w = 0;
    int32_t h = 0;

    if (p == NULL || p->fb == NULL || p->fb->px == NULL)
        return false;

    percent = nd_recprogress_percent(done, total);
    if (percent == p->percent)
        return false;
    p->percent = percent;

    nd_recdraw_clear(p->fb, 0, ND_RECUI_CONTENT_BOTTOM, ND_RECCOL_BLACK);

    if (p->header != NULL && p->header[0] != '\0') {
        (void)nd_recdraw_text_fit(label, sizeof label, p->header, ND_RECFONT_SMALL, p->fb->w - 20);
        nd_recdraw_text(p->fb, 10, 4, label, ND_RECFONT_SMALL, ND_RECCOL_WHITE);
        nd_recdraw_text_size(ND_RECFONT_SMALL, "Ag", NULL, &h);
        nd_recdraw_rect(p->fb, 10, 4 + h + 5, p->fb->w - 10, 4 + h + 5, ND_RECCOL_WHITE);
    }

    /* Nothing is ever drawn on top of the bar. A percentage sitting across
     * its own fill is the one thing that makes a progress bar look broken --
     * the label goes above, the reading below, and they never overlap. */
    if (p->step[0] != '\0') {
        (void)nd_recdraw_text_fit(label, sizeof label, p->step, ND_RECFONT_LARGE, p->fb->w - 16);
        nd_recdraw_text_size(ND_RECFONT_LARGE, label, &w, NULL);
        nd_recdraw_text(p->fb, max32(0, (p->fb->w - w) / 2), p->m.label_y, label, ND_RECFONT_LARGE,
                        ND_RECCOL_WHITE);
    }

    /* A one-pixel outline drawn INSIDE the inclusive box, so the frame
     * occupies rows 79 and 93 and columns 20 and 220. */
    nd_recdraw_rect(p->fb, p->m.bar_x0, p->m.bar_y0, p->m.bar_x1, p->m.bar_y0, ND_RECCOL_WHITE);
    nd_recdraw_rect(p->fb, p->m.bar_x0, p->m.bar_y1, p->m.bar_x1, p->m.bar_y1, ND_RECCOL_WHITE);
    nd_recdraw_rect(p->fb, p->m.bar_x0, p->m.bar_y0, p->m.bar_x0, p->m.bar_y1, ND_RECCOL_WHITE);
    nd_recdraw_rect(p->fb, p->m.bar_x1, p->m.bar_y0, p->m.bar_x1, p->m.bar_y1, ND_RECCOL_WHITE);

    span = (p->m.bar_x1 - ND_RECUI_BAR_INSET) - (p->m.bar_x0 + ND_RECUI_BAR_INSET);
    filled = nd_recprogress_filled(span, percent);
    if (filled > 0)
        nd_recdraw_rect(p->fb, p->m.bar_x0 + ND_RECUI_BAR_INSET, p->m.bar_y0 + ND_RECUI_BAR_INSET,
                        p->m.bar_x0 + ND_RECUI_BAR_INSET + filled, p->m.bar_y1 - ND_RECUI_BAR_INSET,
                        ND_RECCOL_WHITE);

    (void)snprintf(reading, sizeof reading, "%d%%", (int)percent);
    nd_recdraw_text(p->fb, p->m.bar_x0, p->m.status_y, reading, ND_RECFONT_SMALL, ND_RECCOL_WHITE);

    nd_recprogress_human(human_done, sizeof human_done, done);
    nd_recprogress_human(human_total, sizeof human_total, total);
    (void)snprintf(detail, sizeof detail, "%s / %s", human_done, human_total);
    nd_recdraw_text_size(ND_RECFONT_SMALL, detail, &w, NULL);
    nd_recdraw_text(p->fb, p->m.bar_x1 - w, p->m.status_y, detail, ND_RECFONT_SMALL,
                    ND_RECCOL_WHITE);

    return true;
}

/* ------------------------------------------------------------------ *
 * The message page
 * ------------------------------------------------------------------ */

void nd_recmessage_draw(nd_recfb *fb, const char *const *lines, size_t n_lines)
{
    char label[ND_RECUI_ITEM_MAX];
    int32_t line_h = 0;
    int32_t y;
    int32_t prompt_y;
    size_t i;

    if (fb == NULL || fb->px == NULL)
        return;

    nd_recdraw_clear(fb, 0, ND_RECUI_CONTENT_BOTTOM, ND_RECCOL_BLACK);
    nd_recdraw_text_size(ND_RECFONT_LARGE, "Ag", NULL, &line_h);
    y = 10;
    /* The prompt is pinned to the bottom of the content area rather than
     * following the text, so it is in the same place on every message screen
     * and a person learns where to look once. */
    prompt_y = ND_RECUI_CONTENT_BOTTOM - line_h - 4;

    for (i = 0u; i < n_lines && i < ND_RECUI_MAX_MSG_LINE; i++) {
        const char *text = (lines[i] != NULL) ? lines[i] : "";

        /* Stop before the prompt rather than drawing through it. put_px()
         * would clip at the panel edge, but not before it had painted over
         * the softkey strip, where the caller's legend lives. */
        if (y + line_h > prompt_y)
            break;
        (void)nd_recdraw_text_fit(label, sizeof label, text, ND_RECFONT_LARGE, fb->w - 20);
        nd_recdraw_text(fb, 10, y, label, ND_RECFONT_LARGE, ND_RECCOL_WHITE);
        y += line_h + 6;
    }

    nd_recdraw_text(fb, 10, prompt_y, "Press any key", ND_RECFONT_SMALL, ND_RECCOL_WHITE);
}

/* ------------------------------------------------------------------ *
 * The yes/no page
 * ------------------------------------------------------------------ */

void nd_recconfirm_draw(nd_recfb *fb, const char *question, int selected)
{
    static const char *const options[2] = {"yes", "no"};
    nd_reclist_metrics m;
    char line[ND_RECUI_ITEM_MAX];
    const char *rest;
    int32_t line_h = 0;
    int32_t y;
    int32_t option_y;
    int i;

    if (fb == NULL || fb->px == NULL)
        return;

    nd_reclist_metrics_of(fb->w, ND_RECUI_CONTENT_BOTTOM, &m);
    nd_recdraw_clear(fb, 0, ND_RECUI_CONTENT_BOTTOM, ND_RECCOL_BLACK);

    /* No title, no divider: see the header. Both of them together cost the
     * fourth line of the question, and the fourth line is where "settings
     * will be erased" lives. */
    nd_recdraw_text_size(ND_RECFONT_SMALL, "Ag", NULL, &line_h);
    y = 4;
    option_y = ND_RECUI_CONTENT_BOTTOM - (2 * m.item_height) - 4;
    rest = (question != NULL) ? question : "";

    while (*rest != '\0' && y + line_h < option_y) {
        size_t n;

        (void)nd_recdraw_text_fit(line, sizeof line, rest, ND_RECFONT_SMALL, fb->w - 20);
        n = strlen(line);
        if (n == 0u)
            break;
        /* Break on the last space that fits, so a word is not split across
         * two lines. With no space in reach the hard cut stands -- a
         * forty-character token has to go somewhere. */
        if (rest[n] != '\0' && rest[n] != ' ') {
            size_t back = n;

            while (back > 0u && line[back - 1] != ' ')
                back--;
            if (back > 0u) {
                line[back - 1] = '\0';
                n = back;
            }
        }
        nd_recdraw_text(fb, 10, y, line, ND_RECFONT_SMALL, ND_RECCOL_WHITE);
        y += line_h + 3;
        rest += n;
        while (*rest == ' ')
            rest++;
    }

    for (i = 0; i < 2; i++) {
        int32_t row_y = option_y + i * m.item_height;
        int32_t text_h = 0;
        int32_t text_y;

        nd_recdraw_text_size(ND_RECFONT_LARGE, options[i], NULL, &text_h);
        text_y = row_y + max32(0, (m.item_height - text_h) / 2);
        if (i == selected) {
            nd_recdraw_rect(fb, 0, row_y, m.selected_right, row_y + m.item_height, ND_RECCOL_WHITE);
            nd_recdraw_text(fb, 10, text_y, options[i], ND_RECFONT_LARGE, ND_RECCOL_BLACK);
        } else {
            nd_recdraw_text(fb, 10, text_y, options[i], ND_RECFONT_LARGE, ND_RECCOL_WHITE);
        }
    }
}

/* ------------------------------------------------------------------ *
 * The VT
 * ------------------------------------------------------------------ */

static int vt_open(void)
{
    int fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);

    if (fd < 0)
        fd = open("/dev/tty1", O_RDWR | O_CLOEXEC);
    return fd;
}

void nd_recvt_graphics(void)
{
    int fd = vt_open();

    /* Ignored on failure: a build with no VT loses nothing, and the whole
     * point of this call is to stop fbcon echoing keys over our pixels. */
    if (fd < 0)
        return;
    (void)ioctl(fd, IOCTL_REQ(KDSETMODE), KD_GRAPHICS);
    (void)close(fd);
}

void nd_recvt_text(void)
{
    int fd = vt_open();

    if (fd < 0)
        return;
    (void)ioctl(fd, IOCTL_REQ(KDSETMODE), KD_TEXT);
    (void)close(fd);
}

/* nd_bootfb.h -- a one-bit framebuffer for programs that run in the
 * initramfs, before /NeoDCT exists and with no libneodct to link against.
 *
 * Two callers are expected: nd-bootbar, which draws install progress while
 * ndsys-apply.sh writes the system partition, and -- if the recovery-UI
 * workstream wants it -- nd-recui. It is deliberately the smallest thing both
 * can share, and it knows nothing about updates or menus.
 *
 * ============ IT CAN NEVER FAIL ITS CALLER ============
 *
 * nd_bootfb_open() returning false makes every other call in this header a
 * no-op that returns harmless values. There is no error path a caller has to
 * handle, because the caller is in the middle of writing an operating system
 * onto flash and must not care about a screen. A missing /dev/fb0, a bit
 * depth nobody expected, a failed write: all of them mean "draw nothing" and
 * none of them mean "stop".
 *
 * ============ ONE BIT DEEP, ON PURPOSE ============
 *
 * White and black are the same bytes under any channel permutation, and the
 * two framebuffers NeoDCT runs on genuinely disagree about where red sits:
 * QEMU's virtio-gpu reports red.offset 16 (B G R x) and the phone's vfb
 * reports 0 (R G B x). mkinitramfs.py's bmp_to_xrgb8888() documents the same
 * trap for the splash blobs -- "ON THE PHONE THIS IS THE WRONG WAY ROUND AND
 * IT DOES NOT MATTER YET". Monochrome deletes the whole class of bug rather
 * than solving it twice, so there is no colour in this layer and there must
 * never be one.
 *
 * ============ GEOMETRY IS READ, NEVER ASSUMED ============
 *
 * FBIOGET_VSCREENINFO for xres/yres/bits_per_pixel and FBIOGET_FSCREENINFO
 * for line_length, with nd_fb.h's load-bearing "line_length == 0 means
 * compute it" fallback copied verbatim -- the Rockchip driver reports zero
 * there and the running UI has always depended on that answer.
 *
 * Drawing happens in a fixed 240x175 shadow, which is the panel the whole UI
 * is laid out for, and the shadow is blitted to the TOP-LEFT of whatever the
 * framebuffer turns out to be. That is exactly what `cat bootlogo.raw >
 * /dev/fb0` does today in panel_show(), so a boot bar lands where the boot
 * logo did.
 *
 * ============ PRESENT REPAINTS THE WHOLE BAND ============
 *
 * 240x175x4 = 168,000 bytes, every frame, even for a bar that grew by two
 * pixels. Deliberate: the framebuffer console is bound to the same fb0, so a
 * kernel message printed during a UBI erase can scribble across the bar, and
 * a full repaint heals on the next percentage step. It costs no SPI traffic
 * on the phone -- neodct_displayd memcmp's each frame against the last and
 * sends only the changed rectangle.
 *
 * ============ WHY write() AND NOT mmap() ============
 *
 * Because `cat splash.raw > /dev/fb0` is the one panel write this project has
 * already proved works on both targets, and it is a write(). It also behaves
 * identically when the "framebuffer" is an ordinary file, which is what lets
 * the host tests and the frame capture exercise this code rather than a
 * stand-in for it.
 */

#ifndef ND_BOOTFB_H_INCLUDED
#define ND_BOOTFB_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The panel every NeoDCT screen is laid out for. Matches UI_W/UI_H, the
 * splash blobs in mkinitramfs.py, and neodctDisplay.c's FB_W/FB_H. */
#define ND_BOOTFB_W 240
#define ND_BOOTFB_H 175

/* Inclusive on all four sides, like nd_rect and like Pillow's rectangle():
 * ND_BRECT(20, 79, 220, 93) is 201 columns by 15 rows. The widget code this
 * mirrors depends on that, so do not quietly make it exclusive. */
typedef struct {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
} nd_brect;

#define ND_BRECT(a, b, c, d) \
    (nd_brect)               \
    {                        \
        (a), (b), (c), (d)   \
    }

/* The three sizes nd_font_ladder() offers the running UI, and the three the
 * generated tables in nd_bootfont.c carry. */
typedef enum { ND_BOOTFB_SMALL = 14, ND_BOOTFB_MID = 18, ND_BOOTFB_STEP = 20 } nd_bootfb_size;

/* Longest string this layer will lay out. ND_TEXT_LINE_MAX in libneodct. */
#define ND_BOOTFB_LINE_MAX 256

typedef struct {
    int fd;      /* -1 once closed, or when open() never succeeded */
    bool usable; /* false makes every call below a no-op          */
    int32_t xres;
    int32_t yres;
    int32_t bpp;
    size_t line_length;
    int32_t copy_w; /* min(xres, ND_BOOTFB_W)  */
    int32_t copy_h; /* min(yres, ND_BOOTFB_H)  */
    size_t row_bytes;
    uint8_t *frame; /* copy_h rows of row_bytes, allocated once at open */
    /* One byte per pixel, 0 or 1. 42,000 bytes; the caller owns the struct,
     * so put it in static storage rather than on a stack. */
    uint8_t shadow[ND_BOOTFB_W * ND_BOOTFB_H];
} nd_bootfb;

/* Open PATH, read its geometry, allocate the frame. false means "there is no
 * screen here"; the struct is still safe to pass to everything below. */
bool nd_bootfb_open(nd_bootfb *fb, const char *path);

/* Same, with the geometry supplied rather than asked for. This is how the
 * host tests and neodct/tools/bootbar_frames.py draw into an ordinary file:
 * a regular file has no FBIOGET_VSCREENINFO to answer, and inventing a
 * default inside nd_bootfb_open() would be exactly the assumption the header
 * above promises not to make. Never used on a device. */
bool nd_bootfb_open_at(nd_bootfb *fb, const char *path, int32_t xres, int32_t yres, int32_t bpp,
                       size_t line_length);

void nd_bootfb_close(nd_bootfb *fb);

/* All drawing is into the shadow and costs nothing until present(). */
void nd_bootfb_clear(nd_bootfb *fb);
void nd_bootfb_fill(nd_bootfb *fb, nd_brect r, bool white);
void nd_bootfb_outline(nd_bootfb *fb, nd_brect r);
void nd_bootfb_hline(nd_bootfb *fb, int32_t x0, int32_t x1, int32_t y);

/* Ink width of s: the sum of the advances, which is what nd_text_size()
 * reports as a string's width (nd_text_bbox sets x0 = 0 and x1 = pen). Safe
 * on a closed fb -- it reads only the font tables, so a caller can lay a
 * screen out before it knows whether there is one. */
int32_t nd_bootfb_text_w(const char *s, nd_bootfb_size size);

/* _fit_font(): the first of 20, 18, 14 whose ink width for s is <= max_w,
 * and 14 when none of them is. The Update app's label picks its size this
 * way, so the same string is the same size on both screens. */
nd_bootfb_size nd_bootfb_fit(const char *s, int32_t max_w);

/* _ellipsize(): s unchanged if it fits, else the longest prefix for which
 * prefix + "..." fits, else -- and this asymmetry is deliberate in the
 * original -- the ORIGINAL over-wide string rather than "". out is always
 * NUL-terminated. */
void nd_bootfb_ellipsize(char *out, size_t out_sz, const char *s, nd_bootfb_size size,
                         int32_t max_w);

/* y is the ASCENDER line, Pillow's "la" anchor, exactly as nd_draw_text()
 * takes it: the ink starts at y + ink_dy, a couple of pixels lower. */
void nd_bootfb_text(nd_bootfb *fb, int32_t x, int32_t y, const char *s, nd_bootfb_size size);

/* Push the shadow to the panel. Silently does nothing if there is no panel or
 * the write fails -- see IT CAN NEVER FAIL ITS CALLER. */
void nd_bootfb_present(nd_bootfb *fb);

#ifdef __cplusplus
}
#endif

#endif /* ND_BOOTFB_H_INCLUDED */

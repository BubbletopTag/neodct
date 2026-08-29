/* nd_fb.h -- /dev/fb0: two ioctls, an mmap, and two pixel packers.
 *
 * The panel is 240x240; the UI is a 240x175 band letterboxed inside it. On
 * both QEMU and the real hardware the framebuffer itself is reconfigured to
 * 240x175 @ 32bpp before the UI starts (neodct_displayd does it with
 * FBIOPUT_VSCREENINFO), so the live case is one contiguous 168,000-byte write
 * per frame with no centring at all. The centring arithmetic still has to be
 * ported, because a genuine 240x240 framebuffer puts the band at row 32 and
 * that is what test_uistub.py pins.
 *
 * ============ THE line_length FALLBACK IS LOAD-BEARING ============
 *
 * The Python reads line_length at byte offset 48 of fb_fix_screeninfo, which
 * is its offset on 64-bit only; on the 32-bit ARM target that offset is
 * mmio_start. It works solely because the Rockchip driver reports
 * mmio_start == 0 and the "== 0 means compute it" fallback then produces the
 * right answer.
 *
 * So: read the REAL struct from <linux/fb.h>, AND keep
 *     if (line_length == 0) line_length = xres * bpp / 8;
 * because that is the value the phone has always actually used. A driver that
 * pads its rows would change the rendering, and we would want to find that out
 * deliberately rather than by accident.
 *
 * ============ THE MAPPING IS ZEROED ONCE ============
 *
 * At open. That is what makes later partial-band writes safe: the letterbox
 * rows above and below the band are black from boot and are never written
 * again.
 *
 * ============ THE CHANNEL ORDER COMES FROM THE DRIVER ============
 *
 * A 32bpp framebuffer says a pixel is four bytes wide and NOTHING about where
 * red sits inside them, and the two framebuffers NeoDCT runs on disagree:
 *
 *   QEMU     virtio-gpu through DRM's fbdev emulation: red.offset 16, so a
 *            pixel is B G R x.
 *   hardware the kernel's vfb, which neodct_displayd mirrors to the ST7789
 *            over SPI: red.offset 0, so a pixel is R G B x. (drivers/video/
 *            fbdev/vfb.c, the 32bpp case of vfb_check_var.)
 *
 * The Python packed B G R A unconditionally and the daemon unpacked B G R A
 * unconditionally, so the UI came out right on both -- the two halves of one
 * private convention agreeing with each other. Everything else on the phone
 * believes the driver instead: mpv's fbdev output reads red.offset, netsurf's
 * libnsfb reads it, and the framebuffer console behind the recovery menu is
 * the driver. On the hardware all three therefore drew R G B x into a buffer
 * the daemon read as B G R x, and video and web pages came out with red and
 * blue swapped while the UI around them looked fine.
 *
 * So the order is read from fb_var_screeninfo, here and in neodct_displayd,
 * and the private convention is gone. nd_fb_open_mem() and the capture sink
 * have no driver to ask and keep the B G R A the reference frames were cut
 * with.
 */

#ifndef ND_FB_H_INCLUDED
#define ND_FB_H_INCLUDED

#include "nd_image.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which packer nd_fb_update() will use, decided once at open and logged. The
 * log line is verbatim:
 *   "[FB] {xres}x{yres} @ {bpp}bpp, pixel path: {path}"
 * with path one of the strings below.
 *
 * The bit depth does not decide the channel order -- see THE CHANNEL ORDER
 * COMES FROM THE DRIVER above. The first three are what the Python had; the
 * last two are the blue-first halves of the same two depths, reached only
 * when a driver says that is what it wants. */
typedef enum {
    ND_FB_PATH_BGRA32 = 0, /* "BGRA 32bpp (C, fast)"    bytes B G R A       */
    ND_FB_PATH_RGB565,     /* "BGR;16 16bpp (C, fast)"  red in the top bits */
    ND_FB_PATH_RGB888,     /* anything else: raw RGB bytes                  */
    ND_FB_PATH_RGBA32,     /* "RGBA 32bpp (C, fast)"    bytes R G B A       */
    ND_FB_PATH_BGR565      /* "BGR565 16bpp (C, fast)"  blue in the top bits*/
} nd_fb_path;

typedef struct nd_fb nd_fb;

/* Open, query, map, zero. Returns ND_ERR_IO with a logged reason on failure.
 * *out is owned by the caller; release with nd_fb_close(). */
nd_err nd_fb_open(nd_fb **out, const char *path);
void nd_fb_close(nd_fb *fb);

/* Geometry, as the driver reported it. */
int32_t nd_fb_xres(const nd_fb *fb);
int32_t nd_fb_yres(const nd_fb *fb);
int32_t nd_fb_bpp(const nd_fb *fb);
size_t nd_fb_line_length(const nd_fb *fb);
nd_fb_path nd_fb_pixel_path(const nd_fb *fb);

/* Present one frame. The source is centred both ways when it is smaller than
 * the framebuffer and centre-cropped when it is larger:
 *
 *   copy_w = min(src->w, xres);   src_x = max(0, (src->w - copy_w) / 2)
 *   copy_h = min(src->h, yres);   src_y = max(0, (src->h - copy_h) / 2)
 *   dst_x  = max(0, (xres - copy_w) / 2)
 *   dst_y  = max(0, (yres - copy_h) / 2)
 *
 * The write is a single contiguous memcpy when dst_x == 0 and the row length
 * equals line_length -- which is the live case -- and row by row otherwise.
 *
 * NO ALLOCATION. The 32bpp path packs straight from the RGB canvas into the
 * mmap, which is 168 KB the Python spent on a staging buffer and we do not. */
nd_err nd_fb_update(nd_fb *fb, const nd_image *src);

/* The packers, exposed for the unit tests and for nd-shoot.
 *
 * BGRA: memory order B, G, R, A with alpha always 255. Red is 00 00 ff ff --
 * that is XRGB8888 read as a little-endian uint32.
 *
 * RGB565: ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3), written LOW BYTE
 * FIRST. Red 00 f8, green e0 07, blue 1f 00, white ff ff, (18,52,86) aa 11.
 * Pillow's "BGR;16" packer produces identical bytes, so one packer covers
 * both paths.
 *
 * Both write w * h * (4 or 2) bytes and return ND_ERR_TOOLONG if out_sz is
 * short. src must be RGB888 or RGBA8888. */
nd_err nd_fb_pack_bgra(const nd_image *src, uint8_t *out, size_t out_sz);
nd_err nd_fb_pack_rgb565(const nd_image *src, uint8_t *out, size_t out_sz);

/* ------------------------------------------------------------------ *
 * Backlight -- the panel's brightness, not the framebuffer's contents
 * ------------------------------------------------------------------ */

/* The pin the panel's BL wire is on: header pin 11, GPIO1_C5, gpio53.
 * docs/HARDWARE_NOTES.md is the record of how it got there -- it used to be
 * strapped to 3V3, and moving it to a GPIO is what made "screen off" a thing
 * the software can do at all. Dimming needs pwm9 in the device tree, which
 * lives in the boot partition and therefore arrives on a reflash rather than
 * on an update, so the GPIO tier below is not a fallback that will go away.
 *
 * Default the pin ON before software runs (a pull-up on the enable): "no
 * software yet" and "software broken" should both show a lit screen, because
 * the initramfs boot logo and the recovery sad-face are exactly the screens
 * you need when the rootfs is the thing that is wrong. */
#define ND_BL_GPIO_PIN 53

#define ND_BL_GPIO_ROOT      "/sys/class/gpio"
#define ND_BL_BACKLIGHT_ROOT "/sys/class/backlight"

/* Getting this backwards darkens the screen exactly when somebody starts
 * using the phone, which is the hardest possible failure to interpret from
 * the far side of a serial cable. It is a constant, not a probe, because
 * there is nothing to probe: the polarity is a property of the wiring. */
#define ND_BL_ACTIVE_LOW false

/* "On but very dim" must not read as a broken screen, so a nonzero request
 * below this is raised to it rather than honoured. Zero still means off. */
#define ND_BL_MIN_ON_PERCENT 5

typedef enum { ND_BL_PWM = 0, ND_BL_GPIO, ND_BL_NONE } nd_bl_mode;

/* Probing the GPIO tier EXPORTS the pin -- there is no way to ask whether a
 * pin can be driven other than by claiming it. Harmless and idempotent, and
 * the Python behaved the same way, but it means mode() is not a pure read. */
nd_bl_mode nd_backlight_mode(void);
bool nd_backlight_available(void);
bool nd_backlight_set_percent(int32_t percent);
int32_t nd_backlight_get_percent(void); /* -1 when unreadable */
bool nd_backlight_off(void);
bool nd_backlight_on(int32_t percent);

#ifdef __cplusplus
}
#endif

#endif /* ND_FB_H_INCLUDED */

/*
 * neodctDisplay.c v2.2 — userspace ST7789 display daemon for NeoDCT
 * Luckfox Pico Mini B (RV1103), 240x240 Waveshare ST7789 over SPI0
 *
 * v2 changes (single-core RV1103 optimization):
 *   - FRAME SKIP: fb compared against previous frame; identical -> no
 *     convert, no SPI. Idle UI costs ~0 CPU instead of a pegged core.
 *   - DIRTY RECT: only the changed bounding rectangle is converted and
 *     sent (ST7789 CASET/RASET partial window). A clock tick sends a
 *     few KB, not 115 KB.
 *   - Runtime flags: --speed, --fps, --full, --swap-rb, --stats
 *     (no recompile to try 40 MHz).
 *   - DC gpio fd cached (no sysfs open/close per command).
 *   - Chunk size read from spidev bufsiz (add spidev.bufsiz=65536 to
 *     kernel cmdline to cut ioctl count 16x; falls back to 4096).
 *   - Periodic stats: sent/skipped frames, avg rect, convert/spi ms.
 *
 * Since: the red/blue channel order is READ FROM fb_var_screeninfo rather
 * than assumed, so programs that are not NeoDCT reach the panel in their
 * own colours. See the comment above convert_rect().
 *
 * Usage:
 *   neodct_displayd --test              hardware self-check (R/G/B/W/K fills)
 *   neodct_displayd                     mirror /dev/fb0 (diff-based) forever
 *   neodct_displayd --once              render one full frame and exit
 *   neodct_displayd --speed 40000000    SPI clock in Hz (values < 1000 mean MHz)
 *   neodct_displayd --fps 30            poll rate cap
 *   neodct_displayd --full              disable diffing (v1 behavior)
 *   neodct_displayd --swap-rb           invert the detected R/B order
 *   neodct_displayd --stats             print stats every 5 s
 *   neodct_displayd --panel spidev      the panel (default; see below)
 *   neodct_displayd --panel null        compose and discard
 *   neodct_displayd --panel stream:P    compose and write the transcript to P
 *   neodct_displayd --fb-at WxH@B:S F   read F as the framebuffer (host only)
 *
 * Wiring (matches proven-good harness):
 *   CS  = pin 6  (SPI0_CS0)     CLK = pin 7 (SPI0_CLK)   SDA/MOSI = pin 8 (SPI0_MOSI)
 *   RST = pin 12 (GPIO 56)      DC  = pin 13 (GPIO 57)   BL = 3.3V
 *
 * ============ WHAT IS IN THIS FILE AND WHAT IS BEHIND nd_panel.h ==========
 *
 * The transport -- sysfs GPIO, spidev, the chunked transfer, the reset pulse
 * train -- moved to displayd/nd_panel_spidev.c behind the five-entry vtable
 * in displayd/nd_panel.h. Everything that composes stayed here and is SHARED
 * by every backend: force_mode(), init_framebuffer(), convert_rect(),
 * render_dirty(), render_full(), set_window(), fill_color(), panel_init(),
 * now_ms() and the pacer. If any of that had moved, a second machine running
 * this daemon would be testing a second implementation rather than the
 * phone's -- which is the entire reason to run it on a second machine.
 *
 * THE BACKEND IS CHOSEN, NEVER DETECTED, and the default is spidev. There is
 * deliberately no "try spidev, fall back to a stream": a phone whose spidev
 * has not enumerated by the time S90display runs must fail loudly exactly as
 * it does now, and must never quietly start writing a panel stream to nowhere
 * and report success. S90display's own header is the written record of what
 * answering two questions with one test cost last time, and
 * neodct/initramfs/ndsys-panel.sh starts this binary with NO arguments and
 * DEPENDS on it exiting under QEMU.
 *
 * THERE IS STILL NO 240x240 COMPOSE BUFFER IN THIS PROGRAM AND THERE MUST NOT
 * BECOME ONE. The daemon blanks the panel once with fill_color(0,0,0) and
 * thereafter writes only rows [yoff, yoff+copy_h-1]; the composed 240x240
 * frame lives in the panel's GRAM. A shared compose buffer would add 115,200
 * bytes and a second full copy per frame to a single-core 64 MB phone whose
 * dirty-rect optimisation exists precisely because a full frame is expensive.
 */

/* usleep() is XSI, and <unistd.h> hides it under -std=c11 unless a feature
 * macro asks for it. Without this the compiler invents `int usleep()` and
 * calls it with an unprototyped signature -- undefined behaviour that
 * happens to work on glibc/ARM and is not guaranteed to anywhere else.
 * Caught when this file moved into the -Werror build. */
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <time.h>
#include <signal.h>
#include <errno.h>

#include "nd_panel.h"

/* ---------- configuration ---------- */

#define PANEL_W    240
#define PANEL_H    240
#define OFFSET_X   0
#define OFFSET_Y   0           /* if image is shifted, try 80 (240x240 quirk) */

/* The fb is forced to the NeoDCT UI band size; the daemon places the
 * band on the physical panel at --yoff (default: bottom-aligned, top
 * rows stay black -- matching the Nokia faceplate window). */
#define FB_W       240
#define FB_H       175
#define DEFAULT_Y_OFFSET (PANEL_H - FB_H)   /* 65 */

#define FB_DEVICE  "/dev/fb0"

#define DEFAULT_SPI_SPEED 20000000  /* proven; try --speed 40000000 */
#define DEFAULT_FPS       30
#define STATS_INTERVAL_MS 5000.0

/* ---------- runtime options ---------- */

static int opt_speed   = DEFAULT_SPI_SPEED;
static int opt_fps     = DEFAULT_FPS;
static int opt_full    = 0;   /* disable diffing */
static int opt_swap_rb = 0;
static int opt_stats   = 0;
static int opt_yoff    = DEFAULT_Y_OFFSET;  /* panel row where fb band starts */

/* Which transport, and where. Both are argument-only and neither has a
 * detection path -- see the header. */
static const char *opt_panel = "spidev";

/* --fb-at: an ordinary file standing in for /dev/fb0, with its geometry
 * supplied rather than asked for.
 *
 * This is nd_bootfb_open_at()'s argument verbatim and it carries the same
 * restriction: a regular file has no FBIOGET_VSCREENINFO to answer, and
 * inventing a default inside the device path would be exactly the assumption
 * this daemon must not make. NEVER USED ON A DEVICE. force_mode() is skipped
 * on this path because there is no driver to force -- force_mode() is covered
 * under QEMU instead, on vfb, which is the whole of the parity claim. */
static const char *opt_fb_path = NULL;
static unsigned opt_fb_w = 0, opt_fb_h = 0, opt_fb_bpp = 0, opt_fb_stride = 0;

/* ---------- globals ---------- */

static struct nd_panel *panel = NULL;
static int fb_fd  = -1;
static unsigned char *fb_data = NULL;
static size_t fb_size = 0;
static volatile int quit_flag = 0;

static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static unsigned int fb_bytespp = 2;     /* granted fb bytes per pixel (2 or 4) */
static int fb_swap_rb = 0;              /* red/blue order, decided at init */

static unsigned char *out_buf  = NULL;  /* converted RGB565 big-endian rect */
static size_t out_buf_size = 0;
static unsigned char *prev_fb  = NULL;  /* last frame we sent, fb layout    */

/* stats */
static long st_sent = 0, st_skipped = 0;
static double st_conv_ms = 0.0, st_spi_ms = 0.0;
static long long st_bytes = 0;
static long st_rect_px = 0;

/* ---------- time ---------- */

/* CLOCK_MONOTONIC, not gettimeofday().
 *
 * This is the frame pacer's clock, and the pacer sleeps for the difference
 * between a deadline and now. On the wall clock a BACKWARD step makes that
 * difference the size of the step: the phone has no RTC, comes up believing
 * it is 1970 and has its clock set from NTP a minute into every boot -- and
 * the owner can set it by hand in the Clock app at any time. Either one
 * parked this loop, and this loop is the only thing that puts pixels on the
 * ST7789: the panel simply stops updating, with the UI behind it running
 * perfectly. Under QEMU there is no SPI panel and this program does not run.
 *
 * Nothing else here needs the wall clock, so it is monotonic throughout: the
 * stats interval wants elapsed time too. */
static double now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/* ---------- the panel transport ---------- */

/* Both of these are one line through displayd/nd_panel.h's vtable now. What
 * used to be here -- the sysfs GPIO helpers, detect_spi_chunk(), init_spi(),
 * spi_send() and the DC toggle -- is in nd_panel_spidev.c, unchanged. Six
 * indirect calls per frame; the dispatch is not measurable beside the SPI. */
static void write_command(unsigned char cmd)
{
    panel->cmd(panel, cmd);
}

static void write_data(const unsigned char *data, size_t len)
{
    panel->data(panel, data, len);
}

/* ---------- panel ---------- */

static void panel_init(void)
{
    /* Sequence proven on this exact panel+board by fb_diag --test. */
    unsigned char p;

    write_command(0x01);                    /* SWRESET  */ usleep(150000);
    write_command(0x11);                    /* SLPOUT   */ usleep(150000);
    p = 0x55; write_command(0x3A); write_data(&p, 1);  /* COLMOD: RGB565 */ usleep(10000);
    p = 0x00; write_command(0x36); write_data(&p, 1);  /* MADCTL         */ usleep(10000);
    write_command(0x21);                    /* INVON: required on this IPS panel */ usleep(10000);
    write_command(0x13);                    /* NORON    */ usleep(10000);
    write_command(0x29);                    /* DISPON   */ usleep(150000);
    printf("Panel initialized\n");
}

static void set_window(unsigned short x0, unsigned short y0,
                       unsigned short x1, unsigned short y1)
{
    unsigned char caset[4], raset[4];
    x0 += OFFSET_X; x1 += OFFSET_X;
    y0 += OFFSET_Y; y1 += OFFSET_Y;
    caset[0] = x0 >> 8; caset[1] = x0 & 0xFF;
    caset[2] = x1 >> 8; caset[3] = x1 & 0xFF;
    raset[0] = y0 >> 8; raset[1] = y0 & 0xFF;
    raset[2] = y1 >> 8; raset[3] = y1 & 0xFF;
    write_command(0x2A); write_data(caset, 4);
    write_command(0x2B); write_data(raset, 4);
    write_command(0x2C);                    /* RAMWR */
}

static void fill_color(unsigned char r, unsigned char g, unsigned char b)
{
    unsigned short c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);

    set_window(0, 0, PANEL_W - 1, PANEL_H - 1);
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        out_buf[i * 2]     = c >> 8;
        out_buf[i * 2 + 1] = c & 0xFF;
    }
    write_data(out_buf, (size_t)PANEL_W * PANEL_H * 2);
}

/* ---------- framebuffer ---------- */

static void force_mode(void)
{
    /* Prefer 32bpp: Python then packs via Pillow's C-speed "BGRA"
     * rawmode (RGB565 "BGR;16" was removed in Pillow 11) and this
     * daemon does the 565 packing during its existing copy loop.
     * Fall back to 16bpp if the fb driver refuses 32. */
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) return;
    vinfo.xres = FB_W;  vinfo.yres = FB_H;
    vinfo.xres_virtual = FB_W;  vinfo.yres_virtual = FB_H;
    vinfo.bits_per_pixel = 32;
    if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &vinfo) < 0 ||
        (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0 &&
         vinfo.bits_per_pixel != 32)) {
        vinfo.xres = FB_W;  vinfo.yres = FB_H;
        vinfo.xres_virtual = FB_W;  vinfo.yres_virtual = FB_H;
        vinfo.bits_per_pixel = 16;
        if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &vinfo) < 0)
            fprintf(stderr, "force_mode: FBIOPUT failed: %s\n", strerror(errno));
    }
}

/* --fb-at's half of init_framebuffer(): open an ordinary file and FILL IN the
 * geometry the device path would have asked the driver for.
 *
 * The channel offsets are vfb's, because vfb is the driver on both machines
 * and the whole point of this path is to exercise the same convert_rect()
 * branch the phone takes. vfb_check_var() grants R G B x at 32 bpp (red.offset
 * 0) and 5-6-5 at 16 bpp (red.offset 11), measured on this kernel; the
 * red<blue test below then decides fb_swap_rb from those, exactly as it does
 * off a real ioctl. */
static int init_framebuffer_at(void)
{
    struct stat st;

    fb_fd = open(opt_fb_path, O_RDONLY);
    if (fb_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", opt_fb_path, strerror(errno));
        return -1;
    }

    vinfo.xres = opt_fb_w;  vinfo.yres = opt_fb_h;
    vinfo.xres_virtual = opt_fb_w;  vinfo.yres_virtual = opt_fb_h;
    vinfo.bits_per_pixel = opt_fb_bpp;
    finfo.line_length = opt_fb_stride;
    if (opt_fb_bpp == 32) {
        vinfo.red.offset   = 0;  vinfo.red.length    = 8;
        vinfo.green.offset = 8;  vinfo.green.length  = 8;
        vinfo.blue.offset  = 16; vinfo.blue.length   = 8;
        vinfo.transp.offset = 24; vinfo.transp.length = 8;
    } else if (opt_fb_bpp == 16) {
        vinfo.red.offset   = 11; vinfo.red.length   = 5;
        vinfo.green.offset = 5;  vinfo.green.length = 6;
        vinfo.blue.offset  = 0;  vinfo.blue.length  = 5;
    }

    /* ============ THE GEOMETRY IS TYPED BY HAND, SO CHECK ALL OF IT ========
     *
     * Two refusals, and the second was missing.
     *
     * A stride SMALLER than the row it describes is not caught by the length
     * test below -- a 240x175@32 fixture with stride 480 instead of 960 is
     * exactly 84,000 bytes, which is exactly stride * h, so the file "fits".
     * Everything downstream then indexes rows at y * line_length and reads
     * copy_w * fb_bytespp bytes inside them, walking off the end of both the
     * mapping and prev_fb (malloc'd line_length * yres). Reproduced under
     * ASAN before this check existed: a heap-buffer-overflow WRITE of 960
     * bytes 0 bytes past an 84,000-byte region, in render_full()'s memcpy.
     * On a real fb0 the driver supplies both numbers and they agree by
     * construction; --fb-at is the one path where a human types them, which
     * is precisely why it is the one path that has to check.
     *
     * And mmap() past the end of a short file is a SIGBUS the first time a
     * row is read, which on a test host looks like a daemon bug rather than a
     * mis-sized fixture. Refuse instead.
     *
     * fb_fd is closed on both refusals. Nothing leaks today -- main() exits
     * straight after -- but this function's caller does not close it and the
     * next call site would not know that. */
    if (opt_fb_stride < opt_fb_w * (opt_fb_bpp / 8u)) {
        fprintf(stderr, "--fb-at stride %u is less than the %u bytes a row of "
                        "%u pixels at %u bpp needs\n",
                opt_fb_stride, opt_fb_w * (opt_fb_bpp / 8u),
                opt_fb_w, opt_fb_bpp);
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }
    if (fstat(fb_fd, &st) < 0 ||
        (unsigned long long)st.st_size <
            (unsigned long long)opt_fb_stride * opt_fb_h) {
        fprintf(stderr, "%s is %lld bytes, need %llu for %ux%u stride %u\n",
                opt_fb_path, (long long)st.st_size,
                (unsigned long long)opt_fb_stride * opt_fb_h,
                opt_fb_w, opt_fb_h, opt_fb_stride);
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }
    printf("fb at %s: geometry supplied, force_mode() skipped (no driver)\n",
           opt_fb_path);
    return 0;
}

static int init_framebuffer(void)
{
    if (opt_fb_path != NULL) {
        if (init_framebuffer_at() < 0)
            return -1;
    } else {
        fb_fd = open(FB_DEVICE, O_RDWR);
        if (fb_fd < 0) {
            fprintf(stderr, "open %s failed: %s\n", FB_DEVICE, strerror(errno));
            return -1;
        }

        force_mode();

        if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
            ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
            fprintf(stderr, "framebuffer ioctl failed: %s\n", strerror(errno));
            return -1;
        }
    }
    printf("fb0: %ux%u, %u bpp, line_length %u\n",
           vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, finfo.line_length);

    if (vinfo.bits_per_pixel != 16 && vinfo.bits_per_pixel != 32) {
        fprintf(stderr, "expected 16 or 32 bpp framebuffer (got %u)\n",
                vinfo.bits_per_pixel);
        return -1;
    }
    fb_bytespp = vinfo.bits_per_pixel / 8;

    /* Which end of the pixel red is on is the driver's to say, and this
     * daemon used to assume. vfb says red.offset 0 (bytes R G B x); DRM's
     * fbdev emulation says 16 (bytes B G R x). --swap-rb now inverts
     * whatever was detected, which is what it was always for: a driver
     * that fills the struct in wrongly. */
    fb_swap_rb = (vinfo.red.offset < vinfo.blue.offset) ? 1 : 0;
    if (opt_swap_rb)
        fb_swap_rb = !fb_swap_rb;

    printf("fb channels: red@%u green@%u blue@%u -> %s%s\n",
           vinfo.red.offset, vinfo.green.offset, vinfo.blue.offset,
           fb_swap_rb ? "red first" : "blue first",
           opt_swap_rb ? " (--swap-rb inverted it)" : "");
    printf("pixel path: %s\n", fb_bytespp == 4
           ? "fb 32bpp -> daemon packs RGB565"
           : "fb 16bpp -> daemon repacks 565 big-endian");

    fb_size = (size_t)finfo.line_length * vinfo.yres;
    fb_data = mmap(NULL, fb_size, PROT_READ, MAP_SHARED, fb_fd, 0);
    if (fb_data == MAP_FAILED) {
        fprintf(stderr, "mmap framebuffer failed: %s\n", strerror(errno));
        return -1;
    }

    prev_fb = malloc(fb_size);
    if (!prev_fb) {
        fprintf(stderr, "out of memory (prev_fb)\n");
        return -1;
    }
    /* Force first frame to be a full send: make prev differ everywhere. */
    memset(prev_fb, 0xA5, fb_size);
    return 0;
}

/* Convert fb rect to panel big-endian RGB565 into out_buf.
 * 16bpp fb: byte-swap the native little-endian 565.
 * 32bpp fb: pack the 8888 pixel down to 565 -- this is the work Python
 * used to do in a 346 ms/frame interpreter loop.
 *
 * ============ WHICH END RED IS ON IS NOT OURS TO DECIDE ============
 *
 * This read bytes B G R x unconditionally, and the UI packed bytes B G R A
 * unconditionally, so the two halves of NeoDCT agreed with each other and
 * the panel looked right. They agreed on something the driver disagreed
 * with: the phone's fb0 is the kernel's vfb, which declares red.offset 0 --
 * bytes R G B x (drivers/video/fbdev/vfb.c, the 32bpp case of
 * vfb_check_var). Everything on the phone that is not NeoDCT believes that
 * declaration -- mpv's fbdev output, netsurf through libnsfb, the
 * framebuffer console behind the recovery menu -- so all of them drew
 * R G B x into a buffer this function read as B G R x, and video and web
 * pages reached the panel with red and blue swapped.
 *
 * fb_swap_rb now comes from fb_var_screeninfo, so the daemon and the UI
 * both follow the driver instead of each other, and a program that has
 * never heard of NeoDCT gets its colours through intact. */
static size_t convert_rect(unsigned int x0, unsigned int y0,
                           unsigned int x1, unsigned int y1)
{
    size_t n = 0;
    for (unsigned int y = y0; y <= y1; y++) {
        const unsigned char *src = fb_data + (size_t)y * finfo.line_length;
        if (fb_bytespp == 4) {
            for (unsigned int x = x0; x <= x1; x++) {
                const unsigned char *p = src + (size_t)x * 4;
                unsigned char b = p[0], g = p[1], r = p[2];
                if (fb_swap_rb) { unsigned char t = r; r = b; b = t; }
                unsigned short px = (unsigned short)(((r & 0xF8) << 8) |
                                                     ((g & 0xFC) << 3) |
                                                     (b >> 3));
                out_buf[n++] = px >> 8;     /* panel wants big-endian */
                out_buf[n++] = px & 0xFF;
            }
        } else {
            for (unsigned int x = x0; x <= x1; x++) {
                unsigned short px = src[x * 2] | (src[x * 2 + 1] << 8);
                if (fb_swap_rb)
                    px = (unsigned short)(((px & 0xF800) >> 11) | (px & 0x07E0) | ((px & 0x001F) << 11));
                out_buf[n++] = px >> 8;
                out_buf[n++] = px & 0xFF;
            }
        }
    }
    return n;
}

/* Diff fb against prev_fb, send only the dirty bounding rect.
 * Returns 1 if something was sent, 0 if skipped. */
static int render_dirty(void)
{
    unsigned int max_h = PANEL_H - (unsigned int)opt_yoff;
    unsigned int copy_w = vinfo.xres < PANEL_W ? vinfo.xres : PANEL_W;
    unsigned int copy_h = vinfo.yres < max_h ? vinfo.yres : max_h;
    size_t row_bytes = (size_t)copy_w * fb_bytespp;

    /* pass 1: dirty row range */
    unsigned int ymin = copy_h, ymax = 0;
    for (unsigned int y = 0; y < copy_h; y++) {
        size_t off = (size_t)y * finfo.line_length;
        if (memcmp(fb_data + off, prev_fb + off, row_bytes) != 0) {
            if (y < ymin) ymin = y;
            ymax = y;
        }
    }
    if (ymin > ymax)
        return 0;                            /* identical frame: skip */

    /* pass 2: dirty column range across dirty rows */
    size_t bmin = row_bytes, bmax = 0;
    for (unsigned int y = ymin; y <= ymax; y++) {
        const unsigned char *a = fb_data + (size_t)y * finfo.line_length;
        const unsigned char *b = prev_fb + (size_t)y * finfo.line_length;
        if (memcmp(a, b, row_bytes) == 0)
            continue;                        /* clean row inside band */
        size_t i = 0, j = row_bytes - 1;
        while (i < row_bytes && a[i] == b[i]) i++;
        while (j > i && a[j] == b[j]) j--;
        if (i < bmin) bmin = i;
        if (j > bmax) bmax = j;
    }
    unsigned int xmin = (unsigned int)(bmin / fb_bytespp);
    unsigned int xmax = (unsigned int)(bmax / fb_bytespp);

    /* remember what we're sending */
    for (unsigned int y = ymin; y <= ymax; y++) {
        size_t off = (size_t)y * finfo.line_length;
        memcpy(prev_fb + off, fb_data + off, row_bytes);
    }

    double t0 = now_ms();
    size_t n = convert_rect(xmin, ymin, xmax, ymax);
    double t1 = now_ms();

    set_window((unsigned short)xmin, (unsigned short)(ymin + opt_yoff),
               (unsigned short)xmax, (unsigned short)(ymax + opt_yoff));
    write_data(out_buf, n);
    double t2 = now_ms();

    st_conv_ms += t1 - t0;
    st_spi_ms  += t2 - t1;
    st_bytes   += (long long)n;
    st_rect_px += (long)(xmax - xmin + 1) * (long)(ymax - ymin + 1);
    return 1;
}

/* v1 behavior: full frame, unconditional (also used for --once/--full). */
static void render_full(void)
{
    unsigned int max_h = PANEL_H - (unsigned int)opt_yoff;
    unsigned int copy_w = vinfo.xres < PANEL_W ? vinfo.xres : PANEL_W;
    unsigned int copy_h = vinfo.yres < max_h ? vinfo.yres : max_h;

    double t0 = now_ms();
    size_t n = convert_rect(0, 0, copy_w - 1, copy_h - 1);
    double t1 = now_ms();

    set_window(0, (unsigned short)opt_yoff,
               (unsigned short)(copy_w - 1),
               (unsigned short)(opt_yoff + copy_h - 1));
    write_data(out_buf, n);
    double t2 = now_ms();

    for (unsigned int y = 0; y < copy_h; y++) {
        size_t off = (size_t)y * finfo.line_length;
        memcpy(prev_fb + off, fb_data + off, (size_t)copy_w * fb_bytespp);
    }

    st_conv_ms += t1 - t0;
    st_spi_ms  += t2 - t1;
    st_bytes   += (long long)n;
    st_rect_px += (long)copy_w * (long)copy_h;
}

static void print_stats(double window_ms)
{
    long total = st_sent + st_skipped;
    double avg_px = st_sent ? (double)st_rect_px / st_sent : 0.0;
    printf("[stats] %.1fs: sent %ld / skipped %ld of %ld polls | "
           "%.0f KB | avg rect %.0f px | conv %.2f ms/f | spi %.2f ms/f\n",
           window_ms / 1000.0, st_sent, st_skipped, total,
           st_bytes / 1024.0, avg_px,
           st_sent ? st_conv_ms / st_sent : 0.0,
           st_sent ? st_spi_ms / st_sent : 0.0);
    st_sent = st_skipped = 0;
    st_conv_ms = st_spi_ms = 0.0;
    st_bytes = 0;
    st_rect_px = 0;
}

/* ---------- main ---------- */

static void signal_handler(int sig)
{
    (void)sig;
    quit_flag = 1;
}

static int parse_int(const char *s, int fallback)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return fallback;
    return (int)v;
}

int main(int argc, char *argv[])
{
    int test_mode = 0, once_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--test") || !strcmp(argv[i], "-t")) test_mode = 1;
        else if (!strcmp(argv[i], "--once")) once_mode = 1;
        else if (!strcmp(argv[i], "--full")) opt_full = 1;
        else if (!strcmp(argv[i], "--swap-rb")) opt_swap_rb = 1;
        else if (!strcmp(argv[i], "--stats")) opt_stats = 1;
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc) {
            opt_speed = parse_int(argv[++i], DEFAULT_SPI_SPEED);
            if (opt_speed > 0 && opt_speed < 1000)
                opt_speed *= 1000000;        /* "--speed 40" means 40 MHz */
        }
        else if (!strcmp(argv[i], "--yoff") && i + 1 < argc) {
            opt_yoff = parse_int(argv[++i], DEFAULT_Y_OFFSET);
            if (opt_yoff < 0) opt_yoff = 0;
            if (opt_yoff > PANEL_H - 1) opt_yoff = PANEL_H - 1;
        }
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
            opt_fps = parse_int(argv[++i], DEFAULT_FPS);
            if (opt_fps < 1) opt_fps = 1;
        }
        else if (!strcmp(argv[i], "--panel") && i + 1 < argc) {
            opt_panel = argv[++i];
        }
        else if (!strcmp(argv[i], "--fb-at") && i + 2 < argc) {
            const char *spec = argv[++i];
            if (sscanf(spec, "%ux%u@%u:%u", &opt_fb_w, &opt_fb_h,
                       &opt_fb_bpp, &opt_fb_stride) != 4 ||
                opt_fb_w == 0 || opt_fb_h == 0 || opt_fb_stride == 0) {
                fprintf(stderr, "--fb-at: expected WxH@BPP:STRIDE (got '%s')\n", spec);
                return 2;
            }
            opt_fb_path = argv[++i];
        }
        else {
            fprintf(stderr, "unknown arg: %s\n", argv[i]);
            return 2;
        }
    }

    /* After the loop, because --speed is the spidev backend's parameter and
     * may be given in either order. */
    if (!strcmp(opt_panel, "spidev")) {
        panel = nd_panel_spidev(opt_speed);
    } else if (!strcmp(opt_panel, "null")) {
        panel = nd_panel_stream(NULL, PANEL_W, PANEL_H);
    } else if (!strncmp(opt_panel, "stream:", 7) && opt_panel[7] != '\0') {
        panel = nd_panel_stream(opt_panel + 7, PANEL_W, PANEL_H);
    } else {
        /* `stream:` with nothing after it lands here rather than quietly
         * becoming the null sink, which is the one mistake a chosen-never-
         * detected backend must not make. */
        fprintf(stderr, "--panel: expected spidev, null or stream:<path> "
                        "(got '%s')\n", opt_panel);
        return 2;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* The backend is in the banner because S90display now passes it
     * explicitly: the choice has to be visible in `ps` and in the log, or the
     * one thing that decides whether a phone is driving a panel is invisible
     * on the phone. */
    printf("neodct_displayd v2.2 (panel %dx%d via %s, fb band %dx%d at y=%d, "
           "%d Hz SPI, %d fps poll, %s)\n",
           PANEL_W, PANEL_H, panel->name, FB_W, FB_H, opt_yoff, opt_speed,
           opt_fps, opt_full ? "full-frame" : "dirty-rect");

    out_buf_size = (size_t)PANEL_W * PANEL_H * 2;
    out_buf = malloc(out_buf_size);
    if (!out_buf) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    if (panel->open(panel) < 0) return 1;

    panel->reset(panel);
    panel_init();

    /* Blank the whole panel once so regions outside the fb copy area
     * are defined black (v1 re-blanked them every frame). */
    fill_color(0, 0, 0);

    if (test_mode) {
        printf("Self-test: R/G/B/W/K fills\n");
        fill_color(255, 0, 0);     usleep(800000);
        fill_color(0, 255, 0);     usleep(800000);
        fill_color(0, 0, 255);     usleep(800000);
        fill_color(255, 255, 255); usleep(800000);
        fill_color(0, 0, 0);
        printf("Self-test done.\n");
        free(out_buf);
        panel->close(panel);
        return 0;
    }

    if (init_framebuffer() < 0) {
        free(out_buf);
        panel->close(panel);
        return 1;
    }

    /* Say when the panel is actually usable.
     *
     * Everything above this line is the setup a caller has to wait out
     * before it can put a pixel on the screen: reset_display(), panel_init(),
     * the blanking fill, and init_framebuffer() forcing fb0 to 32bpp. The
     * initramfs used to wait for it by sleeping two seconds and hoping --
     * two seconds of an eleven-second boot, spent on a guess, on hardware
     * where it is far too long and on QEMU where there is no panel at all.
     *
     * Opt-in, so nothing changes for anyone who does not set it: with
     * NEODCT_DISPLAYD_READY pointing at a path, that path exists from here
     * on, and the caller can poll for it instead of guessing. Failing to
     * write it is not worth refusing to run over -- the caller falls back to
     * its timeout, which is the old behaviour. */
    {
        const char *ready = getenv("NEODCT_DISPLAYD_READY");

        if (ready != NULL && ready[0] != '\0') {
            FILE *rf = fopen(ready, "w");

            if (rf != NULL) {
                (void)fputs("1\n", rf);
                (void)fclose(rf);
            }
        }
    }

    const double frame_ms = 1000.0 / opt_fps;
    long frames = 0;
    double stats_t0 = now_ms();

    do {
        double t0 = now_ms();

        if (opt_full || once_mode) {
            render_full();
            st_sent++;
        } else {
            if (render_dirty())
                st_sent++;
            else
                st_skipped++;
        }
        frames++;

        if (opt_stats) {
            double since = now_ms() - stats_t0;
            if (since >= STATS_INTERVAL_MS) {
                print_stats(since);
                stats_t0 = now_ms();
            }
        }

        double elapsed = now_ms() - t0;

        /* Clamped as well as monotonic. A monotonic clock cannot go
         * backwards, but a failed clock_gettime() returns 0.0 above, and a
         * sleep computed from a negative elapsed is a panel that stops --
         * which is too expensive an outcome to leave resting on one
         * assumption. The sleep can never exceed one frame. */
        if (elapsed < 0.0)
            elapsed = 0.0;
        if (elapsed < frame_ms)
            usleep((useconds_t)((frame_ms - elapsed) * 1000.0));
    } while (!quit_flag && !once_mode);

    printf("polled %ld frame(s), exiting\n", frames);

    if (prev_fb) free(prev_fb);
    munmap(fb_data, fb_size);
    close(fb_fd);
    panel->close(panel);
    free(out_buf);
    return 0;
}

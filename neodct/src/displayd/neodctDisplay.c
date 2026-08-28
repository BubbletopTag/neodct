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
 *
 * Wiring (matches proven-good harness):
 *   CS  = pin 6  (SPI0_CS0)     CLK = pin 7 (SPI0_CLK)   SDA/MOSI = pin 8 (SPI0_MOSI)
 *   RST = pin 12 (GPIO 56)      DC  = pin 13 (GPIO 57)   BL = 3.3V
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
#include <linux/spi/spidev.h>
#include <linux/fb.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <sys/time.h>

/* ---------- configuration ---------- */

#define DC_PIN     57          /* GPIO1_D1, physical pin 13 */
#define RESET_PIN  56          /* GPIO1_D0, physical pin 12 */

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

#define SPI_DEVICE "/dev/spidev0.0"
#define SPI_BITS   8
#define SPI_MODE   0           /* proven on this board+panel by fb_diag */

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

/* ---------- globals ---------- */

static int spi_fd = -1;
static int fb_fd  = -1;
static int dc_fd  = -1;                 /* cached sysfs value fd for DC */
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
static size_t spi_chunk = 4096;         /* from spidev bufsiz when readable */

/* stats */
static long st_sent = 0, st_skipped = 0;
static double st_conv_ms = 0.0, st_spi_ms = 0.0;
static long long st_bytes = 0;
static long st_rect_px = 0;

/* ---------- time ---------- */

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* ---------- gpio (sysfs) ---------- */

static int gpio_export(int pin)
{
    char path[64], buf[16];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", pin);
    if (access(path, F_OK) == 0)
        return 0;

    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "gpio%d: open export failed: %s\n", pin, strerror(errno));
        return -1;
    }
    snprintf(buf, sizeof(buf), "%d", pin);
    if (write(fd, buf, strlen(buf)) < 0) {
        fprintf(stderr, "gpio%d: export failed: %s\n", pin, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    usleep(10000);
    return 0;
}

static int gpio_direction_out(int pin)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "gpio%d: open direction failed: %s\n", pin, strerror(errno));
        return -1;
    }
    if (write(fd, "out", 3) < 0) {
        fprintf(stderr, "gpio%d: set direction failed: %s\n", pin, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int gpio_open_value(int pin)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        fprintf(stderr, "gpio%d: open value failed: %s\n", pin, strerror(errno));
    return fd;
}

static void gpio_fd_write(int fd, int value)
{
    if (fd >= 0 && write(fd, value ? "1" : "0", 1) < 0)
        fprintf(stderr, "gpio write failed: %s\n", strerror(errno));
}

static int gpio_write(int pin, int value)   /* slow path, used for RESET only */
{
    int fd = gpio_open_value(pin);
    if (fd < 0)
        return -1;
    gpio_fd_write(fd, value);
    close(fd);
    return 0;
}

static int setup_gpio(void)
{
    if (gpio_export(DC_PIN)    < 0) return -1;
    if (gpio_export(RESET_PIN) < 0) return -1;
    if (gpio_direction_out(DC_PIN)    < 0) return -1;
    if (gpio_direction_out(RESET_PIN) < 0) return -1;

    dc_fd = gpio_open_value(DC_PIN);
    if (dc_fd < 0) return -1;

    printf("GPIO ready (DC=%d cached fd, RESET=%d)\n", DC_PIN, RESET_PIN);
    return 0;
}

/* ---------- spi ---------- */

static void detect_spi_chunk(void)
{
    FILE *f = fopen("/sys/module/spidev/parameters/bufsiz", "r");
    if (f) {
        long v = 0;
        if (fscanf(f, "%ld", &v) == 1 && v >= 4096)
            spi_chunk = (size_t)v;
        fclose(f);
    }
    printf("SPI chunk size: %zu bytes%s\n", spi_chunk,
           spi_chunk <= 4096 ? " (boot with spidev.bufsiz=65536 for fewer ioctls)" : "");
}

static int init_spi(void)
{
    spi_fd = open(SPI_DEVICE, O_RDWR);
    if (spi_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", SPI_DEVICE, strerror(errno));
        return -1;
    }

    int mode = SPI_MODE, bits = SPI_BITS, speed = opt_speed;
    if (ioctl(spi_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        fprintf(stderr, "SPI param setup failed: %s\n", strerror(errno));
        close(spi_fd);
        spi_fd = -1;
        return -1;
    }
    detect_spi_chunk();
    printf("SPI up: %s @ %d Hz, mode %d\n", SPI_DEVICE, opt_speed, SPI_MODE);
    return 0;
}

static void spi_send(unsigned char *data, size_t len)
{
    for (size_t i = 0; i < len; i += spi_chunk) {
        size_t chunk = len - i;
        if (chunk > spi_chunk)
            chunk = spi_chunk;
        struct spi_ioc_transfer tr = {
            .tx_buf = (unsigned long)(data + i),
            .rx_buf = 0,
            .len = chunk,
            .delay_usecs = 0,
            .speed_hz = (unsigned)opt_speed,
            .bits_per_word = SPI_BITS,
        };
        if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr) < 0)
            fprintf(stderr, "SPI transfer failed: %s\n", strerror(errno));
    }
}

static void write_command(unsigned char cmd)
{
    gpio_fd_write(dc_fd, 0);
    spi_send(&cmd, 1);
}

static void write_data(unsigned char *data, size_t len)
{
    gpio_fd_write(dc_fd, 1);
    spi_send(data, len);
}

/* ---------- panel ---------- */

static void reset_display(void)
{
    printf("Resetting panel...\n");
    gpio_write(RESET_PIN, 1); usleep(100000);
    gpio_write(RESET_PIN, 0); usleep(100000);
    gpio_write(RESET_PIN, 1); usleep(120000);
}

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

static int init_framebuffer(void)
{
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
        else {
            fprintf(stderr, "unknown arg: %s\n", argv[i]);
            return 2;
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("neodct_displayd v2.2 (panel %dx%d, fb band %dx%d at y=%d, "
           "%d Hz SPI, %d fps poll, %s)\n",
           PANEL_W, PANEL_H, FB_W, FB_H, opt_yoff, opt_speed, opt_fps,
           opt_full ? "full-frame" : "dirty-rect");

    out_buf_size = (size_t)PANEL_W * PANEL_H * 2;
    out_buf = malloc(out_buf_size);
    if (!out_buf) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    if (setup_gpio() < 0) return 1;
    if (init_spi() < 0) return 1;

    reset_display();
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
        close(spi_fd);
        return 0;
    }

    if (init_framebuffer() < 0) {
        free(out_buf);
        close(spi_fd);
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
        if (elapsed < frame_ms)
            usleep((useconds_t)((frame_ms - elapsed) * 1000.0));
    } while (!quit_flag && !once_mode);

    printf("polled %ld frame(s), exiting\n", frames);

    if (prev_fb) free(prev_fb);
    munmap(fb_data, fb_size);
    close(fb_fd);
    if (dc_fd >= 0) close(dc_fd);
    close(spi_fd);
    free(out_buf);
    return 0;
}

/* nd_panel_spidev.c -- the phone's transport, moved out of neodctDisplay.c.
 *
 * Everything here was already in that file and is here unchanged: the sysfs
 * GPIO helpers, the spidev setup, the bufsiz probe, the chunked transfer, the
 * DC toggle that frames a command against data, and reset_display()'s
 * 100/100/120 ms pulse train. Same bodies, same statics, same order, same
 * printf text -- the diff is meant to read as a cut-and-paste, because this
 * is the one file in the tree with no host test and no ASAN coverage, and
 * every function moved is a chance to break a phone nobody here can boot.
 * Anything that looked like an improvement while moving was left alone.
 *
 * The one exception, and it is a cast rather than a change: `.len = chunk`
 * became an explicit (unsigned int), because these new files compile with the
 * full warning set including -Wconversion. The exemption neodctDisplay.c
 * carries is granted specifically to "a proven hardware driver predating the
 * port"; a new file has no such claim and must not inherit one for free.
 *
 * ============ WHAT MUST NOT DRIFT OUT OF HERE ============
 *
 *   * spi_chunk stays read from /sys/module/spidev/parameters/bufsiz, in this
 *     file. It must never become a shared constant: no emulator has a bufsiz,
 *     so a shared one would let a machine with no SPI bus choose the phone's
 *     ioctl count.
 *   * the DC value fd stays cached. It is a named v2.2 change ("DC gpio fd
 *     cached (no sysfs open/close per command)") and an interface that opened
 *     it per call would put a sysfs open in front of every command on the
 *     phone, with no test anywhere that could see it.
 *   * the reset pulse train stays 100/100/120 ms. It is a panel requirement.
 *     A backend whose reset is instant is not an argument for shortening it.
 */

/* usleep() is XSI, and <unistd.h> hides it under -std=c11 unless a feature
 * macro asks for it. Same reason neodctDisplay.c carries this. */
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <errno.h>

#include "nd_panel.h"

/* ---------- configuration (unchanged) ---------- */

#define DC_PIN     57          /* GPIO1_D1, physical pin 13 */
#define RESET_PIN  56          /* GPIO1_D0, physical pin 12 */

#define SPI_DEVICE "/dev/spidev0.0"
#define SPI_BITS   8
#define SPI_MODE   0           /* proven on this board+panel by fb_diag */

/* ---------- state ---------- */

static int spi_fd = -1;
static int dc_fd  = -1;                 /* cached sysfs value fd for DC */
static size_t spi_chunk = 4096;         /* from spidev bufsiz when readable */
static int opt_speed = 0;               /* handed in by nd_panel_spidev() */

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

static void spi_send(const unsigned char *data, size_t len)
{
    for (size_t i = 0; i < len; i += spi_chunk) {
        size_t chunk = len - i;
        if (chunk > spi_chunk)
            chunk = spi_chunk;
        struct spi_ioc_transfer tr = {
            .tx_buf = (unsigned long)(data + i),
            .rx_buf = 0,
            /* chunk <= spi_chunk, which came from bufsiz and is 4096 or
             * 65536. The cast is what -Wconversion asks for; it cannot
             * narrow anything this loop can produce. */
            .len = (unsigned int)chunk,
            .delay_usecs = 0,
            .speed_hz = (unsigned)opt_speed,
            .bits_per_word = SPI_BITS,
        };
        if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr) < 0)
            fprintf(stderr, "SPI transfer failed: %s\n", strerror(errno));
    }
}

/* ---------- the vtable ---------- */

static int spidev_open(struct nd_panel *p)
{
    (void)p;
    /* setup_gpio() before init_spi(), which is the order main() called them
     * in. It matters on the emulator, where gpio-mockup answers and there is
     * no bus: the daemon gets as far as "GPIO ready" and then fails on
     * /dev/spidev0.0, which is the diagnosis somebody reading the log wants. */
    if (setup_gpio() < 0) return -1;
    if (init_spi() < 0) return -1;
    return 0;
}

static void spidev_reset(struct nd_panel *p)
{
    (void)p;
    printf("Resetting panel...\n");
    gpio_write(RESET_PIN, 1); usleep(100000);
    gpio_write(RESET_PIN, 0); usleep(100000);
    gpio_write(RESET_PIN, 1); usleep(120000);
}

static void spidev_cmd(struct nd_panel *p, unsigned char op)
{
    (void)p;
    gpio_fd_write(dc_fd, 0);
    spi_send(&op, 1);
}

static void spidev_data(struct nd_panel *p, const unsigned char *buf, size_t n)
{
    (void)p;
    gpio_fd_write(dc_fd, 1);
    spi_send(buf, n);
}

static void spidev_close(struct nd_panel *p)
{
    (void)p;
    if (dc_fd >= 0) { close(dc_fd); dc_fd = -1; }
    if (spi_fd >= 0) { close(spi_fd); spi_fd = -1; }
}

static struct nd_panel spidev_panel = {
    .name  = "spidev",
    .open  = spidev_open,
    .reset = spidev_reset,
    .cmd   = spidev_cmd,
    .data  = spidev_data,
    .close = spidev_close,
    .ctx   = NULL,
};

struct nd_panel *nd_panel_spidev(int speed_hz)
{
    opt_speed = speed_hz;
    return &spidev_panel;
}

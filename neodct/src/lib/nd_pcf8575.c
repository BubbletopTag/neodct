/* nd_pcf8575.c -- raw i2c access to one PCF8575 sixteen-bit port expander.
 *
 * Ported from System/hw/pcf8575_keypad.py, class PCF8575. No library: the
 * chip has no register or command byte, so a plain two-byte write() and a
 * plain two-byte read() on /dev/i2c-N after a single I2C_SLAVE ioctl ARE the
 * correct raw transactions. There is nothing missing from this driver.
 *
 * Chip model, quasi-bidirectional with no direction registers:
 *   write 1 -> the pin is released to its weak internal pull-up, reads high
 *   write 0 -> the pin is driven hard low
 *
 * Which is what makes a matrix scan work at all: drive one row pin low, read
 * all sixteen bits, and any column bit that came back low is a switch joining
 * that row to that column.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "nd_keypad.h"
#include "nd_log.h"

/* glibc types ioctl's request as unsigned long, musl as int, and the
 * acceptance gate compiles every source under both. Casting at the call site
 * is the only spelling that is warning-clean on each. */
#ifdef __GLIBC__
#define IOCTL_REQ(x) ((unsigned long)(x))
#else
#define IOCTL_REQ(x) ((int)(unsigned int)(x))
#endif

nd_err nd_pcf8575_open(nd_pcf8575 *c, int bus, int addr)
{
    int n;

    if (c == NULL)
        return ND_ERR_INVAL;

    memset(c, 0, sizeof *c);
    c->fd = -1;
    c->bus = bus;
    c->addr = addr;

    n = snprintf(c->dev_path, sizeof c->dev_path, "/dev/i2c-%d", bus);
    if (n < 0 || (size_t)n >= sizeof c->dev_path)
        return ND_ERR_TOOLONG;

    c->fd = open(c->dev_path, O_RDWR | O_CLOEXEC);
    if (c->fd < 0) {
        nd_log_err(ND_LOG_INPUT, "cannot open %s: %s", c->dev_path, strerror(errno));
        return ND_ERR_IO;
    }
    c->owns_fd = true;

    if (ioctl(c->fd, IOCTL_REQ(ND_I2C_SLAVE), (unsigned long)addr) < 0) {
        nd_log_err(ND_LOG_INPUT, "I2C_SLAVE 0x%02X on %s: %s", addr, c->dev_path, strerror(errno));
        (void)close(c->fd);
        c->fd = -1;
        c->owns_fd = false;
        return ND_ERR_HARDWARE;
    }
    return ND_OK;
}

nd_err nd_pcf8575_attach(nd_pcf8575 *c, int fd)
{
    if (c == NULL || fd < 0)
        return ND_ERR_INVAL;

    memset(c, 0, sizeof *c);
    c->fd = fd;
    c->bus = -1;
    c->addr = ND_I2C_ADDR_DEFAULT;
    c->owns_fd = false;
    (void)nd_strlcpy(c->dev_path, "<attached>", sizeof c->dev_path);
    return ND_OK;
}

nd_err nd_pcf8575_write16(nd_pcf8575 *c, uint16_t value)
{
    uint8_t out[2];
    ssize_t n;

    if (c == NULL || c->fd < 0)
        return ND_ERR_INVAL;

    /* Low byte first, which is the order the chip latches its two ports in. */
    out[0] = (uint8_t)(((uint32_t)value) & 0xFFu);
    out[1] = (uint8_t)((((uint32_t)value) >> 8) & 0xFFu);

    n = write(c->fd, out, sizeof out);
    if (n != (ssize_t)sizeof out)
        return ND_ERR_IO;
    return ND_OK;
}

nd_err nd_pcf8575_read16(nd_pcf8575 *c, uint16_t *out)
{
    uint8_t data[2];
    ssize_t n;

    if (c == NULL || c->fd < 0 || out == NULL)
        return ND_ERR_INVAL;

    n = read(c->fd, data, sizeof data);
    if (n != (ssize_t)sizeof data) {
        nd_log_err(ND_LOG_INPUT, "short read from %s (got %d bytes)", c->dev_path, (int)n);
        return ND_ERR_IO;
    }
    *out = (uint16_t)((uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8));
    return ND_OK;
}

void nd_pcf8575_close(nd_pcf8575 *c)
{
    if (c == NULL || c->fd < 0)
        return;

    /* Release every pin to input/pull-up so nothing is left driven low across
     * a restart. A failure here is ignored, exactly as the Python swallows
     * the OSError -- we are closing anyway. */
    (void)nd_pcf8575_write16(c, 0xFFFFu);

    if (c->owns_fd)
        (void)close(c->fd);
    c->fd = -1;
    c->owns_fd = false;
}

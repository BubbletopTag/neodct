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
#include "nd_paths.h"

/* glibc types ioctl's request as unsigned long, musl as int, and the
 * acceptance gate compiles every source under both. Casting at the call site
 * is the only spelling that is warning-clean on each. */
#ifdef __GLIBC__
#define IOCTL_REQ(x) ((unsigned long)(x))
#else
#define IOCTL_REQ(x) ((int)(unsigned int)(x))
#endif

/* Remember what the kernel just said, so a caller three layers up that only
 * ever sees an nd_err can still tell the owner the truth. errno is read
 * FIRST, before any logging: nd_log_err() formats and writes, and both are
 * entitled to clobber errno. */
static void record_failure(nd_pcf8575 *c, nd_pcf8575_stage stage)
{
    c->last_errno = errno;
    c->last_stage = stage;
}

/* One line per failure burst. See io_error_logged in nd_keypad.h: a bus that
 * dies mid-session fails every transfer of every scan, two hundred times a
 * second, and a line each would bury the first one -- which is the only line
 * that says when the bus went. */
static void log_transfer_failure(nd_pcf8575 *c, const char *what, ssize_t got)
{
    if (c->io_error_logged)
        return;
    c->io_error_logged = true;
    if (got < 0)
        nd_log_err(ND_LOG_INPUT, "%s of 2 bytes to %s (0x%02X) failed: %s", what, c->dev_path,
                   (unsigned)c->addr, strerror(c->last_errno));
    else
        nd_log_err(ND_LOG_INPUT, "short %s on %s (got %d of 2 bytes)", what, c->dev_path, (int)got);
}

nd_err nd_pcf8575_open(nd_pcf8575 *c, int bus, int addr)
{
    /* The path actually opened. dev_path stays the VIRTUAL name, because that
     * is the one a person goes and looks at; only the syscall sees the other.
     *
     * Going through nd_path_resolve() is the house rule (CODING-STANDARDS.md:
     * every path does), and this one was the exception -- which meant a host
     * test could point ND_ROOT at a fixture, watch nd_kpsetup_gate_check()
     * find /dev/i2c-3 inside it, and then watch this function look for the
     * REAL /dev/i2c-3 and fail. Two spellings of the same path in one module
     * is a test that cannot reach the code it is aiming at. */
    char resolved[ND_PATH_MAX];
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
    if (nd_path_resolve(resolved, sizeof resolved, c->dev_path) != ND_OK)
        return ND_ERR_TOOLONG;

    c->fd = open(resolved, O_RDWR | O_CLOEXEC);
    if (c->fd < 0) {
        record_failure(c, ND_PCF_STAGE_OPEN);
        nd_log_err(ND_LOG_INPUT, "cannot open %s: %s", c->dev_path, strerror(c->last_errno));
        return ND_ERR_IO;
    }
    c->owns_fd = true;

    if (ioctl(c->fd, IOCTL_REQ(ND_I2C_SLAVE), (unsigned long)addr) < 0) {
        record_failure(c, ND_PCF_STAGE_SLAVE);
        nd_log_err(ND_LOG_INPUT, "I2C_SLAVE 0x%02X on %s: %s", addr, c->dev_path,
                   strerror(c->last_errno));
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

nd_err nd_pcf8575_adopt(nd_pcf8575 *c, int fd, int bus, int addr)
{
    int n;

    if (c == NULL || fd < 0)
        return ND_ERR_INVAL;

    memset(c, 0, sizeof *c);
    c->fd = fd;
    c->bus = bus;
    c->addr = addr;
    /* NOT ours to close. The descriptor belongs to whoever opened it -- on
     * the phone that is the root phase of the boot, and it has to outlive
     * every nd_pcf8575 built over it so the matrix can be rebuilt after a bus
     * failure without asking the kernel for permission a second time. */
    c->owns_fd = false;

    n = snprintf(c->dev_path, sizeof c->dev_path, "/dev/i2c-%d", bus);
    if (n < 0 || (size_t)n >= sizeof c->dev_path)
        (void)nd_strlcpy(c->dev_path, "<adopted>", sizeof c->dev_path);
    return ND_OK;
}

int nd_pcf8575_detach(nd_pcf8575 *c)
{
    int fd;

    if (c == NULL)
        return -1;
    fd = c->fd;
    /* No release word and no close: the caller is taking the descriptor on
     * precisely because it wants the bus left exactly as it is. */
    c->fd = -1;
    c->owns_fd = false;
    return fd;
}

const char *nd_pcf8575_stage_name(nd_pcf8575_stage stage)
{
    switch (stage) {
    case ND_PCF_STAGE_OPEN:
        return "open";
    case ND_PCF_STAGE_SLAVE:
        return "I2C_SLAVE";
    case ND_PCF_STAGE_WRITE:
        return "write";
    case ND_PCF_STAGE_READ:
        return "read";
    case ND_PCF_STAGE_NONE:
    default:
        return "";
    }
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
    if (n != (ssize_t)sizeof out) {
        /* ============ THE SILENT NAK ============
         *
         * This is the FIRST transaction that ever touches the wires: open()
         * and ioctl(I2C_SLAVE) are both local, so a keypad that is absent,
         * unpowered or miswired opens perfectly and fails here. Until 0.5.8b
         * the failure returned without a word, and the owner was told, in
         * writing on the panel, that the phone had "a permission or wiring
         * problem" -- for what was as likely to be an expander whose rail was
         * still rising two hundred milliseconds into the boot.
         *
         * read16() a few lines below has logged its own failures since it was
         * written. The asymmetry was the bug. */
        record_failure(c, ND_PCF_STAGE_WRITE);
        log_transfer_failure(c, "write", n);
        return ND_ERR_IO;
    }
    c->io_error_logged = false;
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
        record_failure(c, ND_PCF_STAGE_READ);
        log_transfer_failure(c, "read", n);
        return ND_ERR_IO;
    }
    c->io_error_logged = false;
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

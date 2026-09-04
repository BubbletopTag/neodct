/* nd_recinput.c -- the half of this task that is load-bearing: making the
 * sixteen keys reach the recovery menu.
 *
 * ============ WHAT WAS ACTUALLY BROKEN ============
 *
 * Recovery has drawn on /dev/tty1 for a long time and mkinitramfs.py has
 * shipped neodct_displayd for as long, so on a phone you could always SEE a
 * 30x10 menu. You could not drive it. The sixteen keys are on a PCF8575 port
 * expander that no kernel driver binds, so no byte ever arrives on the VT and
 * recovery_tty()'s /dev/tty1 branch is a dead end. The graphics are the part
 * that makes it look like the phone; this file is the part that makes it work
 * on one.
 *
 * ============ TWO BACKENDS, PROBED, NEVER CONFIGURED ============
 *
 * Both are opened and BOTH are polled if both open, so a developer with a USB
 * keyboard plugged into a phone keeps the keyboard. If neither opens the
 * caller exits 2 and the shell falls back to its tty menu -- drawing a menu
 * nobody can move is worse than console text that at least works over serial.
 *
 * evdev discovery does NOT copy nd_evdev_discover()'s six-step order: that
 * order is built on /dev/input/by-path and by-id, and there is no udev in an
 * initramfs, so those symlinks do not exist. readdir of every event* is the
 * whole of what is available here.
 *
 * ============ THE KEYMAP IS READ ONCE, BEFORE ANY MENU ============
 *
 * recovery_action_wipe_user deletes everything on the user partition except
 * .ndsys -- including keymap.json. Re-reading it after a wipe would lose the
 * keypad mid-session, on the one screen where somebody has just been told
 * their data is gone. (The first-boot wizard regenerates it on the next boot,
 * so this is not a new bug; it is a constraint on the ordering here.)
 *
 * ============ NO COMPILED-IN DEFAULT MATRIX ============
 *
 * spec-hw-input.md shows row_pins [0,1,2,3] and col_pins [4,5,6,7], and
 * apps/KeypadMapperI2C carries those as fallbacks. Pins alone do not say
 * WHICH switch is Up, and guessing would move the selection at random on a
 * phone somebody is trying to rescue. With no keymap the i2c backend is
 * simply unavailable and the caller says so on the panel.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "nd_recui.h"

/* linux/i2c-dev.h, which musl does not ship a copy of. */
#define ND_REC_I2C_SLAVE 0x0703

#ifdef __GLIBC__
#define IOCTL_REQ(x) ((unsigned long)(x))
#else
#define IOCTL_REQ(x) ((int)(unsigned int)(x))
#endif

/* linux/input-event-codes.h */
#define ND_REC_EV_KEY 0x01

/* The evdev fds are polled on this cadence and the matrix is scanned between
 * two polls, so a key on either backend is seen within a frame. */
#define ND_RECINPUT_TICK_MS 10

#define ND_RECINPUT_KEYMAP_MAX 8192

/* ------------------------------------------------------------------ *
 * Names to codes
 * ------------------------------------------------------------------ */

/* The sixteen keys, and only the sixteen: the enrolment order of
 * nd_kpsetup_targets[] in lib/nd_keypadsetup.c, which is by definition every
 * key the hardware has. nd_keycode_for_name() also accepts "left", "right"
 * and "menu"; those reach only a development QWERTY keyboard and must never
 * be routed to anything here. The unit test walks this table against
 * nd_keycodes.h for exactly that reason. */
static const struct {
    const char *name;
    int32_t code;
} rec_key_names[] = {
    {"navikey", ND_RECKEY_ENTER},
    {"clear", ND_RECKEY_CLEAR},
    {"up", ND_RECKEY_UP},
    {"down", ND_RECKEY_DOWN},
    {"num_1", 2},
    {"num_2", 3},
    {"num_3", 4},
    {"num_4", 5},
    {"num_5", 6},
    {"num_6", 7},
    {"num_7", 8},
    {"num_8", 9},
    {"num_9", 10},
    {"num_0", ND_RECKEY_0},
    {"star", ND_RECKEY_STAR},
    {"hash", ND_RECKEY_HASH},
};

int32_t nd_reckey_for_name(const char *name, size_t len)
{
    size_t i;

    if (name == NULL)
        return -1;
    for (i = 0u; i < sizeof rec_key_names / sizeof rec_key_names[0]; i++) {
        if (strlen(rec_key_names[i].name) == len && memcmp(rec_key_names[i].name, name, len) == 0)
            return rec_key_names[i].code;
    }
    return -1;
}

/* ------------------------------------------------------------------ *
 * keymap.json, without a JSON parser
 * ------------------------------------------------------------------ */

/* Find the key `"name"` -- with both quotes, so "row_pin" (singular, inside
 * the nested "keys" object) can never match "row_pins" -- and return the
 * first character after the colon that follows it. NULL if absent. */
static const char *find_field(const char *text, const char *name)
{
    char needle[32];
    const char *p;
    int n = snprintf(needle, sizeof needle, "\"%s\"", name);

    if (n < 0 || (size_t)n >= sizeof needle)
        return NULL;
    p = strstr(text, needle);
    if (p == NULL)
        return NULL;
    p += (size_t)n;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p != ':')
        return NULL;
    return p + 1;
}

/* strtol over a JSON array of small integers. Anything out of range for the
 * expander drops the whole list, which is the one structural refusal
 * nd_keymap.c also makes -- half a pin list produces a scan that presses
 * nothing. */
/* Skip the whitespace after a colon and require the value to open with `want`.
 * Scanning forward to the next bracket instead would, for a `"row_pins": 5`,
 * happily find col_pins's array further down the file and report ITS pins as
 * the rows -- a keymap that is merely wrong rather than obviously broken, and
 * a scan that presses nothing. */
static const char *value_open(const char *p, char want)
{
    if (p == NULL)
        return NULL;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return (*p == want) ? p + 1 : NULL;
}

static int read_pins(const char *p, uint8_t *out, size_t max, size_t *n_out)
{
    size_t n = 0u;

    *n_out = 0u;
    p = value_open(p, '[');
    if (p == NULL)
        return -1;

    for (;;) {
        char *end = NULL;
        long v;

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')
            p++;
        if (*p == ']' || *p == '\0')
            break;
        v = strtol(p, &end, 10);
        if (end == p)
            return -1;
        if (v < 0 || v >= (long)ND_RECMATRIX_MAX_PINS)
            return -1;
        if (n >= max)
            return -1;
        out[n++] = (uint8_t)v;
        p = end;
    }
    *n_out = n;
    return (n > 0u) ? 0 : -1;
}

/* i2c_addr is written as an int (32 for 0x20), but two other tools have
 * written this field by hand, so "0x20" and "32" are both accepted -- the
 * same latitude nd_keymap.c's json_to_addr() gives. */
static int read_int(const char *p, int *out)
{
    char *end = NULL;
    long v;
    int base = 10;

    if (p == NULL)
        return -1;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '"')
        p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        base = 16;
    v = strtol(p, &end, base);
    if (end == p)
        return -1;
    *out = (int)v;
    return 0;
}

int nd_reckeymap_parse(const char *text, nd_reckeymap *out)
{
    const char *p;
    size_t r;
    size_t c;

    if (text == NULL || out == NULL)
        return -1;

    memset(out, 0, sizeof *out);
    for (r = 0u; r < ND_RECMATRIX_MAX_PINS; r++) {
        for (c = 0u; c < ND_RECMATRIX_MAX_PINS; c++)
            out->code[r][c] = -1;
    }
    out->i2c_bus = 3;     /* ND_I2C_BUS_DEFAULT  */
    out->i2c_addr = 0x20; /* ND_I2C_ADDR_DEFAULT */

    if (read_pins(find_field(text, "row_pins"), out->row_pins, ND_RECMATRIX_MAX_PINS,
                  &out->n_rows) != 0)
        return -1;
    if (read_pins(find_field(text, "col_pins"), out->col_pins, ND_RECMATRIX_MAX_PINS,
                  &out->n_cols) != 0)
        return -1;

    /* Absent means the default, which is what the phone has: nd_keymap_save
     * always writes both, but a hand-edited file might not. */
    (void)read_int(find_field(text, "i2c_bus"), &out->i2c_bus);
    (void)read_int(find_field(text, "i2c_addr"), &out->i2c_addr);

    /* "by_matrix" rather than the nested "keys" object because it is already
     * the flat "row,col" -> name map this needs, and both writers
     * (nd_keymap_save and apps/KeypadMapperI2C) emit it. */
    p = value_open(find_field(text, "by_matrix"), '{');
    if (p == NULL)
        return -1;

    while (*p != '\0' && *p != '}') {
        const char *pos;
        const char *name;
        char *end = NULL;
        long row;
        long col;
        int32_t code;
        size_t name_len;

        if (*p != '"') {
            p++;
            continue;
        }
        pos = ++p;
        while (*p != '\0' && *p != '"')
            p++;
        if (*p != '"')
            return -1;
        p++;

        row = strtol(pos, &end, 10);
        if (end == pos || *end != ',') {
            /* Not a "row,col" key at all. Skipped in silence, exactly as
             * nd_keymap.c skips an entry it does not understand: a keymap
             * missing the '7' key still boots a phone you can fix. */
            continue;
        }
        col = strtol(end + 1, &end, 10);

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p != ':')
            continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p != '"')
            continue;
        name = ++p;
        while (*p != '\0' && *p != '"')
            p++;
        if (*p != '"')
            return -1;
        name_len = (size_t)(p - name);
        p++;

        code = nd_reckey_for_name(name, name_len);
        if (code < 0)
            continue;
        if (row < 0 || row >= (long)ND_RECMATRIX_MAX_PINS)
            continue;
        if (col < 0 || col >= (long)ND_RECMATRIX_MAX_PINS)
            continue;

        out->code[row][col] = code;
        out->any_key = true;
    }

    return out->any_key ? 0 : -1;
}

int nd_reckeymap_load(const char *path, nd_reckeymap *out)
{
    /* Static, not malloc'd: CODING-STANDARDS section 1.5 and this program's
     * "no heap after startup" rule. The file nd_keymap_save() writes is about
     * 2 KB, so 8 KB is four times the real thing. */
    static char buf[ND_RECINPUT_KEYMAP_MAX];
    ssize_t got;
    int fd;

    if (path == NULL || out == NULL)
        return -1;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    got = read(fd, buf, sizeof buf - 1u);
    (void)close(fd);
    if (got <= 0)
        return -1;
    buf[got] = '\0';
    return nd_reckeymap_parse(buf, out);
}

/* ------------------------------------------------------------------ *
 * The PCF8575 matrix
 * ------------------------------------------------------------------ */

static void sleep_us(long usec)
{
    struct timespec req;

    if (usec <= 0)
        return;
    req.tv_sec = usec / 1000000L;
    req.tv_nsec = (usec % 1000000L) * 1000L;
    while (nanosleep(&req, &req) != 0 && errno == EINTR)
        ; /* a signal must not shorten a settle time */
}

/* The chip has no register and no command byte, so a plain two-byte write and
 * a plain two-byte read after one I2C_SLAVE ioctl ARE the correct raw
 * transactions. Low byte first, which is the order it latches its two ports
 * in. Writing 1 releases a pin to its weak pull-up; writing 0 drives it low. */
static int chip_write(int fd, uint16_t value)
{
    uint8_t out[2];

    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
    return (write(fd, out, sizeof out) == (ssize_t)sizeof out) ? 0 : -1;
}

static int chip_read(int fd, uint16_t *value)
{
    uint8_t in[2];

    if (read(fd, in, sizeof in) != (ssize_t)sizeof in)
        return -1;
    *value = (uint16_t)((uint16_t)in[0] | (uint16_t)((uint16_t)in[1] << 8));
    return 0;
}

static void matrix_reset_state(nd_recmatrix *mx, const nd_reckeymap *map)
{
    mx->map = map;
    memset(mx->held, -1, sizeof mx->held);
    /* Release every pin on the way in, so a restart mid-scan cannot leave a
     * row driven low against a pressed key. */
    (void)chip_write(mx->fd, 0xFFFFu);
}

int nd_recmatrix_open(nd_recmatrix *mx, const nd_reckeymap *map)
{
    char path[32];
    int n;

    if (mx == NULL || map == NULL || !map->any_key)
        return -1;

    memset(mx, 0, sizeof *mx);
    mx->fd = -1;

    n = snprintf(path, sizeof path, "/dev/i2c-%d", map->i2c_bus);
    if (n < 0 || (size_t)n >= sizeof path)
        return -1;

    mx->fd = open(path, O_RDWR | O_CLOEXEC);
    if (mx->fd < 0)
        return -1;
    if (ioctl(mx->fd, IOCTL_REQ(ND_REC_I2C_SLAVE), (unsigned long)map->i2c_addr) < 0) {
        fprintf(stderr, "nd-recui: I2C_SLAVE 0x%02X on %s: %s\n", map->i2c_addr, path,
                strerror(errno));
        (void)close(mx->fd);
        mx->fd = -1;
        return -1;
    }
    mx->owns_fd = true;
    matrix_reset_state(mx, map);
    return 0;
}

int nd_recmatrix_attach(nd_recmatrix *mx, const nd_reckeymap *map, int fd)
{
    if (mx == NULL || map == NULL || fd < 0)
        return -1;
    memset(mx, 0, sizeof *mx);
    mx->fd = fd;
    mx->owns_fd = false;
    matrix_reset_state(mx, map);
    return 0;
}

int32_t nd_recmatrix_scan(nd_recmatrix *mx)
{
    bool current[ND_RECMATRIX_MAX_PINS][ND_RECMATRIX_MAX_PINS];
    int32_t pressed = ND_RECKEY_NONE;
    size_t row;
    size_t col;

    if (mx == NULL || mx->fd < 0 || mx->map == NULL)
        return ND_RECKEY_NONE;

    memset(current, 0, sizeof current);

    for (row = 0u; row < mx->map->n_rows; row++) {
        uint16_t value = 0u;

        if (chip_write(mx->fd, (uint16_t)(0xFFFFu & ~(1u << mx->map->row_pins[row]))) != 0)
            return ND_RECKEY_NONE;

        /* The i2c transactions themselves take ~0.5 ms at 100 kHz; the settle
         * guards against line capacitance on a long ribbon. */
        sleep_us(ND_RECMATRIX_SETTLE_US);

        if (chip_read(mx->fd, &value) != 0)
            return ND_RECKEY_NONE;

        for (col = 0u; col < mx->map->n_cols; col++) {
            if (((((uint32_t)value) >> mx->map->col_pins[col]) & 1u) == 0u)
                current[row][col] = true;
        }
    }
    (void)chip_write(mx->fd, 0xFFFFu);

    /* Press edges only, and only the first one. nd_matrix_scan_once() queues
     * the rest for key rollover; its own comment says the queue exists for
     * games, and a menu needs one key at a time. */
    for (row = 0u; row < mx->map->n_rows && pressed == ND_RECKEY_NONE; row++) {
        for (col = 0u; col < mx->map->n_cols; col++) {
            if (current[row][col] && mx->held[row][col] == -1 && mx->map->code[row][col] >= 0) {
                pressed = mx->map->code[row][col];
                break;
            }
        }
    }

    /* A membrane contact chatters on the way up, not on the way down, so a
     * key counts as released only after ND_RECMATRIX_RELEASE_SCANS scans
     * without it -- and a press is reported the instant it appears. */
    for (row = 0u; row < ND_RECMATRIX_MAX_PINS; row++) {
        for (col = 0u; col < ND_RECMATRIX_MAX_PINS; col++) {
            if (current[row][col]) {
                mx->held[row][col] = 0;
            } else if (mx->held[row][col] != -1) {
                mx->held[row][col] = (int8_t)(mx->held[row][col] + 1);
                if (mx->held[row][col] >= (int8_t)ND_RECMATRIX_RELEASE_SCANS)
                    mx->held[row][col] = -1;
            }
        }
    }
    return pressed;
}

void nd_recmatrix_close(nd_recmatrix *mx)
{
    if (mx == NULL || mx->fd < 0)
        return;
    /* Leaving a row driven low across a restart is a real failure -- the next
     * scan reads that row as a wall of pressed keys. nd_pcf8575_close() makes
     * the same parting write for the same reason. */
    (void)chip_write(mx->fd, 0xFFFFu);
    if (mx->owns_fd)
        (void)close(mx->fd);
    mx->fd = -1;
}

/* ------------------------------------------------------------------ *
 * evdev
 * ------------------------------------------------------------------ */

int32_t nd_recevdev_decode(const uint8_t *buf, size_t n)
{
    uint16_t type;
    uint16_t code;
    int32_t value;
    size_t off;

    if (buf == NULL)
        return ND_RECKEY_NONE;

    /* struct input_event carries a struct timeval, so a record is 24 bytes on
     * a 64-bit host and 16 on 32-bit ARM, and the layout is decided by the
     * kernel that made the device rather than by the program reading it. The
     * same binary reads both. */
    if (n == 24u)
        off = 16u;
    else if (n == 16u)
        off = 8u;
    else
        return ND_RECKEY_NONE;

    memcpy(&type, buf + off, sizeof type);
    memcpy(&code, buf + off + 2u, sizeof code);
    memcpy(&value, buf + off + 4u, sizeof value);

    if (type != ND_REC_EV_KEY)
        return ND_RECKEY_NONE;
    /* Value 2 is the kernel's own autorepeat. nd_input synthesises its own
     * and therefore drops it; here there is no synthesiser, so accepting it
     * is what makes a held arrow scroll on the QEMU keyboard. It costs one
     * `||` and the i2c path is unaffected -- the matrix reports edges and has
     * no repeat of its own. */
    if (value != 1 && value != 2)
        return ND_RECKEY_NONE;
    return (int32_t)code;
}

static void open_evdev(nd_recinput *in)
{
    DIR *dir = opendir("/dev/input");
    struct dirent *entry;

    if (dir == NULL)
        return;
    while ((entry = readdir(dir)) != NULL && in->n_evdev < ND_RECINPUT_MAX_EVDEV) {
        char path[64];
        int fd;

        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;
        if (snprintf(path, sizeof path, "/dev/input/%s", entry->d_name) < 0)
            continue;
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0)
            in->evdev[in->n_evdev++] = fd;
    }
    (void)closedir(dir);
}

int nd_recinput_open(nd_recinput *in, const char *keymap_path)
{
    if (in == NULL)
        return -1;

    memset(in, 0, sizeof *in);
    in->matrix.fd = -1;

    open_evdev(in);

    /* Read the keymap ONCE, here, before any menu -- see the header comment
     * about wipe-user-data deleting the file this session is still using. */
    if (keymap_path != NULL && nd_reckeymap_load(keymap_path, &in->map) == 0) {
        if (nd_recmatrix_open(&in->matrix, &in->map) == 0)
            in->have_matrix = true;
    }

    return (in->n_evdev > 0u || in->have_matrix) ? 0 : -1;
}

/* Forget one evdev descriptor. A USB keyboard unplugged mid-menu leaves its
 * fd reporting POLLHUP forever; poll() then returns immediately every time
 * and, since a hangup is not POLLIN, nothing consumes it -- a spin at 100%
 * on the one screen where the phone is meant to be waiting for a person.
 * Dropping the descriptor is what stops that. */
static void forget_evdev(nd_recinput *in, size_t i)
{
    (void)close(in->evdev[i]);
    in->n_evdev--;
    if (i < in->n_evdev)
        in->evdev[i] = in->evdev[in->n_evdev];
}

int32_t nd_recinput_wait(nd_recinput *in)
{
    struct pollfd pfd[ND_RECINPUT_MAX_EVDEV];

    if (in == NULL)
        return ND_RECKEY_NONE;

    for (;;) {
        size_t i;
        int rc;

        if (in->n_evdev > 0u) {
            for (i = 0u; i < in->n_evdev; i++) {
                pfd[i].fd = in->evdev[i];
                pfd[i].events = POLLIN;
                pfd[i].revents = 0;
            }
            /* A short timeout rather than a block, because the matrix has no
             * descriptor to wait on and has to be scanned between polls. */
            rc = poll(pfd, (nfds_t)in->n_evdev, in->have_matrix ? ND_RECINPUT_TICK_MS : -1);
            if (rc > 0) {
                /* Backwards, so forget_evdev()'s swap-with-last cannot move an
                 * entry this pass has not looked at yet. */
                i = in->n_evdev;
                while (i-- > 0u) {
                    uint8_t buf[24];
                    ssize_t got;
                    int32_t key;

                    if ((pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                        forget_evdev(in, i);
                        continue;
                    }
                    if ((pfd[i].revents & POLLIN) == 0)
                        continue;
                    got = read(in->evdev[i], buf, sizeof buf);
                    if (got < 0 && errno != EINTR && errno != EAGAIN) {
                        forget_evdev(in, i);
                        continue;
                    }
                    if (got <= 0)
                        continue;
                    key = nd_recevdev_decode(buf, (size_t)got);
                    if (key != ND_RECKEY_NONE)
                        return key;
                }
            } else if (rc < 0 && errno != EINTR) {
                return ND_RECKEY_NONE;
            }
        }

        if (in->have_matrix) {
            int32_t key = nd_recmatrix_scan(&in->matrix);

            if (key != ND_RECKEY_NONE)
                return key;
            if (in->n_evdev == 0u)
                sleep_us(ND_RECINPUT_TICK_MS * 1000L);
        } else if (in->n_evdev == 0u) {
            /* Nothing left to wait on. The caller exits 2 and the shell falls
             * back to its tty menu. */
            return ND_RECKEY_NONE;
        }
    }
}

void nd_recinput_close(nd_recinput *in)
{
    size_t i;

    if (in == NULL)
        return;
    for (i = 0u; i < in->n_evdev; i++)
        (void)close(in->evdev[i]);
    in->n_evdev = 0u;
    if (in->have_matrix) {
        nd_recmatrix_close(&in->matrix);
        in->have_matrix = false;
    }
}

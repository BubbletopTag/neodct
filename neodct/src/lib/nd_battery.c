/* nd_battery.c -- the MAX1704x fuel gauge, with a simulation fallback.
 *
 * Port of System/core/BatteryService/__init__.py. Reads VCELL from a
 * MAX17043/44/48/49 at 0x36 on /dev/i2c-3 using the same raw write-then-read
 * pattern as System/hw/pcf8575_keypad.py and engineering/tools/max1704x_watch.py.
 * If there is no bus node at all (QEMU, or hardware without the battery board)
 * the service runs in Simulation Mode, exactly as ModemService does.
 *
 * Simulation Mode reports a fixed 3.85 V. For testing the warning and shutdown
 * flow in QEMU, override it with NEODCT_BATT_SIM_VCELL, or live at runtime by
 * writing a voltage to /tmp/neodct_sim_vcell (`echo 3.30 > /tmp/neodct_sim_vcell`
 * from the serial console).
 *
 * ============ AND SIMULATION IS NOT WHAT A BROKEN GAUGE GETS ============
 *
 * It was, for five releases, and it is the worst thing this file ever did.
 * Every failure -- the path, open(), the I2C_SLAVE ioctl, the register read, a
 * floating bus -- fell into one `goto simulate` that latched a fixed 3.85 V
 * for the life of the process. 3.85 V is above every threshold below, so a
 * phone that could not read its own battery ALSO had its low-battery warning
 * and its 3.20 V protective shutdown silently switched off, and then ran the
 * cell flat while drawing three cheerful bars.
 *
 * On this phone that was not a hypothetical. nd_battery_open() runs inside
 * nd_ui_init(), i.e. after nd-core has dropped to ndusr, and /dev/i2c-3 is
 * root:root 0600 from devtmpfs until udev applies group i2c -- so on any boot
 * where the coldplug was late the gauge lost the race, once, permanently, and
 * said "HARDWARE NOT FOUND ... a stub for the QEMU dev environment" about a
 * chip soldered six millimetres from the CPU.
 *
 * Three things changed. The verdict is now one of THREE (nd_battery_source):
 * absent, live, or present-and-unreadable, split by errno at open(). Unreadable
 * reports NO voltage rather than a plausible one. And nothing is decided once
 * -- the probe re-runs from the poll the status bar already turns, fast for
 * the first half-minute and slowly thereafter, so a gauge whose grant arrived
 * late, or whose rail was still rising, or that was plugged in afterwards,
 * simply starts working.
 *
 * Voltage policy, per the 0.2.4a bring-up sweep on the bench supply:
 *   * 3.25-3.35 V is "dead" (gauge segment 0), 3.95-4.10 V is "full" (4)
 *   * <= 3.45 V latches a LOW BATTERY warning
 *   * <= 3.25 V latches a BATTERY CRITICALLY LOW warning
 *   * <= 3.20 V, confirmed over three consecutive polls, requests shutdown
 *
 * ============ THE FIVE-SAMPLE MEAN IS SUMMED IN INSERTION ORDER ============
 *
 * IEEE-754 addition is not associative. sum(deque)/len(deque) in the Python
 * adds oldest-to-newest starting from an integer zero, and any other order --
 * a running total kept across polls, a pairwise sum, a Kahan compensation --
 * can differ in the last bit. That is not academic here: the thresholds are
 * compared with <=, and 3.20 V sampled five times sums to exactly 16.0 and
 * divides to exactly 3.2, which IS <= ND_SHUTDOWN_V. Shift a ULP and the phone
 * does not shut down. So the ring buffer below is iterated oldest-first and
 * the accumulator starts at 0.0, and neither of those is a stylistic choice.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "nd_battery.h"
#include "nd_keypad.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_settings.h"
#include "nd_types.h"
#include "nd_vclock.h"

/* Gauge segment boundaries: below 3.35 V -> 0 (dead), each 0.20 V band adds a
 * segment, >= 3.95 V -> 4 (full). Matches bat-0..bat-4 in ui_home.json. */
const double ND_LEVEL_THRESHOLDS[4] = {3.35, 3.55, 3.75, 3.95};

/* nd_battery_poll()'s non-NULL return. A literal owned by libneodct, as the
 * header promises; callers compare it with strcmp, never with ==. */
static const char BATT_SHUTDOWN[] = "shutdown";

typedef enum { BATT_WARN_NONE = 0, BATT_WARN_LOW, BATT_WARN_CRITICAL } batt_warning;

struct nd_battery {
    int bus;
    int addr;
    int fd;       /* -1 unless the gauge is LIVE */
    bool owns_fd; /* false when the fd came from nd_battery_provide_bus_fd() */
    bool hardware;
    uint16_t version;

    /* Which of the three nd_battery_source answers this is, why, and when it
     * is worth asking again. `hardware` is kept as the derived boolean the
     * rest of the tree already reads; `source` is what actually decides. */
    nd_battery_source source;
    char why[96];
    bool retry_possible; /* the errno was one a later attempt could get past */
    double next_probe;   /* monotonic; when the next reprobe is due */
    int probe_tries;

    /* deque(maxlen=5): head is the oldest, and iteration for the mean runs
     * head, head+1, ... head+count-1 modulo the window. */
    double samples[ND_BATT_SMOOTH_WINDOW];
    size_t sample_count;
    size_t sample_head;

    double smoothed;
    bool have_smoothed;
    int32_t level;
    bool low_armed;
    bool crit_armed;
    batt_warning pending;
    int shutdown_count;
    double last_poll;
    int read_error_streak;
};

/* ------------------------------------------------------------------ *
 * Register access
 * ------------------------------------------------------------------ */

/* Write the register pointer, STOP, then a separate read transaction. NOT a
 * repeated-START combined transfer -- do not "improve" this into an I2C_RDWR
 * ioctl, the bus timing on the real board differs. */
static int read16(int fd, uint8_t reg, uint16_t *out)
{
    uint8_t r = reg;
    uint8_t d[2];

    if (write(fd, &r, 1u) != 1)
        return -1;
    if (read(fd, d, 2u) != 2)
        return -1;
    *out = (uint16_t)(((uint16_t)d[0] << 8) | (uint16_t)d[1]);
    return 0;
}

static int write16(int fd, uint8_t reg, uint16_t val)
{
    uint8_t b[3];

    b[0] = reg;
    b[1] = (uint8_t)(val >> 8);
    b[2] = (uint8_t)(val & 0xFFu);
    return write(fd, b, 3u) == 3 ? 0 : -1;
}

static int32_t signed16(uint16_t v)
{
    return (v & 0x8000u) != 0u ? (int32_t)v - 0x10000 : (int32_t)v;
}

/* ------------------------------------------------------------------ *
 * Construction
 * ------------------------------------------------------------------ */

/* Python's int(str, base). Rejects trailing junk, which int() also does. */
static bool parse_int(const char *text, int base, long *out)
{
    char *end = NULL;
    long value;

    if (text == NULL || text[0] == '\0')
        return false;

    errno = 0;
    value = strtol(text, &end, base);
    if (errno != 0 || end == text || *end != '\0')
        return false;

    *out = value;
    return true;
}

/* _config_from_settings(). Both keys are read, and if EITHER is unusable the
 * Python's try block aborts and both fall back to the defaults -- so a garbled
 * bus number also discards a perfectly good address. Reproduced. */
static void config_from_settings(int *bus, int *addr)
{
    char raw_bus[64];
    char raw_addr[64];
    long v_bus;
    long v_addr;

    if (nd_settings_get_copy(ND_SET_HW_BATT_I2C_BUS, ND_SET_HW_BATT_I2C_BUS_DFLT, raw_bus,
                             sizeof raw_bus) != ND_OK ||
        !parse_int(raw_bus, 10, &v_bus)) {
        nd_log(ND_LOG_BATT, "Settings unavailable (%s is not an integer); using i2c defaults.",
               ND_SET_HW_BATT_I2C_BUS);
        return;
    }

    /* base 0, so "0x36" parses as hex and "54" as decimal. */
    if (nd_settings_get_copy(ND_SET_HW_BATT_I2C_ADDR, ND_SET_HW_BATT_I2C_ADDR_DFLT, raw_addr,
                             sizeof raw_addr) != ND_OK ||
        !parse_int(raw_addr, 0, &v_addr)) {
        nd_log(ND_LOG_BATT, "Settings unavailable (%s is not an integer); using i2c defaults.",
               ND_SET_HW_BATT_I2C_ADDR);
        return;
    }

    *bus = (int)v_bus;
    *addr = (int)v_addr;
}

/* ------------------------------------------------------------------ *
 * The bus descriptor that crosses the privilege drop
 * ------------------------------------------------------------------ */

/* Process-wide, and process-wide for the same reason nd_input's keypad
 * descriptor is (nd_keypad.h): the boot sequence that can still open this as
 * root is several modules away from nd_ui_init(), which is where the gauge is
 * actually constructed, and it IS a process-wide fact -- this process opened
 * the gauge's bus once, before it stopped being root. */
static int g_bus_fd = -1;
static int g_bus_fd_bus = -1;

void nd_battery_provide_bus_fd(int fd, int bus)
{
    g_bus_fd = fd;
    g_bus_fd_bus = bus;
}

bool nd_battery_open_bus_as_root(void)
{
    int bus = ND_BATT_DEFAULT_I2C_BUS;
    int addr = ND_BATT_DEFAULT_I2C_ADDR;
    char dev[64];
    char resolved[ND_PATH_MAX];
    int fd;

    /* Only from a process that still is root. Called from anywhere else this
     * would open the node with exactly the permissions the caller already
     * has, i.e. it would buy nothing and hide the fact that it bought
     * nothing. */
    if (geteuid() != 0)
        return false;

    (void)nd_settings_init();
    config_from_settings(&bus, &addr);

    (void)nd_snprintf(dev, sizeof dev, "/dev/i2c-%d", bus);
    if (nd_path_resolve(resolved, sizeof resolved, dev) != ND_OK)
        return false;

    /* O_CLOEXEC: nd-core forks and execs an app per launch, and the broker has
     * already forked by the time this runs, so nothing but this process ever
     * sees the descriptor. */
    fd = open(resolved, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        /* Worth a line even as root: if root cannot open it, the node is not
         * there at all and the phone has no fuel gauge, which is a different
         * and much larger fact than "udev was late". */
        nd_log(ND_LOG_BATT, "Fuel gauge bus %s could not be opened as root: %s.", dev,
               strerror(errno));
        return false;
    }

    nd_log(ND_LOG_BATT, "Fuel gauge bus %s opened before the privilege drop.", dev);
    nd_battery_provide_bus_fd(fd, bus);
    return true;
}

/* ------------------------------------------------------------------ *
 * Probing, and asking again
 * ------------------------------------------------------------------ */

bool nd_battery_errno_means_absent(int err)
{
    switch (err) {
    /* The node is not there: no i2c-dev, no adapter, or an adapter that has
     * not registered yet. */
    case ENOENT:
    case ENODEV:
    case ENXIO:
        return true;
    default:
        /* EACCES above all. The node EXISTS -- devtmpfs made it the moment
         * the kernel registered the adapter -- and something refused us. That
         * is a phone with a fuel gauge it cannot read, and calling it
         * "hardware not found" is how a flat battery became a surprise. */
        return false;
    }
}

/* The verdict, in one place: remember it, decide whether it is worth asking
 * again, and say it out loud EXACTLY ONCE per distinct reason.
 *
 * The once-per-reason rule is what makes the retry affordable. This runs
 * every two seconds for the first half-minute of a bad boot, and a serial
 * console that prints the same three lines fifteen times has hidden whatever
 * else went wrong beside it -- which on this phone is the keypad, on the same
 * bus, in the same second. */
static void settle(nd_battery *b, nd_battery_source src, const char *why, int err)
{
    bool changed = (src != b->source) || strcmp(why, b->why) != 0;

    b->source = src;
    b->hardware = (src == ND_BATT_SRC_LIVE);
    (void)nd_strlcpy(b->why, why, sizeof b->why);
    /* A gauge that has never answered is retried only when the kernel's
     * refusal was one a later attempt could get past. That table lives in
     * nd_input (nd_keypad.h) and is deliberately shared: the keypad loses the
     * identical race, against the identical errnos, on the identical bus. */
    b->retry_possible = (src != ND_BATT_SRC_LIVE) && nd_input_errno_is_transient(err);

    if (!changed)
        return;

    switch (src) {
    case ND_BATT_SRC_LIVE:
        return; /* the caller logs the two lines that name the chip */
    case ND_BATT_SRC_SIM:
        /* The node is genuinely absent. This is the QEMU and dev-board case
         * and the message is still the reassuring one, because here it is
         * true. */
        nd_log(ND_LOG_BATT, "HARDWARE NOT FOUND: Running in Simulation Mode (%s).", why);
        nd_log(ND_LOG_BATT, "This battery gauge is a stub for the QEMU dev environment.");
        nd_log(ND_LOG_BATT, "Simulated VCELL=%.2f V (override: %s env var or %s).",
               ND_SIM_DEFAULT_VCELL, ND_SIM_ENV_VAR, ND_BATT_SIM_FILE);
        return;
    case ND_BATT_SRC_UNREADABLE:
        /* And this is the one that used to print the paragraph above on a
         * phone that has a fuel gauge soldered to it. Error level, no mention
         * of QEMU, and it says what it will do next. */
        nd_log_err(ND_LOG_BATT, "FUEL GAUGE UNREADABLE: %s", why);
        nd_log_err(ND_LOG_BATT,
                   "The gauge is present and cannot be read: no voltage is reported "
                   "at all rather than a plausible one, and the low-battery warning "
                   "and the %.2f V shutdown are unavailable until it answers.",
                   ND_SHUTDOWN_V);
        return;
    }
}

/* One attempt at the gauge. Logs nothing itself -- settle() decides, because
 * unlike the original this now runs over and over. */
static void probe_once(nd_battery *b)
{
    char dev[64];
    char resolved[ND_PATH_MAX];
    char why[sizeof b->why];
    uint16_t version = 0u;
    uint16_t raw_v = 0u;
    bool owned = true;
    int fd;
    int err;

    (void)nd_snprintf(dev, sizeof dev, "/dev/i2c-%d", b->bus);

    if (nd_path_resolve(resolved, sizeof resolved, dev) != ND_OK) {
        /* Not an errno at all: the bus number produced a name too long for a
         * path. Nothing a later attempt changes, so errno 0 -- which
         * nd_input_errno_is_transient() answers false for -- disarms the
         * retry. */
        settle(b, ND_BATT_SRC_SIM, "device path too long", 0);
        return;
    }

    /* The root-phase descriptor if this process kept one, otherwise our own
     * open. Note it is NOT a dup of the keypad's: I2C_SLAVE is per-file-
     * description state, so a shared description would have the expander and
     * the gauge overwriting each other's target address. */
    if (g_bus_fd >= 0 && g_bus_fd_bus == b->bus) {
        fd = g_bus_fd;
        owned = false;
    } else {
        /* O_CLOEXEC because the core forks and execs an app per launch and the
         * gauge handle has no business crossing that boundary. Python 3 makes
         * every os.open() non-inheritable for the same reason. */
        fd = open(resolved, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            err = errno;
            (void)nd_snprintf(why, sizeof why, "%s: %s", dev, strerror(err));
            settle(b, nd_battery_errno_means_absent(err) ? ND_BATT_SRC_SIM : ND_BATT_SRC_UNREADABLE,
                   why, err);
            return;
        }
    }

    if (ioctl(fd, ND_I2C_SLAVE, (unsigned long)b->addr) != 0) {
        err = errno;
        (void)nd_snprintf(why, sizeof why, "%s: I2C_SLAVE 0x%02X: %s", dev, b->addr, strerror(err));
        if (owned)
            (void)close(fd);
        /* Past open() the node demonstrably exists, so every remaining
         * failure is UNREADABLE by construction -- there is no errno here
         * that can mean "no gauge in this phone". */
        settle(b, ND_BATT_SRC_UNREADABLE, why, err);
        return;
    }
    if (read16(fd, ND_REG_VERSION, &version) != 0 || read16(fd, ND_REG_VCELL, &raw_v) != 0) {
        err = errno;
        (void)nd_snprintf(why, sizeof why, "%s: register read: %s", dev, strerror(err));
        if (owned)
            (void)close(fd);
        settle(b, ND_BATT_SRC_UNREADABLE, why, err);
        return;
    }
    /* An all-zero or all-ones read is the bus floating, not a battery. */
    if (raw_v == 0x0000u || raw_v == 0xFFFFu) {
        (void)nd_snprintf(why, sizeof why, "%s: implausible VCELL read 0x%04X", dev, raw_v);
        if (owned)
            (void)close(fd);
        /* EIO rather than 0: a floating bus is exactly the transient the
         * retry exists for -- an expander or a gauge whose rail is still
         * rising reads 0xFFFF for a few tens of milliseconds. */
        settle(b, ND_BATT_SRC_UNREADABLE, why, EIO);
        return;
    }

    b->fd = fd;
    b->owns_fd = owned;
    b->version = version;
    b->read_error_streak = 0;
    settle(b, ND_BATT_SRC_LIVE, "", 0);
    nd_log(ND_LOG_BATT, "MAX1704x fuel gauge @ 0x%02X on %s (VERSION=0x%04X).", b->addr, dev,
           version);
    nd_log(ND_LOG_BATT, "Using REAL battery gauge: VCELL=%.3f V.", (double)raw_v * ND_VCELL_LSB);
}

/* When to ask again. Fast while the boot race could still resolve, then slow
 * for ever -- see the block above ND_BATT_REPROBE_S in nd_battery.h. */
static void schedule_next_probe(nd_battery *b, double now)
{
    if (b->probe_tries < ND_BATT_REPROBE_FAST_TRIES) {
        b->next_probe = now + ND_BATT_REPROBE_S;
        return;
    }
    if (b->probe_tries == ND_BATT_REPROBE_FAST_TRIES)
        nd_log_err(ND_LOG_BATT, "Fuel gauge has not answered in %.0fs; still trying, every %.0fs.",
                   ND_BATT_REPROBE_S * (double)ND_BATT_REPROBE_FAST_TRIES, ND_BATT_REPROBE_SLOW_S);
    b->next_probe = now + ND_BATT_REPROBE_SLOW_S;
}

static void probe_hardware(nd_battery *b, double now)
{
    b->probe_tries++;
    probe_once(b);
    if (b->source == ND_BATT_SRC_LIVE) {
        if (b->probe_tries > 1)
            nd_log(ND_LOG_BATT, "Fuel gauge recovered on attempt %d.", b->probe_tries);
        return;
    }
    schedule_next_probe(b, now);
}

nd_err nd_battery_open(nd_battery **out, int bus, int addr)
{
    nd_battery *b;

    if (out == NULL)
        return ND_ERR_INVAL;
    *out = NULL;

    nd_log(ND_LOG_BATT, "Initializing BatteryService...");

    b = calloc(1u, sizeof *b);
    if (b == NULL)
        return ND_ERR_NOMEM;

    /* Whichever of the two the caller left as -1 comes from settings; the
     * other keeps the caller's value, as in `bus = cfg_bus if bus is None`. */
    if (bus < 0 || addr < 0) {
        int cfg_bus = ND_BATT_DEFAULT_I2C_BUS;
        int cfg_addr = ND_BATT_DEFAULT_I2C_ADDR;

        config_from_settings(&cfg_bus, &cfg_addr);
        if (bus < 0)
            bus = cfg_bus;
        if (addr < 0)
            addr = cfg_addr;
    }

    b->bus = bus;
    b->addr = addr;
    b->fd = -1;
    b->owns_fd = false;
    /* 3 matches the pre-0.2.4a static gauge, so the home screen never shows an
     * empty battery in the window before the first successful read. */
    b->level = 3;
    b->low_armed = true;
    b->crit_armed = true;

    probe_hardware(b, nd_time_monotonic());

    /* Seed the gauge immediately so the first home-screen frame is real. */
    (void)nd_battery_poll(b, true);

    *out = b;
    return ND_OK;
}

void nd_battery_close(nd_battery *b)
{
    if (b == NULL)
        return;
    /* Not the descriptor nd_battery_provide_bus_fd() lent us: it belongs to
     * the process, outlives every gauge built over it, and closing it here
     * would mean a gauge reopened after a teardown had to win the udev race
     * all over again. */
    if (b->fd >= 0 && b->owns_fd)
        (void)close(b->fd);
    free(b);
}

/* ------------------------------------------------------------------ *
 * Reading a voltage
 * ------------------------------------------------------------------ */

/* Python's float(str) on the stripped contents. Accepts what strtod accepts,
 * which is what float() accepts for everything this file will ever see. */
static bool parse_double(const char *text, double *out)
{
    char *end = NULL;
    double value;

    if (text[0] == '\0')
        return false;

    errno = 0;
    value = strtod(text, &end);
    if (errno == ERANGE || end == text)
        return false;

    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r' || *end == '\f' ||
           *end == '\v')
        end++;
    if (*end != '\0')
        return false;

    *out = value;
    return true;
}

/* Has somebody NAMED a voltage? The file wins over the environment variable,
 * so a running phone can be dropped into a low-battery state from the serial
 * console without a restart.
 *
 * Split out from read_vcell_sim() because the two callers want different
 * things from it. Simulation Mode wants a voltage whatever happens, and falls
 * back to 3.85 V. An UNREADABLE gauge wants only the EXPLICIT answer: a
 * developer who wrote a voltage into the sim file is driving the state
 * machine on purpose and must keep working, but nobody may be handed 3.85 V
 * for a gauge that is sitting right there refusing to be read. */
static bool read_vcell_override(double *out)
{
    char resolved[ND_PATH_MAX];
    char raw[64];
    const char *env;
    double value;
    FILE *f;

    if (nd_path_resolve(resolved, sizeof resolved, ND_BATT_SIM_FILE) == ND_OK) {
        f = fopen(resolved, "r");
        if (f != NULL) {
            size_t n = fread(raw, 1u, sizeof raw - 1u, f);

            raw[n] = '\0';
            (void)fclose(f);
            if (parse_double(raw, &value)) {
                *out = value;
                return true;
            }
        }
    }

    env = getenv(ND_SIM_ENV_VAR);
    if (env != NULL && parse_double(env, &value)) {
        *out = value;
        return true;
    }
    return false;
}

static double read_vcell_sim(void)
{
    double value;

    return read_vcell_override(&value) ? value : ND_SIM_DEFAULT_VCELL;
}

/* A gauge that WAS answering has stopped. Close it, put it back into
 * UNREADABLE and re-arm the probe.
 *
 * Without this a gauge that fell off the bus kept `hardware` true for ever:
 * the meter went on showing the last level it had managed to read, with no
 * "?" and nothing on the console after the first line, which is the silent
 * half of the same bug the constructor had. It is the same decision, for the
 * same reason, that nd_input makes at ND_MATRIX_DEAD_SCANS on this bus. */
static void gauge_lost(nd_battery *b, const char *what, double now)
{
    char why[sizeof b->why];

    (void)nd_snprintf(why, sizeof why, "/dev/i2c-%d: the gauge stopped answering (%s)", b->bus,
                      what);
    if (b->fd >= 0 && b->owns_fd)
        (void)close(b->fd);
    b->fd = -1;
    b->owns_fd = false;
    b->read_error_streak = 0;
    /* A gauge that answered once is always worth asking again, whatever the
     * last errno was -- it demonstrably exists. */
    settle(b, ND_BATT_SRC_UNREADABLE, why, EIO);
    /* A fresh budget: this is not the boot race the fast phase was written
     * for, so it gets its own half-minute of trying to get the gauge back. */
    b->probe_tries = 0;
    schedule_next_probe(b, now);
}

static bool read_vcell(nd_battery *b, double *out, double now)
{
    uint16_t raw = 0u;
    char why[64];

    if (b->source == ND_BATT_SRC_SIM) {
        *out = read_vcell_sim();
        return true;
    }

    if (b->source == ND_BATT_SRC_UNREADABLE) {
        /* NOT read_vcell_sim(). This is the whole point of the state: a phone
         * that cannot read its own battery reports NOTHING -- the meter draws
         * "?" and nd_battery_vcell() answers false -- rather than the 3.85 V
         * that sits above every threshold in this file and quietly disables
         * the low-battery warning and the protective shutdown together. An
         * explicit override still wins, because that is a developer driving
         * the state machine deliberately. */
        return read_vcell_override(out);
    }

    if (read16(b->fd, ND_REG_VCELL, &raw) != 0)
        (void)nd_snprintf(why, sizeof why, "%s", strerror(errno));
    else if (raw == 0x0000u || raw == 0xFFFFu)
        (void)nd_snprintf(why, sizeof why, "implausible VCELL read 0x%04X", raw);
    else
        why[0] = '\0';

    if (why[0] != '\0') {
        b->read_error_streak++;
        /* Only the first failure is logged. A gauge that has fallen off the
         * bus fails twice a second, and a serial console full of that hides
         * whatever else went wrong at the same moment. */
        if (b->read_error_streak == 1)
            nd_log(ND_LOG_BATT, "VCELL read failed: %s", why);
        if (b->read_error_streak >= ND_BATT_DEAD_READS)
            gauge_lost(b, why, now);
        return false;
    }

    if (b->read_error_streak != 0) {
        nd_log(ND_LOG_BATT, "VCELL reads recovered after %d failures.", b->read_error_streak);
        b->read_error_streak = 0;
    }

    *out = (double)raw * ND_VCELL_LSB;
    return true;
}

/* ------------------------------------------------------------------ *
 * The state machine
 * ------------------------------------------------------------------ */

static void samples_append(nd_battery *b, double v)
{
    if (b->sample_count < ND_BATT_SMOOTH_WINDOW) {
        b->samples[(b->sample_head + b->sample_count) % ND_BATT_SMOOTH_WINDOW] = v;
        b->sample_count++;
    } else {
        /* Full: the new value lands where the oldest was and the head steps
         * on, which leaves the ring in oldest-to-newest order again. */
        b->samples[b->sample_head] = v;
        b->sample_head = (b->sample_head + 1u) % ND_BATT_SMOOTH_WINDOW;
    }
}

/* sum(deque) / len(deque), added oldest-first from a zero accumulator. See the
 * file header: the order is load-bearing. */
static double samples_mean(const nd_battery *b)
{
    double sum = 0.0;
    size_t i;

    for (i = 0u; i < b->sample_count; i++)
        sum += b->samples[(b->sample_head + i) % ND_BATT_SMOOTH_WINDOW];

    return sum / (double)b->sample_count;
}

static int32_t level_for(double v)
{
    int32_t level = 0;
    size_t i;

    for (i = 0u; i < ND_ARRAY_LEN(ND_LEVEL_THRESHOLDS); i++) {
        if (v >= ND_LEVEL_THRESHOLDS[i])
            level++;
    }
    return level;
}

const char *nd_battery_poll(nd_battery *b, bool force)
{
    double now;
    double vcell;
    double v;

    if (b == NULL)
        return NULL;

    now = nd_time_monotonic();
    if (!force && (now - b->last_poll) < ND_BATT_POLL_INTERVAL_S)
        return NULL;
    b->last_poll = now;

    /* ============ ASK AGAIN FOR A GAUGE THAT IS NOT THERE ============
     *
     * This is the only clock the service has, and it is the one the status
     * bar already turns: nd_ui_read_keypress() polls the battery on every
     * key wait, i.e. several times a second on any screen. Putting the
     * reprobe here rather than in a thread of its own is what makes "the
     * gauge came back" cost nothing on a phone where it never went away.
     *
     * Before the read, so a probe that succeeds this tick produces a real
     * voltage on this tick instead of on the next one. */
    if (b->source != ND_BATT_SRC_LIVE && b->retry_possible && now >= b->next_probe)
        probe_hardware(b, now);

    /* A transient read failure keeps the last known state rather than
     * pretending the pack is flat. */
    if (!read_vcell(b, &vcell, now))
        return NULL;

    samples_append(b, vcell);
    v = samples_mean(b);
    b->smoothed = v;
    b->have_smoothed = true;
    b->level = level_for(v);

    /* Recovery (charger attached) re-arms the one-shot warnings and drops any
     * not-yet-shown warning that no longer applies. */
    if (!b->low_armed && v > ND_LOW_WARN_V + ND_REARM_HYSTERESIS_V) {
        b->low_armed = true;
        if (b->pending == BATT_WARN_LOW)
            b->pending = BATT_WARN_NONE;
    }
    if (!b->crit_armed && v > ND_CRITICAL_WARN_V + ND_REARM_HYSTERESIS_V) {
        b->crit_armed = true;
        if (b->pending == BATT_WARN_CRITICAL)
            b->pending = BATT_WARN_NONE;
    }

    if (v <= ND_SHUTDOWN_V) {
        b->shutdown_count++;
        /* Returns BEFORE the latch block, so a poll that asks for shutdown
         * never also latches a warning. Port the ordering, not just the
         * conditions. */
        if (b->shutdown_count >= ND_SHUTDOWN_CONFIRM_SAMPLES)
            return BATT_SHUTDOWN;
    } else {
        b->shutdown_count = 0;
    }

    if (v <= ND_CRITICAL_WARN_V) {
        if (b->crit_armed) {
            b->crit_armed = false;
            b->low_armed = false; /* don't follow up with the milder warning */
            b->pending = BATT_WARN_CRITICAL;
        }
    } else if (v <= ND_LOW_WARN_V) {
        if (b->low_armed) {
            b->low_armed = false;
            if (b->pending != BATT_WARN_CRITICAL)
                b->pending = BATT_WARN_LOW;
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ *
 * Readouts
 * ------------------------------------------------------------------ */

int32_t nd_battery_level(const nd_battery *b)
{
    return b != NULL ? b->level : 3;
}

bool nd_battery_vcell(const nd_battery *b, double *out)
{
    if (b == NULL || !b->have_smoothed)
        return false;
    if (out != NULL)
        *out = b->smoothed;
    return true;
}

const char *nd_battery_take_pending_warning(nd_battery *b)
{
    batt_warning warning;

    if (b == NULL)
        return NULL;

    warning = b->pending;
    b->pending = BATT_WARN_NONE;

    switch (warning) {
    case BATT_WARN_LOW:
        return ND_BATT_WARN_LOW;
    case BATT_WARN_CRITICAL:
        return ND_BATT_WARN_CRITICAL;
    case BATT_WARN_NONE:
        break;
    }
    return NULL;
}

bool nd_battery_has_hardware(const nd_battery *b)
{
    return b != NULL && b->hardware;
}

nd_battery_source nd_battery_source_of(const nd_battery *b)
{
    return b != NULL ? b->source : ND_BATT_SRC_SIM;
}

const char *nd_battery_fault(const nd_battery *b)
{
    return b != NULL ? b->why : "";
}

bool nd_battery_debug_snapshot(nd_battery *b, nd_battery_snap *out)
{
    uint16_t raw_vcell = 0u;
    uint16_t raw_soc = 0u;
    uint16_t raw_crate = 0u;
    uint16_t raw_config = 0u;

    if (b == NULL || out == NULL)
        return false;

    memset(out, 0, sizeof *out);
    out->crate = NAN;
    /* These four are plain attributes of the service object, not register
     * reads, and debug_snapshot() puts them in the dict BEFORE the try:. So
     * they are filled in first and they survive BOTH false returns -- which
     * is what keeps "i2c-3 @ 0x36" on screen under an ERROR row. The Python
     * reaches that state by having simulate_status() set `hardware = True` on
     * a service whose fd is still None; the C cannot patch a field, so the
     * app's own gate decides and this function always answers with what it
     * knows. */
    out->bus = b->bus;
    out->addr = b->addr;
    out->ic_version = (int)b->version;
    out->level = b->level;

    if (!b->hardware)
        return false; /* `if not self.hardware: return None` */

    /* Four fresh reads, in this order. The FuelGauge app shows them side by
     * side and a reordering makes the CRATE it prints belong to a different
     * instant than the VCELL beside it. */
    if (read16(b->fd, ND_REG_VCELL, &raw_vcell) != 0 || read16(b->fd, ND_REG_SOC, &raw_soc) != 0 ||
        read16(b->fd, ND_REG_CRATE, &raw_crate) != 0 ||
        read16(b->fd, ND_REG_CONFIG, &raw_config) != 0)
        return false;

    out->vcell = (double)raw_vcell * ND_VCELL_LSB;
    out->soc_percent = (double)raw_soc / 256.0;
    /* 0xFFFF marks a 17043/44, which has no CRATE register at all. */
    out->crate = raw_crate == 0xFFFFu ? NAN : (double)signed16(raw_crate) * ND_CRATE_LSB;
    /* The five the FuelGauge app prints and cannot reconstruct; see
     * nd_battery.h. raw_crate is deliberately NOT among them -- the app shows
     * only the scaled %/hr, and NaN already carries "this part has none". */
    out->raw_vcell = raw_vcell;
    out->raw_soc = raw_soc;
    out->raw_config = raw_config;
    out->bus = b->bus;
    out->addr = b->addr;
    return true;
}

bool nd_battery_quickstart(nd_battery *b)
{
    if (b == NULL || !b->hardware)
        return false;

    if (write16(b->fd, ND_REG_MODE, ND_QUICKSTART_MODE) != 0) {
        nd_log(ND_LOG_BATT, "Quick-start failed: %s", strerror(errno));
        return false;
    }
    return true;
}

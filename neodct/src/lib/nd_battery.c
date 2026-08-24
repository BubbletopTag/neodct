/* nd_battery.c -- the MAX1704x fuel gauge, with a simulation fallback.
 *
 * Port of System/core/BatteryService/__init__.py. Reads VCELL from a
 * MAX17043/44/48/49 at 0x36 on /dev/i2c-3 using the same raw write-then-read
 * pattern as System/hw/pcf8575_keypad.py and engineering/tools/max1704x_watch.py.
 * If the gauge cannot be probed at construction (QEMU, or hardware without the
 * battery board) the service runs in Simulation Mode, exactly as ModemService
 * does.
 *
 * Simulation Mode reports a fixed 3.85 V. For testing the warning and shutdown
 * flow in QEMU, override it with NEODCT_BATT_SIM_VCELL, or live at runtime by
 * writing a voltage to /tmp/neodct_sim_vcell (`echo 3.30 > /tmp/neodct_sim_vcell`
 * from the serial console).
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
    int fd; /* -1 in Simulation Mode; open for the process lifetime otherwise */
    bool hardware;
    uint16_t version;

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

static void probe_hardware(nd_battery *b)
{
    char dev[64];
    char resolved[ND_PATH_MAX];
    char why[80];
    uint16_t version = 0u;
    uint16_t raw_v = 0u;
    int fd;

    (void)nd_snprintf(dev, sizeof dev, "/dev/i2c-%d", b->bus);

    if (nd_path_resolve(resolved, sizeof resolved, dev) != ND_OK) {
        (void)nd_snprintf(why, sizeof why, "device path too long");
        goto simulate;
    }

    /* O_CLOEXEC because the core forks and execs an app per launch and the
     * gauge handle has no business crossing that boundary. Python 3 makes
     * every os.open() non-inheritable for the same reason. */
    fd = open(resolved, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        (void)nd_snprintf(why, sizeof why, "%s: %s", dev, strerror(errno));
        goto simulate;
    }

    if (ioctl(fd, ND_I2C_SLAVE, (unsigned long)b->addr) != 0) {
        (void)nd_snprintf(why, sizeof why, "%s: I2C_SLAVE 0x%02X: %s", dev, b->addr,
                          strerror(errno));
        (void)close(fd);
        goto simulate;
    }
    if (read16(fd, ND_REG_VERSION, &version) != 0 || read16(fd, ND_REG_VCELL, &raw_v) != 0) {
        (void)nd_snprintf(why, sizeof why, "%s: register read: %s", dev, strerror(errno));
        (void)close(fd);
        goto simulate;
    }
    /* An all-zero or all-ones read is the bus floating, not a battery. */
    if (raw_v == 0x0000u || raw_v == 0xFFFFu) {
        (void)nd_snprintf(why, sizeof why, "%s: implausible VCELL read 0x%04X", dev, raw_v);
        (void)close(fd);
        goto simulate;
    }

    b->fd = fd;
    b->hardware = true;
    b->version = version;
    nd_log(ND_LOG_BATT, "MAX1704x fuel gauge @ 0x%02X on %s (VERSION=0x%04X).", b->addr, dev,
           version);
    nd_log(ND_LOG_BATT, "Using REAL battery gauge: VCELL=%.3f V.", (double)raw_v * ND_VCELL_LSB);
    return;

simulate:
    nd_log(ND_LOG_BATT, "HARDWARE NOT FOUND: Running in Simulation Mode (%s).", why);
    nd_log(ND_LOG_BATT, "This battery gauge is a stub for the QEMU dev environment.");
    nd_log(ND_LOG_BATT, "Simulated VCELL=%.2f V (override: %s env var or %s).",
           ND_SIM_DEFAULT_VCELL, ND_SIM_ENV_VAR, ND_BATT_SIM_FILE);
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
    /* 3 matches the pre-0.2.4a static gauge, so the home screen never shows an
     * empty battery in the window before the first successful read. */
    b->level = 3;
    b->low_armed = true;
    b->crit_armed = true;

    probe_hardware(b);

    /* Seed the gauge immediately so the first home-screen frame is real. */
    (void)nd_battery_poll(b, true);

    *out = b;
    return ND_OK;
}

void nd_battery_close(nd_battery *b)
{
    if (b == NULL)
        return;
    if (b->fd >= 0)
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

/* The file wins over the environment variable, so a running phone can be
 * dropped into a low-battery state from the serial console without a restart. */
static double read_vcell_sim(void)
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
            if (parse_double(raw, &value))
                return value;
        }
    }

    env = getenv(ND_SIM_ENV_VAR);
    if (env != NULL && parse_double(env, &value))
        return value;

    return ND_SIM_DEFAULT_VCELL;
}

static bool read_vcell(nd_battery *b, double *out)
{
    uint16_t raw = 0u;
    char why[64];

    if (!b->hardware) {
        *out = read_vcell_sim();
        return true;
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

    /* A transient read failure keeps the last known state rather than
     * pretending the pack is flat. */
    if (!read_vcell(b, &vcell))
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

bool nd_battery_debug_snapshot(nd_battery *b, nd_battery_snap *out)
{
    uint16_t raw_vcell = 0u;
    uint16_t raw_soc = 0u;
    uint16_t raw_crate = 0u;
    uint16_t raw_config = 0u;

    if (b == NULL || out == NULL || !b->hardware)
        return false;

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
    out->level = b->level;
    out->ic_version = (int)b->version;
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

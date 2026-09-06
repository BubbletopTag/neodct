/* nd_battery.h -- the MAX17048-family fuel gauge on i2c.
 *
 * Reads a cell voltage, smooths it over five samples, turns it into a 0..4
 * bar level, and raises a warning or a shutdown when it gets low. With no BUS
 * NODE AT ALL it simulates 3.85 V, which is what makes the whole UI runnable
 * on a desktop -- but only then: a gauge that is present and unreadable
 * reports no voltage and says so. See nd_battery_source.
 *
 * The hysteresis matters: a warning re-arms only after the voltage has climbed
 * 0.05 V back above its threshold, and a shutdown needs three consecutive
 * confirming samples. Without both, a phone under load flickers between
 * "fine" and "shutting down" as the transmitter keys up.
 */

#ifndef ND_BATTERY_H_INCLUDED
#define ND_BATTERY_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ND_I2C_SLAVE                0x0703 /* ioctl request */
#define ND_REG_VCELL                0x02
#define ND_REG_SOC                  0x04
#define ND_REG_MODE                 0x06
#define ND_REG_VERSION              0x08
#define ND_REG_CONFIG               0x0C
#define ND_REG_CRATE                0x16      /* 17048/49 only */
#define ND_VCELL_LSB                78.125e-6 /* volts per LSB, i.e. 1/12800 */
#define ND_CRATE_LSB                0.208     /* %/hr per LSB, signed        */
#define ND_QUICKSTART_MODE          0x4000
#define ND_LOW_WARN_V               3.45
#define ND_CRITICAL_WARN_V          3.25
#define ND_SHUTDOWN_V               3.20
#define ND_REARM_HYSTERESIS_V       0.05
#define ND_SHUTDOWN_CONFIRM_SAMPLES 3
#define ND_BATT_POLL_INTERVAL_S     2.0
#define ND_BATT_SMOOTH_WINDOW       5
#define ND_SIM_DEFAULT_VCELL        3.85
#define ND_SIM_ENV_VAR              "NEODCT_BATT_SIM_VCELL"
#define ND_BATT_SIM_FILE            "/tmp/neodct_sim_vcell"
#define ND_BATT_DEFAULT_I2C_BUS     3
#define ND_BATT_DEFAULT_I2C_ADDR    0x36

/* ============ HOW OFTEN A GAUGE THAT IS NOT THERE IS ASKED AGAIN ============
 *
 * The gauge used to be probed exactly once, from nd_battery_open(), and every
 * failure latched Simulation Mode for the life of the process. On a desktop
 * that is correct and permanent. On the phone it is a race: nd_battery_open()
 * runs inside nd_ui_init(), which runs after nd-core has stopped being root,
 * and /dev/i2c-3 is root:root 0600 from devtmpfs until udev applies
 * `SUBSYSTEM=="i2c-dev", GROUP="i2c"`. A gauge that was unreachable for the
 * first two seconds of a boot was unreachable for the rest of the day.
 *
 * So it is asked again, from the poll the status bar already drives. FAST for
 * the first half-minute, because everything that can lose that race resolves
 * inside it -- the coldplug measures about 2.7 s, an expander's rail tens of
 * milliseconds -- and then SLOW for ever, which costs one open(2) every thirty
 * seconds and is what makes a battery board plugged in later start working.
 *
 * The retry is only armed when the errno was one a later attempt could get
 * past; nd_input_errno_is_transient() (nd_keypad.h) is that policy table and
 * this file deliberately reuses it rather than writing a second copy. */
#define ND_BATT_REPROBE_S          2.0
#define ND_BATT_REPROBE_FAST_TRIES 15
#define ND_BATT_REPROBE_SLOW_S     30.0

/* Consecutive failed VCELL reads after which a gauge that HAD been answering
 * counts as gone rather than glitching, so that the "?" comes back on the
 * meter and the probe is re-armed. At the 2 s poll interval this is half a
 * minute of a gauge that is not answering, which no bus contention explains.
 * The same decision, for the same reason, as ND_MATRIX_DEAD_SCANS. */
#define ND_BATT_DEAD_READS 15

/* ============ THREE SOURCES, BECAUSE TWO WAS A LIE ============
 *
 * "hardware" and "not hardware" were the only two answers this service had,
 * and both ways of not having hardware came out as Simulation Mode -- with a
 * fixed, plausible 3.85 V behind it. That is the same lie nd_modem.h grew
 * ND_MODEM_LINK_UNREACHABLE to stop telling, on the same bus, on the same
 * phone, in the same second: a device that is PRESENT and could not be opened
 * reported as a device that is ABSENT.
 *
 * It is worse here than in the modem, because 3.85 V is above every threshold
 * in this file. A phone that cannot read its own battery does not merely draw
 * the wrong gauge -- it silently switches off the low-battery warning and the
 * <= 3.20 V protective shutdown, and then runs its cell flat.
 *
 *   ND_BATT_SRC_SIM         There is no bus node. QEMU, or a board with no
 *                           battery daughterboard. Simulation is the TRUTH
 *                           here, 3.85 V is the designed answer, and the
 *                           /tmp/neodct_sim_vcell hook drives the whole
 *                           warning and shutdown flow on a desktop.
 *
 *   ND_BATT_SRC_LIVE        The gauge answered.
 *
 *   ND_BATT_SRC_UNREADABLE  The node exists and the gauge could not be read:
 *                           EACCES because the udev grant has not landed, a
 *                           refused I2C_SLAVE, a NAK, a floating bus. There
 *                           IS a gauge in this phone and this service cannot
 *                           reach it. It reports NO voltage at all rather
 *                           than a comfortable one -- the meter shows "?" --
 *                           and it keeps trying. */
typedef enum { ND_BATT_SRC_SIM = 0, ND_BATT_SRC_LIVE, ND_BATT_SRC_UNREADABLE } nd_battery_source;

/* Volts at which the meter steps up a bar. */
extern const double ND_LEVEL_THRESHOLDS[4]; /* { 3.35, 3.55, 3.75, 3.95 } */

/* What nd_battery_take_pending_warning() can hand back. */
#define ND_BATT_WARN_LOW      "low"
#define ND_BATT_WARN_CRITICAL "critical"

/* ============ THE FIVE APPENDED FIELDS ============
 *
 * The first five members are the frozen set. The five below them were added
 * when the engineering FuelGauge app (id 9004) was ported, because that app
 * -- the only caller this struct exists for -- prints all ten and can
 * reconstruct none of the five:
 *
 *   raw_vcell   drawn as "%.4f V  (0x%04X)" beside the volts. Recovering it
 *   raw_soc     as vcell / ND_VCELL_LSB is a double division by a value that
 *   raw_config  is not a power of two, so it is not guaranteed to land back
 *               on the integer the chip returned; CFG has no float form at
 *               all and cannot be recovered by any arithmetic.
 *   bus, addr   drawn as "i2c-3 @ 0x36" along the bottom. They are settled
 *               inside nd_battery_open() from system.hw.battery_i2c_bus /
 *               _addr, and nothing else exposes them -- an app re-reading
 *               those settings would be guessing that the core opened the
 *               gauge the same way.
 *
 * Appended rather than inserted, so no existing initialiser or field offset
 * moves. See OPEN-QUESTIONS.md B-1.
 */
typedef struct {
    double vcell;
    double soc_percent;
    double crate;  /* %/hr, signed; NaN when the part has no CRATE   */
    int32_t level; /* 0..4                                          */
    int ic_version;
    uint16_t raw_vcell;  /* REG_VCELL  as read                       */
    uint16_t raw_soc;    /* REG_SOC    as read                       */
    uint16_t raw_config; /* REG_CONFIG as read                       */
    int bus;             /* the i2c bus this gauge was opened on     */
    int addr;            /* its 7-bit address                        */
} nd_battery_snap;

typedef struct nd_battery nd_battery;

/* Pass -1 for bus and addr to read them from settings
 * (system.hw.battery_i2c_bus / _addr, the latter parsed base 0 so "0x36"
 * works). Never fails hard: no hardware means simulation mode. */
nd_err nd_battery_open(nd_battery **out, int bus, int addr);
void nd_battery_close(nd_battery *b);

/* The tick. Returns "shutdown" when the phone must power off now, NULL
 * otherwise. force skips the 2 s interval check. The returned string is a
 * literal owned by libneodct. */
const char *nd_battery_poll(nd_battery *b, bool force);

/* 0..4. Starts at 3 before the first successful read, so the home screen never
 * shows an empty battery during boot. */
int32_t nd_battery_level(const nd_battery *b);

/* false means "no reading yet", which is distinct from 0.0 V. */
bool nd_battery_vcell(const nd_battery *b, double *out);

/* ND_BATT_WARN_LOW, ND_BATT_WARN_CRITICAL or NULL. Consuming it re-arms
 * nothing -- re-arming is by voltage hysteresis only. */
const char *nd_battery_take_pending_warning(nd_battery *b);

/* The engineering FuelGauge app's readout. false in simulation mode, which is
 * why that app refuses to run without hardware.
 *
 * *out is zeroed on entry, with crate NaN. A FALSE RETURN STILL LEAVES bus,
 * addr, ic_version and level FILLED IN whenever the gauge claims to be
 * present, because the Python builds those four into the dict before the
 * register reads and hands them back with an "error" key when a read throws --
 * which is what keeps "i2c-3 @ 0x36" on screen under an ERROR row. Use
 * nd_battery_has_hardware() to tell the two false cases apart. */
bool nd_battery_debug_snapshot(nd_battery *b, nd_battery_snap *out);

bool nd_battery_quickstart(nd_battery *b);

/* Read by the home screen to decide whether to draw the "?" label over the
 * battery sprite, and by FuelGauge to decide whether to run at all. */
bool nd_battery_has_hardware(const nd_battery *b);

/* Which of the three above this gauge is in, right now. A NULL gauge is
 * ND_BATT_SRC_SIM: a core with no BatteryService is not a core with a broken
 * one. nd_battery_has_hardware() is exactly "source == ND_BATT_SRC_LIVE" and
 * stays the answer to "may I read registers"; this is the answer to "is
 * something wrong with this phone". */
nd_battery_source nd_battery_source_of(const nd_battery *b);

/* Why the last probe failed, in the kernel's own words ("/dev/i2c-3:
 * Permission denied"). "" when nothing has failed. A string literal or a
 * buffer owned by the gauge; valid until the next poll. For the engineering
 * FuelGauge app's error row and for a log a person greps. */
const char *nd_battery_fault(const nd_battery *b);

/* THE PURE DECISION, exported because it is the whole difference between "no
 * gauge" and "a gauge I cannot reach" and it deserves a test rather than a
 * reading of the code.
 *
 * open(2) on the bus node is the only place the two can be told apart, and it
 * tells them apart by errno alone: the node is missing (or its adapter is)
 * for exactly these three, and for every other failure -- EACCES above all --
 * the node is THERE and something else refused. Anything the phone can
 * produce here except those three is a fault to be reported and retried, not
 * an absence to be simulated. */
bool nd_battery_errno_means_absent(int err);

/* ============ CROSSING THE PRIVILEGE DROP ============
 *
 * The gauge shares /dev/i2c-3 with the keypad matrix, and it loses the same
 * udev race in the same millisecond -- it just loses it more quietly, and it
 * loses it FIRST, so on a bad boot its "/dev/i2c-3: Permission denied" is the
 * earliest line in core.log that proves the grant was late.
 *
 * nd_input solved that by opening the bus while nd-core is still root and
 * keeping the descriptor across the setuid (nd_input_provide_keypad_fd()).
 * This is the same door for the gauge, and it needs its OWN open rather than
 * a dup of the keypad's: I2C_SLAVE is per-file-description state, so a dup
 * would have the gauge and the expander fighting over one address register.
 *
 * nd_battery_open_bus_as_root() is the one line the boot sequence calls, just
 * beside the keypad bring-up and before the drop; it reads the bus number
 * from settings itself so the caller needs to know nothing. Both are no-ops
 * that answer false when there is nothing to open, so a build that never
 * calls them behaves exactly as before -- the probe and its retry still work
 * on their own, they are just exposed to a race this removes.
 *
 * The descriptor is O_CLOEXEC and is opened AFTER the broker has already
 * forked, so no app and no broker child can ever inherit it. If anyone moves
 * the broker fork below this point, that stops being true. */
void nd_battery_provide_bus_fd(int fd, int bus);
bool nd_battery_open_bus_as_root(void);

#ifdef __cplusplus
}
#endif

#endif /* ND_BATTERY_H_INCLUDED */

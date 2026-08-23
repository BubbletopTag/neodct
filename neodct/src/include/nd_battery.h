/* nd_battery.h -- the MAX17048-family fuel gauge on i2c.
 *
 * Reads a cell voltage, smooths it over five samples, turns it into a 0..4
 * bar level, and raises a warning or a shutdown when it gets low. With no
 * hardware it simulates 3.85 V, which is what makes the whole UI runnable on
 * a desktop.
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

/* Volts at which the meter steps up a bar. */
extern const double ND_LEVEL_THRESHOLDS[4]; /* { 3.35, 3.55, 3.75, 3.95 } */

/* What nd_battery_take_pending_warning() can hand back. */
#define ND_BATT_WARN_LOW      "low"
#define ND_BATT_WARN_CRITICAL "critical"

typedef struct {
    double vcell;
    double soc_percent;
    double crate;  /* %/hr, signed; NaN when the part has no CRATE   */
    int32_t level; /* 0..4                                          */
    int ic_version;
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
 * why that app refuses to run without hardware. */
bool nd_battery_debug_snapshot(nd_battery *b, nd_battery_snap *out);

bool nd_battery_quickstart(nd_battery *b);

/* Read by the home screen to decide whether to draw the "?" label over the
 * battery sprite, and by FuelGauge to decide whether to run at all. */
bool nd_battery_has_hardware(const nd_battery *b);

#ifdef __cplusplus
}
#endif

#endif /* ND_BATTERY_H_INCLUDED */

"""BatteryService -- MAX1704x fuel gauge with a QEMU simulation fallback.

Reads VCELL from a MAX17043/44/48/49 at 0x36 using the same raw-ioctl
pattern as System/hw/pcf8575_keypad.py and engineering/tools/max1704x_watch.py.
If the gauge cannot be probed at init (QEMU, or hardware without the battery
board), the service runs in Simulation Mode like ModemService does.

Simulation Mode reports a fixed 3.85 V. For testing the warning/shutdown flow
in QEMU, override the simulated voltage with the NEODCT_BATT_SIM_VCELL env
var, or live at runtime by writing a voltage to /tmp/neodct_sim_vcell
(e.g. `echo 3.30 > /tmp/neodct_sim_vcell` from the serial console).

Voltage policy (per the 0.2.4a bring-up sweep on the bench supply):
  * 3.25-3.35 V is "dead" (gauge segment 0), 3.95-4.10 V is "full" (segment 4)
  * <= 3.45 V latches a LOW BATTERY warning
  * <= 3.25 V latches a BATTERY CRITICALLY LOW warning
  * <= 3.20 V (confirmed over consecutive polls) requests graceful shutdown
"""

import fcntl
import os
import time
from collections import deque

I2C_SLAVE = 0x0703

REG_VCELL = 0x02
REG_SOC = 0x04
REG_MODE = 0x06
REG_VERSION = 0x08
REG_CONFIG = 0x0C
REG_CRATE = 0x16  # MAX17048/49 only; reads garbage/0xFFFF on 17043/44

VCELL_LSB = 78.125e-6  # volts/LSB across the full 16-bit register
CRATE_LSB = 0.208      # %/hr per LSB, signed (17048/49)
QUICKSTART_MODE = 0x4000

# Gauge segment boundaries: below 3.35 V -> 0 (dead), each 0.20 V band adds
# a segment, >= 3.95 V -> 4 (full). Matches bat-0..bat-4 in ui_home.json.
LEVEL_THRESHOLDS = (3.35, 3.55, 3.75, 3.95)

LOW_WARN_V = 3.45
CRITICAL_WARN_V = 3.25
SHUTDOWN_V = 3.20
REARM_HYSTERESIS_V = 0.05    # recovery above a threshold re-arms its warning
SHUTDOWN_CONFIRM_SAMPLES = 3  # consecutive polls at/below SHUTDOWN_V required

POLL_INTERVAL_S = 2.0
SMOOTH_WINDOW = 5  # moving average; rides out load-transient sag

SIM_DEFAULT_VCELL = 3.85
SIM_ENV_VAR = "NEODCT_BATT_SIM_VCELL"
SIM_FILE = "/tmp/neodct_sim_vcell"

DEFAULT_I2C_BUS = 3
DEFAULT_I2C_ADDR = 0x36


class BatteryService:
    def __init__(self, bus=None, addr=None):
        print("[BATT] Initializing BatteryService...")

        if bus is None or addr is None:
            cfg_bus, cfg_addr = self._config_from_settings()
            bus = cfg_bus if bus is None else bus
            addr = cfg_addr if addr is None else addr
        self.bus = bus
        self.addr = addr

        self.fd = None
        self.hardware = False
        self.version = None

        self._samples = deque(maxlen=SMOOTH_WINDOW)
        self._smoothed = None
        self._level = 3  # matches the pre-0.2.4a static gauge until first poll
        self._low_armed = True
        self._crit_armed = True
        self._pending_warning = None
        self._shutdown_count = 0
        self._last_poll = 0.0
        self._read_error_streak = 0

        self._probe_hardware()
        # Seed the gauge immediately so the first home-screen frame is real.
        self.poll(force=True)

    # --- setup -----------------------------------------------------------

    def _config_from_settings(self):
        try:
            from System.core.SettingsStorage import get_setting
            bus = int(str(get_setting("system.hw.battery_i2c_bus", DEFAULT_I2C_BUS)))
            addr = int(str(get_setting("system.hw.battery_i2c_addr", "0x36")), 0)
            return bus, addr
        except Exception as exc:
            print(f"[BATT] Settings unavailable ({exc}); using i2c defaults.")
            return DEFAULT_I2C_BUS, DEFAULT_I2C_ADDR

    def _probe_hardware(self):
        dev = "/dev/i2c-%d" % self.bus
        fd = None
        try:
            fd = os.open(dev, os.O_RDWR)
            fcntl.ioctl(fd, I2C_SLAVE, self.addr)
            version = self._read_reg_fd(fd, REG_VERSION)
            raw_v = self._read_reg_fd(fd, REG_VCELL)
            if raw_v in (0x0000, 0xFFFF):
                raise IOError("implausible VCELL read 0x%04X" % raw_v)
        except Exception as exc:
            if fd is not None:
                try:
                    os.close(fd)
                except Exception:
                    pass
            print(f"[BATT] HARDWARE NOT FOUND: Running in Simulation Mode ({exc}).")
            print("[BATT] This battery gauge is a stub for the QEMU dev environment.")
            print("[BATT] Simulated VCELL=%.2f V (override: %s env var or %s)."
                  % (SIM_DEFAULT_VCELL, SIM_ENV_VAR, SIM_FILE))
            return

        self.fd = fd
        self.hardware = True
        self.version = version
        print("[BATT] MAX1704x fuel gauge @ 0x%02X on %s (VERSION=0x%04X)."
              % (self.addr, dev, version))
        print("[BATT] Using REAL battery gauge: VCELL=%.3f V." % (raw_v * VCELL_LSB))

    # --- register access -------------------------------------------------

    @staticmethod
    def _read_reg_fd(fd, reg):
        os.write(fd, bytes([reg]))
        d = os.read(fd, 2)
        return (d[0] << 8) | d[1]

    @staticmethod
    def _write_reg_fd(fd, reg, val):
        os.write(fd, bytes([reg, (val >> 8) & 0xFF, val & 0xFF]))

    @staticmethod
    def _signed16(v):
        return v - 0x10000 if v & 0x8000 else v

    def _read_vcell(self):
        if not self.hardware:
            return self._read_vcell_sim()
        try:
            raw = self._read_reg_fd(self.fd, REG_VCELL)
            if raw in (0x0000, 0xFFFF):
                raise IOError("implausible VCELL read 0x%04X" % raw)
        except Exception as exc:
            self._read_error_streak += 1
            if self._read_error_streak == 1:
                print(f"[BATT] VCELL read failed: {exc}")
            return None
        if self._read_error_streak:
            print("[BATT] VCELL reads recovered after %d failures." % self._read_error_streak)
            self._read_error_streak = 0
        return raw * VCELL_LSB

    def _read_vcell_sim(self):
        try:
            with open(SIM_FILE) as f:
                return float(f.read().strip())
        except Exception:
            pass
        try:
            return float(os.environ[SIM_ENV_VAR])
        except (KeyError, ValueError):
            return SIM_DEFAULT_VCELL

    # --- state machine ----------------------------------------------------

    def poll(self, force=False):
        """Sample the gauge (rate-limited) and update warning latches.

        Returns "shutdown" once the pack is confirmed at/below SHUTDOWN_V;
        otherwise None. Warnings are latched -- fetch with take_pending_warning().
        """
        now = time.monotonic()
        if not force and (now - self._last_poll) < POLL_INTERVAL_S:
            return None
        self._last_poll = now

        vcell = self._read_vcell()
        if vcell is None:
            return None  # transient read failure: keep the last known state

        self._samples.append(vcell)
        v = sum(self._samples) / len(self._samples)
        self._smoothed = v
        self._level = self._level_for(v)

        # Recovery (charger attached) re-arms the one-shot warnings and
        # drops any not-yet-shown warning that no longer applies.
        if not self._low_armed and v > LOW_WARN_V + REARM_HYSTERESIS_V:
            self._low_armed = True
            if self._pending_warning == "low":
                self._pending_warning = None
        if not self._crit_armed and v > CRITICAL_WARN_V + REARM_HYSTERESIS_V:
            self._crit_armed = True
            if self._pending_warning == "critical":
                self._pending_warning = None

        if v <= SHUTDOWN_V:
            self._shutdown_count += 1
            if self._shutdown_count >= SHUTDOWN_CONFIRM_SAMPLES:
                return "shutdown"
        else:
            self._shutdown_count = 0

        if v <= CRITICAL_WARN_V:
            if self._crit_armed:
                self._crit_armed = False
                self._low_armed = False  # don't follow up with the milder warning
                self._pending_warning = "critical"
        elif v <= LOW_WARN_V:
            if self._low_armed:
                self._low_armed = False
                if self._pending_warning != "critical":
                    self._pending_warning = "low"

        return None

    @staticmethod
    def _level_for(v):
        level = 0
        for threshold in LEVEL_THRESHOLDS:
            if v >= threshold:
                level += 1
        return level

    # --- readouts ----------------------------------------------------------

    def level(self):
        """Current 0..4 gauge segment (bat-0.png .. bat-4.png)."""
        return self._level

    def vcell(self):
        """Smoothed cell voltage in volts, or None before the first good read."""
        return self._smoothed

    def take_pending_warning(self):
        """Pop the latched warning: "low", "critical", or None."""
        warning = self._pending_warning
        self._pending_warning = None
        return warning

    def debug_snapshot(self):
        """Fresh register dump for the FuelGauge engineering app.

        Returns None in Simulation Mode. On a read failure the dict carries
        an "error" key with the reason instead of register values.
        """
        if not self.hardware:
            return None
        snap = {
            "bus": self.bus,
            "addr": self.addr,
            "version": self.version,
            "level": self._level,
            "smoothed_v": self._smoothed,
        }
        try:
            raw_vcell = self._read_reg_fd(self.fd, REG_VCELL)
            raw_soc = self._read_reg_fd(self.fd, REG_SOC)
            raw_crate = self._read_reg_fd(self.fd, REG_CRATE)
            raw_config = self._read_reg_fd(self.fd, REG_CONFIG)
        except Exception as exc:
            snap["error"] = str(exc)
            return snap
        snap.update({
            "raw_vcell": raw_vcell,
            "vcell": raw_vcell * VCELL_LSB,
            "raw_soc": raw_soc,
            "soc": raw_soc / 256.0,
            "raw_crate": raw_crate,
            # 0xFFFF marks a 17043/44 without the CRATE register.
            "crate": None if raw_crate == 0xFFFF else self._signed16(raw_crate) * CRATE_LSB,
            "config": raw_config,
        })
        return snap

    def quickstart(self):
        """Force the gauge to re-seed SOC from VCELL. Hardware only."""
        if not self.hardware:
            return False
        try:
            self._write_reg_fd(self.fd, REG_MODE, QUICKSTART_MODE)
            return True
        except Exception as exc:
            print(f"[BATT] Quick-start failed: {exc}")
            return False

    def close(self):
        if self.fd is not None:
            try:
                os.close(self.fd)
            except Exception:
                pass
            self.fd = None

#!/usr/bin/env python3
"""max1704x_watch.py -- live console dump of a MAX17043/44/48/49 fuel gauge.

Address 0x36 on the bus is the classic MAX1704x family. Pure stdlib
(fcntl/os raw ioctls), no smbus2 dependency -- same pattern as
System/hw/pcf8575_keypad.py so it drops into the NeoDCT tree unchanged.

Usage:
  python3 max1704x_watch.py                    # bus 1, addr 0x36, 4 Hz
  python3 max1704x_watch.py --interval 0.1     # faster spam
  python3 max1704x_watch.py --quickstart       # force SOC re-guess from VCELL first
  python3 max1704x_watch.py --csv > sweep.csv  # machine-readable log

Bench sweep notes:
  * VCELL is the direct ADC reading -- trust this during your 4.0 -> 3.2 V sweep.
  * SOC is the ModelGauge *estimate* and is heavily low-pass filtered; it will
    lag a fast supply sweep by design. --quickstart snaps it to a fresh
    voltage-based first guess (useful once at the start of the test).
  * Share grounds between the bench supply and the Pi.
"""

import argparse
import fcntl
import os
import sys
import time

I2C_SLAVE = 0x0703

# MAX1704x register map (all 16-bit, big-endian on the wire)
REG_VCELL   = 0x02
REG_SOC     = 0x04
REG_MODE    = 0x06
REG_VERSION = 0x08
REG_CONFIG  = 0x0C
REG_CRATE   = 0x16   # MAX17048/49 only; undefined on 17043/44

VCELL_LSB = 78.125e-6   # volts/LSB across full 16 bits
                        # (identical math for 17043: 1.25 mV per 12-bit LSB,
                        #  low nibble reads zero, so raw16 * 78.125 uV holds)
CRATE_LSB = 0.208       # %/hr per LSB, signed


class Max1704x:
    def __init__(self, bus, addr):
        self.fd = os.open("/dev/i2c-%d" % bus, os.O_RDWR)
        fcntl.ioctl(self.fd, I2C_SLAVE, addr)

    def read_reg(self, reg):
        os.write(self.fd, bytes([reg]))
        d = os.read(self.fd, 2)
        return (d[0] << 8) | d[1]

    def write_reg(self, reg, val):
        os.write(self.fd, bytes([reg, (val >> 8) & 0xFF, val & 0xFF]))

    def quickstart(self):
        self.write_reg(REG_MODE, 0x4000)

    def close(self):
        os.close(self.fd)


def signed16(v):
    return v - 0x10000 if v & 0x8000 else v


def main():
    ap = argparse.ArgumentParser(description="MAX1704x live watcher")
    ap.add_argument("--bus", type=int, default=3)
    ap.add_argument("--addr", type=lambda s: int(s, 0), default=0x36)
    ap.add_argument("--interval", type=float, default=0.25,
                    help="seconds between samples (default 0.25)")
    ap.add_argument("--quickstart", action="store_true",
                    help="issue MODE quick-start before sampling")
    ap.add_argument("--csv", action="store_true",
                    help="CSV output instead of human-readable lines")
    args = ap.parse_args()

    gauge = Max1704x(args.bus, args.addr)

    version = gauge.read_reg(REG_VERSION)
    config = gauge.read_reg(REG_CONFIG)
    if not args.csv:
        print("MAX1704x @ 0x%02X bus %d  VERSION=0x%04X  CONFIG=0x%04X"
              % (args.addr, args.bus, version, config))
        print("(CRATE column only meaningful on MAX17048/49; "
              "garbage/0xFFFF means you have a 17043/44)")

    if args.quickstart:
        gauge.quickstart()
        if not args.csv:
            print("quick-start issued, SOC re-seeded from VCELL")
        time.sleep(0.2)

    if args.csv:
        print("t_s,vcell_v,soc_pct,crate_pct_hr,raw_vcell,raw_soc,raw_crate")

    t0 = time.monotonic()
    v_min, v_max = 99.0, 0.0
    prev_v = None

    try:
        while True:
            raw_v = gauge.read_reg(REG_VCELL)
            raw_soc = gauge.read_reg(REG_SOC)
            raw_crate = gauge.read_reg(REG_CRATE)

            vcell = raw_v * VCELL_LSB
            soc = raw_soc / 256.0
            crate = signed16(raw_crate) * CRATE_LSB

            v_min = min(v_min, vcell)
            v_max = max(v_max, vcell)
            t = time.monotonic() - t0

            if args.csv:
                print("%.3f,%.4f,%.2f,%.2f,0x%04X,0x%04X,0x%04X"
                      % (t, vcell, soc, crate, raw_v, raw_soc, raw_crate))
            else:
                dv = "" if prev_v is None else "  dV=%+6.1f mV" % ((vcell - prev_v) * 1000)
                print("t=%7.2fs  VCELL=%.4f V  SOC=%6.2f %%  CRATE=%+7.2f %%/hr%s"
                      % (t, vcell, soc, crate, dv))
            sys.stdout.flush()
            prev_v = vcell

            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        if not args.csv:
            print("\nsweep summary: VCELL min %.4f V, max %.4f V" % (v_min, v_max))
        gauge.close()


if __name__ == "__main__":
    main()
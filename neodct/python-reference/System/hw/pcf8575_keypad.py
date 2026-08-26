"""
pcf8575_keypad.py -- PCF8575 I2C matrix keypad backend for NeoDCT.

Pure stdlib (os + fcntl). No smbus2, no gpiozero. The PCF8575 has no
register/command byte, so plain 2-byte read()/write() on /dev/i2c-N
after an I2C_SLAVE ioctl ARE the correct raw transactions. This ports
to the Buildroot/Luckfox rootfs with zero added Python packages.

Chip model (quasi-bidirectional, no direction registers):
  write 1 -> pin released to weak internal pull-up ("input", reads high)
  write 0 -> pin driven hard low
Scan: drive one row pin low, read all 16 bits; a column bit reading low
means the key at (that row, that column) is pressed.

Standalone test (prints raw scan hits, no NeoDCT UI needed):
  python3 -m System.hw.pcf8575_keypad            # from /NeoDCT
  python3 pcf8575_keypad.py --bus 3 --addr 0x20  # directly
"""

# I love Claude! I don't think I would've ever figured this out without Claude Fable. LMAO.
# Most hardware drivers have a trend of being written in userspace, this makes them pretty hackable for the user so have lots of fun breaking it! ;)

import fcntl
import os
import time

I2C_SLAVE = 0x0703  # linux/i2c-dev.h

DEFAULT_BUS = 3
DEFAULT_ADDR = 0x20
DEFAULT_ROW_PINS = [0, 1, 2, 3]   # expander pins P00-P03
DEFAULT_COL_PINS = [4, 5, 6, 7]   # expander pins P04-P07

# Consecutive empty scans required before a held key is considered
# released. At the ~5 ms poll cadence used by read_key this gives
# roughly 15 ms of release debounce for mushy membrane contacts.
RELEASE_SCANS = 3


class PCF8575:
    """Minimal raw-I2C access to one PCF8575 on /dev/i2c-<bus>."""

    def __init__(self, bus=DEFAULT_BUS, addr=DEFAULT_ADDR):
        self.bus = int(bus)
        self.addr = int(addr)
        self.dev_path = f"/dev/i2c-{self.bus}"
        self.fd = os.open(self.dev_path, os.O_RDWR)
        try:
            fcntl.ioctl(self.fd, I2C_SLAVE, self.addr)
        except OSError:
            os.close(self.fd)
            raise

    def write16(self, value):
        value &= 0xFFFF
        os.write(self.fd, bytes((value & 0xFF, (value >> 8) & 0xFF)))

    def read16(self):
        data = os.read(self.fd, 2)
        if len(data) != 2:
            raise OSError(f"short read from {self.dev_path} (got {len(data)} bytes)")
        return data[0] | (data[1] << 8)

    def close(self):
        if self.fd is not None:
            try:
                # Release every pin to input/pull-up so nothing is left
                # driven low across restarts.
                self.write16(0xFFFF)
            except OSError:
                pass
            os.close(self.fd)
            self.fd = None


class I2CMatrixScanner:
    """
    Edge-detecting matrix scanner over a PCF8575.

    scan_once() returns a (row_idx, col_idx) tuple exactly once per
    physical press, mirroring the semantics of the gpiozero
    MatrixScanner used elsewhere in NeoDCT, plus release debounce.
    """

    def __init__(self, row_pins=None, col_pins=None,
                 bus=DEFAULT_BUS, addr=DEFAULT_ADDR):
        self.row_pins = list(row_pins if row_pins is not None else DEFAULT_ROW_PINS)
        self.col_pins = list(col_pins if col_pins is not None else DEFAULT_COL_PINS)
        self._validate_pins()
        self.chip = PCF8575(bus=bus, addr=addr)
        self.chip.write16(0xFFFF)
        self._held = {}       # (row, col) -> consecutive scans missing
        self._pending = []    # extra new presses queued for later scan_once calls

    def _validate_pins(self):
        seen = set()
        for pin in self.row_pins + self.col_pins:
            pin = int(pin)
            if not 0 <= pin <= 15:
                raise ValueError(f"expander pin {pin} out of range 0-15")
            if pin in seen:
                raise ValueError(f"expander pin {pin} listed twice")
            seen.add(pin)

    def _raw_scan(self):
        """One full pass. Returns the set of every pressed (row_idx, col_idx).

        Scanning the whole matrix (instead of stopping at the first hit)
        gives key rollover: pressing a second key while one is still held
        must be seen, or games miss direction changes.
        """
        found = set()
        for row_idx, row_pin in enumerate(self.row_pins):
            self.chip.write16(0xFFFF & ~(1 << row_pin))
            # The I2C transactions themselves take ~0.5 ms at 100 kHz;
            # a short settle guards against line capacitance.
            time.sleep(0.0005)
            value = self.chip.read16()
            for col_idx, col_pin in enumerate(self.col_pins):
                if not (value >> col_pin) & 1:
                    found.add((row_idx, col_idx))
        self.chip.write16(0xFFFF)
        return found

    def scan_once(self):
        current = self._raw_scan()

        # Edge-detect per key, with release debounce per key: a key only
        # counts as released after RELEASE_SCANS consecutive scans without it.
        new_presses = [pos for pos in current if pos not in self._held]

        for pos in current:
            self._held[pos] = 0
        for pos in list(self._held):
            if pos not in current:
                self._held[pos] += 1
                if self._held[pos] >= RELEASE_SCANS:
                    del self._held[pos]

        if new_presses:
            new_presses.sort()
            self._pending.extend(new_presses[1:])
            return new_presses[0]
        if self._pending:
            return self._pending.pop(0)
        return None

    def close(self):
        self.chip.close()


class I2CMatrixKeypadInput:
    """
    Drop-in input backend for NeoDCT_UI, interface-compatible with
    MatrixKeypadInput: read_key(timeout) -> Linux keycode or None.

    Expects the cfg dict produced by _load_matrix_keymap(), i.e. keys:
      row_pins, col_pins   -- expander pin indices (0-15)
      matrix_to_code       -- {(row_idx, col_idx): linux_keycode}
      i2c_bus, i2c_addr    -- bus number and chip address
      path                 -- keymap file path (for log messages)
    """

    def __init__(self, cfg):
        self.path = cfg.get("path", "?")
        self.matrix_to_code = dict(cfg["matrix_to_code"])
        self.scanner = I2CMatrixScanner(
            row_pins=cfg["row_pins"],
            col_pins=cfg["col_pins"],
            bus=cfg.get("i2c_bus", DEFAULT_BUS),
            addr=cfg.get("i2c_addr", DEFAULT_ADDR),
        )
        self._last_unmapped = None

    def close(self):
        self.scanner.close()

    def read_key(self, timeout):
        timeout = max(0.0, float(timeout))
        deadline = time.monotonic() + timeout

        while True:
            pressed = self.scanner.scan_once()
            if pressed is not None:
                code = self.matrix_to_code.get(pressed)
                if code is not None:
                    self._last_unmapped = None
                    return code
                if pressed != self._last_unmapped:
                    self._last_unmapped = pressed
                    print(f"[INPUT] I2C matrix key {pressed} has no mapping in {self.path}")
            if time.monotonic() >= deadline:
                return None
            time.sleep(0.005)


def _parse_addr(text):
    text = str(text).strip()
    return int(text, 16) if text.lower().startswith("0x") else int(text)


def _main():
    import argparse

    parser = argparse.ArgumentParser(description="PCF8575 keypad scan test")
    parser.add_argument("--bus", type=int, default=DEFAULT_BUS)
    parser.add_argument("--addr", type=_parse_addr, default=DEFAULT_ADDR)
    parser.add_argument("--rows", default=",".join(map(str, DEFAULT_ROW_PINS)),
                        help="comma-separated expander pins, e.g. 0,1,2,3")
    parser.add_argument("--cols", default=",".join(map(str, DEFAULT_COL_PINS)))
    args = parser.parse_args()

    rows = [int(p) for p in args.rows.split(",")]
    cols = [int(p) for p in args.cols.split(",")]

    scanner = I2CMatrixScanner(rows, cols, bus=args.bus, addr=args.addr)
    print(f"scanning 0x{args.addr:02X} on /dev/i2c-{args.bus} "
          f"rows={rows} cols={cols} -- press keys, Ctrl-C to stop")
    try:
        while True:
            hit = scanner.scan_once()
            if hit is not None:
                row_idx, col_idx = hit
                print(f"press: matrix R{row_idx} C{col_idx} "
                      f"(row pin P{rows[row_idx]}, col pin P{cols[col_idx]})")
            time.sleep(0.005)
    except KeyboardInterrupt:
        print("\nbye")
    finally:
        scanner.close()


if __name__ == "__main__":
    _main()

#!/usr/bin/env python3
"""
build_keymap_i2c.py -- console keymap builder for the PCF8575 I2C keypad.

Solves the first-boot chicken-and-egg on the Luckfox: KeypadMapperI2C
lives inside the NeoDCT UI, but navigating the UI needs a working
keypad. This tool does the same capture over a serial/ADB console and
writes the exact JSON that System/core/main.py's _load_matrix_keymap()
consumes, so the UI comes up with input working on the next start.

Usage (from anywhere; sys.path is bootstrapped from this file's location):
  python3 build_keymap_i2c.py                     # defaults: bus 1, 0x20
  python3 build_keymap_i2c.py --bus 3             # Luckfox I2C3
  python3 build_keymap_i2c.py --bus 3 --test      # verify an existing map
  python3 build_keymap_i2c.py --output /tmp/km.json   # dry-run elsewhere

During capture, console keys (via serial/ADB stdin):
  s + Enter  skip current key      r + Enter  redo previous key
  q + Enter  quit without saving
Physical keypad presses do the actual capturing.
"""

import argparse
import json
import os
import select
import sys
import time

# Bootstrap: tools/ -> engineering/ -> System/ -> NeoDCT root
_NEODCT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))))
if _NEODCT_ROOT not in sys.path:
    sys.path.insert(0, _NEODCT_ROOT)

from System.hw.pcf8575_keypad import (  # noqa: E402
    I2CMatrixScanner,
    PCF8575,
    DEFAULT_BUS,
    DEFAULT_ADDR,
    DEFAULT_ROW_PINS,
    DEFAULT_COL_PINS,
)

DEFAULT_OUTPUT = "/NeoDCT/User/keymap.json"

# Must mirror System/core/main.py MATRIX_NAME_TO_CODE names.
KEY_TARGETS = [
    ("navikey", "NaviKey (center/enter)"),
    ("clear", "C (clear/back)"),
    ("up", "Up"),
    ("down", "Down"),
    ("num_1", "1"),
    ("num_2", "2"),
    ("num_3", "3"),
    ("num_4", "4"),
    ("num_5", "5"),
    ("num_6", "6"),
    ("num_7", "7"),
    ("num_8", "8"),
    ("num_9", "9"),
    ("num_0", "0"),
    ("star", "*"),
    ("hash", "#"),
]

NAME_TO_CODE = {
    "navikey": 28, "clear": 14, "up": 103, "down": 108,
    "left": 105, "right": 106, "menu": 50,
    "num_1": 2, "num_2": 3, "num_3": 4, "num_4": 5, "num_5": 6,
    "num_6": 7, "num_7": 8, "num_8": 9, "num_9": 10, "num_0": 11,
    "star": 42, "hash": 43,
}


def _parse_addr(text):
    text = str(text).strip()
    return int(text, 16) if text.lower().startswith("0x") else int(text)


def _parse_pins(text):
    return [int(p) for p in str(text).split(",") if p.strip()]


def _stdin_command():
    """Non-blocking read of a console command line. Returns '' if none."""
    ready, _, _ = select.select([sys.stdin], [], [], 0)
    if not ready:
        return ""
    return sys.stdin.readline().strip().lower()


def _drain_keypad(scanner, seconds=0.4):
    """Let the held key release so one press can't capture two prompts."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        scanner.scan_once()
        time.sleep(0.02)


def _wait_press_or_command(scanner):
    """Poll the keypad and stdin together. Returns ('key', (r,c)) or ('cmd', str)."""
    while True:
        pos = scanner.scan_once()
        if pos is not None:
            return "key", pos
        cmd = _stdin_command()
        if cmd in ("s", "r", "q"):
            return "cmd", cmd
        time.sleep(0.01)


def capture(scanner, row_pins, col_pins):
    captured = {}          # name -> entry dict
    position_owner = {}    # (r,c) -> name
    order = list(KEY_TARGETS)
    index = 0

    print("\n=== capture ===")
    print("press the PHYSICAL keypad button for each prompt.")
    print("console: s=skip  r=redo previous  q=quit without saving\n")

    while index < len(order):
        name, label = order[index]
        print(f"[{index + 1:2d}/{len(order)}] press: {label:<24} ", end="", flush=True)

        kind, value = _wait_press_or_command(scanner)

        if kind == "cmd":
            if value == "q":
                print("\nquit -- nothing saved.")
                return None
            if value == "s":
                print("skipped")
                index += 1
                continue
            if value == "r":
                if index == 0:
                    print("nothing to redo")
                    continue
                index -= 1
                prev_name = order[index][0]
                old = captured.pop(prev_name, None)
                if old is not None:
                    position_owner.pop((old["row"], old["col"]), None)
                print(f"redoing '{order[index][1]}'")
                continue

        row_idx, col_idx = value
        owner = position_owner.get((row_idx, col_idx))
        if owner is not None:
            print(f"R{row_idx}C{col_idx} already mapped to '{owner}' -- press a different key")
            _drain_keypad(scanner)
            continue

        captured[name] = {
            "label": label,
            "row": int(row_idx),
            "col": int(col_idx),
            "row_pin": int(row_pins[row_idx]),
            "col_pin": int(col_pins[col_idx]),
        }
        position_owner[(row_idx, col_idx)] = name
        print(f"-> R{row_idx} C{col_idx} (pins P{row_pins[row_idx]}/P{col_pins[col_idx]})")
        _drain_keypad(scanner)
        index += 1

    if not captured:
        print("no keys captured -- nothing saved.")
        return None
    return captured


def save(captured, path, bus, addr, row_pins, col_pins):
    payload = {
        "format": "neodct.keymap.v3.matrix.i2c",
        "generated_at_unix": int(time.time()),
        "output": path,
        "driver": "pcf8575-i2c",
        "i2c_bus": int(bus),
        "i2c_addr": int(addr),
        "row_pins": list(row_pins),
        "col_pins": list(col_pins),
        "keys": captured,
        "by_matrix": {f"{v['row']},{v['col']}": k for k, v in captured.items()},
        "by_code": {},
    }
    out_dir = os.path.dirname(path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"\nsaved {len(captured)} keys -> {path}")


def test_mode(scanner, path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            payload = json.load(f)
    except Exception as exc:
        print(f"cannot read {path}: {exc}")
        return 1

    by_matrix = {}
    for name, entry in payload.get("keys", {}).items():
        by_matrix[(int(entry["row"]), int(entry["col"]))] = name

    print(f"testing {path} ({len(by_matrix)} keys). press keys; Ctrl-C to stop.")
    try:
        while True:
            pos = scanner.scan_once()
            if pos is not None:
                name = by_matrix.get(pos)
                code = NAME_TO_CODE.get(name)
                if name:
                    print(f"R{pos[0]}C{pos[1]} -> {name} (keycode {code})")
                else:
                    print(f"R{pos[0]}C{pos[1]} -> UNMAPPED")
            time.sleep(0.005)
    except KeyboardInterrupt:
        print("\ndone.")
    return 0


def _bipartition(pairs):
    """2-color the connection graph: one color class = rows, other = cols.
    Returns (side_a, side_b, conflicts)."""
    adj = {}
    for a, b in pairs:
        adj.setdefault(a, set()).add(b)
        adj.setdefault(b, set()).add(a)
    color = {}
    conflicts = []
    for start in sorted(adj):
        if start in color:
            continue
        color[start] = 0
        queue = [start]
        while queue:
            node = queue.pop()
            for nb in adj[node]:
                if nb not in color:
                    color[nb] = 1 - color[node]
                    queue.append(nb)
                elif color[nb] == color[node]:
                    conflicts.append((node, nb))
    side_a = sorted(p for p, c in color.items() if c == 0)
    side_b = sorted(p for p, c in color.items() if c == 1)
    return side_a, side_b, conflicts


def discover_mode(bus, addr):
    """Drive each of the 16 pins low in turn; a pressed key connects one
    pin from each matrix side, so its pair reads low. Press EVERY key on
    the keypad once (slowly), then Ctrl-C for the computed row/col split."""
    try:
        chip = PCF8575(bus=bus, addr=addr)
    except Exception as exc:
        print(f"scanner init failed: {exc}")
        print("check: device exists (ls /dev/i2c-*), chip visible "
              f"(i2cdetect -y {bus}), pull-ups on SDA/SCL.")
        return 1
    pairs = set()
    print("DISCOVER: press every key on the keypad once. Ctrl-C when done.")
    print("(each press should print a pin pair; hold keys briefly)\n")
    try:
        last_seen = set()
        while True:
            seen = set()
            for drive in range(16):
                chip.write16(0xFFFF & ~(1 << drive))
                time.sleep(0.0005)
                v = chip.read16()
                for bit in range(16):
                    if bit != drive and not (v >> bit) & 1:
                        seen.add((min(drive, bit), max(drive, bit)))
            chip.write16(0xFFFF)
            for p in seen - last_seen:
                marker = "" if p not in pairs else " (already known)"
                print(f"  connection: P{p[0]} <-> P{p[1]}{marker}")
            pairs |= seen
            last_seen = seen
            time.sleep(0.03)
    except KeyboardInterrupt:
        pass
    finally:
        chip.close()

    print(f"\n{len(pairs)} distinct key connections observed.")
    if not pairs:
        print("no connections seen -- check wiring/pull-ups and press harder.")
        return 1

    side_a, side_b, conflicts = _bipartition(pairs)
    if conflicts:
        print("WARNING: connections conflict with a clean matrix split "
              f"(e.g. {conflicts[0]}). A wire may be loose or two keys were "
              "pressed at once. Re-run and press one key at a time.")
    print(f"\nsuggested split (either assignment works):")
    print(f"  --rows {','.join(map(str, side_a))} --cols {','.join(map(str, side_b))}")
    print(f"\nnext: python3 {sys.argv[0]} --bus {bus} "
          f"--rows {','.join(map(str, side_a))} --cols {','.join(map(str, side_b))}")
    return 0


def main():
    ap = argparse.ArgumentParser(description="console PCF8575 keymap builder")
    ap.add_argument("--bus", type=int, default=DEFAULT_BUS)
    ap.add_argument("--addr", type=_parse_addr, default=DEFAULT_ADDR)
    ap.add_argument("--rows", type=_parse_pins,
                    default=list(DEFAULT_ROW_PINS), help="e.g. 0,1,2,3")
    ap.add_argument("--cols", type=_parse_pins,
                    default=list(DEFAULT_COL_PINS), help="e.g. 4,5,6,7")
    ap.add_argument("--output", default=DEFAULT_OUTPUT)
    ap.add_argument("--test", action="store_true",
                    help="verify an existing keymap instead of capturing")
    ap.add_argument("--discover", action="store_true",
                    help="find which expander pins the keypad uses and "
                         "compute the row/col split (press every key once)")
    args = ap.parse_args()

    print(f"PCF8575 @ /dev/i2c-{args.bus} addr 0x{args.addr:02X}")

    if args.discover:
        return discover_mode(args.bus, args.addr)

    print(f"rows={args.rows} cols={args.cols}")

    try:
        scanner = I2CMatrixScanner(args.rows, args.cols,
                                   bus=args.bus, addr=args.addr)
    except Exception as exc:
        print(f"scanner init failed: {exc}")
        print("check: device exists (ls /dev/i2c-*), chip visible "
              f"(i2cdetect -y {args.bus}), pull-ups on SDA/SCL.")
        return 1

    try:
        if args.test:
            return test_mode(scanner, args.output)
        captured = capture(scanner, args.rows, args.cols)
        if captured is None:
            return 1
        save(captured, args.output, args.bus, args.addr, args.rows, args.cols)
        print("restart the NeoDCT UI (or reboot) to pick it up.")
        print(f"verify first with: python3 {sys.argv[0]} --bus {args.bus} --test")
        return 0
    finally:
        scanner.close()


if __name__ == "__main__":
    sys.exit(main())
"""
i2c_keypad_setup.py -- first-boot on-screen keypad setup for NeoDCT.

Solves the "fresh image, no keymap, no serial cable" problem: when the UI
starts with no /NeoDCT/User/keymap.json and a PCF8575 answers on the setup
bus, this wizard walks the user through pressing every key, discovers the
row/col pin split automatically from the observed pin pairs, writes the
same keymap JSON as engineering/tools/consolei2ckeypadbuilder.py, and
restarts the UI so the map takes effect.

All feedback is drawn straight to the framebuffer; the only input device
is the keypad being enrolled, so prompts are strictly press-one-key.
"""

import json
import os
import sys
import time

from PIL import Image, ImageDraw, ImageFont

from System.hw.pcf8575_keypad import PCF8575

KEYMAP_PATH = "/NeoDCT/User/keymap.json"
SETUP_BUS_ENV = "NEODCT_KEYPAD_SETUP_BUS"
DEFAULT_SETUP_BUS = 3
PROBE_ADDRS = tuple(range(0x20, 0x28))

FIRST_KEY_TIMEOUT = 120.0   # seconds to wait for the very first press
KEY_TIMEOUT = 60.0          # per-key timeout after setup has started
RELEASE_SCANS = 3           # consecutive empty scans = key released

FONT_PATH = "/NeoDCT/System/ui/resources/fonts/font.ttf"
UI_W, UI_H = 240, 175

# Must mirror System/core/main.py MATRIX_NAME_TO_CODE names and the console
# builder's enrolment order.
KEY_TARGETS = [
    ("navikey", "NaviKey (center)"),
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


# --- drawing ---------------------------------------------------------------

class SetupScreen:
    def __init__(self, fb):
        self.fb = fb
        self.canvas = Image.new("RGB", (UI_W, UI_H), "black")
        self.draw = ImageDraw.Draw(self.canvas)
        try:
            self.font_big = ImageFont.truetype(FONT_PATH, 26)
            self.font = ImageFont.truetype(FONT_PATH, 18)
            self.font_small = ImageFont.truetype(FONT_PATH, 14)
        except Exception:
            self.font_big = ImageFont.load_default()
            self.font = self.font_big
            self.font_small = self.font_big

    def _text_size(self, text, font):
        bbox = self.draw.textbbox((0, 0), text, font=font)
        return bbox[2] - bbox[0], bbox[3] - bbox[1]

    def _center(self, text, font, y, fill="white"):
        w, _ = self._text_size(text, font)
        self.draw.text(((UI_W - w) // 2, y), text, font=font, fill=fill)

    def message(self, title, lines=(), footer=None):
        self.draw.rectangle((0, 0, UI_W, UI_H), fill="black")
        self._center(title, self.font, 18)
        y = 58
        for line in lines:
            self._center(line, self.font_small, y)
            y += 20
        if footer:
            self._center(footer, self.font_small, UI_H - 24, fill="gray")
        self.fb.update(self.canvas)

    def prompt(self, label, index, total, note=None):
        self.draw.rectangle((0, 0, UI_W, UI_H), fill="black")
        self._center("Keypad setup", self.font_small, 6, fill="gray")

        counter = f"{index + 1}/{total}"
        w, _ = self._text_size(counter, self.font_small)
        self.draw.text((UI_W - 8 - w, 6), counter, font=self.font_small, fill="white")

        self._center("Press:", self.font, 46)
        self._center(label, self.font_big, 76)

        # progress bar
        bar_y = UI_H - 42
        self.draw.rectangle((16, bar_y, UI_W - 16, bar_y + 8), outline="white")
        if index > 0:
            fill_w = int((UI_W - 34) * (index / float(total)))
            self.draw.rectangle((17, bar_y + 1, 17 + fill_w, bar_y + 7), fill="white")

        if note:
            self._center(note, self.font_small, UI_H - 24)
        self.fb.update(self.canvas)


# --- scanning ----------------------------------------------------------------

class PairScanner:
    """Full 16-pin discovery scan: every pressed key shows up as an
    unordered pin pair, no row/col assumption needed."""

    def __init__(self, chip):
        self.chip = chip
        self._last = set()
        self._empty = 0

    def scan_pairs(self):
        seen = set()
        for drive in range(16):
            self.chip.write16(0xFFFF & ~(1 << drive))
            time.sleep(0.0005)
            value = self.chip.read16()
            for bit in range(16):
                if bit != drive and not (value >> bit) & 1:
                    seen.add((min(drive, bit), max(drive, bit)))
        self.chip.write16(0xFFFF)
        return seen

    def wait_release(self, max_seconds=10.0):
        deadline = time.monotonic() + max_seconds
        empties = 0
        while time.monotonic() < deadline:
            if not self.scan_pairs():
                empties += 1
                if empties >= RELEASE_SCANS:
                    return
            else:
                empties = 0
            time.sleep(0.01)

    def wait_new_pair(self, timeout):
        """Block until exactly one key is pressed; returns its pin pair,
        or None on timeout."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            pairs = self.scan_pairs()
            if len(pairs) == 1:
                return next(iter(pairs))
            time.sleep(0.01)
        return None


def _bipartition(pairs):
    """2-color the connection graph: one color class = rows, other = cols."""
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


# --- keymap output ------------------------------------------------------------

def _build_payload(pair_by_name, bus, addr):
    all_pairs = set(pair_by_name.values())
    side_a, side_b, conflicts = _bipartition(all_pairs)
    if conflicts:
        return None, f"P{conflicts[0][0]}/P{conflicts[0][1]} conflict"

    row_pins, col_pins = side_a, side_b
    keys = {}
    for (name, label), pair in ((t, pair_by_name[t[0]]) for t in KEY_TARGETS
                                if t[0] in pair_by_name):
        a, b = pair
        if a in row_pins and b in col_pins:
            row_pin, col_pin = a, b
        elif b in row_pins and a in col_pins:
            row_pin, col_pin = b, a
        else:
            return None, f"key '{name}' does not fit the matrix split"
        keys[name] = {
            "label": label,
            "row": row_pins.index(row_pin),
            "col": col_pins.index(col_pin),
            "row_pin": row_pin,
            "col_pin": col_pin,
        }

    payload = {
        "format": "neodct.keymap.v3.matrix.i2c",
        "generated_at_unix": int(time.time()),
        "output": KEYMAP_PATH,
        "driver": "pcf8575-i2c",
        "i2c_bus": int(bus),
        "i2c_addr": int(addr),
        "row_pins": row_pins,
        "col_pins": col_pins,
        "keys": keys,
        "by_matrix": {f"{v['row']},{v['col']}": k for k, v in keys.items()},
        "by_code": {},
    }
    return payload, None


def _save_keymap(payload, path=None):
    path = path or KEYMAP_PATH
    out_dir = os.path.dirname(path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2, sort_keys=True)
        f.write("\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


# --- wizard --------------------------------------------------------------------

def _probe_chip(bus):
    for addr in PROBE_ADDRS:
        try:
            chip = PCF8575(bus=bus, addr=addr)
            chip.write16(0xFFFF)
            chip.read16()
            return chip, addr
        except OSError:
            try:
                chip.close()
            except Exception:
                pass
            continue
    return None, None


def run_wizard(fb, chip, addr, bus, restart=True):
    """Interactive enrolment. Returns True if a keymap was written."""
    screen = SetupScreen(fb)
    scanner = PairScanner(chip)

    screen.message("Keypad setup",
                   (f"Keypad found on bus {bus}",
                    f"(PCF8575 at 0x{addr:02X})",
                    "Press each key as asked."))
    time.sleep(2.0)

    pair_by_name = {}
    used_pairs = {}
    note = None
    total = len(KEY_TARGETS)

    for index, (name, label) in enumerate(KEY_TARGETS):
        timeout = FIRST_KEY_TIMEOUT if index == 0 else KEY_TIMEOUT
        while True:
            screen.prompt(label, index, total, note)
            pair = scanner.wait_new_pair(timeout)
            if pair is None:
                screen.message("Setup aborted",
                               ("No key was pressed.",
                                "Starting without a keymap."))
                time.sleep(2.5)
                return False
            if pair in used_pairs:
                note = f"Already used by '{used_pairs[pair]}'"
                scanner.wait_release()
                continue
            pair_by_name[name] = pair
            used_pairs[pair] = label
            note = None
            scanner.wait_release()
            break

    payload, err = _build_payload(pair_by_name, bus, addr)
    if payload is None:
        screen.message("Setup failed", (err, "Starting without a keymap."))
        time.sleep(3.0)
        return False

    try:
        _save_keymap(payload)
    except Exception as exc:
        screen.message("Setup failed", (f"Could not save: {exc}",))
        time.sleep(3.0)
        return False

    screen.message("Keymap saved!",
                   (f"{len(pair_by_name)} keys mapped.",
                    "Restarting UI..."))
    time.sleep(1.5)

    if restart:
        chip.close()
        os.execv(sys.executable, [sys.executable] + sys.argv)
    return True


def _is_real_hardware():
    # Same hint the launcher uses: the Rockchip FIQ console only exists on
    # real hardware, never in QEMU.
    return os.path.exists("/dev/ttyFIQ0")


def maybe_run_first_time_setup(fb, restart=True):
    """Called once at boot, before NeoDCT_UI is built. No-op unless this is
    a fresh image (no keymap) with a PCF8575 answering on the setup bus.

    On real hardware every skip is announced on screen (and serial), so a
    silently-dead keypad can be diagnosed without a serial cable.
    """
    if os.path.exists(KEYMAP_PATH):
        print(f"[SETUP] Keymap already present ({KEYMAP_PATH}); skipping setup.")
        return False

    bus = int(os.environ.get(SETUP_BUS_ENV, DEFAULT_SETUP_BUS))
    dev = f"/dev/i2c-{bus}"
    is_hw = _is_real_hardware()

    if not os.path.exists(dev) and not is_hw:
        # QEMU / dev boxes: no bus, no keypad, stay quiet.
        return False

    screen = SetupScreen(fb)

    # The i2c device node can appear a moment after the UI starts (udev
    # coldplug); give it a few seconds on hardware.
    if not os.path.exists(dev):
        screen.message("Keypad setup", (f"Waiting for {dev}...",))
        deadline = time.monotonic() + 8.0
        while not os.path.exists(dev) and time.monotonic() < deadline:
            time.sleep(0.25)
        if not os.path.exists(dev):
            print(f"[SETUP] {dev} never appeared; no keymap and no bus. "
                  f"Set {SETUP_BUS_ENV} if the keypad is on another bus.")
            screen.message("No keypad bus",
                           (f"{dev} does not exist.",
                            "Starting without a keymap."))
            time.sleep(3.0)
            return False

    chip, addr = _probe_chip(bus)
    if chip is None:
        print(f"[SETUP] No PCF8575 answered on {dev} "
              f"(tried 0x{PROBE_ADDRS[0]:02X}-0x{PROBE_ADDRS[-1]:02X}).")
        screen.message("No keypad found",
                       (f"Nothing answered on {dev}",
                        f"(addresses 0x{PROBE_ADDRS[0]:02X}-0x{PROBE_ADDRS[-1]:02X}).",
                        "Starting without a keymap."))
        time.sleep(3.0)
        return False

    print(f"[SETUP] No keymap; PCF8575 found at 0x{addr:02X} on bus {bus}. "
          "Starting first-time keypad setup.")
    try:
        return run_wizard(fb, chip, addr, bus, restart=restart)
    finally:
        try:
            chip.close()
        except Exception:
            pass


def _standalone():
    """Manual test entry point:

      python3 -m System.hw.i2c_keypad_setup [--force] [--bus N]

    --force removes an existing keymap first so the wizard always runs.
    Run from /NeoDCT so the System package imports resolve.
    """
    import argparse

    ap = argparse.ArgumentParser(description="NeoDCT keypad setup wizard")
    ap.add_argument("--force", action="store_true",
                    help="run even if a keymap already exists (it is replaced)")
    ap.add_argument("--bus", type=int, default=None,
                    help=f"i2c bus to probe (default {DEFAULT_SETUP_BUS} "
                         f"or ${SETUP_BUS_ENV})")
    args = ap.parse_args()

    if args.bus is not None:
        os.environ[SETUP_BUS_ENV] = str(args.bus)
    if args.force and os.path.exists(KEYMAP_PATH):
        os.rename(KEYMAP_PATH, KEYMAP_PATH + ".bak")
        print(f"[SETUP] Existing keymap moved to {KEYMAP_PATH}.bak")

    from System.core.main import Framebuffer
    ran = maybe_run_first_time_setup(Framebuffer(), restart=False)
    print(f"[SETUP] wizard {'completed' if ran else 'did not run'}")
    if ran:
        print("[SETUP] Keymap written. Restart the UI (or reboot) to use it.")
    return 0 if ran else 1


if __name__ == "__main__":
    sys.exit(_standalone())

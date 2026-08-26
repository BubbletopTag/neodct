import glob
import json
import os
import time

from System.ui.framework import MessageDialog, SoftKeyBar
from System.hw.pcf8575_keypad import (
    I2CMatrixScanner,
    DEFAULT_BUS,
    DEFAULT_ADDR,
    DEFAULT_ROW_PINS,
    DEFAULT_COL_PINS,
)

ROOT_ID = 10
OUTPUT_PATH = "/NeoDCT/User/keymap.json"
KEY_MENU = 50
I2C_REQUIRED_MSG = "This app requires I2C. No /dev/i2c-* devices found. This application can not run in QEMU."
KEYPAD_ROWS_ENV = "NEODCT_KEYPAD_ROWS"
KEYPAD_COLS_ENV = "NEODCT_KEYPAD_COLS"
I2C_BUS_ENV = "NEODCT_I2C_KEYPAD_BUS"
I2C_ADDR_ENV = "NEODCT_I2C_KEYPAD_ADDR"

KEY_TARGETS = [
    ("navikey", "NaviKey"),
    ("clear", "C"),
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


def _i2c_available():
    return len(glob.glob("/dev/i2c-*")) > 0


def _parse_pins(raw, fallback):
    text = (raw or "").strip()
    if not text:
        return list(fallback)
    out = []
    for chunk in text.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        out.append(int(chunk))
    return out if out else list(fallback)


def _parse_addr(raw, fallback):
    text = (raw or "").strip()
    if not text:
        return fallback
    return int(text, 16) if text.lower().startswith("0x") else int(text)


def _i2c_config():
    try:
        rows = _parse_pins(os.environ.get(KEYPAD_ROWS_ENV, ""), DEFAULT_ROW_PINS)
        cols = _parse_pins(os.environ.get(KEYPAD_COLS_ENV, ""), DEFAULT_COL_PINS)
        bus = int(os.environ.get(I2C_BUS_ENV, "").strip() or DEFAULT_BUS)
        addr = _parse_addr(os.environ.get(I2C_ADDR_ENV, ""), DEFAULT_ADDR)
    except Exception as exc:
        print(f"[KEYMAP-I2C] Invalid override: {exc}; using defaults.")
        rows = list(DEFAULT_ROW_PINS)
        cols = list(DEFAULT_COL_PINS)
        bus = DEFAULT_BUS
        addr = DEFAULT_ADDR
    return rows, cols, bus, addr


def _wrap_text(ui, text, max_width, font):
    words = (text or "").split()
    if not words:
        return [""]

    lines = []
    current = ""

    def fits(candidate):
        width, _ = ui.get_text_size(candidate, font)
        return width <= max_width

    for word in words:
        candidate = f"{current} {word}".strip() if current else word
        if fits(candidate):
            current = candidate
        else:
            if current:
                lines.append(current)
            current = word

    if current:
        lines.append(current)
    return lines


class KeypadMapperI2C:
    def __init__(self, ui):
        self.ui = ui
        self.softkey = SoftKeyBar(ui)
        self.screen_w = getattr(ui, "W", 240)
        self.screen_h = getattr(ui, "H", 175)
        self.softkey_h = getattr(ui, "SOFTKEY_H", 30)
        self.content_bottom = getattr(ui, "content_bottom", self.screen_h - self.softkey_h)
        self.row_pins, self.col_pins, self.i2c_bus, self.i2c_addr = _i2c_config()
        self.scanner = None

    def _draw_capture_prompt(self, label, index, total):
        self.ui.draw.rectangle((0, 0, self.screen_w, self.screen_h), fill="black")

        title = "Keypad Mapper I2C"
        tw, _ = self.ui.get_text_size(title, self.ui.font_n)
        self.ui.draw.text(((self.screen_w - tw) // 2, 8), title, font=self.ui.font_n, fill="white")
        self.ui.draw.line((12, 32, self.screen_w - 12, 32), fill="white")

        progress = f"{ROOT_ID}-{index}/{total}"
        pw, _ = self.ui.get_text_size(progress, self.ui.font_s)
        self.ui.draw.text((self.screen_w - pw - 8, 38), progress, font=self.ui.font_s, fill="gray")

        body = [
            f"Press: {label}",
            "",
            "Capture one keypad button now.",
            "Menu key cancels.",
            f"Bus: /dev/i2c-{self.i2c_bus} Addr: 0x{self.i2c_addr:02X}",
            f"Rows: P{self.row_pins} Cols: P{self.col_pins}",
        ]

        y = 56
        line_h = self.ui.get_text_size("Ag", self.ui.font_s)[1] + 4
        for raw in body:
            lines = [""] if raw == "" else _wrap_text(self.ui, raw, self.screen_w - 16, self.ui.font_s)
            for line in lines:
                if y > self.content_bottom - line_h:
                    break
                self.ui.draw.text((8, y), line, font=self.ui.font_s, fill="white")
                y += line_h

        self.softkey.update("Capture", present=False)
        self.ui.fb.update(self.ui.canvas)

    def _open_scanner(self):
        self.scanner = I2CMatrixScanner(
            self.row_pins, self.col_pins, bus=self.i2c_bus, addr=self.i2c_addr
        )
        print(
            f"[KEYMAP-I2C] Scanner ready. bus={self.i2c_bus} addr=0x{self.i2c_addr:02X} "
            f"rows={self.row_pins} cols={self.col_pins}"
        )

    def _close_scanner(self):
        if self.scanner is None:
            return
        self.scanner.close()
        self.scanner = None

    def _wait_for_matrix_press(self):
        while True:
            key = self.ui.read_keypress(0.01)
            if key == KEY_MENU:
                return None

            pos = self.scanner.scan_once()
            if pos is not None:
                return pos

            time.sleep(0.01)

    def _capture_keymap(self):
        keymap_by_name = {}
        used_positions = set()

        total = len(KEY_TARGETS)
        for index, (name, label) in enumerate(KEY_TARGETS, start=1):
            while True:
                self._draw_capture_prompt(label, index, total)
                position = self._wait_for_matrix_press()

                if position is None:
                    MessageDialog(self.ui, "Calibration canceled. Keymap not saved.").show()
                    return None

                if position in used_positions:
                    row_idx, col_idx = position
                    MessageDialog(
                        self.ui,
                        f"Matrix R{row_idx} C{col_idx} is already mapped. Press a different key for {label}.",
                    ).show()
                    continue

                row_idx, col_idx = position
                keymap_by_name[name] = {
                    "label": label,
                    "row": int(row_idx),
                    "col": int(col_idx),
                    "row_pin": int(self.row_pins[row_idx]),
                    "col_pin": int(self.col_pins[col_idx]),
                }
                used_positions.add(position)
                break

        return keymap_by_name

    def _save_keymap(self, keymap_by_name):
        payload = {
            "format": "neodct.keymap.v3.matrix.i2c",
            "generated_at_unix": int(time.time()),
            "output": OUTPUT_PATH,
            "driver": "pcf8575-i2c",
            "i2c_bus": self.i2c_bus,
            "i2c_addr": self.i2c_addr,
            "row_pins": self.row_pins,
            "col_pins": self.col_pins,
            "keys": keymap_by_name,
            "by_matrix": {f"{v['row']},{v['col']}": k for k, v in keymap_by_name.items()},
            "by_code": {},
        }

        out_dir = os.path.dirname(OUTPUT_PATH)
        os.makedirs(out_dir, exist_ok=True)

        with open(OUTPUT_PATH, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2, sort_keys=True)
            f.write("\n")

    def run(self):
        MessageDialog(
            self.ui,
            "This tool captures PCF8575 I2C keypad presses and writes JSON to /NeoDCT/User/keymap.json.",
            title="Keypad Mapper I2C",
        ).show()

        # Suspend the UI's own input backend during capture so it cannot scan
        # the PCF8575 at the same time as the scanner.
        suspended_input = getattr(self.ui, "matrix_input", None)
        self.ui.matrix_input = None

        try:
            self._open_scanner()
            captured = self._capture_keymap()
            if not captured:
                return
            self._save_keymap(captured)
        except Exception as exc:
            print("[KEYMAP-I2C] Capture/save error:", exc)
            MessageDialog(self.ui, f"Failed to write keymap: {exc}").show()
            return
        finally:
            self._close_scanner()
            self.ui.matrix_input = suspended_input

        MessageDialog(self.ui, f"Keymap saved to\n{OUTPUT_PATH}").show()


def run(ui):
    if not _i2c_available():
        MessageDialog(ui, I2C_REQUIRED_MSG).show()
        return

    app = KeypadMapperI2C(ui)
    app.run()

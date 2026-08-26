# NeoDCT Main.py. This file handles drawing to the framebuffer and running the main screen and app selector.
# NeoDCT OS is an embedded OS created by Aiden Colgan for the NeoDCT Nokia 5190 modernization project
# It is based on buildroot embedded Linux with the NeoDCT frontend written in python.

import sys
import mmap
import os
import struct
import time
import select
import json
import fcntl
import traceback
import glob
import gc
# --- THE FIX: Import ImageFile to handle "broken" JPEGs ---
from PIL import Image, ImageDraw, ImageFont, ImageEnhance, ImageFile 
from System.ui.framework import AppSelector, SoftKeyBar
from System.core.SettingsStorage import get_setting
import importlib.util
import sqlite3
from System.core.ModemService import ModemService
from System.core.BatteryService import BatteryService
from System.core.NotifyService import NotifyService
from System.core.CrashHandler import show_app_crash, log_crash
import System.ui.Dialer.call_screen as dialer_ui
import System.ui.Dialer.incoming_screen as incoming_ui
import System.apps.PhoneBook.shared.list_ui as contact_manager
from System.core.ErrorScreen import show_alpha_security_notice_once, show_error

# --- CONFIG ---
# 1. Allow loading images even if they are missing EOF markers
ImageFile.LOAD_TRUNCATED_IMAGES = True

FB_PATH = "/dev/fb0"
KEYPAD_PATH = "/dev/input/event0"
KEYPAD_DEVICE_ENV = "NEODCT_KEYPAD_DEVICE"
KEYMAP_PATH = "/NeoDCT/User/keymap.json"
UI_WIDTH = 240
UI_HEIGHT = 175
SOFTKEY_HEIGHT = 30
WIDTH = UI_WIDTH
HEIGHT = UI_HEIGHT
WALLPAPER_PATH = "/NeoDCT/User/wallpaper.jpg"
SERIAL_CONSOLE_DEVICE = os.environ.get("NEODCT_SERIAL_DEVICE", "/dev/ttyAMA0")

MATRIX_NAME_TO_CODE = {
    "navikey": 28,
    "clear": 14,
    "up": 103,
    "down": 108,
    "left": 105,
    "right": 106,
    "menu": 50,
    "enter": 28,
    "back": 14,
    "num_1": 2,
    "num_2": 3,
    "num_3": 4,
    "num_4": 5,
    "num_5": 6,
    "num_6": 7,
    "num_7": 8,
    "num_8": 9,
    "num_9": 10,
    "num_0": 11,
    "star": 42,
    "hash": 43,
}

try:
    from gpiozero import Button, OutputDevice
    GPIOZERO_IMPORT_ERROR = None
except Exception as exc:
    Button = None
    OutputDevice = None
    GPIOZERO_IMPORT_ERROR = str(exc)


def _setting_is_enabled(value, default=True):
    if value is None:
        return default
    text = str(value).strip().lower()
    if text in ("1", "true", "on", "yes", "enabled"):
        return True
    if text in ("0", "false", "off", "no", "disabled"):
        return False
    return default


def _event_device_name(path):
    real_path = os.path.realpath(path)
    event_name = os.path.basename(real_path)
    sys_name_path = f"/sys/class/input/{event_name}/device/name"
    try:
        with open(sys_name_path, "r") as f:
            return f.read().strip()
    except Exception:
        return "unknown"


def _discover_keypad_path():
    """
    Pick a keyboard-capable input event device.
    Order:
    1) explicit env override
    2) /dev/input/by-path/*-kbd
    3) /dev/input/by-id/*-kbd
    4) legacy /dev/input/event0
    5) first /dev/input/event*
    """
    override = os.environ.get(KEYPAD_DEVICE_ENV, "").strip()
    if override:
        if os.path.exists(override):
            selected = os.path.realpath(override)
            print(f"[INPUT] Using {KEYPAD_DEVICE_ENV}: {selected} ({_event_device_name(selected)})")
            return selected
        print(f"[INPUT] {KEYPAD_DEVICE_ENV} not found: {override}")

    candidates = []
    candidates.extend(sorted(glob.glob("/dev/input/by-path/*-kbd")))
    candidates.extend(sorted(glob.glob("/dev/input/by-id/*-kbd")))
    if os.path.exists(KEYPAD_PATH):
        candidates.append(KEYPAD_PATH)

    seen = set()
    for path in candidates:
        resolved = os.path.realpath(path)
        if resolved in seen:
            continue
        seen.add(resolved)
        if os.path.exists(resolved):
            print(f"[INPUT] Selected keyboard device: {resolved} ({_event_device_name(resolved)})")
            return resolved

    event_devices = sorted(glob.glob("/dev/input/event*"))
    if event_devices:
        fallback = os.path.realpath(event_devices[0])
        print(f"[INPUT] Fallback input device: {fallback} ({_event_device_name(fallback)})")
        return fallback

    print(f"[INPUT] No input event device found; defaulting to {KEYPAD_PATH}")
    return KEYPAD_PATH


def _gpio_available():
    return len(glob.glob("/dev/gpiochip*")) > 0


def _load_matrix_keymap(path=KEYMAP_PATH):
    if not os.path.exists(path):
        return None

    try:
        with open(path, "r", encoding="utf-8") as f:
            payload = json.load(f)
    except Exception as exc:
        print(f"[INPUT] Keymap read failed ({path}): {exc}")
        return None

    row_pins = payload.get("row_pins")
    col_pins = payload.get("col_pins")
    keys = payload.get("keys")

    if not isinstance(row_pins, list) or not isinstance(col_pins, list) or not isinstance(keys, dict):
        print(f"[INPUT] Keymap ignored (missing matrix fields): {path}")
        return None

    try:
        row_pins = [int(pin) for pin in row_pins]
        col_pins = [int(pin) for pin in col_pins]
    except Exception as exc:
        print(f"[INPUT] Keymap ignored (invalid pin list): {exc}")
        return None

    matrix_to_code = {}
    for name, entry in keys.items():
        if not isinstance(entry, dict):
            continue
        code = MATRIX_NAME_TO_CODE.get(name)
        if code is None:
            continue
        try:
            row = int(entry["row"])
            col = int(entry["col"])
        except Exception:
            continue
        matrix_to_code[(row, col)] = int(code)

    if not matrix_to_code:
        print(f"[INPUT] Keymap ignored (no recognized keys): {path}")
        return None

    try:
        i2c_addr_raw = payload.get("i2c_addr", 0x20)
        if isinstance(i2c_addr_raw, str):
            i2c_addr = int(i2c_addr_raw, 16) if i2c_addr_raw.lower().startswith("0x") else int(i2c_addr_raw)
        else:
            i2c_addr = int(i2c_addr_raw)
        i2c_bus = int(payload.get("i2c_bus", 3))
    except Exception as exc:
        print(f"[INPUT] Keymap ignored (invalid i2c fields): {exc}")
        return None

    return {
        "path": path,
        "format": payload.get("format", "unknown"),
        "driver": payload.get("driver", "gpiozero-matrix"),
        "row_pins": row_pins,
        "col_pins": col_pins,
        "matrix_to_code": matrix_to_code,
        "i2c_bus": i2c_bus,
        "i2c_addr": i2c_addr,
    }


class MatrixKeypadInput:
    def __init__(self, cfg):
        self.path = cfg["path"]
        self.row_pins = list(cfg["row_pins"])
        self.col_pins = list(cfg["col_pins"])
        self.matrix_to_code = dict(cfg["matrix_to_code"])
        self.rows = [OutputDevice(pin, initial_value=True) for pin in self.row_pins]
        self.cols = [Button(pin, pull_up=True) for pin in self.col_pins]
        self._held = set()
        self._pending = []
        self._last_unmapped = None

    def close(self):
        for row in self.rows:
            try:
                row.on()
            except Exception:
                pass
            try:
                row.close()
            except Exception:
                pass
        for col in self.cols:
            try:
                col.close()
            except Exception:
                pass

    def _scan_once(self):
        # Full-matrix scan with per-key edge detection, so a second key
        # pressed while another is held is still reported (key rollover).
        current = set()
        for row_idx, row in enumerate(self.rows):
            row.off()
            time.sleep(0.001)
            for col_idx, col in enumerate(self.cols):
                if col.is_pressed:
                    current.add((row_idx, col_idx))
            row.on()

        new_presses = sorted(current - self._held)
        self._held = current
        if new_presses:
            self._pending.extend(new_presses[1:])
            pressed = new_presses[0]
        elif self._pending:
            pressed = self._pending.pop(0)
        else:
            if not current:
                self._last_unmapped = None
            return None

        code = self.matrix_to_code.get(pressed)
        if code is None:
            if pressed != self._last_unmapped:
                self._last_unmapped = pressed
                print(f"[INPUT] Matrix key {pressed} has no mapping in {self.path}")
            return None

        self._last_unmapped = None
        return code

    def read_key(self, timeout):
        timeout = max(0.0, float(timeout))
        deadline = time.monotonic() + timeout

        while True:
            code = self._scan_once()
            if code is not None:
                return code
            if time.monotonic() >= deadline:
                return None
            time.sleep(0.005)

# --- HARDWARE DRIVER ---
class Framebuffer:
    def __init__(self):
        self.fd = os.open(FB_PATH, os.O_RDWR)
        
        # Get Screen Info
        vinfo = fcntl.ioctl(self.fd, 0x4600, b'\0'*160)
        self.xres, self.yres = struct.unpack_from("II", vinfo, 0)
        self.bpp = struct.unpack_from("I", vinfo, 24)[0]
        
        # Get Line Length (Stride)
        finfo = fcntl.ioctl(self.fd, 0x4602, b'\0'*64)
        self.line_length = struct.unpack_from("I", finfo, 48)[0]
        if self.line_length == 0: self.line_length = self.xres * (self.bpp // 8)

        self.size = self.line_length * self.yres
        self.mm = mmap.mmap(self.fd, self.size, mmap.MAP_SHARED, mmap.PROT_WRITE | mmap.PROT_READ)
        self.bytes_per_pixel = max(1, self.bpp // 8)
        self.stride_pixels = self.line_length // self.bytes_per_pixel

        # Initialize framebuffer memory once to black; allows partial-band writes later.
        self.mm.seek(0)
        self.mm.write(b"\x00" * self.size)

        self.native_img = None
        self._black = (0, 0, 0)

        # Detect the pixel conversion path ONCE (Pillow >= 11 removed the
        # "BGR;16" packer, which silently forced the slow Python loop).
        self._has_bgr16 = False
        if self.bpp == 16:
            try:
                Image.new("RGB", (1, 1)).tobytes("raw", "BGR;16")
                self._has_bgr16 = True
            except Exception:
                self._has_bgr16 = False

        if self.bpp == 32:
            path = "BGRA 32bpp (C, fast)"
        elif self._has_bgr16:
            path = "BGR;16 16bpp (C, fast)"
        else:
            path = "PYTHON RGB565 PACK 16bpp (SLOW -- ~350ms/frame on RV1103!)"
        print(f"[FB] {self.xres}x{self.yres} @ {self.bpp}bpp, pixel path: {path}")

        # RGB565 fallback tables/buffers exist only on the 16bpp-without-
        # "BGR;16" path; other configs don't pin ~350 KB (64 MB device).
        self._r565 = self._g565 = self._b565 = None
        self._rgb565_out = self._rgb565_band_out = None
        if self.bpp == 16 and not self._has_bgr16:
            print("[FB] WARNING: on hardware, ensure neodct_displayd v2.1+ runs "
                  "BEFORE the UI so the framebuffer is switched to 32bpp.")
            self._r565 = [(i & 0xF8) << 8 for i in range(256)]
            self._g565 = [(i & 0xFC) << 3 for i in range(256)]
            self._b565 = [(i >> 3) for i in range(256)]
            self._rgb565_out = bytearray(self.size)
            self._rgb565_band_out = bytearray(self.xres * self.yres * 2)

    def _pack_rgb565(self, src_bytes, out_buf):
        r565 = self._r565
        g565 = self._g565
        b565 = self._b565
        j = 0
        # Loop remains Python, but table lookups reduce arithmetic overhead.
        for i in range(0, len(src_bytes), 3):
            rgb565 = r565[src_bytes[i]] | g565[src_bytes[i + 1]] | b565[src_bytes[i + 2]]
            out_buf[j] = rgb565 & 0xFF
            out_buf[j + 1] = (rgb565 >> 8) & 0xFF
            j += 2
        return j

    def _write_center_band(self, band_data, copy_w, copy_h, dst_x, dst_y):
        bpp = self.bytes_per_pixel
        row_bytes = copy_w * bpp
        write_len = copy_h * row_bytes

        # Fast contiguous write when the active region spans full framebuffer width.
        if dst_x == 0 and row_bytes == self.line_length:
            off = dst_y * self.line_length
            self.mm.seek(off)
            self.mm.write(band_data[:write_len])
            return

        # Generic row-by-row write for side padding/stride mismatches.
        src_off = 0
        dst_off = (dst_y * self.line_length) + (dst_x * bpp)
        for _ in range(copy_h):
            self.mm.seek(dst_off)
            self.mm.write(band_data[src_off:src_off + row_bytes])
            src_off += row_bytes
            dst_off += self.line_length

    def update(self, pil_image):
        # Avoid needless conversion copies if input is already RGB.
        src = pil_image if pil_image.mode == "RGB" else pil_image.convert("RGB")
        copy_w = min(src.width, self.xres)
        copy_h = min(src.height, self.yres)

        src_x = max(0, (src.width - copy_w) // 2)
        src_y = max(0, (src.height - copy_h) // 2)
        # Only crop when needed to avoid extra allocations.
        if src_x == 0 and src_y == 0 and copy_w == src.width and copy_h == src.height:
            cropped = src
        else:
            cropped = src.crop((src_x, src_y, src_x + copy_w, src_y + copy_h))

        dst_x = max(0, (self.xres - copy_w) // 2)
        dst_y = max(0, (self.yres - copy_h) // 2)

        # Fast path: source already aligned to full framebuffer width.
        # Common NeoDCT case on 240x240 fb with 240x175 UI band.
        if src_x == 0 and src_y == 0 and copy_w == src.width and copy_h == src.height:
            if self.bpp == 16:
                if self._has_bgr16:
                    band = src.tobytes("raw", "BGR;16")
                else:
                    src_bytes = src.tobytes()
                    used = self._pack_rgb565(src_bytes, self._rgb565_band_out)
                    band = self._rgb565_band_out[:used]
                self._write_center_band(band, copy_w, copy_h, dst_x, dst_y)
                return
            if self.bpp == 32:
                band = src.convert("RGBA").tobytes("raw", "BGRA")
                self._write_center_band(band, copy_w, copy_h, dst_x, dst_y)
                return

        # Clear reusable target, then paste current frame.
        if self.native_img is None:
            self.native_img = Image.new("RGB", (self.stride_pixels, self.yres), "black")
        self.native_img.paste(self._black, (0, 0, self.stride_pixels, self.yres))
        self.native_img.paste(cropped, (dst_x, dst_y))

        if self.bpp == 32:
            data = self.native_img.convert("RGBA").tobytes("raw", "BGRA")
        elif self.bpp == 16:
            rgb_img = self.native_img
            if self._has_bgr16:
                # Fast path when Pillow build supports this packer.
                data = rgb_img.tobytes("raw", "BGR;16")
            else:
                # Fallback for Pillow >= 11 builds: software-pack RGB565.
                out = self._rgb565_out
                self._pack_rgb565(rgb_img.tobytes(), out)
                data = out
        else:
            data = self.native_img.tobytes()
            
        self.mm.seek(0)
        self.mm.write(data if len(data) == self.size else data[:self.size])

def init_databases():
        """ Checks for User DBs and creates them if missing. """
        
        db_path = "/NeoDCT/User/db"
        if not os.path.exists(db_path):
            print(f"[CORE] Creating User DB directory: {db_path}")
            os.makedirs(db_path)
            
        # --- PHONEBOOK DB ---
        pb_file = f"{db_path}/phonebook.db"
        conn = sqlite3.connect(pb_file)
        c = conn.cursor()
        # WAL hardening so the database is not corrupted by a power loss mid-write.
        # Every later connection like Phonebook and Messages uses WAL automagically!!! (hopefully)
        c.execute("PRAGMA journal_mode=WAL")

        c.execute('''CREATE TABLE IF NOT EXISTS contacts
                     (id INTEGER PRIMARY KEY AUTOINCREMENT, 
                      name TEXT, 
                      number TEXT, 
                      speed_dial INTEGER)''')
                      
        c.execute("SELECT count(*) FROM contacts")
        if c.fetchone()[0] == 0:
            print("[CORE] Seeding default contacts...")
            c.execute("INSERT INTO contacts (name, number, speed_dial) VALUES (?, ?, ?)", 
                      ("NeoDCT Support", "555-1234", 2))
            conn.commit()
            
        conn.close()

        # --- INBOX DB ---
        inbox_file = f"{db_path}/sms_inbox.db"
        conn = sqlite3.connect(inbox_file)
        c = conn.cursor()
        c.execute("PRAGMA journal_mode=WAL")
        c.execute('''CREATE TABLE IF NOT EXISTS inbox
                     (id INTEGER PRIMARY KEY AUTOINCREMENT,
                      message TEXT,
                      sender TEXT,
                      timestamp INTEGER,
                      is_read INTEGER DEFAULT 0)''')
        conn.commit()
        conn.close()

        # --- OUTBOX DB ---
        outbox_file = f"{db_path}/sms_outbox.db"
        conn = sqlite3.connect(outbox_file)
        c = conn.cursor()
        c.execute("PRAGMA journal_mode=WAL")
        c.execute('''CREATE TABLE IF NOT EXISTS outbox
                     (id INTEGER PRIMARY KEY AUTOINCREMENT,
                      message TEXT,
                      timestamp INTEGER)''')
        conn.commit()
        conn.close()

        print("[CORE] Databases initialized successfully.")

class IncomingCall(BaseException):
    """Raised out of read_keypress when the modem reports a RING.

    Derives from BaseException on purpose, exactly like KeyboardInterrupt:
    apps catch `Exception` around their own loops, and a ringing phone
    must not be swallowed by a game's error handling. Unwinding runs the
    apps' finally blocks, so MusicPlayer/Koki release the ALSA device
    before the ringtone starts. The 3310 didn't multitask either.
    """

    def __init__(self, number=None):
        super().__init__(number)
        self.number = number


# --- UI LOGIC ---
class NeoDCT_UI:
    def __init__(self, fb_driver):
        init_databases()       
    
        self.modem = ModemService()
        self.battery = BatteryService()
        self.notify = NotifyService()
        self._unread_sms = self._count_unread_sms()
        self._handling_call = False   # guards the ring UI from re-raising
        self._ring_seen_at = None
        self._shutting_down = False
        self.dial_buffer = ""
        
        self.DEV_KEYMAP = {
            2: "1", 3: "2", 4: "3", 5: "4", 6: "5", 
            7: "6", 8: "7", 9: "8", 10: "9", 11: "0",
            12: "-", 52: ".", 51: ",", 42: "*", 43: "#", 28: "#"
        }
        self.W = UI_WIDTH
        self.H = UI_HEIGHT
        self.SOFTKEY_H = SOFTKEY_HEIGHT
        self.content_bottom = self.H - self.SOFTKEY_H
        self.keypad_fd = None
        self.keypad_path = None
        self.matrix_input = None

        matrix_cfg = _load_matrix_keymap(KEYMAP_PATH)
        if matrix_cfg:
            driver = matrix_cfg.get("driver", "gpiozero-matrix")
            if driver == "pcf8575-i2c":
                i2c_dev = f"/dev/i2c-{matrix_cfg['i2c_bus']}"
                if not os.path.exists(i2c_dev):
                    print(f"[INPUT] Keymap wants {driver}, but {i2c_dev} does not exist.")
                else:
                    try:
                        from System.hw.pcf8575_keypad import I2CMatrixKeypadInput
                        self.matrix_input = I2CMatrixKeypadInput(matrix_cfg)
                        print(
                            f"[INPUT] I2C matrix input active from {matrix_cfg['path']} "
                            f"(bus={matrix_cfg['i2c_bus']} addr=0x{matrix_cfg['i2c_addr']:02X} "
                            f"rows={matrix_cfg['row_pins']} cols={matrix_cfg['col_pins']})."
                        )
                    except Exception as exc:
                        self.matrix_input = None
                        print(f"[INPUT] I2C matrix init failed; falling back to evdev: {exc}")
            elif GPIOZERO_IMPORT_ERROR is not None:
                print(f"[INPUT] Keymap present, but gpiozero is unavailable: {GPIOZERO_IMPORT_ERROR}")
            elif not _gpio_available():
                print("[INPUT] Keymap present, but no /dev/gpiochip* devices were found.")
            else:
                try:
                    self.matrix_input = MatrixKeypadInput(matrix_cfg)
                    print(
                        f"[INPUT] Matrix input active from {matrix_cfg['path']} "
                        f"(rows={matrix_cfg['row_pins']} cols={matrix_cfg['col_pins']})."
                    )
                except Exception as exc:
                    self.matrix_input = None
                    print(f"[INPUT] Matrix init failed; falling back to evdev: {exc}")

        self.keypad_path = _discover_keypad_path()
        try:
            self.keypad_fd = os.open(self.keypad_path, os.O_RDONLY | os.O_NONBLOCK)
        except Exception as e:
            print(f"[INPUT] Failed opening {self.keypad_path}: {e}")
            if self.keypad_path != KEYPAD_PATH:
                try:
                    print(f"[INPUT] Falling back to {KEYPAD_PATH}")
                    self.keypad_path = KEYPAD_PATH
                    self.keypad_fd = os.open(self.keypad_path, os.O_RDONLY | os.O_NONBLOCK)
                except Exception as e2:
                    print(f"[INPUT] Evdev fallback failed: {e2}")
                    self.keypad_fd = None
            else:
                self.keypad_fd = None

        if self.keypad_fd is not None:
            print(f"[INPUT] Listening on {self.keypad_path}")
        elif self.matrix_input is None:
            print("[INPUT] WARNING: no active input backend.")
        self.softkey = SoftKeyBar(self)

        self.fb = fb_driver
        self.canvas = Image.new("RGB", (self.W, self.H), "black")
        self.draw = ImageDraw.Draw(self.canvas)
        
        self.state = "HOME"
        
        font_path = "/NeoDCT/System/ui/resources/fonts/font.ttf"
        try:
            self.font_s = ImageFont.truetype(font_path, 14)
            self.font_md = ImageFont.truetype(font_path, 18) 
            self.font_n = ImageFont.truetype(font_path, 20)
            self.font_xl = ImageFont.truetype(font_path, 24) 
            print("[UI] Custom font loaded.")
        except:
            print("[UI] Font load failed, using default.")
            self.font_s = ImageFont.load_default()
            self.font_md = ImageFont.load_default()
            self.font_n = ImageFont.load_default()
            self.font_xl = ImageFont.load_default()

        show_alpha_security_notice_once(self)
        self.home_layout = self.load_layout("/NeoDCT/System/ui/resources/ui_home.json")
        self.image_cache = {}
        
        # --- WALLPAPER LOADING ---
        wallpaper_setting = get_setting("system.ui.wallpaper", "NONE")
        wallpaper_path = None
        if wallpaper_setting and wallpaper_setting.upper() != "NONE":
            if wallpaper_setting.lower().endswith((".jpg", ".jpeg")) and os.path.exists(wallpaper_setting):
                wallpaper_path = wallpaper_setting
            else:
                print(f"[UI] Invalid wallpaper setting: {wallpaper_setting}")
                if os.path.exists(WALLPAPER_PATH):
                    wallpaper_path = WALLPAPER_PATH

        self.wallpaper = self.load_wallpaper(wallpaper_path) if wallpaper_path else None
        
        engineering_mode = _setting_is_enabled(
            get_setting("system.ui.engineering_mode", "ON"),
            default=True,
        )
        self.engineering_mode = engineering_mode

        self.apps = []
        app_dirs = ["/NeoDCT/System/apps"]
        if engineering_mode:
            app_dirs.append("/NeoDCT/System/engineering/apps")

        try:
            for app_dir in app_dirs:
                self._scan_apps_from_dir(app_dir)
            self.apps.sort(key=lambda x: x["id"])
        except Exception as e:
            print(f"[OS] App scan error: {e}")

    def _scan_apps_from_dir(self, app_dir):
        if not os.path.exists(app_dir):
            try:
                os.makedirs(app_dir)
            except:
                return

        for folder in os.listdir(app_dir):
            manifest_path = f"{app_dir}/{folder}/manifest.json"
            if not os.path.exists(manifest_path):
                continue
            try:
                with open(manifest_path, "r") as f:
                    data = json.load(f)
                    self.apps.append({
                        "name": data.get("name", folder),
                        "icon": f"{app_dir}/{folder}/" + data.get("icon", "icon.png"),
                        "path": f"{app_dir}/{folder}",
                        "exec": data.get("exec", "main.py"),
                        "id": int(data.get("id", 999))
                    })
            except:
                pass

    def load_layout(self, path):
        try:
            with open(path, 'r') as f:
                return json.load(f)
        except: return None

    def load_wallpaper(self, path):
        """ Loads wallpaper, resizes to fit, and dims it by 70%. """
        if not os.path.exists(path):
            print("[UI] No wallpaper found.")
            return None
        
        try:
            print(f"[UI] Loading wallpaper: {path}")
            # Ensure we can load truncated/imperfect JPEGs
            ImageFile.LOAD_TRUNCATED_IMAGES = True 
            
            img = Image.open(path)
            img.load() # Force load pixel data
            
            # Convert and Resize
            img = img.convert("RGB")
            img = img.resize((self.W, self.H), Image.Resampling.LANCZOS)
            
            # Dim the image by 70% (Brightness 0.3) for text readability
            enhancer = ImageEnhance.Brightness(img)
            dimmed_img = enhancer.enhance(0.3)
            
            return dimmed_img

        except Exception as e:
            print(f"[UI] Wallpaper load error: {e}")
            return None
            
            
    IMAGE_CACHE_MAX = 32

    def get_image(self, path, max_size=None, scale=None):
        """Load (and cache) an RGBA image.

        max_size: optional int -- downscale so neither side exceeds it and
        cache the SCALED copy under a separate key. Callers that only ever
        draw an icon small (AppSelector) should pass this so the cache holds
        ~KB thumbnails instead of full-size art; on 64 MB the full-size icon
        set alone is ~1 MB of RGBA.
        scale: optional float -- exact-ratio resize, cached at display size
        only (status-bar sprites via _get_status_icon).
        """
        if path.startswith("/home"):
            if "System" in path:
                rel_path = path.split("NeoDCT")[-1]
                clean_path = "/NeoDCT" + rel_path
            else: clean_path = path
        else: clean_path = path

        if max_size is not None:
            key = f"{clean_path}@{int(max_size)}"
        elif scale is not None:
            key = f"{clean_path}@x{scale:g}"
        else:
            key = clean_path
        if key in self.image_cache:
            return self.image_cache[key]

        try:
            img = Image.open(clean_path).convert("RGBA")
            if max_size is not None and (img.width > max_size or img.height > max_size):
                img.thumbnail((max_size, max_size), Image.Resampling.LANCZOS)
            elif scale is not None:
                w = max(1, int(img.width * scale))
                h = max(1, int(img.height * scale))
                if (w, h) != img.size:
                    img = img.resize((w, h), Image.Resampling.LANCZOS)
            self._cache_put(key, img)
            return img
        except: return None

    def _cache_put(self, key, img):
        # FIFO cap so the cache cannot grow without bound on 64 MB.
        if len(self.image_cache) >= self.IMAGE_CACHE_MAX:
            self.image_cache.pop(next(iter(self.image_cache)))
        self.image_cache[key] = img

    def get_text_size(self, text, font):
        bbox = self.draw.textbbox((0, 0), text, font=font)
        return (bbox[2] - bbox[0], bbox[3] - bbox[1])

    # --- HOME SCREEN ---
    def render_element(self, el):
        if el["type"] == "text":
            text = el["text"]
            if text == "12:00": text = time.strftime("%H:%M")
            elif text == "No Service":
                # Registered modem -> real carrier name (Tello/T-Mobile);
                # otherwise the layout's placeholder stands.
                carrier = self.modem.operator_display()
                if carrier: text = carrier
            
            # Font Selection
            if el["font_size"] >= 20: font = self.font_xl
            elif el["font_size"] >= 16: font = self.font_n
            else: font = self.font_s
            
            w, h = self.get_text_size(text, font)
            x = int((el["x"] / 240.0) * self.W)
            y = int((el["y"] / 240.0) * self.H)
            
            if "center_h" in el["anchor"]: x -= w // 2
            elif "right" in el["anchor"]: x -= w
            
            self.draw.text((x, y), text, font=font, fill=el["color"])

        elif el["type"] == "icon_set":
            # Battery gauge and cell signal are live (fuel gauge / modem
            # AT+CSQ). In modem Simulation Mode signal_level() is None and
            # the layout's sim_val still applies, like before.
            count = el.get("count", 5)
            bat_label = None
            if el.get("prefix") == "bat":
                val = self.battery.level()
                if not self.battery.hardware:
                    bat_label = "?"
            elif el.get("prefix") == "sig":
                bars = self.modem.signal_level()
                val = int(el.get("sim_val", 3)) if bars is None else bars
            else:
                val = int(el.get("sim_val", 3))
            custom_path = el.get("custom_images", {}).get(str(val))
            x = int((el["x"] / 240.0) * self.W)
            y = int((el["y"] / 240.0) * self.H)

            vis_box = None
            if custom_path:
                img = self._get_status_icon(custom_path)
                if img:
                    self.canvas.paste(img, (x, y), img)
                    if bat_label is not None:
                        vis_box = img.getbbox() or (0, 0, img.width, img.height)
            else:
                step = max(3, int(self.W * 0.021))
                for i in range(count):
                    h = (i + 1) * 3
                    color = "white" if i <= val else "#333333"
                    bx = x + (i * step)
                    self.draw.rectangle((bx, y + 15 - h, bx + 3, y + 15), fill=color)
                vis_box = (0, 0, count * step, 15)

            # Draw the '?' even when the sprite failed to load, so a missing
            # asset can't silently hide the "no battery" state.
            if bat_label is not None:
                self._draw_status_label(bat_label, x, y, vis_box or (0, 0, 12, 15))

    def _get_status_icon(self, path):
        """Status-bar sprite scaled by H/240 (home layout coords are authored
        for a 240px-tall UI), cached at display size only."""
        return self.get_image(path, scale=self.H / 240.0)

    def _draw_status_label(self, text, icon_x, icon_y, vis_box):
        """Draw a small status value (e.g. battery '85%' or '?') just left of the
        icon's visible (opaque) region, vertically centered on it. Aligning to
        the opaque box -- not the full sprite -- keeps it next to a battery
        graphic that only fills the lower part of a tall image."""
        left, top, right, bottom = vis_box
        tw, th = self.get_text_size(text, self.font_s)
        tx = max(0, icon_x + left - tw - 4)
        ty = icon_y + top + max(0, ((bottom - top) - th) // 2)
        self.draw.text((tx, ty), text, font=self.font_s, fill="white")

    def render_home(self):
        # 1. Background Logic
        if self.wallpaper:
            self.canvas.paste(self.wallpaper, (0, 0))
        elif self.home_layout:
            bg_path = self.home_layout.get("background")
            if bg_path:
                # Convert/resize once and keep the screen-sized copy;
                # render_home runs every frame and a per-frame LANCZOS
                # resize of a full-screen image is pure waste.
                bg = getattr(self, "_home_bg", None)
                if bg is None:
                    bg = self.get_image(bg_path)
                    if bg:
                        bg = bg.convert("RGB").resize((self.W, self.H), Image.Resampling.LANCZOS)
                        self._home_bg = bg
                if bg:
                    self.canvas.paste(bg, (0, 0))
            else:
                self.draw.rectangle((0, 0, self.W, self.H), fill="black")
        else:
            self.draw.rectangle((0, 0, self.W, self.H), fill="black")

        # 2. Render Elements
        if self.home_layout:
            for el in self.home_layout["elements"]:
                # The carrier line makes room for the "N messages
                # received" banner, like on the 3310.
                if self.notify.active() and el.get("type") == "text" \
                        and el.get("text") == "No Service":
                    continue
                self.render_element(el)
        else:
            self.draw.text((10,10), "No Layout Found", fill="red")

        # 3. Notification layer (NotifyService): flashing envelope in the
        # status strip while unread mail exists, banner while undismissed.
        if self.notify.active() or self._unread_sms > 0:
            if int(time.time() * 2) % 2 == 0:
                env = self._get_status_icon("/NeoDCT/System/ui/resources/img/envelope.png")
                if env:
                    icon_scale = self.H / 240.0
                    self.canvas.paste(env, (int(46 * icon_scale) + 7,
                                            int(10 * icon_scale)), env)
        if self.notify.active():
            y = max(46, int(self.content_bottom * 0.34))
            for line in self.notify.banner_lines():
                self.draw.text((30, y), line, font=self.font_n, fill="white")
                y += 24

    def render_home_dialing(self):
        if self.wallpaper:
            self.canvas.paste(self.wallpaper, (0, 0))
        else:
            self.draw.rectangle((0, 0, self.W, self.H), fill="black")
        
        if self.dial_buffer:
            w, h = self.get_text_size(self.dial_buffer, self.font_xl)
            y = max(50, int(self.content_bottom * 0.35))
            self.draw.text(((self.W - w)//2, y), self.dial_buffer, font=self.font_xl, fill="white")


    def launch_app(self, app):
        path = os.path.join(app["path"], app["exec"])
        try:
            spec = importlib.util.spec_from_file_location("neodct_app", path)
            if spec is None or spec.loader is None:
                print(f"[OS] App load failed: {path}")
                return

            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)

            if hasattr(module, "run"):
                module.run(self)
            else:
                print(f"[OS] App has no run(ui): {path}")
        except (KeyboardInterrupt, IncomingCall):
            # A ringing phone is not an app crash: let it unwind to the
            # core loop (the app's own finally blocks already ran).
            raise
        except BaseException:
            app_name = app.get("name", "(unknown)")
            print(f"[OS] App crashed: {app_name} ({path})")
            traceback.print_exc()
            show_app_crash(self, app_name=app_name, exc_info=sys.exc_info())
        finally:
            gc.collect()
            # Messages may have been read (or arrived) inside the app.
            self._unread_sms = self._count_unread_sms()

    def render_menu(self):
        try:
            menu = AppSelector("Main Menu", self.apps, self, background=self.wallpaper)
            choice = menu.show()
            if choice != -1:
                print(f"[OS] Launching App ID: {choice}")
                self.launch_app(self.apps[choice])
        except (KeyboardInterrupt, IncomingCall):
            raise
        except BaseException:
            print("[OS] Menu crashed")
            traceback.print_exc()
            log_crash("menu", sys.exc_info())
        finally:
            # Always unwind menu state so one bad app/menu event cannot trap the core loop.
            self.state = "HOME"

    def update(self):
        if self.state == "HOME":
            self.render_home()
            self.softkey.update("Read" if self.notify.active() else "Menu",
                                present=False)
            self.fb.update(self.canvas)

        elif self.state == "HOME_DIALING":
            self.render_home_dialing()
            self.softkey.update("Call", present=False)
            self.fb.update(self.canvas)

        elif self.state == "MENU":
            self.render_menu()

    def _battery_tick(self):
        """Sample the fuel gauge and enforce the shutdown floor.

        Lives in read_keypress because every screen (home loop, menus, apps,
        modal dialogs) funnels through it, so the 3.20 V cutoff holds
        system-wide. The poll itself is rate-limited inside BatteryService.
        """
        if self._shutting_down:
            return
        try:
            event = self.battery.poll()
        except Exception as exc:
            print(f"[BATT] Poll failed: {exc}")
            return
        if event == "shutdown":
            self._shutdown_low_battery()

    def _modem_tick(self):
        """Pump the modem's URC/CSQ polling from the same chokepoint as the
        battery: read_keypress runs on every screen, so RING and signal
        updates keep flowing inside apps and dialogs too. Rate limiting
        lives inside ModemService.poll()."""
        try:
            self.modem.poll()
        except Exception as exc:
            print(f"[MODEM] Poll failed: {exc}")
            return
        while True:
            event = self.modem.take_pending_event()
            if event is None:
                break
            try:
                if not self._handle_modem_event(event):
                    break   # port busy: event requeued, retry next tick
            except Exception as exc:
                print(f"[MODEM] Event {event[0]} failed: {exc}")

    def _handle_modem_event(self, event):
        """Turn modem events into inbox rows + notifications.

        Returns False when the AT port was busy (S45modem/atcmd) and the
        event was pushed back for a later tick."""
        kind, data = event
        if kind == "sms_received":
            status, record = self.modem.fetch_sms(data)
            if status == "busy":
                self.modem.requeue_event(event)
                return False
            if status == "ok":
                row_id = self._store_incoming_sms(record["sender"], record["body"])
                self.notify.post_sms(row_id)
                self._unread_sms += 1
        elif kind == "sms_stored_check":
            status, records = self.modem.read_stored_sms()
            if status == "busy":
                self.modem.requeue_event(event)
                return False
            for i, record in enumerate(records):
                row_id = self._store_incoming_sms(record["sender"], record["body"])
                self.notify.post_sms(row_id, tone=(i == 0))  # one beep, not N
                self._unread_sms += 1
        elif kind == "sms_sim":
            sender, body = data
            row_id = self._store_incoming_sms(sender, body)
            self.notify.post_sms(row_id)
            self._unread_sms += 1
        # Call events (incoming/connected/ended/...) are left alone until
        # the incoming-call UI lands.
        return True

    def _store_incoming_sms(self, sender, body):
        conn = sqlite3.connect("/NeoDCT/User/db/sms_inbox.db")
        c = conn.cursor()
        c.execute('''CREATE TABLE IF NOT EXISTS inbox
                     (id INTEGER PRIMARY KEY AUTOINCREMENT,
                      message TEXT,
                      sender TEXT,
                      timestamp INTEGER,
                      is_read INTEGER DEFAULT 0)''')
        c.execute("INSERT INTO inbox (message, sender, timestamp, is_read) "
                  "VALUES (?, ?, ?, 0)", (body, sender, int(time.time())))
        row_id = c.lastrowid
        conn.commit()
        conn.close()
        print(f"[NOTIFY] SMS stored (id {row_id}) from {sender}")
        return row_id

    def _ring_tick(self):
        """Interrupt whatever is running when the phone rings.

        Called from read_keypress, so it fires on every screen: the home
        loop, menus, modal dialogs and apps all funnel through here.
        """
        if self._handling_call:
            return   # already showing the ring/call UI
        if self.modem.state != "RINGING":
            self._ring_seen_at = None
            return
        if self._ring_seen_at is None:
            self._ring_seen_at = time.monotonic()
        # +CLIP lands a beat after RING; give it one poll cycle so the
        # screen opens with the caller's name instead of "Unknown".
        if self.modem.caller_id is None \
                and (time.monotonic() - self._ring_seen_at) < 0.6:
            return
        raise IncomingCall(self.modem.caller_id)

    def handle_incoming_call(self, number):
        """Ring, then answer/decline. Owns the ringer and the call UI."""
        self._handling_call = True
        self.state = "HOME"
        name = None
        try:
            print(f"[CORE] Incoming call from {number or 'unknown'}")
            self.notify.start_ring()
            result = incoming_ui.show_incoming(self, number)
            self.notify.stop_ring()

            if result == "answered":
                if self.modem.answer():
                    dialer_ui.show_calling(self, number or "", name)
                    # show_calling returns on End or remote hangup; make
                    # sure the line is really down either way.
                    self.modem.hangup()
                else:
                    print("[CORE] Answer failed; releasing the call.")
                    self.modem.hangup()
            elif result == "declined":
                print("[CORE] Call declined.")
                self.modem.hangup()
            else:
                print("[CORE] Caller hung up before we answered.")
        except KeyboardInterrupt:
            raise
        except BaseException:
            print("[CORE] Incoming call flow crashed")
            traceback.print_exc()
            try:
                self.modem.hangup()
            except Exception:
                pass
        finally:
            self.notify.stop_ring()
            self._handling_call = False
            self.state = "HOME"

    def _play_dtmf(self, char):
        """Dial-pad beep while typing a number. Pure vibes: VoLTE never
        hears these, they just sound right."""
        name = {"*": "star", "#": "hash"}.get(char, char)
        if name.isdigit() or name in ("star", "hash"):
            self.notify.play_tone(f"/NeoDCT/System/tones/dtmf/{name}.wav")

    def _count_unread_sms(self):
        try:
            conn = sqlite3.connect("/NeoDCT/User/db/sms_inbox.db")
            n = conn.execute(
                "SELECT COUNT(*) FROM inbox WHERE is_read = 0").fetchone()[0]
            conn.close()
            return n
        except Exception:
            return 0

    def _open_notification(self):
        """Home-screen Read softkey: jump to the new message (or inbox)."""
        kind = self.notify.kind()
        count = self.notify.count()
        target = self.notify.latest_data()
        self.notify.dismiss()
        if kind != "sms":
            return
        app = next((a for a in self.apps if a.get("name") == "Messages"), None)
        path = (os.path.join(app["path"], app["exec"]) if app
                else "/NeoDCT/System/apps/Messages/main.py")
        try:
            spec = importlib.util.spec_from_file_location("neodct_app", path)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            if count == 1 and target is not None:
                module.open_message(self, target)
            else:
                module.open_inbox(self)
        except KeyboardInterrupt:
            raise
        except BaseException:
            print("[NOTIFY] Read flow crashed")
            traceback.print_exc()
        finally:
            self._unread_sms = self._count_unread_sms()

    def _shutdown_low_battery(self):
        self._shutting_down = True
        vcell = self.battery.vcell() or 0.0
        print(f"[BATT] Battery empty (VCELL={vcell:.3f} V). Graceful shutdown.")
        try:
            show_error(self, "Battery empty. Shutting down...",
                       title="LOW BATTERY", button_text=None, wait_for_ack=False)
        except Exception:
            traceback.print_exc()
        time.sleep(3)
        os.sync()
        rc = os.system("poweroff")
        if rc != 0:
            print(f"[BATT] poweroff failed (rc={rc}); resuming so dev sessions survive.")
            self._shutting_down = False
            return
        # Freeze on the shutdown notice while init takes us down.
        while True:
            time.sleep(1)

    def show_pending_battery_warning(self):
        # Deferred to the home loop so a modal never lands mid-frame inside
        # an app; latched warnings pop as soon as we are back on HOME.
        if self.state not in ("HOME", "HOME_DIALING"):
            return
        warning = self.battery.take_pending_warning()
        if warning is None:
            return
        message = "BATTERY CRITICALLY LOW!" if warning == "critical" else "LOW BATTERY!"
        vcell = self.battery.vcell() or 0.0
        print(f"[BATT] Warning: {message} (VCELL={vcell:.3f} V)")
        show_error(self, message, title="Battery")

    def read_keypress(self, timeout=0.1):
        self._battery_tick()
        self._modem_tick()
        self._ring_tick()

        # Primary path: GPIO matrix keymap (if present and initialized).
        # Backward-compatible fallback: still read evdev keyboard events.
        if self.matrix_input is not None:
            key = self.matrix_input.read_key(timeout)
            if key is not None:
                return key
            # No matrix key this cycle; continue to evdev fallback if available.

        if self.keypad_fd is None:
            # With no evdev device AND no matrix backend, nothing above waited,
            # so sleep out the timeout to avoid a 100% CPU busy-loop in wait_for_key().
            if self.matrix_input is None:
                time.sleep(max(0.0, timeout))
            return None

        try:
            r, _, _ = select.select([self.keypad_fd], [], [], timeout)
        except Exception:
            return None
        if not r:
            return None

        try:
            data = os.read(self.keypad_fd, 24)
        except Exception:
            return None

        if len(data) == 24:
            sec, usec, etype, code, val = struct.unpack('llHHI', data)
        elif len(data) == 16:
            sec, usec, etype, code, val = struct.unpack('IIHHI', data)
        else:
             return None

        if etype == 1 and val == 1:
            return code
        return None

    def wait_for_key(self):
        while True:
            key = self.read_keypress(0.1)
            if key is not None:
                return key

    def handle_input(self, code):
        # Notification banner owns enter + C on the home screen: Read
        # opens the message, C clears the banner (mail stays unread and
        # the envelope keeps flashing). Everything else acts as usual.
        if self.state == "HOME" and self.notify.active():
            if code == 28:
                self._open_notification()
                return
            if code == 14:
                self.notify.dismiss()
                return

        if code == 28:
            if self.state == "HOME":
                self.state = "MENU"
            elif self.state == "HOME_DIALING":
                self.modem.dial(self.dial_buffer)
                dialer_ui.show_calling(self, self.dial_buffer)
                self.dial_buffer = ""
                self.state = "HOME"

        elif code == 14:
            if self.state == "HOME_DIALING":
                self.dial_buffer = self.dial_buffer[:-1]
                if not self.dial_buffer:
                    self.state = "HOME"

        elif code in (103, 108) and self.state == "HOME":
            result = contact_manager.show_contact_selector(self, title="Select", btn_text="Call")
            if result:
                # show_contact_selector returns (contact_row, selection_index);
                # contact_row is (id, name, number, speed_dial).
                target, _ = result
                number = target[2]
                name = target[1]
                self.modem.dial(number)
                dialer_ui.show_calling(self, number, name)

        elif code in self.DEV_KEYMAP and self.state in ("HOME", "HOME_DIALING"):
            char = self.DEV_KEYMAP[code]
            self.dial_buffer += char
            self.state = "HOME_DIALING"
            self._play_dtmf(char)

def run(fb):
    # First boot with an i2c keypad but no keymap: run the on-screen setup
    # wizard (it exec-restarts the UI after saving, so it may not return).
    try:
        from System.hw.i2c_keypad_setup import maybe_run_first_time_setup
        maybe_run_first_time_setup(fb)
    except Exception:
        print("[SETUP] First-time keypad setup failed; continuing boot.")
        traceback.print_exc()

    ui = NeoDCT_UI(fb)
    print("[CORE] Entering Main Loop...")

    while True:
        try:
            ui.update()
            ui.show_pending_battery_warning()
            key = ui.read_keypress(0.1)
            if key is not None:
                print(f"[INPUT] Code: {key}")
                ui.handle_input(key)
        except IncomingCall as call:
            ui.handle_incoming_call(call.number)
        except KeyboardInterrupt:
            raise
        except BaseException:
            print("[CORE] Unhandled exception in main loop")
            traceback.print_exc()
            log_crash("core-main-loop", sys.exc_info())
            time.sleep(0.1)

if __name__ == "__main__":
    fb = Framebuffer()
    run(fb)

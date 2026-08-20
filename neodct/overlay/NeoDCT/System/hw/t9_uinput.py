"""
t9_uinput.py -- T9 keypad -> Linux virtual-keyboard bridges.

Two consumers, both keypad-only hardware talking to a program that expects
a keyboard: LinuxShell (start_shell_bridge, types into the console) and
the browser (start_browser_bridge, drives netsurf's cursor).

On keypad-only hardware the console on /dev/tty2 has no keyboard, so this
module creates a virtual keyboard via /dev/uinput and mirrors i2c keypad
presses into it through the shared multi-tap engine (System.hw.t9_engine):

  digits 2-9   multi-tap letters (cycling sends backspace + new char)
  1 / 0        punctuation cycle / space
  #            cycles abc -> ABC -> 123
  * (123 mode) literal '*'
  enter/clear/arrows  pass straight through (NeoDCT keycodes ARE Linux
                      keycodes, so no translation table is needed)

The bridge must only run while LinuxShell owns the screen: the UI main
loop is blocked in p.wait() then, so the i2c bus is free. Start it with
start_shell_bridge(ui) -- returns None on QEMU/dev builds where a real
keyboard already reaches the console.

Pure stdlib (os/fcntl/struct/threading), same as the keypad driver.
"""

import fcntl
import os
import struct
import threading
import time

from System.hw.t9_engine import KEY_HASH, T9Engine

# linux/input-event-codes.h
EV_SYN = 0x00
EV_KEY = 0x01
SYN_REPORT = 0

KEY_BACKSPACE = 14
KEY_ENTER = 28
KEY_LEFTSHIFT = 42
KEY_UP = 103
KEY_LEFT = 105
KEY_RIGHT = 106
KEY_DOWN = 108

# linux/uinput.h ioctls
UI_SET_EVBIT = 0x40045564    # _IOW('U', 100, int)
UI_SET_KEYBIT = 0x40045565   # _IOW('U', 101, int)
UI_DEV_CREATE = 0x5501       # _IO('U', 1)
UI_DEV_DESTROY = 0x5502      # _IO('U', 2)

BUS_VIRTUAL = 0x06

# struct input_event: native long timeval adapts to 32-bit ARM (16 bytes)
# and the 64-bit host used by the test suite (24 bytes).
_INPUT_EVENT = struct.Struct("llHHi")
# struct uinput_user_dev: name[80], input_id (4x u16), ff_effects_max,
# absmax/absmin/absfuzz/absflat[64] each.
_USER_DEV = struct.Struct("80s4HI64i64i64i64i")

# US-layout char -> keycode, unshifted
_PLAIN = {
    "a": 30, "b": 48, "c": 46, "d": 32, "e": 18, "f": 33, "g": 34,
    "h": 35, "i": 23, "j": 36, "k": 37, "l": 38, "m": 50, "n": 49,
    "o": 24, "p": 25, "q": 16, "r": 19, "s": 31, "t": 20, "u": 22,
    "v": 47, "w": 17, "x": 45, "y": 21, "z": 44,
    "1": 2, "2": 3, "3": 4, "4": 5, "5": 6,
    "6": 7, "7": 8, "8": 9, "9": 10, "0": 11,
    " ": 57, "-": 12, "=": 13, "[": 26, "]": 27, ";": 39, "'": 40,
    "`": 41, "\\": 43, ",": 51, ".": 52, "/": 53, "\t": 15, "\n": 28,
}
# US-layout char -> keycode pressed with shift
_SHIFTED = {
    "!": 2, "@": 3, "#": 4, "$": 5, "%": 6,
    "^": 7, "&": 8, "*": 9, "(": 10, ")": 11,
    "_": 12, "+": 13, "{": 26, "}": 27, ":": 39, '"': 40,
    "~": 41, "|": 43, "<": 51, ">": 52, "?": 53,
}

# NeoDCT keypad codes forwarded to the console unchanged
PASSTHROUGH_CODES = (28, 14, 103, 105, 106, 108)  # enter, clear, arrows


def char_to_keypress(char):
    """Map one char to (keycode, needs_shift), or None if untypeable."""
    if char in _PLAIN:
        return (_PLAIN[char], False)
    if char in _SHIFTED:
        return (_SHIFTED[char], True)
    lower = char.lower()
    if char.isalpha() and lower in _PLAIN:
        return (_PLAIN[lower], True)
    return None


class UInputKeyboard:
    """Minimal virtual keyboard on /dev/uinput.

    Pass fd= to skip device setup (tests write into a pipe instead)."""

    def __init__(self, path="/dev/uinput", fd=None, name="neodct-t9-keypad"):
        if fd is not None:
            self.fd = fd
            self._owns_device = False
            return

        self.fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        self._owns_device = True
        try:
            fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
            for code in self._needed_keycodes():
                fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
            os.write(self.fd, _USER_DEV.pack(
                name.encode("ascii"), BUS_VIRTUAL, 0x1, 0x1, 1, 0,
                *([0] * 256)))
            fcntl.ioctl(self.fd, UI_DEV_CREATE)
            # Give the kernel/console a moment to bind the new device.
            time.sleep(0.2)
        except Exception:
            os.close(self.fd)
            self.fd = None
            raise

    @staticmethod
    def _needed_keycodes():
        codes = set(_PLAIN.values()) | set(_SHIFTED.values())
        codes |= {KEY_LEFTSHIFT}
        codes |= set(PASSTHROUGH_CODES)
        return sorted(codes)

    def _emit(self, etype, code, value):
        now = time.time()
        sec = int(now)
        usec = int((now - sec) * 1_000_000)
        os.write(self.fd, _INPUT_EVENT.pack(sec, usec, etype, code, value))

    def _syn(self):
        self._emit(EV_SYN, SYN_REPORT, 0)

    def send_key(self, code, shift=False):
        if shift:
            self._emit(EV_KEY, KEY_LEFTSHIFT, 1)
            self._syn()
        self._emit(EV_KEY, code, 1)
        self._syn()
        self._emit(EV_KEY, code, 0)
        self._syn()
        if shift:
            self._emit(EV_KEY, KEY_LEFTSHIFT, 0)
            self._syn()

    def type_char(self, char):
        hit = char_to_keypress(char)
        if hit is None:
            return False
        code, shift = hit
        self.send_key(code, shift=shift)
        return True

    def backspace(self):
        self.send_key(KEY_BACKSPACE)

    def close(self):
        if self.fd is None:
            return
        if self._owns_device:
            try:
                fcntl.ioctl(self.fd, UI_DEV_DESTROY)
            except OSError:
                pass
        try:
            os.close(self.fd)
        finally:
            self.fd = None


class T9ShellBridge:
    """Polls the keypad in a thread and feeds the console keyboard."""

    def __init__(self, read_key, keyboard, engine=None, poll_timeout=0.05):
        self._read_key = read_key
        self.keyboard = keyboard
        self.engine = engine or T9Engine()
        self._poll_timeout = poll_timeout
        self._stop = threading.Event()
        self._thread = None

    def handle_code(self, code):
        if code in PASSTHROUGH_CODES:
            self.engine.reset()
            self.keyboard.send_key(code)
            return
        op = self.engine.press(code)
        if op is None:
            return
        kind, value = op
        if kind == "append":
            self.keyboard.type_char(value)
        elif kind == "replace":
            self.keyboard.backspace()
            self.keyboard.type_char(value)
        # "mode": nothing to send; the shell prompt has no indicator

    def start(self):
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._loop, name="t9-shell-bridge", daemon=True)
        self._thread.start()

    def _loop(self):
        while not self._stop.is_set():
            try:
                code = self._read_key(self._poll_timeout)
            except Exception:
                time.sleep(0.1)
                continue
            if code is None:
                continue
            try:
                self.handle_code(code)
            except Exception:
                pass

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
            self._thread = None
        try:
            self.keyboard.close()
        except Exception:
            pass


def start_shell_bridge(ui, keyboard_factory=None):
    """Start the keypad->console bridge for LinuxShell.

    Returns the running bridge, or None when there is no i2c keypad
    (QEMU/dev: the real keyboard already reaches the console) or uinput
    is unavailable. Caller must stop() the bridge when the shell exits."""
    matrix = getattr(ui, "matrix_input", None)
    if matrix is None:
        return None
    if keyboard_factory is None:
        keyboard_factory = UInputKeyboard
    try:
        keyboard = keyboard_factory()
    except Exception as exc:
        print(f"[T9] uinput keyboard unavailable: {exc}")
        return None
    bridge = T9ShellBridge(matrix.read_key, keyboard)
    bridge.start()
    return bridge


# --- browser bridge --------------------------------------------------------

# Cursor mode is a fourth stop on the # cycle rather than a separate toggle
# key, because # is already the only "change what the keys mean" key the
# phone has and users know it as that.
CURSOR_MODE = "nav"

# Keypad digit -> arrow. The enrolment in System/hw/i2c_keypad_setup.py
# collects Up and Down and no Left or Right at all, so netsurf can never
# see a horizontal arrow unless the number pad stands in for one.
BROWSER_NAV_CODES = {
    3: KEY_UP,      # keypad 2
    5: KEY_LEFT,    # keypad 4
    6: KEY_ENTER,   # keypad 5 -- follow the highlighted link
    7: KEY_RIGHT,   # keypad 6
    9: KEY_DOWN,    # keypad 8
}


class T9BrowserBridge(T9ShellBridge):
    """Keypad bridge for netsurf: a d-pad until you ask to type.

    Same wiring as the shell bridge, but the number pad drives the cursor
    instead of typing, so a page can be scrolled and links followed on a
    keypad that has no arrows. # walks one cycle through

        nav -> abc -> ABC -> 123 -> nav

    so text entry for a URL is still reachable and returns to the cursor
    the same way it was left.
    """

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._pos = 0            # 0 = cursor, 1..n = engine.modes[pos - 1]

    @property
    def mode(self):
        if self._pos == 0:
            return CURSOR_MODE
        return self.engine.modes[self._pos - 1]

    @property
    def cursor_mode(self):
        return self._pos == 0

    def cycle_mode(self):
        """Advance the # cycle, treating cursor as the mode before abc."""
        self.engine.reset()
        self._pos = (self._pos + 1) % (len(self.engine.modes) + 1)
        if self._pos:
            self.engine.set_mode_index(self._pos - 1)
        return self.mode

    def handle_code(self, code):
        if code == KEY_HASH:
            self.cycle_mode()
            return
        if self.cursor_mode:
            arrow = BROWSER_NAV_CODES.get(code)
            if arrow is not None:
                self.keyboard.send_key(arrow)
            elif code in PASSTHROUGH_CODES:
                self.keyboard.send_key(code)
            # every other key is inert here: typing a letter by accident
            # while scrolling is worse than the press doing nothing
            return
        super().handle_code(code)


def start_browser_bridge(ui, keyboard_factory=None):
    """Start the keypad->netsurf bridge. Same contract as
    start_shell_bridge: None when there is no i2c keypad (QEMU/dev, where
    a real keyboard already reaches netsurf) or uinput is unavailable."""
    matrix = getattr(ui, "matrix_input", None)
    if matrix is None:
        return None
    if keyboard_factory is None:
        keyboard_factory = UInputKeyboard
    try:
        keyboard = keyboard_factory()
    except Exception as exc:
        print(f"[T9] uinput keyboard unavailable: {exc}")
        return None
    bridge = T9BrowserBridge(matrix.read_key, keyboard)
    bridge.start()
    return bridge

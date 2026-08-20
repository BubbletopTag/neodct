# NeoDCT framework.py

import select
import os
import math
import time

from PIL import Image, ImageDraw

from System.hw import t9_engine


def _t9_active(ui):
    """T9 multi-tap runs only on the real i2c keypad; a dev keyboard
    (QEMU) has full QWERTY so the DEV_KEYMAP path is used instead."""
    return getattr(ui, "matrix_input", None) is not None

DEFAULT_UI_W = 240
DEFAULT_UI_H = 175
DEFAULT_SOFTKEY_H = 30
APP_SELECTOR_ICON_MAX = 175


def _ui_width(ui):
    return int(getattr(ui, "W", DEFAULT_UI_W))


def _ui_height(ui):
    return int(getattr(ui, "H", DEFAULT_UI_H))


def _softkey_height(ui):
    return int(getattr(ui, "SOFTKEY_H", DEFAULT_SOFTKEY_H))


def _content_bottom(ui):
    return _ui_height(ui) - _softkey_height(ui)


def _header_divider_y(ui):
    return max(30, int(_ui_height(ui) * 0.11))


def _font_ladder(ui, *names):
    """The named ui fonts that actually exist, biggest first."""
    fonts = []
    for name in names:
        font = getattr(ui, name, None)
        if font is not None and font not in fonts:
            fonts.append(font)
    return fonts


def _fit_font(ui, text, max_w, fonts):
    """The largest of `fonts` that draws `text` inside max_w."""
    for font in fonts:
        if ui.get_text_size(text, font)[0] <= max_w:
            return font
    return fonts[-1]


def _ellipsize(ui, text, font, max_w):
    """Trim `text` until it fits, marking that something was cut."""
    if ui.get_text_size(text, font)[0] <= max_w:
        return text
    trimmed = text
    while trimmed and ui.get_text_size(trimmed + "...", font)[0] > max_w:
        trimmed = trimmed[:-1]
    return (trimmed + "...") if trimmed else text


def _wrap_lines(ui, text, font, max_w):
    """Word-wrap `text` to max_w pixels, keeping blank lines as blanks.

    A blank line comes back as "" so the caller can decide what a paragraph
    break is worth -- a full empty line of type is far too much on a screen
    this size.
    """
    def width(candidate):
        return ui.get_text_size(candidate, font)[0]

    lines = []
    for raw in (text or "").splitlines() or [""]:
        if not raw.strip():
            lines.append("")
            continue
        current = ""
        for word in raw.split(" "):
            if not word:
                continue
            candidate = word if not current else current + " " + word
            if width(candidate) <= max_w or not current:
                current = candidate
            else:
                lines.append(current)
                current = word
        lines.append(current)
    while lines and lines[-1] == "":
        lines.pop()
    return lines


class AppSelector:
    def __init__(self, title, items, ui, background=None):
        self.title = title
        self.items = items # This is now a LIST OF DICTS: [{"name": "Phonebook", "icon": "..."}]
        self.ui = ui
        self.background = background # Store the background image
        self.selected_index = 0
        
    def draw(self):
        screen_w = _ui_width(self.ui)
        screen_h = _ui_height(self.ui)
        softkey_h = _softkey_height(self.ui)
        content_bottom = _content_bottom(self.ui)
        header_y = _header_divider_y(self.ui)

        # 1. Background
        if self.background:
            self.ui.canvas.paste(self.background, (0, 0))
        else:
            self.ui.draw.rectangle((0, 0, screen_w, screen_h), fill="black")
        
        if not self.items:
            text = "No Apps"
            w, h = self.ui.get_text_size(text, self.ui.font_n)
            y = max(header_y, header_y + ((content_bottom - header_y - h) // 2))
            self.ui.draw.text(((screen_w - w) // 2, y), text, font=self.ui.font_n, fill="white")
            self.ui.fb.update(self.ui.canvas)
            return

        current_app = self.items[self.selected_index]
        
        # 2. Draw Header (App Name) - Centered, Medium
        name = current_app["name"]
        w, h = self.ui.get_text_size(name, self.ui.font_xl)
        title_y = header_y - 16
        self.ui.draw.text(((screen_w - w)//2, title_y), name, font=self.ui.font_xl, fill="white")
        
        # 3. Draw Icon (Centered)
        icon_path = current_app.get("icon")
        icon_y = header_y + max(24, int((content_bottom - header_y) * 0.22))
        if icon_path:
            icon_cap = min(APP_SELECTOR_ICON_MAX, max(24, content_bottom - icon_y - 8))
            # Ask for the icon pre-scaled to display size: the cache then holds
            # a small thumbnail instead of the full-size art (the full icon set
            # is ~1 MB of RGBA on a 64 MB device), and no copy/thumbnail work
            # happens per frame. Falls back for UIs without the max_size param.
            try:
                img = self.ui.get_image(icon_path, max_size=icon_cap)
            except TypeError:
                img = self.ui.get_image(icon_path)
            if img:
                ix = (screen_w - img.width) // 2
                iy = icon_y
                # Paste with the icon's own alpha so transparent pixels don't
                # go black over a background.
                self.ui.canvas.paste(img, (ix, iy), img)
            else:
                placeholder_size = icon_cap
                px = (screen_w - placeholder_size) // 2
                py = icon_y
                self.ui.draw.rectangle((px, py, px + placeholder_size, py + placeholder_size), outline="white")
                qw, qh = self.ui.get_text_size("?", self.ui.font_xl)
                self.ui.draw.text(
                    (px + (placeholder_size - qw) // 2, py + (placeholder_size - qh) // 2),
                    "?",
                    font=self.ui.font_xl,
                    fill="white",
                )

        # 4. Draw Footer "Select"
        w, h = self.ui.get_text_size("Select", self.ui.font_n)
        footer_y = content_bottom + max(0, (softkey_h - h) // 2)
        self.ui.draw.text(((screen_w - w)//2, footer_y), "Select", font=self.ui.font_n, fill="white")

        # 5. Draw "Nokia Style" Scrollbar (Right Edge)
        bar_x = screen_w - 8
        track_top = header_y + 6
        track_bottom = max(track_top, content_bottom - 10)
        self.ui.draw.line((bar_x, track_top, bar_x, track_bottom), fill="white", width=2)
        
        # Calculate Notch Position
        if len(self.items) > 1:
            step = (track_bottom - track_top) / (len(self.items) - 1)
            notch_y = track_top + (self.selected_index * step)
        else:
            notch_y = track_top
            
        self.ui.draw.rectangle((bar_x - 4, notch_y - 3, bar_x + 2, notch_y + 3), fill="white")
        
        # Optional: Draw Page Number "4"
        page_num = str(self.selected_index + 1)
        w, h = self.ui.get_text_size(page_num, self.ui.font_n)
        self.ui.draw.text((screen_w - 5 - w, 10), page_num, font=self.ui.font_n, fill="white")

        self.ui.fb.update(self.ui.canvas)

    def show(self):
        """ Blocking loop """
        
        # --- INPUT FLUSH ---
        fd = getattr(self.ui, "keypad_fd", None)
        if fd is not None:
            while True:
                r, w, x = select.select([fd], [], [], 0.01)
                if r:
                    try: os.read(fd, 24)
                    except: pass
                else: break 

        self.draw()

        while True:
            key = self.ui.wait_for_key()

            # Empty list (e.g. app scan failed): navigation would divide by
            # zero and Enter would index past the end, so only allow backing out.
            if not self.items:
                if key in (14, 28):
                    return -1
                continue

            if key == 108: # DOWN (Next App)
                self.selected_index = (self.selected_index + 1) % len(self.items)
                self.draw()

            elif key == 103: # UP (Previous App)
                self.selected_index = (self.selected_index - 1) % len(self.items)
                self.draw()

            elif key == 28: # ENTER Only (Legacy '50' Removed)
                return self.selected_index

            elif key == 14: # BACKSPACE Only (Legacy '46' Removed)
                return -1

"""

The SoftKeyBar class defines and aims to replicate the middle navigation button present on the Nokia 5190

"""
class SoftKeyBar:
    def __init__(self, ui):
        self.ui = ui
        self.height = _softkey_height(ui)
        self.y_start = _ui_height(ui) - self.height
        self.current_text = None
        
        # --- ROBUST TRANSPARENCY CHECK ---
        # We detect if we are the 'Main' system bar or an 'App' bar based on initialization order.
        #
        # 1. When main.py starts, it calls SoftKeyBar(self). 
        #    At that exact moment, 'self.softkey' has NOT been assigned to the UI object yet.
        #    So hasattr(ui, 'softkey') is False. -> We are the System Bar -> Transparent.
        #
        # 2. When an App (like Messages) runs later, it calls SoftKeyBar(ui).
        #    By then, ui.softkey ALREADY exists.
        #    So hasattr(ui, 'softkey') is True. -> We are an App Bar -> Opaque (Black).
        
        self.is_transparent = not hasattr(ui, 'softkey')

    def update(self, new_text, present=True):
        screen_w = _ui_width(self.ui)
        screen_h = _ui_height(self.ui)
        wallpaper = getattr(self.ui, "wallpaper", None)
        
        if self.is_transparent and wallpaper:
            # TRANSPARENT MODE (Home Screen only)
            # Crop the bottom strip from the wallpaper and paste it
            box = (0, self.y_start, screen_w, screen_h)
            try:
                bg_slice = wallpaper.crop(box)
                self.ui.canvas.paste(bg_slice, box)
            except Exception:
                self.ui.draw.rectangle((0, self.y_start, screen_w, screen_h), fill="black")
        else:
            # OPAQUE MODE (Apps, Dialogs, Lists)
            # Always draw black to cover scrolling lists or game graphics
            self.ui.draw.rectangle((0, self.y_start, screen_w, screen_h), fill="black")

        if new_text:
            w, h = self.ui.get_text_size(new_text, self.ui.font_n)
            x = (screen_w - w) // 2
            y = self.y_start + ((self.height - h) // 2)
            self.ui.draw.text((x, y), new_text, font=self.ui.font_n, fill="white")

        self.current_text = new_text

        if present:
            self.ui.fb.update(self.ui.canvas)

"""

Header widget defines the "page number" little tooltip in the top right corner

"""
class HeaderWidget:
    def __init__(self, ui, root_id):
        self.ui = ui
        self.root_id = root_id # The "1" in "1-2"
        
    def draw(self, sub_index=None):
        """
        Draws the breadcrumb index at top right.
        sub_index: The list item number (e.g. 2 for Pizza Hut).
        If None, just draws the root ID.
        """
        if sub_index is not None:
            text = f"{self.root_id}-{sub_index}"
        else:
            text = f"{self.root_id}"
            
        w, h = self.ui.get_text_size(text, self.ui.font_n)
        x = _ui_width(self.ui) - 5 - w
        y = 5 
        self.ui.draw.text((x, y), text, font=self.ui.font_n, fill="white")

"""

VerticalList is used to draw lists for different menu, selecting contacts etc.
It is very commonly used in the System apps

"""
class VerticalList:
    def __init__(self, ui, title, items, app_id=99):
        self.ui = ui
        self.title = title
        self.items = items  # List of strings ["Mom", "Dad"] etc.
        self.app_id = app_id
        
        self.header = HeaderWidget(ui, app_id)
        self.selected_index = 0
        self.window_start = 0
        self.max_lines = 3
        
    def draw(self):
        screen_w = _ui_width(self.ui)
        screen_h = _ui_height(self.ui)
        content_bottom = _content_bottom(self.ui)
        header_y = _header_divider_y(self.ui)

        # 1. Clear Screen
        self.ui.draw.rectangle((0, 0, screen_w, content_bottom), fill="black")
        
        # 2. Draw Title and Header
        self.ui.draw.text((5, 0), self.title, font=self.ui.font_xl, fill="white")
        self.header.draw(self.selected_index + 1)
        
        # 3. Draw Divider Line
        self.ui.draw.line((0, header_y, screen_w, header_y), fill="white")

        # 4. Draw List Items
        y_start = header_y + 10
        content_height = max(1, content_bottom - y_start - 4)
        target_lines = 3
        line_height = max(28, content_height // target_lines)
        item_height = max(24, line_height - 4)
        self.max_lines = min(target_lines, max(1, content_height // line_height))
        item_font = getattr(self.ui, "font_md", self.ui.font_n)

        if self.selected_index < self.window_start:
            self.window_start = self.selected_index
        max_start = max(0, len(self.items) - self.max_lines)
        if self.window_start > max_start:
            self.window_start = max_start

        bar_x = screen_w - 5
        selected_right = max(20, bar_x - 10)
        
        for i in range(self.max_lines):
            item_idx = self.window_start + i
            if item_idx >= len(self.items): break
            
            y = y_start + (i * line_height)
            item_text = self.items[item_idx]
            text_h = self.ui.get_text_size(item_text, item_font)[1]
            text_y = y + max(0, (item_height - text_h) // 2)
            
            # Draw Selection Box
            if item_idx == self.selected_index:
                self.ui.draw.rectangle((0, y, selected_right, y + item_height), fill="white")
                self.ui.draw.text((10, text_y), item_text, font=item_font, fill="black")
            else:
                self.ui.draw.text((10, text_y), item_text, font=item_font, fill="white")

        # 5. Draw Scrollbar
        track_top = y_start
        track_bottom = max(track_top, content_bottom - 5)
        self.ui.draw.line((bar_x, track_top, bar_x, track_bottom), fill="gray", width=1)
        
        if len(self.items) > 1:
            step = (track_bottom - track_top) / (len(self.items) - 1)
            notch_y = track_top + (self.selected_index * step)
        else:
            notch_y = track_top
            
        self.ui.draw.rectangle((bar_x - 2, notch_y - 3, bar_x + 2, notch_y + 3), fill="white")

        # 6. Flush
        self.ui.fb.update(self.ui.canvas)

    def show(self):
        """ Blocking loop. Returns the selected index OR -1 for back. """
        self.draw()
        
        while True:
            key = self.ui.wait_for_key()
            
            if key == 108: # DOWN
                if self.selected_index < len(self.items) - 1:
                    self.selected_index += 1
                    if self.selected_index >= self.window_start + self.max_lines:
                        self.window_start += 1
                self.draw()
                        
            elif key == 103: # UP
                if self.selected_index > 0:
                    self.selected_index -= 1
                    if self.selected_index < self.window_start:
                        self.window_start -= 1
                self.draw()
            
            # --- NUMBER SHORTCUTS ---
            elif 2 <= key <= 10: 
                shortcut_idx = key - 2
                if shortcut_idx < len(self.items):
                    return shortcut_idx
                        
            elif key == 28: # ENTER Only (Legacy '50' Removed)
                return self.selected_index 
            
            elif key == 14: # BACKSPACE Only (Legacy '46' Removed)
                return -1           

"""

TextInput is a basic text form to allow for short inputs like a phone number, name, date, time etc.

"""
class TextInput:
    def __init__(self, ui, title, prompt, initial_text="", input_filter="any"):
        self.ui = ui
        self.title = title   # Header Title (e.g. "Add Entry")
        self.prompt = prompt # Instruction (e.g. "Name:")
        self.text = initial_text
        # "any" / "letters" / "numbers" -- what the field accepts
        self.input_filter = input_filter
        self.t9 = t9_engine.T9Engine(input_filter=input_filter)

        # Development Key Map (PC Keyboard -> Char)
        self.DEV_KEYMAP = {
            2: "1", 3: "2", 4: "3", 5: "4", 6: "5", 
            7: "6", 8: "7", 9: "8", 10: "9", 11: "0",
            # QWERTY
            16: "q", 17: "w", 18: "e", 19: "r", 20: "t", 21: "y", 22: "u", 23: "i", 24: "o", 25: "p",
            30: "a", 31: "s", 32: "d", 33: "f", 34: "g", 35: "h", 36: "j", 37: "k", 38: "l",
            44: "z", 45: "x", 46: "c", 47: "v", 48: "b", 49: "n", 50: "m",
            57: " ", 52: ".", 51: ",", 12: "-"
        }

    def draw(self, blink_state=True):
        screen_w = _ui_width(self.ui)
        content_bottom = _content_bottom(self.ui)
        header_y = _header_divider_y(self.ui)

        # 1. Clear Screen
        self.ui.draw.rectangle((0, 0, screen_w, content_bottom), fill="black")
        
        # 2. Header
        self.ui.draw.text((5, 5), self.title, font=self.ui.font_xl, fill="white")
        self.ui.draw.line((0, header_y, screen_w, header_y), fill="white")
        
        # 3. Prompt
        prompt_y = header_y + 20
        self.ui.draw.text((10, prompt_y), self.prompt, font=self.ui.font_n, fill="white")

        # 3b. T9 mode indicator ("abc"/"ABC"/"123"), right of the prompt
        if _t9_active(self.ui):
            label = self.t9.mode
            w = self.ui.get_text_size(label, self.ui.font_n)[0]
            self.ui.draw.text((screen_w - 12 - w, prompt_y), label,
                              font=self.ui.font_n, fill="white")

        # 4. Input Box Container
        box_y = prompt_y + 30
        box_h = max(24, min(40, content_bottom - box_y - 10))
        box_right = max(20, screen_w - 10)
        self.ui.draw.rectangle((10, box_y, box_right, box_y + box_h), outline="white")
        
        # 5. The Text
        display_text = self.text + ("_" if blink_state else "")
        text_h = self.ui.get_text_size(display_text or "A", self.ui.font_n)[1]
        text_y = box_y + max(0, (box_h - text_h) // 2)
        self.ui.draw.text((15, text_y), display_text, font=self.ui.font_n, fill="white")
        
        self.ui.fb.update(self.ui.canvas)

    def handle_key(self, key):
        """Apply one keycode to the field (no drawing).
        Returns "confirm", "cancel", "typed", "backspace", "mode", or None."""
        # ENTER / NAVI-CENTER -> Confirm (Legacy '50' Removed)
        if key in (28, 96):
            return "confirm"

        # BACKSPACE / C BUTTON
        if key == 14:
            self.t9.reset()
            if len(self.text) > 0:
                self.text = self.text[:-1]
                return "backspace"
            return "cancel"

        # TYPING -- T9 multi-tap on the i2c keypad
        if _t9_active(self.ui):
            op = self.t9.press(key)
            if op is None:
                return None
            kind, value = op
            if kind == "append":
                self.text += value
                return "typed"
            if kind == "replace":
                self.text = self.text[:-1] + value
                return "typed"
            if kind == "mode":
                return "mode"
            return None

        # TYPING -- dev keyboard (QWERTY)
        char = self.DEV_KEYMAP.get(key)
        if char is None or not t9_engine.char_allowed(char, self.input_filter):
            return None
        if len(self.text) == 0: char = char.upper()
        self.text += char
        return "typed"

    def show(self):
        """ Blocking Loop. Returns STRING if confirmed, NONE if cancelled. """
        from System.ui.framework import SoftKeyBar # Local import to avoid circular dep
        softkey = SoftKeyBar(self.ui)
        softkey.update("OK")

        cursor_on = True
        last_blink = time.time()
        self.draw(cursor_on)

        while True:
            # --- Blink Logic ---
            if time.time() - last_blink > 0.5:
                cursor_on = not cursor_on
                last_blink = time.time()
                self.draw(cursor_on)

            # --- Input ---
            key = self.ui.wait_for_key()
            if key is None: continue

            action = self.handle_key(key)
            if action == "confirm":
                return self.text
            if action == "cancel":
                return None
            if action in ("typed", "backspace", "mode"):
                self.draw(cursor_on)

"""

TextInputLong is a long-form text entry widget for composing messages and notes.

"""
class TextInputLong:
    def __init__(self, ui, title, initial_text="", on_empty_backspace=None,
                 input_filter="any"):
        self.ui = ui
        self.title = title
        self.text = initial_text or ""
        self.cursor = len(self.text)
        self.on_empty_backspace = on_empty_backspace
        # "any" / "letters" / "numbers" -- what the field accepts
        self.input_filter = input_filter
        self.t9 = t9_engine.T9Engine(input_filter=input_filter)
        self.font = getattr(ui, "font_s", None) or ui.font_n
        self.text_area_top = _header_divider_y(ui) + 10
        self.text_area_bottom = _content_bottom(ui) - 4

        # Development Key Map (PC Keyboard -> Char)
        self.DEV_KEYMAP = {
            2: "1", 3: "2", 4: "3", 5: "4", 6: "5",
            7: "6", 8: "7", 9: "8", 10: "9", 11: "0",
            # QWERTY
            16: "q", 17: "w", 18: "e", 19: "r", 20: "t", 21: "y", 22: "u", 23: "i", 24: "o", 25: "p",
            30: "a", 31: "s", 32: "d", 33: "f", 34: "g", 35: "h", 36: "j", 37: "k", 38: "l",
            44: "z", 45: "x", 46: "c", 47: "v", 48: "b", 49: "n", 50: "m",
            57: " ", 52: ".", 51: ",", 12: "-"
        }

    def get_text(self):
        return self.text

    def set_text(self, text):
        self.text = text or ""
        self.cursor = len(self.text)

    def clear_text(self):
        self.text = ""
        self.cursor = 0

    def set_on_empty_backspace(self, callback):
        self.on_empty_backspace = callback

    def _wrap_text(self, text, max_w):
        def text_w(s):
            return self.ui.get_text_size(s, self.font)[0]

        def break_long_word(word):
            out = []
            cur = ""
            for ch in word:
                nxt = cur + ch
                if cur and text_w(nxt) > max_w:
                    out.append(cur)
                    cur = ch
                else:
                    cur = nxt
            if cur:
                out.append(cur)
            return out or [word]

        lines = []
        for raw in (text or "").splitlines() or [""]:
            words = raw.split(" ")
            cur = ""
            for w in words:
                if w == "":
                    continue
                if text_w(w) > max_w:
                    if cur:
                        lines.append(cur)
                        cur = ""
                    lines.extend(break_long_word(w))
                    continue

                cand = w if not cur else (cur + " " + w)
                if text_w(cand) <= max_w:
                    cur = cand
                else:
                    if cur:
                        lines.append(cur)
                    cur = w
            lines.append(cur)

        if not lines:
            return [""]
        return lines

    def _current_lines(self, blink_state):
        cursor_marker = "_" if blink_state else ""
        display_text = self.text + cursor_marker
        return self._wrap_text(display_text, max(20, _ui_width(self.ui) - 20))

    def draw(self, blink_state=True):
        screen_w = _ui_width(self.ui)
        content_bottom = _content_bottom(self.ui)
        header_y = _header_divider_y(self.ui)

        self.ui.draw.rectangle((0, 0, screen_w, content_bottom), fill="black")

        # Header
        self.ui.draw.text((5, 5), self.title, font=self.ui.font_xl, fill="white")
        char_count = str(len(self.text))
        w, _ = self.ui.get_text_size(char_count, self.ui.font_n)
        self.ui.draw.text((screen_w - 5 - w, 5), char_count, font=self.ui.font_n, fill="white")
        # T9 mode indicator ("abc"/"ABC"/"123"), left of the char count
        if _t9_active(self.ui):
            label = self.t9.mode
            lw, _ = self.ui.get_text_size(label, self.ui.font_n)
            self.ui.draw.text((screen_w - 5 - w - 10 - lw, 5), label,
                              font=self.ui.font_n, fill="white")
        self.ui.draw.line((0, header_y, screen_w, header_y), fill="white")

        lines = self._current_lines(blink_state)
        _, line_h = self.ui.get_text_size("Ag", self.font)
        line_h += 3
        max_lines = max(1, int((self.text_area_bottom - self.text_area_top) / line_h))
        start = max(0, len(lines) - max_lines)

        y = self.text_area_top
        for line in lines[start:start + max_lines]:
            self.ui.draw.text((10, y), line, font=self.font, fill="white")
            y += line_h

        self.ui.fb.update(self.ui.canvas)

    def handle_key(self, key):
        if key == 14: # Backspace
            self.t9.reset()
            if len(self.text) == 0:
                if callable(self.on_empty_backspace):
                    self.on_empty_backspace()
                return "empty_backspace"

            if self.cursor > 0:
                self.text = self.text[:self.cursor - 1] + self.text[self.cursor:]
                self.cursor = max(0, self.cursor - 1)
            return "backspace"

        # T9 multi-tap on the i2c keypad
        if _t9_active(self.ui):
            op = self.t9.press(key)
            if op is None:
                return None
            kind, value = op
            if kind == "append":
                self.text = self.text[:self.cursor] + value + self.text[self.cursor:]
                self.cursor += 1
                return "typed"
            if kind == "replace":
                if self.cursor > 0:
                    self.text = (self.text[:self.cursor - 1] + value
                                 + self.text[self.cursor:])
                return "typed"
            if kind == "mode":
                return "mode"
            return None

        # Dev keyboard (QWERTY)
        if key in self.DEV_KEYMAP:
            char = self.DEV_KEYMAP[key]
            if not t9_engine.char_allowed(char, self.input_filter):
                return None
            # Simple capitalization logic for start of message
            if len(self.text) == 0:
                char = char.upper()
            self.text = self.text[:self.cursor] + char + self.text[self.cursor:]
            self.cursor += 1
            return "typed"

        return None
"""
MessageDialog is a simple full-screen modal used for notices/warnings.

Rules:
- The caller (/System/core or an app) owns the *policy* (what happens after OK).
- This class only handles drawing + key handling.
"""

DEFAULT_WARNING_ICON = "/NeoDCT/System/ui/resources/img/errorscreen/warning.png"
class MessageDialog:
    def __init__(
        self,
        ui,
        message,
        *,
        title=None,
        icon_path=None,
        button_text="OK",
        accept_keys=(28,),
        cancel_keys=(14,),
        margin=8,
    ):
        self.ui = ui
        self.title = title
        self.message = message or ""
        self.icon_path = icon_path or DEFAULT_WARNING_ICON
        self.button_text = button_text
        self.accept_keys = tuple(accept_keys or ())
        self.cancel_keys = tuple(cancel_keys or ())
        self.margin = int(margin)

        # Fonts are defined by the main UI object.
        self.font_title = getattr(ui, "font_md", None) or getattr(ui, "font_n", None) or getattr(ui, "font_s", None)
        self.font_body = getattr(ui, "font_s", None) or getattr(ui, "font_n", None)

    def _flush_input(self):
        """Drain pending key events so OK doesn't instantly dismiss."""
        fd = getattr(self.ui, "keypad_fd", None)
        if fd is None:
            return
        while True:
            r, _, _ = select.select([fd], [], [], 0.0)
            if not r:
                break
            try:
                os.read(fd, 24)
            except Exception:
                break

    def _wrap_text(self, text, font, max_w):
        """Word-wrap text to max_w pixels using ui.get_text_size."""
        def text_w(s):
            return self.ui.get_text_size(s, font)[0]

        def break_long_word(word):
            out = []
            cur = ""
            for ch in word:
                nxt = cur + ch
                if cur and text_w(nxt) > max_w:
                    out.append(cur)
                    cur = ch
                else:
                    cur = nxt
            if cur:
                out.append(cur)
            return out or [word]

        lines = []
        for raw in (text or "").splitlines() or [""]:
            words = raw.split(" ")
            cur = ""
            for w in words:
                if w == "":
                    continue
                if text_w(w) > max_w:
                    if cur:
                        lines.append(cur)
                        cur = ""
                    lines.extend(break_long_word(w))
                    continue

                cand = w if not cur else (cur + " " + w)
                if text_w(cand) <= max_w:
                    cur = cand
                else:
                    if cur:
                        lines.append(cur)
                    cur = w
            lines.append(cur)

        while lines and lines[-1] == "":
            lines.pop()
        return lines

    def _draw(self):
        ui = self.ui
        screen_w = _ui_width(ui)
        screen_h = _ui_height(ui)
        content_bottom = _content_bottom(ui)

        # Full clear
        ui.draw.rectangle((0, 0, screen_w, screen_h), fill="black")

        # Icon (optional)
        icon = None
        if self.icon_path:
            try:
                icon = ui.get_image(self.icon_path)
            except Exception:
                icon = None
        if icon:
            ui.canvas.paste(icon, (self.margin, self.margin), icon)

        # Title (optional)
        y = self.margin
        if self.title and self.font_title:
            title_x = self.margin + (icon.width + 6 if icon else 0)
            ui.draw.text((title_x, self.margin), self.title, font=self.font_title, fill="white")
            _, th = ui.get_text_size(self.title, self.font_title)
            y = max(y, self.margin + th + 6)
        if icon:
            # The body must clear the icon even when the title is shorter
            # than it, or the first line lands on the triangle.
            y = max(y, self.margin + icon.height + 6)

        # Body. Short notices ("LOW BATTERY!") get the Nokia alert look:
        # normal font, centered. Paragraphs keep the small left-aligned form.
        max_w = screen_w - (self.margin * 2)
        alert_font = getattr(self.ui, "font_n", None) or self.font_body
        alert_lines = self._wrap_text(self.message, alert_font, max_w)
        if len(alert_lines) <= 2:
            font_body, lines, centered = alert_font, alert_lines, True
        else:
            font_body, lines, centered = self.font_body, self._wrap_text(self.message, self.font_body, max_w), False

        line_h = ui.get_text_size("Ag", font_body)[1] + 3
        max_lines = max(1, int((content_bottom - y - self.margin) / line_h))

        if len(lines) > max_lines:
            lines = lines[:max_lines]
            if lines:
                lines[-1] = (lines[-1] + " …") if not lines[-1].endswith("…") else lines[-1]

        # Vertically center the body in the space above the softkey.
        y += max(0, (content_bottom - self.margin - y - len(lines) * line_h) // 2)

        for line in lines:
            if centered:
                lw, _ = ui.get_text_size(line, font_body)
                x = max(self.margin, (screen_w - lw) // 2)
            else:
                x = self.margin
            ui.draw.text((x, y), line, font=font_body, fill="white")
            y += line_h

        # Softkey (draw but don't present yet)
        SoftKeyBar(ui).update(self.button_text, present=False)

        # Present once
        ui.fb.update(ui.canvas)

    def render(self):
        """Draw the dialog without waiting for a key (e.g. shutdown notices)."""
        self._flush_input()
        self._draw()

    def show(self):
        """Blocking modal. Returns the key that dismissed it."""
        self._flush_input()
        self._draw()

        while True:
            key = self.ui.wait_for_key()
            if key in self.accept_keys or key in self.cancel_keys:
                return key
                


    """
    PagedList: Nokia-style "one item per screen" menu with a right-side scrollbar.
    - UP/DOWN cycles through pages
    - ENTER selects (returns index)
    - BACKSPACE cancels (returns -1)

    items can be:
      - ["Text Messages", "SMS Settings", ...]
      - [{"name": "Text Messages"}, {"name": "SMS Settings"}, ...]
    """
    
# Add this class to framework.py (e.g., after VerticalList).
# Requires HeaderWidget + SoftKeyBar already present in this file.
#

class PagedList:
    def __init__(self, ui, title, items, root_id=99, show_select_hint=True):
        self.ui = ui
        self.title = title
        self.items = items or []
        self.root_id = root_id
        self.selected_index = 0

        self.header = HeaderWidget(ui, root_id)
        self.softkey = SoftKeyBar(ui) if show_select_hint else None
        self._show_select_hint = show_select_hint

        header_y = _header_divider_y(ui)
        self._content_top = header_y + 8
        self._content_bottom = _content_bottom(ui) - 10
        self._bar_x = _ui_width(ui) - 5

    def _get_item_name(self, idx):
        if not self.items:
            return ""
        item = self.items[idx]
        if isinstance(item, dict):
            return str(item.get("name", ""))
        return str(item)

    def _wrap_to_lines(self, text, font, max_width, max_lines=2):
        """
        Word-wrap into up to max_lines, truncating the last line with "..." if needed.
        Uses ui.get_text_size for width measurement.
        """
        words = (text or "").split()
        if not words:
            return [""]

        lines = []
        cur = ""

        def fits(s):
            w, _ = self.ui.get_text_size(s, font)
            return w <= max_width

        i = 0
        while i < len(words) and len(lines) < max_lines:
            w = words[i]
            candidate = (cur + " " + w).strip() if cur else w
            if fits(candidate):
                cur = candidate
                i += 1
                continue

            # current line can't fit candidate; push current if non-empty
            if cur:
                lines.append(cur)
                cur = ""
                continue

            # single word too long: hard-truncate
            trimmed = w
            while trimmed and not fits(trimmed + "..."):
                trimmed = trimmed[:-1]
            if trimmed:
                lines.append(trimmed + "..." if (i < len(words) - 1) else trimmed)
            else:
                lines.append("...")
            i += 1

        if len(lines) < max_lines and cur:
            lines.append(cur)

        # If words remain, truncate last line with ellipsis
        if i < len(words):
            last = lines[-1] if lines else ""
            if last.endswith("..."):
                return lines
            trimmed = last
            while trimmed and not fits(trimmed + "..."):
                trimmed = trimmed[:-1]
            lines[-1] = (trimmed + "...") if trimmed else "..."

        return lines[:max_lines]

    def draw(self):
        screen_w = _ui_width(self.ui)
        screen_h = _ui_height(self.ui)
        header_y = _header_divider_y(self.ui)

        # Clear full screen
        self.ui.draw.rectangle((0, 0, screen_w, screen_h), fill="black")

        # Title + divider
        self.ui.draw.text((5, 5), self.title, font=self.ui.font_xl, fill="white")
        self.ui.draw.line((0, header_y, screen_w, header_y), fill="white")

        # Empty state
        if not self.items:
            self.header.draw(None)
            text = "No Items"
            w, h = self.ui.get_text_size(text, self.ui.font_n)
            y = self._content_top + max(0, ((self._content_bottom - self._content_top) - h) // 2)
            self.ui.draw.text(((screen_w - w) // 2, y), text, font=self.ui.font_n, fill="white")
            if self.softkey and self._show_select_hint:
                self.softkey.update(None, present=False)
            self.ui.fb.update(self.ui.canvas)
            return

        # Header "root-sub"
        self.header.draw(self.selected_index + 1)

        # Main page text (large, 2-line wrap)
        name = self._get_item_name(self.selected_index)
        max_w = max(20, self._bar_x - 12)
        lines = self._wrap_to_lines(name, self.ui.font_xl, max_w, max_lines=2)

        # Vertical placement: visually centered in content area
        _, line_h = self.ui.get_text_size("Ag", self.ui.font_xl)
        total_h = len(lines) * (line_h + 6) - 6
        y0 = self._content_top + max(0, ((self._content_bottom - self._content_top) - total_h) // 2)

        for li, line in enumerate(lines):
            w, _ = self.ui.get_text_size(line, self.ui.font_xl)
            x = max(5, (max_w - w) // 2)
            y = y0 + li * (line_h + 6)
            self.ui.draw.text((x, y), line, font=self.ui.font_xl, fill="white")

        # Scrollbar (right edge)
        track_top = self._content_top
        track_bottom = max(track_top, self._content_bottom)
        self.ui.draw.line((self._bar_x, track_top, self._bar_x, track_bottom), fill="white", width=2)

        if len(self.items) > 1:
            step = (track_bottom - track_top) / (len(self.items) - 1)
            notch_y = track_top + (self.selected_index * step)
        else:
            notch_y = track_top

        self.ui.draw.rectangle((self._bar_x - 4, notch_y - 3, self._bar_x + 2, notch_y + 3), fill="white")

        # Bottom hint
        if self.softkey and self._show_select_hint:
            self.softkey.update("Select", present=False)

        self.ui.fb.update(self.ui.canvas)

    def show(self):
        """Blocking loop. Returns selected index or -1 for back."""
        # Input flush (mirrors AppSelector behavior)
        fd = getattr(self.ui, "keypad_fd", None)
        if fd is not None:
            while True:
                r, _, _ = select.select([fd], [], [], 0.01)
                if r:
                    try:
                        os.read(fd, 24)
                    except:
                        pass
                else:
                    break

        if self.selected_index >= len(self.items):
            self.selected_index = 0

        self.draw()

        while True:
            key = self.ui.wait_for_key()

            if key == 108:  # DOWN
                if self.items:
                    self.selected_index = (self.selected_index + 1) % len(self.items)
                    self.draw()

            elif key == 103:  # UP
                if self.items:
                    self.selected_index = (self.selected_index - 1) % len(self.items)
                    self.draw()

            elif key == 28:  # ENTER
                return self.selected_index

            elif key == 14:  # BACKSPACE
                return -1

"""

TextScroller is the Nokia-style instructions/help reader: a full screen of
wrapped text paged with the softkey. The softkey reads "More" until the last
page, where it becomes "Back" (see the 5190 game instructions screens).

"""
class TextScroller:
    def __init__(self, ui, text, more_text="More", back_text="Back"):
        self.ui = ui
        self.text = text or ""
        self.more_text = more_text
        self.back_text = back_text
        self.page = 0

        self.font = getattr(ui, "font_n", None) or ui.font_md
        self.margin = 10
        self.top = 8

    def _wrap_text(self, text, max_w):
        return _wrap_lines(self.ui, text, self.font, max_w) or [""]

    def _paginate(self):
        """Pages of (line, height). A blank line is a gap, not a line.

        Giving a paragraph break the full height of a line of 20px type is
        what turned one changelog into five screens of paging.
        """
        screen_w = _ui_width(self.ui)
        content_bottom = _content_bottom(self.ui)
        lines = self._wrap_text(self.text, screen_w - (self.margin * 2))
        line_h = self.ui.get_text_size("Ag", self.font)[1] + 4
        gap_h = max(4, line_h // 3)
        budget = content_bottom - self.top - 4

        pages, current, used = [], [], 0
        for line in lines:
            height = line_h if line else gap_h
            if current and used + height > budget:
                pages.append(current)
                current, used = [], 0
            if not line and not current:
                continue        # never start a page with an empty gap
            current.append((line, height))
            used += height
        if current:
            pages.append(current)
        return pages or [[("", line_h)]], line_h

    def draw(self):
        screen_w = _ui_width(self.ui)
        content_bottom = _content_bottom(self.ui)
        pages, line_h = self._paginate()
        self.page = max(0, min(self.page, len(pages) - 1))

        self.ui.draw.rectangle((0, 0, screen_w, content_bottom), fill="black")
        y = self.top
        for line, height in pages[self.page]:
            if line:
                self.ui.draw.text((self.margin, y), line, font=self.font,
                                  fill="white")
            y += height

        last_page = self.page >= len(pages) - 1
        SoftKeyBar(self.ui).update(self.back_text if last_page else self.more_text)
        return last_page

    def show(self):
        """Blocking loop. ENTER/DOWN pages forward (exits past the last page),
        UP pages back, BACKSPACE exits immediately."""
        last_page = self.draw()

        while True:
            key = self.ui.wait_for_key()

            if key in (28, 108):  # ENTER (softkey) / DOWN
                if last_page:
                    return
                self.page += 1
                last_page = self.draw()

            elif key == 103:  # UP
                if self.page > 0:
                    self.page -= 1
                    last_page = self.draw()

            elif key == 14:  # BACKSPACE
                return

"""

LevelSelector is the Nokia game difficulty picker: a "Level" list with an OK
softkey. Digit keys jump straight to that level, like on the 5190.

"""
class LevelSelector(VerticalList):
    def __init__(self, ui, current=1, count=9, title="Level", app_id=6):
        items = [f"Level {n}" for n in range(1, count + 1)]
        super().__init__(ui, title, items, app_id=app_id)
        self.selected_index = max(0, min(count - 1, int(current) - 1))

    def show(self):
        """Blocking loop. Returns the chosen level (1-based) or None on back."""
        SoftKeyBar(self.ui).update("OK", present=False)
        choice = super().show()
        if choice < 0:
            return None
        return choice + 1

"""

InfoScreen is a simple centered label/value readout (e.g. "Top score" / "385",
or the Call Log duration screens) dismissed with the softkey. Unlike
MessageDialog it has no icon and is not a warning; it is a plain reading.

"""
class InfoScreen:
    def __init__(self, ui, title, value=None, softkey_text="Back"):
        self.ui = ui
        self.title = title or ""
        self.value = value
        self.softkey_text = softkey_text

    def show(self):
        ui = self.ui
        screen_w = _ui_width(ui)
        content_bottom = _content_bottom(ui)

        ui.draw.rectangle((0, 0, screen_w, content_bottom), fill="black")

        title_font = ui.font_n
        value_font = ui.font_xl

        tw, th = ui.get_text_size(self.title, title_font)
        if self.value is None:
            ty = max(0, (content_bottom - th) // 2)
            ui.draw.text(((screen_w - tw) // 2, ty), self.title, font=title_font, fill="white")
        else:
            value_text = str(self.value)
            vw, vh = ui.get_text_size(value_text, value_font)
            gap = 10
            total = th + gap + vh
            ty = max(0, (content_bottom - total) // 2)
            ui.draw.text(((screen_w - tw) // 2, ty), self.title, font=title_font, fill="white")
            ui.draw.text(((screen_w - vw) // 2, ty + th + gap), value_text, font=value_font, fill="white")

        SoftKeyBar(ui).update(self.softkey_text)

        while True:
            key = ui.wait_for_key()
            if key in (28, 14):
                return key

"""

ProgressScreen is the "this will take a while" screen: a step label, the bar,
and the reading underneath it. Nothing is ever drawn on top of the bar -- a
percentage sitting across its own fill is the one thing that makes a progress
bar look broken.

Callers drive it with draw(done, total) as often as they like; it only
repaints when the whole percentage changes, so a copy loop can call it per
chunk without slowing the copy down.

"""
class ProgressScreen:
    BAR_HEIGHT = 14
    BAR_MARGIN = 20
    INSET = 2

    def __init__(self, ui, step, header=None, hint=None, detail=None):
        self.ui = ui
        self.step = step or ""
        self.header = header
        self.hint = hint
        # detail: callable(done, total) -> str, shown right of the percentage
        # (the update app puts "5.6 of 12.4 MB" there).
        self.detail = detail
        self._percent = None

        width = _ui_width(ui)
        bottom = _content_bottom(ui)
        self.font_step = getattr(ui, "font_n", None) or ui.font_md
        self.font_small = getattr(ui, "font_s", None) or self.font_step
        # "Backing up your data" does not fit at full size; the ladder drops
        # a step rather than letting the label run off both edges.
        self._label_fonts = _font_ladder(ui, "font_n", "font_md", "font_s") \
            or [self.font_step]

        step_h = ui.get_text_size("Ag", self.font_step)[1]
        small_h = ui.get_text_size("Ag", self.font_small)[1]

        self.header_box = (0, 4, width, 4 + small_h)
        self.divider_y = self.header_box[3] + 5

        bar_top = int(bottom * 0.55)
        self.bar_box = (self.BAR_MARGIN, bar_top,
                        width - self.BAR_MARGIN, bar_top + self.BAR_HEIGHT)

        label_y = bar_top - 14 - step_h
        self.label_box = (0, label_y, width, label_y + step_h)

        status_y = self.bar_box[3] + 9
        self.status_box = (self.BAR_MARGIN, status_y,
                           width - self.BAR_MARGIN, status_y + small_h)

        hint_y = bottom - small_h - 6
        self.hint_box = (0, hint_y, width, hint_y + small_h)

    def set_step(self, step):
        """Change what the screen says it is doing, and repaint on next draw."""
        self.step = step or ""
        self._percent = None

    def _centered(self, text, font, box):
        text_w = self.ui.get_text_size(text, font)[0]
        return (max(0, (box[0] + box[2] - text_w) // 2), box[1])

    def draw(self, done, total):
        percent = int(done * 100 / total) if total else 100
        percent = max(0, min(100, percent))
        if percent == self._percent:
            return False
        self._percent = percent

        ui = self.ui
        width = _ui_width(ui)
        bottom = _content_bottom(ui)
        ui.draw.rectangle((0, 0, width, bottom), fill="black")

        if self.header:
            ui.draw.text((10, self.header_box[1]), self.header,
                         font=self.font_small, fill="white")
            ui.draw.line((10, self.divider_y, width - 10, self.divider_y),
                         fill="white")

        if self.step:
            room = width - 16
            font = _fit_font(ui, self.step, room, self._label_fonts)
            label = _ellipsize(ui, self.step, font, room)
            ui.draw.text(self._centered(label, font, self.label_box),
                         label, font=font, fill="white")

        left, top, right, base = self.bar_box
        ui.draw.rectangle((left, top, right, base), outline="white", width=1)
        span = (right - self.INSET) - (left + self.INSET)
        filled = int(span * percent / 100.0)
        if filled > 0:
            ui.draw.rectangle((left + self.INSET, top + self.INSET,
                               left + self.INSET + filled, base - self.INSET),
                              fill="white")

        reading = "%d%%" % percent
        detail = self.detail(done, total) if self.detail else ""
        if detail:
            ui.draw.text((self.status_box[0], self.status_box[1]), reading,
                         font=self.font_small, fill="white")
            detail_w = ui.get_text_size(detail, self.font_small)[0]
            ui.draw.text((self.status_box[2] - detail_w, self.status_box[1]),
                         detail, font=self.font_small, fill="white")
        else:
            ui.draw.text(self._centered(reading, self.font_small, self.status_box),
                         reading, font=self.font_small, fill="white")

        if self.hint:
            hint = _ellipsize(ui, self.hint, self.font_small, width - 16)
            ui.draw.text(self._centered(hint, self.font_small, self.hint_box),
                         hint, font=self.font_small, fill="white")

        SoftKeyBar(ui).update("", present=False)
        ui.fb.update(ui.canvas)
        return True


"""

DetailPage is a page you read: an optional picture, a title, a line or two of
detail, then body text -- all in one column that scrolls by a line at a time
with UP/DOWN, with the Nokia scrollbar down the right edge when there is more
below the fold.

It replaces the pattern of "MessageDialog, then a TextScroller" for anything
that wants to show a thing and its details together (the update page, an
about screen). Paragraph breaks cost half a line instead of a whole empty
one, so a changelog reads as a changelog instead of as five screens.

"""
class DetailPage:
    MARGIN = 10
    IMAGE_MAX = 64
    MIN_IMAGE = 40
    SCROLLBAR_W = 8

    def __init__(self, ui, title="", subtitle=None, body="", image=None,
                 badge=None, header=None, softkey_text="OK",
                 accept_keys=(28,), cancel_keys=(14,)):
        self.ui = ui
        self.title = title or ""
        self.subtitle = subtitle or ""
        self.badge = badge or ""
        self.body = body or ""
        self.header = header
        self.softkey_text = softkey_text
        self.accept_keys = tuple(accept_keys or ())
        self.cancel_keys = tuple(cancel_keys or ())
        self.offset = 0

        self.font_title = getattr(ui, "font_n", None) or ui.font_md
        self.font_small = getattr(ui, "font_s", None) or self.font_title
        self.line_height = ui.get_text_size("Ag", self.font_small)[1] + 3

        width = _ui_width(ui)
        small_h = ui.get_text_size("Ag", self.font_small)[1]
        top = 4
        if self.header:
            self.header_box = (0, top, width, top + small_h)
            self.divider_y = self.header_box[3] + 5
            top = self.divider_y + 6
        else:
            self.header_box = None
            self.divider_y = None
        # Two pixels of air above the softkey bar: the page must never look
        # like its last line is sitting on the softkey.
        self.viewport = (0, top, width, _content_bottom(ui) - 2)

        self.image = self._prepare_image(image)
        self._blocks = self._layout()
        self.content_height = sum(height for _, height in self._blocks)

    # --- layout -----------------------------------------------------------

    @property
    def viewport_height(self):
        return self.viewport[3] - self.viewport[1]

    @property
    def scrollable(self):
        return self.content_height > self.viewport_height

    @property
    def max_offset(self):
        return max(0, self.content_height - self.viewport_height)

    def _prepare_image(self, image):
        if image is None:
            return None
        if isinstance(image, str):
            try:
                return self.ui.get_image(image, max_size=self.IMAGE_MAX)
            except TypeError:
                return self.ui.get_image(image)
        if image.width > self.IMAGE_MAX or image.height > self.IMAGE_MAX:
            image = image.copy()
            image.thumbnail((self.IMAGE_MAX, self.IMAGE_MAX),
                            Image.Resampling.LANCZOS)
        return image

    def _text_width(self):
        room = _ui_width(self.ui) - (self.MARGIN * 2)
        return room - self.SCROLLBAR_W if self.scrollable else room

    def _hero_block(self, image):
        """Picture on the left, everything it is on the right.

        Stacking the two used up the whole screen before a word of the body
        got a look in, which is the difference between a page you read and a
        page you have to scroll to find out anything at all.
        """
        width = _ui_width(self.ui)
        text_x = self.MARGIN + image.width + 8
        column = width - text_x - self.MARGIN - self.SCROLLBAR_W

        title_font = _fit_font(
            self.ui, self.title, column,
            _font_ladder(self.ui, "font_n", "font_md", "font_s") or [self.font_title])
        title = _ellipsize(self.ui, self.title, title_font, column)
        title_h = self.ui.get_text_size("Ag", title_font)[1] + 5 if title else 0

        subtitle_lines = _wrap_lines(self.ui, self.subtitle, self.font_small,
                                     column) if self.subtitle else []
        rows = [(title, title_font, title_h)]
        rows += [(line, self.font_small, self.line_height)
                 for line in subtitle_lines]
        if self.badge:
            rows.append((_ellipsize(self.ui, self.badge, self.font_small, column),
                         self.font_small, self.line_height))
        rows = [row for row in rows if row[0]]

        stack_h = sum(height for _, _, height in rows)
        inner = max(image.height, stack_h)
        height = inner + 6

        def paint(canvas, draw, y, image=image, rows=rows):
            box = (self.MARGIN, y + 3 + (inner - image.height) // 2)
            if image.mode == "RGBA":
                canvas.paste(image, box, image)
            else:
                canvas.paste(image, box)
            row_y = y + 3 + (inner - stack_h) // 2
            for text, font, row_h in rows:
                draw.text((text_x, row_y), text, font=font, fill="white")
                row_y += row_h

        return paint, height

    def _fitted_hero(self):
        """The hero row, shrunk until the body can start on the same screen.

        Whatever the picture and the details add up to, the first line of
        what changed has to be visible without touching a key -- so the
        picture gives ground rather than the page turning into a hero card
        with everything worth reading below the fold.
        """
        image = self.image
        while True:
            paint, height = self._hero_block(image)
            if height + self.line_height <= self.viewport_height:
                return paint, height, image
            if image.height <= self.MIN_IMAGE:
                return paint, height, image
            smaller = image.copy()
            side = max(self.MIN_IMAGE, image.height - 8)
            smaller.thumbnail((side, side), Image.Resampling.LANCZOS)
            image = smaller

    def _layout(self):
        """The page as a list of (draw_callable, height) blocks."""
        blocks = []
        width = _ui_width(self.ui)
        self.hero_box = None

        def centered(text, font):
            def paint(canvas, draw, y):
                text_w = self.ui.get_text_size(text, font)[0]
                draw.text(((width - text_w) // 2, y), text, font=font, fill="white")
            return paint

        if self.image is not None and (self.subtitle or self.badge):
            paint_hero, hero_h, self.image = self._fitted_hero()
            blocks.append((paint_hero, hero_h))
            self.hero_box = (0, 0, width, hero_h)
        elif self.image is not None:
            image = self.image

            def paint_image(canvas, draw, y, image=image):
                box = ((width - image.width) // 2, y)
                if image.mode == "RGBA":
                    canvas.paste(image, box, image)
                else:
                    canvas.paste(image, box)

            blocks.append((paint_image, image.height + 8))
            if self.title:
                title_h = self.ui.get_text_size("Ag", self.font_title)[1]
                blocks.append((centered(self.title, self.font_title), title_h + 6))
        else:
            # Nothing to sit beside: centre the type instead.
            if self.title:
                title_h = self.ui.get_text_size("Ag", self.font_title)[1]
                blocks.append((centered(self.title, self.font_title), title_h + 6))
            if self.subtitle:
                for line in _wrap_lines(self.ui, self.subtitle, self.font_small,
                                        width - self.MARGIN * 2):
                    blocks.append((centered(line, self.font_small), self.line_height))
            if self.badge:
                blocks.append((centered(self.badge, self.font_small),
                               self.line_height + 4))

        self.body_top = sum(height for _, height in blocks)

        if self.body:
            rule_h = 10
            # The rule is the first thing to go when the page is tight: it
            # separates, but it does not say anything.
            if blocks and self.body_top + rule_h + self.line_height \
                    <= self.viewport_height:
                def paint_rule(canvas, draw, y):
                    draw.line((self.MARGIN * 3, y + 4, width - self.MARGIN * 3, y + 4),
                              fill="white")
                blocks.append((paint_rule, rule_h))
                self.body_top += rule_h

            # Wrapping depends on whether a scrollbar is taking room, which
            # depends on the height wrapping produces. Measure without it,
            # then redo it if the page turned out to need one.
            body_width = _ui_width(self.ui) - (self.MARGIN * 2)
            for _ in range(2):
                lines = _wrap_lines(self.ui, self.body, self.font_small, body_width)
                text_blocks = []
                for line in lines:
                    if not line:
                        # A paragraph break is a breath, not an empty line.
                        text_blocks.append((None, self.line_height // 2))
                        continue

                    def paint_line(canvas, draw, y, line=line):
                        draw.text((self.MARGIN, y), line, font=self.font_small,
                                  fill="white")
                    text_blocks.append((paint_line, self.line_height))

                height = sum(h for _, h in blocks) + sum(h for _, h in text_blocks)
                narrowed = body_width - self.SCROLLBAR_W
                if height <= self.viewport_height or body_width == narrowed:
                    break
                body_width = narrowed
            blocks.extend(text_blocks)
        return blocks

    # --- drawing ----------------------------------------------------------

    def draw(self):
        ui = self.ui
        width = _ui_width(ui)
        bottom = _content_bottom(ui)
        ui.draw.rectangle((0, 0, width, bottom), fill="black")

        if self.header:
            ui.draw.text((self.MARGIN, self.header_box[1]), self.header,
                         font=self.font_small, fill="white")
            ui.draw.line((self.MARGIN, self.divider_y, width - self.MARGIN,
                          self.divider_y), fill="white")

        # The column is painted into its own image and pasted, so scrolled
        # text is clipped by construction rather than by careful arithmetic.
        column = Image.new("RGB", (width, max(1, self.viewport_height)), "black")
        painter = ImageDraw.Draw(column)
        # A page that fits is centred: a few words pinned to the top of an
        # otherwise black screen reads as a crash rather than as an answer.
        y = -self.offset
        if not self.scrollable:
            y += (self.viewport_height - self.content_height) // 2
        for paint, height in self._blocks:
            # Blocks that would be sliced by the bottom edge are left for the
            # next scroll: half a line of type at the fold reads as a bug,
            # and the scrollbar is what says there is more to come.
            if paint is not None and y + height > 0 \
                    and y + height <= self.viewport_height:
                paint(column, painter, y)
            y += height
        ui.canvas.paste(column, (0, self.viewport[1]))

        if self.scrollable:
            self._draw_scrollbar()

        SoftKeyBar(ui).update(self.softkey_text, present=False)
        ui.fb.update(ui.canvas)

    def _draw_scrollbar(self):
        ui = self.ui
        x = _ui_width(ui) - 5
        top = self.viewport[1] + 2
        base = self.viewport[3] - 2
        ui.draw.line((x, top, x, base), fill="white", width=2)
        travel = base - top - 10
        position = top + int(travel * (self.offset / float(self.max_offset or 1)))
        ui.draw.rectangle((x - 3, position, x + 3, position + 10), fill="white")

    # --- input ------------------------------------------------------------

    def handle_key(self, key):
        """Scroll on UP/DOWN. True when the page moved."""
        if key == 108:
            new = min(self.max_offset, self.offset + self.line_height)
        elif key == 103:
            new = max(0, self.offset - self.line_height)
        else:
            return False
        if new == self.offset:
            return False
        self.offset = new
        self.draw()
        return True

    def show(self):
        """Blocking. Returns the key that left the page."""
        self.draw()
        while True:
            key = self.ui.wait_for_key()
            if key in self.accept_keys or key in self.cancel_keys:
                return key
            self.handle_key(key)

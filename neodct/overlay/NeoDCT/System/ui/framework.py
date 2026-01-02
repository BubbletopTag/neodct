# NeoDCT framework.py

import select
import os
import math
import time

class AppSelector:
    def __init__(self, title, items, ui, background=None):
        self.title = title
        self.items = items # This is now a LIST OF DICTS: [{"name": "Phonebook", "icon": "..."}]
        self.ui = ui
        self.background = background # Store the background image
        self.selected_index = 0
        
    def draw(self):
        # 1. Background
        if self.background:
            self.ui.canvas.paste(self.background, (0, 0))
        else:
            self.ui.draw.rectangle((0, 0, 240, 240), fill="black")
        
        if not self.items:
            self.ui.draw.text((80, 100), "No Apps", font=self.ui.font_n, fill="white")
            self.ui.fb.update(self.ui.canvas)
            return

        current_app = self.items[self.selected_index]
        
        # 2. Draw Header (App Name) - Centered, Large
        name = current_app["name"]
        w, h = self.ui.get_text_size(name, self.ui.font_xl)
        self.ui.draw.text(((240 - w)//2, 40), name, font=self.ui.font_xl, fill="white")
        
        # 3. Draw Icon (Centered)
        icon_path = current_app.get("icon")
        if icon_path:
            img = self.ui.get_image(icon_path)
            if img:
                ix = (240 - img.width) // 2
                iy = 90 
                # If we have a background, we need to paste the icon using its alpha mask
                # otherwise transparent pixels will be black if 'img' is just RGB
                if self.background:
                    self.ui.canvas.paste(img, (ix, iy), img)
                else:
                    self.ui.canvas.paste(img, (ix, iy), img)
            else:
                self.ui.draw.rectangle((95, 90, 145, 140), outline="white")
                self.ui.draw.text((105, 105), "?", font=self.ui.font_xl, fill="white")

        # 4. Draw Footer "Select"
        w, h = self.ui.get_text_size("Select", self.ui.font_n)
        self.ui.draw.text(((240 - w)//2, 210), "Select", font=self.ui.font_n, fill="white")

        # 5. Draw "Nokia Style" Scrollbar (Right Edge)
        bar_x = 230
        self.ui.draw.line((bar_x, 40, bar_x, 200), fill="white", width=2)
        
        # Calculate Notch Position
        if len(self.items) > 1:
            step = 160 / (len(self.items) - 1)
            notch_y = 40 + (self.selected_index * step)
        else:
            notch_y = 40
            
        self.ui.draw.rectangle((bar_x - 4, notch_y - 3, bar_x + 2, notch_y + 3), fill="white")
        
        # Optional: Draw Page Number "4"
        page_num = str(self.selected_index + 1)
        w, h = self.ui.get_text_size(page_num, self.ui.font_n)
        self.ui.draw.text((220, 10), page_num, font=self.ui.font_n, fill="white")

        self.ui.fb.update(self.ui.canvas)

    def show(self):
        """ Blocking loop """
        
        # --- INPUT FLUSH ---
        if hasattr(self.ui, 'keypad_fd'):
            while True:
                r, w, x = select.select([self.ui.keypad_fd], [], [], 0.01)
                if r:
                    try: os.read(self.ui.keypad_fd, 24)
                    except: pass
                else: break 

        self.draw() 
        
        while True:
            key = self.ui.wait_for_key()
            
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
        self.height = 30
        self.y_start = 240 - self.height
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
        wallpaper = getattr(self.ui, "wallpaper", None)
        
        if self.is_transparent and wallpaper:
            # TRANSPARENT MODE (Home Screen only)
            # Crop the bottom strip from the wallpaper and paste it
            box = (0, self.y_start, 240, 240)
            try:
                bg_slice = wallpaper.crop(box)
                self.ui.canvas.paste(bg_slice, box)
            except Exception:
                self.ui.draw.rectangle((0, self.y_start, 240, 240), fill="black")
        else:
            # OPAQUE MODE (Apps, Dialogs, Lists)
            # Always draw black to cover scrolling lists or game graphics
            self.ui.draw.rectangle((0, self.y_start, 240, 240), fill="black")

        if new_text:
            w, h = self.ui.get_text_size(new_text, self.ui.font_n)
            x = (240 - w) // 2
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
        x = 235 - w
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
        self.max_lines = 3 # We can fit 3 items comfortably below the title
        
    def draw(self):
        # 1. Clear Screen
        self.ui.draw.rectangle((0, 0, 240, 210), fill="black")
        
        # 2. Draw Title and Header
        self.ui.draw.text((5, 5), self.title, font=self.ui.font_xl, fill="white")
        self.header.draw(self.selected_index + 1)
        
        # 3. Draw Divider Line
        self.ui.draw.line((0, 35, 240, 35), fill="white")

        # 4. Draw List Items
        y_start = 45
        line_height = 55
        
        for i in range(self.max_lines):
            item_idx = self.window_start + i
            if item_idx >= len(self.items): break
            
            y = y_start + (i * line_height)
            item_text = self.items[item_idx]
            
            # Draw Selection Box
            if item_idx == self.selected_index:
                self.ui.draw.rectangle((0, y, 225, y+50), fill="white")
                self.ui.draw.text((10, y+10), item_text, font=self.ui.font_n, fill="black")
            else:
                self.ui.draw.text((10, y+10), item_text, font=self.ui.font_n, fill="white")

        # 5. Draw Scrollbar
        bar_x = 235
        self.ui.draw.line((bar_x, 45, bar_x, 205), fill="gray", width=1)
        
        if len(self.items) > 1:
            track_h = 160
            step = track_h / (len(self.items) - 1)
            notch_y = 45 + (self.selected_index * step)
        else:
            notch_y = 45
            
        self.ui.draw.rectangle((bar_x - 3, notch_y - 3, bar_x + 3, notch_y + 3), fill="white")

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
    def __init__(self, ui, title, prompt, initial_text=""):
        self.ui = ui
        self.title = title   # Header Title (e.g. "Add Entry")
        self.prompt = prompt # Instruction (e.g. "Name:")
        self.text = initial_text
        
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
        # 1. Clear Screen
        self.ui.draw.rectangle((0, 0, 240, 210), fill="black")
        
        # 2. Header
        self.ui.draw.text((5, 5), self.title, font=self.ui.font_xl, fill="white")
        self.ui.draw.line((0, 35, 240, 35), fill="white")
        
        # 3. Prompt
        self.ui.draw.text((10, 55), self.prompt, font=self.ui.font_n, fill="white")
        
        # 4. Input Box Container
        box_y = 85
        self.ui.draw.rectangle((10, box_y, 230, box_y + 40), outline="white")
        
        # 5. The Text
        display_text = self.text + ("_" if blink_state else "")
        self.ui.draw.text((15, box_y + 10), display_text, font=self.ui.font_n, fill="white")
        
        self.ui.fb.update(self.ui.canvas)

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

            # ENTER / NAVI-CENTER -> Confirm (Legacy '50' Removed)
            if key in (28, 96): 
                return self.text

            # BACKSPACE / C BUTTON
            elif key == 14:
                if len(self.text) > 0:
                    self.text = self.text[:-1]
                    self.draw(cursor_on)
                else:
                    return None
            
            # TYPING
            elif key in self.DEV_KEYMAP:
                char = self.DEV_KEYMAP[key]
                if len(self.text) == 0: char = char.upper()
                self.text += char
                self.draw(cursor_on)

"""

TextInputLong is a long-form text entry widget for composing messages and notes.

"""
class TextInputLong:
    def __init__(self, ui, title, initial_text="", on_empty_backspace=None):
        self.ui = ui
        self.title = title
        self.text = initial_text or ""
        self.cursor = len(self.text)
        self.on_empty_backspace = on_empty_backspace
        self.font = getattr(ui, "font_s", None) or ui.font_n
        self.text_area_top = 45
        self.text_area_bottom = 210
        self._last_backspace_time = 0.0
        self._backspace_hold_start = None
        self._backspace_repeat_count = 0
        self._backspace_hold_gap = 0.18
        self._backspace_hold_duration = 0.8
        self._backspace_hold_repeats = 8

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
        return self._wrap_text(display_text, 220)

    def draw(self, blink_state=True):
        self.ui.draw.rectangle((0, 0, 240, 210), fill="black")

        # Header
        self.ui.draw.text((5, 5), self.title, font=self.ui.font_xl, fill="white")
        char_count = str(len(self.text))
        w, _ = self.ui.get_text_size(char_count, self.ui.font_n)
        self.ui.draw.text((235 - w, 5), char_count, font=self.ui.font_n, fill="white")
        self.ui.draw.line((0, 35, 240, 35), fill="white")

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
        if key == 14:
            now = time.time()
            if len(self.text) == 0:
                if callable(self.on_empty_backspace):
                    self.on_empty_backspace()
                return "empty_backspace"
            gap = now - self._last_backspace_time
            if gap > self._backspace_hold_gap:
                self._backspace_hold_start = now
                self._backspace_repeat_count = 1
            else:
                self._backspace_repeat_count += 1
                if self._backspace_hold_start and (now - self._backspace_hold_start) >= self._backspace_hold_duration:
                    if self._backspace_repeat_count >= self._backspace_hold_repeats:
                        self.clear_text()
                        self._backspace_hold_start = None
                        self._backspace_repeat_count = 0
                        self._last_backspace_time = now
                        return "cleared"
            if self.cursor > 0:
                self.text = self.text[:self.cursor - 1] + self.text[self.cursor:]
                self.cursor = max(0, self.cursor - 1)
            self._last_backspace_time = now
            return "backspace"

        if key in self.DEV_KEYMAP:
            self._backspace_hold_start = None
            self._backspace_repeat_count = 0
            char = self.DEV_KEYMAP[key]
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

        # Full clear
        ui.draw.rectangle((0, 0, 240, 240), fill="black")

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
        elif icon:
            y = self.margin + icon.height + 6

        # Body
        content_bottom = 240 - 30  # SoftKeyBar height
        max_w = 240 - (self.margin * 2)
        lines = self._wrap_text(self.message, self.font_body, max_w)

        line_h = ui.get_text_size("Ag", self.font_body)[1] + 3
        max_lines = max(1, int((content_bottom - y - self.margin) / line_h))

        if len(lines) > max_lines:
            lines = lines[:max_lines]
            if lines:
                lines[-1] = (lines[-1] + " …") if not lines[-1].endswith("…") else lines[-1]

        for line in lines:
            ui.draw.text((self.margin, y), line, font=self.font_body, fill="white")
            y += line_h

        # Softkey (draw but don't present yet)
        SoftKeyBar(ui).update(self.button_text, present=False)

        # Present once
        ui.fb.update(ui.canvas)

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

        # Layout tuning for 240x240
        self._content_top = 40     # start of big text area
        self._content_bottom = 200 # end of big text area
        self._bar_x = 235          # scrollbar x

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
        # Clear full screen
        self.ui.draw.rectangle((0, 0, 240, 240), fill="black")

        # Title + divider
        self.ui.draw.text((5, 5), self.title, font=self.ui.font_xl, fill="white")
        self.ui.draw.line((0, 35, 240, 35), fill="white")

        # Empty state
        if not self.items:
            self.header.draw(None)
            self.ui.draw.text((70, 110), "No Items", font=self.ui.font_n, fill="white")
            if self.softkey and self._show_select_hint:
                self.softkey.update(None, present=False)
            self.ui.fb.update(self.ui.canvas)
            return

        # Header "root-sub"
        self.header.draw(self.selected_index + 1)

        # Main page text (large, 2-line wrap)
        name = self._get_item_name(self.selected_index)
        max_w = 210  # leave room for scrollbar
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
        track_top = 40
        track_bottom = 200
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
        if hasattr(self.ui, "keypad_fd"):
            while True:
                r, _, _ = select.select([self.ui.keypad_fd], [], [], 0.01)
                if r:
                    try:
                        os.read(self.ui.keypad_fd, 24)
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

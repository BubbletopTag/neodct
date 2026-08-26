# /NeoDCT/System/ui/Dialer/call_screen.py
#
# Nokia-style in-call UI for NeoDCT.
# Invoked by kernel/main.py via:
#   dialer_ui.show_calling(self, number, name=None)

import os
import time
import struct
import select
from System.ui.framework import SoftKeyBar

KEYPAD_PATH = "/dev/input/event0"
WIDTH = 240
HEIGHT = 175


def _flush_input(fd):
    """Drain any pending key events so we don't instantly 'End' due to buffered input."""
    try:
        while True:
            r, _, _ = select.select([fd], [], [], 0.0)
            if not r:
                break
            os.read(fd, 24)
    except Exception:
        pass


def _read_keypress(fd, timeout=0.10):
    """
    Returns a Linux input key code on key-press, else None.
    Matches the event unpacking style used in kernel/main.py.
    """
    try:
        r, _, _ = select.select([fd], [], [], timeout)
        if not r:
            return None

        data = os.read(fd, 24)
        if len(data) == 24:
            sec, usec, etype, code, val = struct.unpack('llHHI', data)
            if etype == 1 and val == 1:
                return code

        # Some platforms return 16-byte events
        if len(data) == 16:
            sec, usec, etype, code, val = struct.unpack('IIHHI', data)
            if etype == 1 and val == 1:
                return code

    except Exception:
        return None

    return None


def _draw_handset_icon(draw, x, y):
    """
    Simple fallback icon (you can replace with a PNG later).
    Draws a tiny handset-like shape.
    """
    # Outer silhouette
    draw.rectangle((x, y + 2, x + 18, y + 10), outline="white")
    # "ear" and "mouth" blocks
    draw.rectangle((x + 1, y + 3, x + 5, y + 5), fill="white")
    draw.rectangle((x + 13, y + 7, x + 17, y + 9), fill="white")


def _fit_text(ui, text, max_width, prefer_font):
    """
    If text doesn't fit, fall back to a smaller font or truncate with '…'.
    """
    # Try preferred font first
    w, _ = ui.get_text_size(text, prefer_font)
    if w <= max_width:
        return text, prefer_font

    # Try smaller font
    small_font = getattr(ui, "font_s", prefer_font)
    w2, _ = ui.get_text_size(text, small_font)
    if w2 <= max_width:
        return text, small_font

    # Truncate with ellipsis using small font
    if not text:
        return "", small_font

    ell = "…"
    # Leave room for ellipsis
    lo, hi = 0, len(text)
    best = ""
    while lo <= hi:
        mid = (lo + hi) // 2
        candidate = text[:mid] + ell
        cw, _ = ui.get_text_size(candidate, small_font)
        if cw <= max_width:
            best = candidate
            lo = mid + 1
        else:
            hi = mid - 1

    return best if best else ell, small_font


def draw_call_screen(ui, number, name=None):
    """
    Draw a Nokia-like 'Call 1' screen with the dialed number under it.
    Top-right is clock (per your note).
    Bottom center softkey label is 'End'.
    """
    screen_w = getattr(ui, "W", WIDTH)
    screen_h = getattr(ui, "H", HEIGHT)
    softkey_h = getattr(ui, "SOFTKEY_H", 30)
    content_bottom = getattr(ui, "content_bottom", screen_h - softkey_h)

    # Clear full screen
    ui.draw.rectangle((0, 0, screen_w, screen_h), fill="black")

    # --- Top left: handset icon (placeholder) ---
    # (You can replace this with a PNG later)
    _draw_handset_icon(ui.draw, 8, 10)

    # --- Top right: CLOCK ---
   # clock_text = time.strftime("%H:%M")
   # clock_font = getattr(ui, "font_s", None)
   # ui.draw.text((193, 12), clock_text, font=clock_font, fill="white")


    # --- Main labels (left aligned like the reference) ---
    # Live progress from AT+CLCC: Calling... -> Ringing... -> Call 1.
    modem = getattr(ui, "modem", None)
    status, secs = ("CONNECTED", None)
    if modem is not None and hasattr(modem, "call_status"):
        status, secs = modem.call_status()
    label = {"CALLING": "Calling...", "RINGING": "Ringing..."}.get(status, "Call 1")
    label_font = getattr(ui, "font_n", None) or getattr(ui, "font_s", None)

    label_x = max(34, int(screen_w * 0.23))
    label_y = max(50, int(content_bottom * 0.20))
    ui.draw.text((label_x, label_y), label, font=label_font, fill="white")

    # Number directly under "Call 1"
    # (This is the specific change you requested.)
    num_text = number if number else ""
    # Fit within screen width with some right padding
    fitted_num, num_font = _fit_text(ui, num_text, max_width=screen_w - label_x - 10, prefer_font=label_font)

    num_y = label_y + 26  # Nokia-ish spacing under the label
    ui.draw.text((label_x, num_y), fitted_num, font=num_font, fill="white")

    # Connected: running mm:ss under the number.
    if secs is not None:
        timer_text = "%02d:%02d" % (secs // 60, secs % 60)
        timer_font = getattr(ui, "font_s", num_font)
        ui.draw.text((label_x, num_y + 24), timer_text, font=timer_font, fill="gray")

    # Optional: if you ever want the contact name too, you can add it above/below.
    # Keeping it off by default to match your request.

    home_elements = (getattr(ui, "home_layout", None) or {}).get("elements", [])
    for el in home_elements:
        if el.get("type") == "text" and el.get("text") == "12:00":
            ui.render_element(el)  # exact same placement and anchoring as HOME
            break
    for el in home_elements:
        if el.get("type") == "icon_set" and el.get("prefix") in ("bat", "sig"):
            ui.render_element(el)



def show_calling(ui, number, name=None):
    """
    Blocking call UI. Exits when user presses End.
    Uses key 14 (Backspace/C) and also allows 28 (center) as End.
    """
    softkey = SoftKeyBar(ui)
    use_ui_reader = hasattr(ui, "read_keypress")
    fd = None

    # Legacy fallback path for standalone execution.
    if not use_ui_reader:
        if hasattr(ui, "keypad_fd"):
            fd = ui.keypad_fd
        else:
            fd = os.open(KEYPAD_PATH, os.O_RDONLY | os.O_NONBLOCK)
            ui.keypad_fd = fd
        _flush_input(fd)

    # Main loop: update screen periodically so clock updates
    last_draw = 0.0
    while True:
        now = time.time()
        if now - last_draw >= 0.25:
            draw_call_screen(ui, number, name=name)
            ui.fb.update(ui.canvas)
            last_draw = now
            softkey.update("End")

        if use_ui_reader:
            key = ui.read_keypress(0.10)
        else:
            key = _read_keypress(fd, timeout=0.10)

        # Remote hangup / call failure: read_keypress pumped the modem
        # URCs (NO CARRIER, VOICE CALL: END), so IDLE means it's over.
        modem = getattr(ui, "modem", None)
        if modem is not None and modem.state == "IDLE":
            return

        if key is None:
            continue

        # End call: C / Backspace (14) or Center/Enter (28)
        if key in (14, 28):
            try:
                ui.modem.hangup()
            except Exception:
                pass
            return

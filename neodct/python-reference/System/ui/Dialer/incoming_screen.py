# /NeoDCT/System/ui/Dialer/incoming_screen.py
#
# Nokia 3310-style incoming call screen (see the reference photo):
#   * caller name (from the phonebook) or number across the top, shrunk
#     to fit -- a full USA number "+1 (616) 555-1234" never runs off,
#   * flashing "calling" near the bottom left,
#   * softkey says "Answer"; C/backspace declines.
#
# Invoked by core/main.py when ModemService raises an incoming call --
# it interrupts whatever app was running, like the real thing.

import time

from System.ui.framework import SoftKeyBar

KEY_ANSWER = 28
KEY_DECLINE = 14
BLINK_S = 0.5
REFRESH_S = 0.1


def _lookup_contact_name(number):
    """Phonebook name for a caller, or None. Digits-only comparison so
    stored "555-1234" still matches a "+15551234" caller ID."""
    if not number:
        return None
    try:
        from System.apps.PhoneBook.shared.list_ui import get_all_contacts
        wanted = "".join(c for c in str(number) if c.isdigit())
        if not wanted:
            return None
        for row in get_all_contacts():
            stored = "".join(c for c in str(row[2]) if c.isdigit())
            if not stored:
                continue
            # Compare the last 10 digits: country code/trunk prefixes
            # differ between the SIM's caller ID and stored contacts.
            if stored[-10:] == wanted[-10:] or stored == wanted:
                return row[1]
    except Exception:
        pass
    return None


def _fit_caller_text(ui, text, max_width):
    """Largest font that fits, truncating only as a last resort."""
    for attr in ("font_n", "font_s"):
        font = getattr(ui, attr, None)
        if font is None:
            continue
        if ui.get_text_size(text, font)[0] <= max_width:
            return text, font
    font = getattr(ui, "font_s", None) or getattr(ui, "font_n", None)
    trimmed = text
    while trimmed and ui.get_text_size(trimmed + "...", font)[0] > max_width:
        trimmed = trimmed[:-1]
    return (trimmed + "..." if trimmed else "?"), font


def draw_incoming_screen(ui, caller_text, blink_on):
    screen_w = getattr(ui, "W", 240)
    screen_h = getattr(ui, "H", 175)
    content_bottom = getattr(ui, "content_bottom", screen_h - 30)

    ui.draw.rectangle((0, 0, screen_w, screen_h), fill="black")

    # Caller: name or number, centered across the top.
    text, font = _fit_caller_text(ui, caller_text, screen_w - 16)
    w = ui.get_text_size(text, font)[0]
    ui.draw.text(((screen_w - w) // 2, max(18, int(content_bottom * 0.18))),
                 text, font=font, fill="white")

    # Flashing "calling" near the bottom left, like the 3310 -- starting
    # just right of the signal column (36px-wide asset at x=7, scaled by
    # H/240 exactly like render_element scales status icons).
    if blink_on:
        calling_x = 7 + int(36 * (screen_h / 240.0)) + 6
        label_font = getattr(ui, "font_n", font)
        ui.draw.text((calling_x, content_bottom - 26), "calling",
                     font=label_font, fill="white")

    # Status icons stay put (signal/battery), same as the in-call screen.
    home_elements = (getattr(ui, "home_layout", None) or {}).get("elements", [])
    for el in home_elements:
        if el.get("type") == "icon_set" and el.get("prefix") in ("bat", "sig"):
            ui.render_element(el)


def show_incoming(ui, number, name=None):
    """Blocking ring UI. Returns "answered", "declined", or "gone" when
    the caller hangs up first. The ringer is owned by the caller
    (core/main.py) so it starts the instant RING arrives."""
    softkey = SoftKeyBar(ui)
    modem = getattr(ui, "modem", None)

    def caller_label(num):
        return name or _lookup_contact_name(num) or num or "Unknown"

    caller_text = caller_label(number)
    blink_on = True
    last_blink = 0.0
    while True:
        now = time.monotonic()

        # A late +CLIP (caller ID lands just after the first RING) fills
        # in the name/number without waiting for the next call.
        if modem is not None and modem.caller_id and modem.caller_id != number:
            number = modem.caller_id
            caller_text = caller_label(number)
            last_blink = 0.0   # redraw now

        if now - last_blink >= BLINK_S:
            blink_on = not blink_on
            last_blink = now
            draw_incoming_screen(ui, caller_text, blink_on)
            softkey.update("Answer", present=False)
            ui.fb.update(ui.canvas)

        # read_keypress pumps the modem, so URCs land while we ring.
        key = ui.read_keypress(REFRESH_S)

        if modem is not None and modem.state not in ("RINGING", "INCOMING"):
            if modem.state == "IDLE":
                return "gone"      # caller gave up / network dropped it
            return "answered"      # somebody else moved us to CONNECTED

        if key == KEY_ANSWER:
            return "answered"
        if key == KEY_DECLINE:
            return "declined"

import os
import sqlite3
import time
from System.ui.framework import (
    MessageDialog,
    PagedList,
    HeaderWidget,
    SoftKeyBar,
    TextInput,
    TextInputLong,
    VerticalList,
)
import System.apps.PhoneBook.shared.list_ui as contact_manager

ROOT_ID_MESSAGES = 2  # matches "2-1" style header
ARROW_KEYS = (103, 105, 106, 108)
SMS_MAX_CHARS = 160  # one GSM text-mode segment
DB_DIR = "/NeoDCT/User/db"
INBOX_DB = f"{DB_DIR}/sms_inbox.db"
OUTBOX_DB = f"{DB_DIR}/sms_outbox.db"


def _screen_metrics(ui):
    screen_w = getattr(ui, "W", 240)
    screen_h = getattr(ui, "H", 175)
    softkey_h = getattr(ui, "SOFTKEY_H", 30)
    content_bottom = getattr(ui, "content_bottom", screen_h - softkey_h)
    header_y = max(30, int(screen_h * 0.11))
    return screen_w, screen_h, content_bottom, header_y


def _show_stub_screen(ui, title, root_id, sub_index):
    screen_w, screen_h, content_bottom, header_y = _screen_metrics(ui)

    ui.draw.rectangle((0, 0, screen_w, screen_h), fill="black")
    ui.draw.text((5, 5), title, font=ui.font_xl, fill="white")
    ui.draw.line((0, header_y, screen_w, header_y), fill="white")

    HeaderWidget(ui, root_id).draw(sub_index)

    y = header_y + max(16, int((content_bottom - header_y) * 0.25))
    ui.draw.text((10, y), "Not implemented", font=ui.font_n, fill="white")
    ui.draw.text((10, y + 25), "Press BACK", font=ui.font_n, fill="gray")

    SoftKeyBar(ui).update("Back", present=False)
    ui.fb.update(ui.canvas)

    while True:
        key = ui.wait_for_key()
        if key == 14:  # BACKSPACE
            return

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
            continue

        if current:
            lines.append(current)
            current = word
        else:
            trimmed = word
            while trimmed and not fits(trimmed + "..."):
                trimmed = trimmed[:-1]
            lines.append(trimmed + "..." if trimmed else "...")
            current = ""

    if current:
        lines.append(current)

    return lines

def _format_timestamp(ts):
    if not ts:
        return "Unknown time"
    return time.strftime("%Y-%m-%d %H:%M", time.localtime(ts))

def _fetch_inbox_messages():
    if not os.path.exists(INBOX_DB):
        return []
    conn = sqlite3.connect(INBOX_DB)
    c = conn.cursor()
    c.execute("SELECT id, message, sender, timestamp, is_read FROM inbox ORDER BY timestamp DESC")
    data = c.fetchall()
    conn.close()
    return data

def _fetch_outbox_messages():
    if not os.path.exists(OUTBOX_DB):
        return []
    conn = sqlite3.connect(OUTBOX_DB)
    c = conn.cursor()
    c.execute("SELECT id, message, timestamp FROM outbox ORDER BY timestamp DESC")
    data = c.fetchall()
    conn.close()
    return data

def _mark_read(message_id):
    if not os.path.exists(INBOX_DB) or message_id is None:
        return
    conn = sqlite3.connect(INBOX_DB)
    conn.execute("UPDATE inbox SET is_read = 1 WHERE id = ?", (message_id,))
    conn.commit()
    conn.close()


def _fetch_inbox_message(message_id):
    if not os.path.exists(INBOX_DB):
        return None
    conn = sqlite3.connect(INBOX_DB)
    c = conn.cursor()
    c.execute("SELECT id, message, sender, timestamp, is_read FROM inbox WHERE id = ?",
              (message_id,))
    row = c.fetchone()
    conn.close()
    return row


def open_message(ui, message_id):
    """Jump straight into one message (the home screen's Read softkey)."""
    row = _fetch_inbox_message(message_id)
    if row is None:
        return _show_inbox(ui, ROOT_ID_MESSAGES, 1)
    msg_id, message, sender, timestamp, _ = row
    _mark_read(msg_id)
    _show_message_detail(ui, "Inbox", f"{ROOT_ID_MESSAGES}-1", 1, message,
                         message_id=msg_id, sender=sender, timestamp=timestamp)


def open_inbox(ui):
    """Open the inbox list (Read softkey with several new messages)."""
    _show_inbox(ui, ROOT_ID_MESSAGES, 1)


def _delete_inbox_message(message_id):
    if not os.path.exists(INBOX_DB):
        return
    conn = sqlite3.connect(INBOX_DB)
    c = conn.cursor()
    c.execute("DELETE FROM inbox WHERE id = ?", (message_id,))
    conn.commit()
    conn.close()

def _delete_outbox_message(message_id):
    if not os.path.exists(OUTBOX_DB):
        return
    conn = sqlite3.connect(OUTBOX_DB)
    c = conn.cursor()
    c.execute("DELETE FROM outbox WHERE id = ?", (message_id,))
    conn.commit()
    conn.close()

def _save_outbox_message(text):
    os.makedirs(DB_DIR, exist_ok=True)
    conn = sqlite3.connect(OUTBOX_DB)
    c = conn.cursor()
    c.execute(
        """CREATE TABLE IF NOT EXISTS outbox
           (id INTEGER PRIMARY KEY AUTOINCREMENT,
            message TEXT,
            timestamp INTEGER)"""
    )
    c.execute("INSERT INTO outbox (message, timestamp) VALUES (?, ?)", (text, int(time.time())))
    conn.commit()
    conn.close()

class ContactNumberInput(TextInput):
    """TextInput plus PhoneBook: any arrow key opens the shared contact
    selector and autofills the picked contact's number. Numbers-only, so
    the keypad types digits/star/hash literally (no T9 multi-tap)."""

    def __init__(self, ui, title, prompt, initial_text=""):
        super().__init__(ui, title, prompt, initial_text,
                         input_filter="numbers")

    def show(self):
        softkey = SoftKeyBar(self.ui)
        softkey.update("OK")
        cursor_on = True
        last_blink = time.time()
        self.draw(cursor_on)

        while True:
            if time.time() - last_blink > 0.5:
                cursor_on = not cursor_on
                last_blink = time.time()
                self.draw(cursor_on)

            key = self.ui.wait_for_key()
            if key is None:
                continue

            if key in ARROW_KEYS:
                picked = contact_manager.show_contact_selector(
                    self.ui, title="Contacts", btn_text="OK",
                    header_root=f"{ROOT_ID_MESSAGES}-3")
                if picked:
                    contact, _ = picked   # row: (id, name, number, speed_dial)
                    self.text = str(contact[2])
                self.draw(cursor_on)
                softkey.update("OK")
                continue

            action = self.handle_key(key)
            if action == "confirm":
                return self.text
            if action == "cancel":
                return None
            if action in ("typed", "backspace", "mode"):
                self.draw(cursor_on)


def _draw_sending(ui, number):
    screen_w, _, content_bottom, _ = _screen_metrics(ui)
    ui.draw.rectangle((0, 0, screen_w, content_bottom), fill="black")
    w, h = ui.get_text_size("Sending...", ui.font_n)
    y = max(10, (content_bottom - h) // 2 - 12)
    ui.draw.text(((screen_w - w) // 2, y), "Sending...", font=ui.font_n, fill="white")
    w2, _ = ui.get_text_size(number, ui.font_s)
    ui.draw.text(((screen_w - w2) // 2, y + h + 8), number, font=ui.font_s, fill="gray")
    SoftKeyBar(ui).update("", present=False)
    ui.fb.update(ui.canvas)


def _send_message_flow(ui, text, root_id, sub_index):
    """Number prompt (arrows open the contact picker) -> modem send ->
    result dialog. Returns True when the network accepted the message."""
    text = (text or "").strip()
    if not text:
        MessageDialog(ui, "Message is empty!").show()
        return False
    if len(text) > SMS_MAX_CHARS:
        MessageDialog(ui, "Too long for one SMS (%d/%d)."
                      % (len(text), SMS_MAX_CHARS)).show()
        return False

    number = ContactNumberInput(ui, "Send To", "Number:").show()
    if number is None:
        return False
    number = "".join(c for c in number if c in "0123456789*#+")
    if not number:
        MessageDialog(ui, "No number given!").show()
        return False

    modem = getattr(ui, "modem", None)
    if modem is None:
        MessageDialog(ui, "ModemService is not running.").show()
        return False

    _draw_sending(ui, number)
    ok, detail = modem.send_sms(number, text)
    if ok:
        MessageDialog(ui, "Message sent!").show()
        return True
    MessageDialog(ui, "Send failed: %s" % detail).show()
    return False


def _show_empty_state(ui, title, root_id, sub_index, message):
    screen_w, screen_h, content_bottom, header_y = _screen_metrics(ui)

    ui.draw.rectangle((0, 0, screen_w, screen_h), fill="black")
    ui.draw.text((5, 5), title, font=ui.font_xl, fill="white")
    ui.draw.line((0, header_y, screen_w, header_y), fill="white")
    HeaderWidget(ui, root_id).draw(sub_index)

    w, h = ui.get_text_size(message, ui.font_n)
    y = header_y + max(0, ((content_bottom - header_y) - h) // 2)
    ui.draw.text(((screen_w - w) // 2, y), message, font=ui.font_n, fill="white")
    SoftKeyBar(ui).update("Back", present=False)
    ui.fb.update(ui.canvas)

    while True:
        key = ui.wait_for_key()
        if key == 14:
            return

def _show_message_detail(ui, title, root_id, sub_index, message, message_id=None, sender=None, timestamp=None):
    screen_w, screen_h, content_bottom, header_y = _screen_metrics(ui)
    softkey = SoftKeyBar(ui)
    header = HeaderWidget(ui, root_id)

    timestamp_text = _format_timestamp(timestamp)
    meta_lines = []
    if sender:
        meta_lines.append(f"From: {sender}")
    meta_lines.append(f"Time: {timestamp_text}")

    body_lines = _wrap_text(ui, message, max(20, screen_w - 20), ui.font_n)

    while True:
        ui.draw.rectangle((0, 0, screen_w, screen_h), fill="black")
        ui.draw.text((5, 5), title, font=ui.font_xl, fill="white")
        ui.draw.line((0, header_y, screen_w, header_y), fill="white")
        header.draw(sub_index)

        y = header_y + 10
        for line in meta_lines:
            if y > content_bottom - 18:
                break
            ui.draw.text((10, y), line, font=ui.font_s, fill="gray")
            y += 18

        y += 4
        for line in body_lines:
            if y > content_bottom - 22:
                break
            ui.draw.text((10, y), line, font=ui.font_n, fill="white")
            y += 22

        softkey.update("Options", present=False)
        ui.fb.update(ui.canvas)

        key = ui.wait_for_key()
        if key == 14:
            return
        if key in (28, 96):
            if title == "Inbox":
                options = VerticalList(ui, "Options", ["Just Erase for now"], app_id=root_id)
                selection = options.show()
                if selection == 0 and message_id is not None:
                    _delete_inbox_message(message_id)
                    MessageDialog(ui, "Erased!").show()
                    return "deleted"
            elif title == "Outbox":
                options = VerticalList(ui, "Options", ["Erase", "Send"], app_id=root_id)
                selection = options.show()
                if selection == 0 and message_id is not None:
                    _delete_outbox_message(message_id)
                    MessageDialog(ui, "Erased!").show()
                    return "deleted"
                if selection == 1:
                    _send_message_flow(ui, message, root_id, sub_index)

def _show_inbox(ui, root_id, sub_index):
    while True:
        messages = _fetch_inbox_messages()
        if not messages:
            _show_empty_state(ui, "Inbox", f"{root_id}-{sub_index}", None, "No Messages")
            return

        list_items = [
            f"{sender}" if is_read else f"* {sender}"
            for _, message, sender, _, is_read in messages
        ]
        header_root = f"{root_id}-{sub_index}"
        v_list = VerticalList(ui, "Inbox", list_items, app_id=header_root)
        softkey = SoftKeyBar(ui)

        softkey.update("Open", present=False)
        selection_index = v_list.show()
        if selection_index == -1:
            return
        message_id, message, sender, timestamp, _ = messages[selection_index]
        _mark_read(message_id)
        result = _show_message_detail(
            ui,
            "Inbox",
            header_root,
            selection_index + 1,
            message,
            message_id=message_id,
            sender=sender,
            timestamp=timestamp,
        )
        if result == "deleted":
            continue

def _show_outbox(ui, root_id, sub_index):
    while True:
        messages = _fetch_outbox_messages()
        if not messages:
            _show_empty_state(ui, "Outbox", f"{root_id}-{sub_index}", None, "No Messages")
            return

        list_items = [message for _, message, _ in messages]
        header_root = f"{root_id}-{sub_index}"
        v_list = VerticalList(ui, "Outbox", list_items, app_id=header_root)
        softkey = SoftKeyBar(ui)

        softkey.update("Open", present=False)
        selection_index = v_list.show()
        if selection_index == -1:
            return
        message_id, message, timestamp = messages[selection_index]
        result = _show_message_detail(
            ui,
            "Outbox",
            header_root,
            selection_index + 1,
            message,
            message_id=message_id,
            timestamp=timestamp,
        )
        if result == "deleted":
            continue

def _show_write_message(ui, root_id, sub_index):
    softkey = SoftKeyBar(ui)
    input_widget = TextInputLong(ui, "Write")

    cursor_on = True
    last_blink = time.time()
    input_widget.draw(cursor_on)
    softkey.update("Options")

    while True:
        if time.time() - last_blink > 0.5:
            cursor_on = not cursor_on
            last_blink = time.time()
            input_widget.draw(cursor_on)
            softkey.update("Options")

        key = ui.wait_for_key()
        if key is None:
            continue

        if key in (28, 96):
            options = VerticalList(ui, "Options", ["Send", "Save"], app_id=f"{root_id}-{sub_index}")
            selection = options.show()
            if selection == 0:
                if _send_message_flow(ui, input_widget.get_text(), root_id, sub_index):
                    return   # sent -- leave the composer
                # failed/cancelled: fall through, keep the draft on screen
            elif selection == 1:
                _save_outbox_message(input_widget.get_text())
                MessageDialog(ui, "Saved!").show()

            input_widget.draw(cursor_on)
            softkey.update("Options")
            continue

        result = input_widget.handle_key(key)
        if result == "empty_backspace":
            return
        input_widget.draw(cursor_on)
        softkey.update("Options")

def run(ui):
    menu = PagedList(
        ui=ui,
        title="Messages",
        items=[
            "Inbox",
            "Outbox",
            "Write Message",
        ],
        root_id=ROOT_ID_MESSAGES,
        show_select_hint=True,
    )

    while True:
        sel = menu.show()
        if sel < 0:
            return

        if sel == 0:
            _show_inbox(ui, ROOT_ID_MESSAGES, 1)
        elif sel == 1:
            _show_outbox(ui, ROOT_ID_MESSAGES, 2)
        elif sel == 2:
            _show_write_message(ui, ROOT_ID_MESSAGES, 3)

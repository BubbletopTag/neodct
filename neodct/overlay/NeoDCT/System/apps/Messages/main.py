import os
import sqlite3
import time
from System.ui.framework import (
    MessageDialog,
    PagedList,
    HeaderWidget,
    SoftKeyBar,
    TextInputLong,
    VerticalList,
)

ROOT_ID_MESSAGES = 2  # matches "2-1" style header
DB_DIR = "/NeoDCT/User/db"
INBOX_DB = f"{DB_DIR}/sms_inbox.db"
OUTBOX_DB = f"{DB_DIR}/sms_outbox.db"

def _show_stub_screen(ui, title, root_id, sub_index):
    ui.draw.rectangle((0, 0, 240, 240), fill="black")
    ui.draw.text((5, 5), title, font=ui.font_xl, fill="white")
    ui.draw.line((0, 35, 240, 35), fill="white")

    HeaderWidget(ui, root_id).draw(sub_index)

    ui.draw.text((10, 90), "Not implemented", font=ui.font_n, fill="white")
    ui.draw.text((10, 115), "Press BACK", font=ui.font_n, fill="gray")

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

def _delete_inbox_message(message_id):
    conn = sqlite3.connect(INBOX_DB)
    c = conn.cursor()
    c.execute("DELETE FROM inbox WHERE id=?", (message_id,))
    conn.commit()
    conn.close()

def _delete_outbox_message(message_id):
    conn = sqlite3.connect(OUTBOX_DB)
    c = conn.cursor()
    c.execute("DELETE FROM outbox WHERE id=?", (message_id,))
    conn.commit()
    conn.close()

def _show_erased_notice(ui):
    ui.draw.rectangle((0, 0, 240, 210), fill="black")
    ui.draw.text((60, 100), "Erased!", font=ui.font_xl, fill="white")
    ui.fb.update(ui.canvas)
    time.sleep(1)

def _show_empty_state(ui, title, root_id, sub_index, message):
    ui.draw.rectangle((0, 0, 240, 240), fill="black")
    ui.draw.text((5, 5), title, font=ui.font_xl, fill="white")
    ui.draw.line((0, 35, 240, 35), fill="white")
    HeaderWidget(ui, root_id).draw(sub_index)

    w, _ = ui.get_text_size(message, ui.font_n)
    ui.draw.text(((240 - w) // 2, 110), message, font=ui.font_n, fill="white")
    SoftKeyBar(ui).update("Back", present=False)
    ui.fb.update(ui.canvas)

    while True:
        key = ui.wait_for_key()
        if key == 14:
            return

def _show_message_detail(
    ui,
    title,
    root_id,
    sub_index,
    message,
    sender=None,
    timestamp=None,
    message_id=None,
    box_type=None,
):
    softkey = SoftKeyBar(ui)
    header = HeaderWidget(ui, root_id)

    timestamp_text = _format_timestamp(timestamp)
    meta_lines = []
    if sender:
        meta_lines.append(f"From: {sender}")
    meta_lines.append(f"Time: {timestamp_text}")

    body_lines = _wrap_text(ui, message, 220, ui.font_n)

    while True:
        ui.draw.rectangle((0, 0, 240, 240), fill="black")
        ui.draw.text((5, 5), title, font=ui.font_xl, fill="white")
        ui.draw.line((0, 35, 240, 35), fill="white")
        header.draw(sub_index)

        y = 45
        for line in meta_lines:
            ui.draw.text((10, y), line, font=ui.font_s, fill="gray")
            y += 18

        y += 4
        for line in body_lines:
            if y > 210:
                break
            ui.draw.text((10, y), line, font=ui.font_n, fill="white")
            y += 22

        softkey.update("Options", present=False)
        ui.fb.update(ui.canvas)

        key = ui.wait_for_key()
        if key == 14:
            return
        if key in (28, 96):
            if box_type == "inbox":
                options = ["Erase"]
            elif box_type == "outbox":
                options = ["Erase", "Send"]
            else:
                options = []
            if not options:
                continue
            option_list = VerticalList(ui, "Options", options, app_id=root_id)
            selection = option_list.show()
            if selection == -1:
                continue
            if options[selection] == "Erase" and message_id is not None:
                if box_type == "inbox":
                    _delete_inbox_message(message_id)
                elif box_type == "outbox":
                    _delete_outbox_message(message_id)
                _show_erased_notice(ui)
                return "deleted"
            if options[selection] == "Send":
                MessageDialog(
                    ui,
                    "This feature requires Telephony. Will hopefully be functional by M3",
                ).show()

def _show_inbox(ui, root_id, sub_index):
    messages = _fetch_inbox_messages()
    if not messages:
        _show_empty_state(ui, "Inbox", f"{root_id}-{sub_index}", None, "No Messages")
        return

    header_root = f"{root_id}-{sub_index}"
    softkey = SoftKeyBar(ui)

    while True:
        messages = _fetch_inbox_messages()
        if not messages:
            _show_empty_state(ui, "Inbox", header_root, None, "No Messages")
            return
        list_items = [
            f"{sender}" if is_read else f"* {sender}"
            for _, message, sender, _, is_read in messages
        ]
        v_list = VerticalList(ui, "Inbox", list_items, app_id=header_root)
        softkey.update("Open", present=False)
        selection_index = v_list.show()
        if selection_index == -1:
            return
        message_id, message, sender, timestamp, _ = messages[selection_index]
        result = _show_message_detail(
            ui,
            "Inbox",
            header_root,
            selection_index + 1,
            message,
            sender,
            timestamp,
            message_id=message_id,
            box_type="inbox",
        )
        if result == "deleted":
            continue

def _show_outbox(ui, root_id, sub_index):
    messages = _fetch_outbox_messages()
    if not messages:
        _show_empty_state(ui, "Outbox", f"{root_id}-{sub_index}", None, "No Messages")
        return

    header_root = f"{root_id}-{sub_index}"
    softkey = SoftKeyBar(ui)

    while True:
        messages = _fetch_outbox_messages()
        if not messages:
            _show_empty_state(ui, "Outbox", header_root, None, "No Messages")
            return
        list_items = [message for _, message, _ in messages]
        v_list = VerticalList(ui, "Outbox", list_items, app_id=header_root)
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
            None,
            timestamp,
            message_id=message_id,
            box_type="outbox",
        )
        if result == "deleted":
            continue

def _show_write_message(ui, root_id, sub_index):
    softkey = SoftKeyBar(ui)
    input_widget = TextInputLong(ui, "Write Message")

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
                MessageDialog(
                    ui,
                    "This feature requires Telephony. Will hopefully be functional by M3",
                ).show()
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

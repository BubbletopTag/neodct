import os
import sqlite3
import time

from System.ui.framework import (
    SoftKeyBar,
    PagedList,
    VerticalList,
    InfoScreen,
)

# Nokia 5190-style Call log (menu 3). No modem yet, so the lists read from a
# call_log database that the Dialer can start filling in later; the layout and
# navigation are the real thing (3-1 .. 3-5, Select softkey, list screens).

APP_ID = 3
DB_PATH = "/NeoDCT/User/db/call_log.db"

ROOT_ITEMS = [
    "Missed calls",        # 3-1
    "Received calls",      # 3-2
    "Dialed calls",        # 3-3
    "Clear call lists",    # 3-4
    "Show call duration",  # 3-5
]

CLEAR_ITEMS = ["All", "Missed", "Dialed", "Received"]

DURATION_ITEMS = [
    "Last call duration",
    "Received calls' duration",
    "Dialed calls' duration",
    "Clear timers",
]

# Settings keys for the call timers (seconds). The Dialer should bump these
# when real calls exist; until then they read 00:00:00.
TIMER_KEYS = {
    "last": "calllog.duration.last",
    "received": "calllog.duration.received",
    "dialed": "calllog.duration.dialed",
}


# --- storage ----------------------------------------------------------------

def _connect():
    parent = os.path.dirname(DB_PATH)
    if parent:
        os.makedirs(parent, exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        """CREATE TABLE IF NOT EXISTS calls
           (id INTEGER PRIMARY KEY AUTOINCREMENT,
            type TEXT,             -- 'missed' | 'received' | 'dialed'
            number TEXT,
            timestamp INTEGER,
            duration INTEGER DEFAULT 0)"""
    )
    return conn


def fetch_calls(call_type):
    try:
        conn = _connect()
        rows = conn.execute(
            "SELECT number, timestamp FROM calls WHERE type=? ORDER BY id DESC LIMIT 20",
            (call_type,),
        ).fetchall()
        conn.close()
        return rows
    except Exception as exc:
        print(f"[CallLog] DB read failed: {exc}")
        return []


def clear_calls(call_type=None):
    try:
        conn = _connect()
        if call_type is None:
            conn.execute("DELETE FROM calls")
        else:
            conn.execute("DELETE FROM calls WHERE type=?", (call_type,))
        conn.commit()
        conn.close()
        return True
    except Exception as exc:
        print(f"[CallLog] DB clear failed: {exc}")
        return False


def _get_timer_seconds(kind):
    try:
        from System.core.SettingsStorage import get_setting
        return int(get_setting(TIMER_KEYS[kind], "0") or 0)
    except Exception:
        return 0


def _set_timer_seconds(kind, seconds):
    try:
        from System.core.SettingsStorage import set_setting
        set_setting(TIMER_KEYS[kind], str(int(seconds)))
        return True
    except Exception as exc:
        print(f"[CallLog] Timer write failed: {exc}")
        return False


def format_duration(seconds):
    seconds = max(0, int(seconds))
    return f"{seconds // 3600:02d}:{(seconds % 3600) // 60:02d}:{seconds % 60:02d}"


def format_call_time(timestamp):
    try:
        return time.strftime("%d.%m. %H:%M", time.localtime(int(timestamp)))
    except Exception:
        return ""


# --- screens ------------------------------------------------------------------

def show_call_list(ui, title, call_type):
    calls = fetch_calls(call_type)
    if not calls:
        InfoScreen(ui, title, "No numbers").show()
        return

    while True:
        items = [number or "Unknown" for number, _ in calls]
        menu = VerticalList(ui, title, items, app_id=APP_ID)
        SoftKeyBar(ui).update("Details", present=False)
        choice = menu.show()
        if choice < 0:
            return
        number, timestamp = calls[choice]
        InfoScreen(ui, number or "Unknown", format_call_time(timestamp)).show()


def show_clear_menu(ui):
    menu = VerticalList(ui, "Clear call lists", CLEAR_ITEMS, app_id=APP_ID)
    SoftKeyBar(ui).update("OK", present=False)
    choice = menu.show()
    if choice < 0:
        return
    target = {0: None, 1: "missed", 2: "dialed", 3: "received"}[choice]
    if clear_calls(target):
        InfoScreen(ui, "List cleared", softkey_text="OK").show()


def show_duration_menu(ui):
    menu = PagedList(ui, "Call duration", DURATION_ITEMS, root_id=f"{APP_ID}-5")
    while True:
        choice = menu.show()
        if choice < 0:
            return
        if choice == 0:
            InfoScreen(ui, "Last call duration",
                       format_duration(_get_timer_seconds("last"))).show()
        elif choice == 1:
            InfoScreen(ui, "Received calls' duration",
                       format_duration(_get_timer_seconds("received"))).show()
        elif choice == 2:
            InfoScreen(ui, "Dialed calls' duration",
                       format_duration(_get_timer_seconds("dialed"))).show()
        elif choice == 3:
            for kind in TIMER_KEYS:
                _set_timer_seconds(kind, 0)
            InfoScreen(ui, "Timers cleared", softkey_text="OK").show()


def run(ui):
    menu = PagedList(ui, "Call log", ROOT_ITEMS, root_id=APP_ID)
    while True:
        choice = menu.show()
        if choice < 0:
            return
        if choice == 0:
            show_call_list(ui, "Missed calls", "missed")
        elif choice == 1:
            show_call_list(ui, "Received calls", "received")
        elif choice == 2:
            show_call_list(ui, "Dialed calls", "dialed")
        elif choice == 3:
            show_clear_menu(ui)
        elif choice == 4:
            show_duration_menu(ui)

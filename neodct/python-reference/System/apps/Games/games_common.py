import time

# Shared bits for the Games app (Snake + Memory).

APP_ID = 6

KEY_ENTER = 28
KEY_BACK = 14
KEY_UP = 103
KEY_DOWN = 108
KEY_LEFT = 105
KEY_RIGHT = 106
KEY_NUM_2 = 3
KEY_NUM_4 = 5
KEY_NUM_5 = 6
KEY_NUM_6 = 7
KEY_NUM_8 = 9

DIR_KEYS = {
    KEY_UP: (0, -1), KEY_NUM_2: (0, -1),
    KEY_DOWN: (0, 1), KEY_NUM_8: (0, 1),
    KEY_LEFT: (-1, 0), KEY_NUM_4: (-1, 0),
    KEY_RIGHT: (1, 0), KEY_NUM_6: (1, 0),
}


def poll_key(ui, timeout):
    """Non-blocking-ish key read that works on hardware and in the harness."""
    if hasattr(ui, "read_keypress"):
        try:
            return ui.read_keypress(timeout)
        except Exception:
            pass
    time.sleep(timeout)
    return None


def get_setting_int(key, default):
    try:
        from System.core.SettingsStorage import get_setting
        return int(get_setting(key, str(default)) or default)
    except Exception:
        return default


def set_setting_value(key, value):
    try:
        from System.core.SettingsStorage import set_setting
        set_setting(key, str(value))
        return True
    except Exception as exc:
        print(f"[Games] Setting write failed ({key}): {exc}")
        return False

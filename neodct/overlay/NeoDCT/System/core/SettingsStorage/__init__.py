import os

SETTINGS_PATH = "/NeoDCT/User/settings.prop"

# Facts about the installed image, generated at build time by
# neodct/scripts/post-build-set-buildtime.sh. This file lives inside the
# read-only rootfs, so it is replaced wholesale by a system update -- unlike
# settings.prop, which is user data on its own partition and survives one.
VERSION_PATH = "/NeoDCT/System/version.prop"

# Anything under this prefix describes the image, never a user preference.
# It is read from VERSION_PATH and is never persisted to settings.prop.
SYSTEM_PREFIX = "system.os."

DEFAULTS = {
    "system.audio.ringtone": "/NeoDCT/System/tones/Low.mp3",
    "system.ui.wallpaper": "NONE",
    "system.ui.engineering_mode": "ON",
    "system.os.versionnumber": "0.3.1a",
    "system.os.versionname": "NeoDCT System v0.3.1a",
    "system.os.platform": "unknown",
    "system.hw.battery_i2c_bus": "3",
    "system.hw.battery_i2c_addr": "0x36",
}


def _parse_settings(text):
    settings = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        settings[key.strip()] = value.strip()
    return settings


def _format_settings(settings):
    lines = []
    for key in sorted(settings.keys()):
        value = settings[key]
        lines.append(f"{key}={value}")
    return "\n".join(lines) + "\n"


def _ensure_parent(path):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)


def load_settings():
    if not os.path.exists(SETTINGS_PATH):
        return {}
    try:
        with open(SETTINGS_PATH, "r") as f:
            return _parse_settings(f.read())
    except Exception as exc:
        print(f"[Settings] Failed to read {SETTINGS_PATH}: {exc}")
        return {}


def load_version():
    """Read the image's version.prop. Missing or corrupt reads as empty."""
    try:
        with open(VERSION_PATH, "r") as f:
            return _parse_settings(f.read())
    except Exception:
        return {}


def save_settings(settings):
    """Persist user settings. Never writes system.os.* -- those are image facts.

    Failures are swallowed: an unwritable user partition must not stop the
    phone from booting, it just means preferences don't stick.
    """
    settings = {key: value for key, value in settings.items()
                if not key.startswith(SYSTEM_PREFIX)}
    try:
        _ensure_parent(SETTINGS_PATH)
        temp_path = SETTINGS_PATH + ".tmp"
        data = _format_settings(settings)
        with open(temp_path, "w") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(temp_path, SETTINGS_PATH)
    except Exception as exc:
        print(f"[Settings] Failed to write {SETTINGS_PATH}: {exc}")


def load_with_defaults(defaults=None):
    """Effective settings: defaults < settings.prop < version.prop."""
    stored = load_settings()
    defaults = defaults or {}

    settings = dict(defaults)
    settings.update(stored)
    # Image facts always win, so an update is visible immediately even
    # though settings.prop outlived it.
    settings.update(load_version())

    stale = [key for key in stored if key.startswith(SYSTEM_PREFIX)]
    missing = [key for key in defaults if key not in stored]
    if stale or missing or not os.path.exists(SETTINGS_PATH):
        save_settings(settings)
    return settings


def get_setting(key, default=None):
    settings = load_with_defaults(DEFAULTS)
    return settings.get(key, default)


def set_setting(key, value):
    settings = load_with_defaults(DEFAULTS)
    settings[key] = value
    save_settings(settings)

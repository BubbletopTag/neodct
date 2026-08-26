"""The screen's backlight, by whatever means this phone actually has.

Three tiers, best first, because the hardware arrives in stages and the
software should not have to wait for it:

  pwm    /sys/class/backlight/<dev>/brightness -- real dimming, 0-100%.
         Needs pwm9 enabled in the device tree, which lives in the boot
         partition, which means a reflash.

  gpio   /sys/class/gpio/gpio53/value -- on or off, nothing between.
         Works with no device tree change at all: the pin defaults to
         plain GPIO function, and neodct_displayd already drives RST and
         DC this same way, so the interface is known to exist here.

  none   No backlight control. QEMU, and any phone where BL is still
         soldered to 3V3. Every call succeeds and does nothing, so callers
         never have to ask which phone they are on.

Screen-off is most of the saving; dimming is the refinement. Doing the
tiers this way means the idle-blank policy can ship and start working
immediately, and quietly gets smoother the day the device tree catches up.

Wiring: BL -> pin 11, GPIO1_C5_d / PWM9_M1 -> gpio 53.
(bank*32 + group*8 + index = 1*32 + 2*8 + 5; the same arithmetic gives
56 and 57 for the RST and DC pins already in HARDWARE_NOTES.md.)
"""

import glob
import os

# GPIO1_C5, pin 11 on the Luckfox Pico Mini B header.
GPIO_PIN = 53
GPIO_ROOT = "/sys/class/gpio"
BACKLIGHT_ROOT = "/sys/class/backlight"

# Some boards switch the LED with a transistor that inverts the sense.
# Overridable rather than assumed: getting it backwards means the screen
# goes dark exactly when somebody starts using the phone.
ACTIVE_LOW = False

MODE_PWM = "pwm"
MODE_GPIO = "gpio"
MODE_NONE = "none"

# Below this, "dim" is indistinguishable from "off" on a small panel, and
# a backlight at 2% reads as a fault rather than a power saving.
MIN_ON_PERCENT = 5


def _first_backlight():
    """The first /sys/class/backlight device, or None."""
    for path in sorted(glob.glob(os.path.join(BACKLIGHT_ROOT, "*"))):
        if os.path.exists(os.path.join(path, "brightness")):
            return path
    return None


def _gpio_path(pin=None):
    return os.path.join(GPIO_ROOT, "gpio%d" % (GPIO_PIN if pin is None else pin))


def _read(path, default=None):
    try:
        with open(path) as handle:
            return handle.read().strip()
    except OSError:
        return default


def _write(path, value):
    try:
        with open(path, "w") as handle:
            handle.write(str(value))
        return True
    except OSError:
        return False


def _export_gpio(pin=None):
    """Make gpio<pin> writable. True when it is ours to drive."""
    pin = GPIO_PIN if pin is None else pin
    path = _gpio_path(pin)
    if not os.path.isdir(path):
        # EBUSY here means somebody exported it already, which is fine.
        _write(os.path.join(GPIO_ROOT, "export"), pin)
    if not os.path.isdir(path):
        return False
    direction = os.path.join(path, "direction")
    if _read(direction) != "out":
        _write(direction, "out")
    return os.path.exists(os.path.join(path, "value"))


def mode():
    """Which of the three tiers this phone has."""
    if _first_backlight():
        return MODE_PWM
    if os.path.isdir(GPIO_ROOT) and _export_gpio():
        return MODE_GPIO
    return MODE_NONE


def available():
    return mode() != MODE_NONE


def set_percent(percent):
    """Set the backlight, 0-100. True when something actually changed it.

    0 means off. Anything from 1 up is clamped to at least MIN_ON_PERCENT,
    so "on but very dim" cannot be mistaken for a broken screen.
    """
    try:
        percent = int(percent)
    except (TypeError, ValueError):
        return False
    percent = max(0, min(100, percent))
    if 0 < percent < MIN_ON_PERCENT:
        percent = MIN_ON_PERCENT

    device = _first_backlight()
    if device:
        top = _read(os.path.join(device, "max_brightness"), "255")
        try:
            top = int(top)
        except (TypeError, ValueError):
            top = 255
        return _write(os.path.join(device, "brightness"),
                      round(top * percent / 100.0))

    if _export_gpio():
        lit = percent > 0
        return _write(os.path.join(_gpio_path(), "value"),
                      "0" if lit == ACTIVE_LOW else "1")

    return False


def get_percent():
    """The backlight now, 0-100, or None when it cannot be read."""
    device = _first_backlight()
    if device:
        now = _read(os.path.join(device, "brightness"))
        top = _read(os.path.join(device, "max_brightness"), "255")
        try:
            return int(round(int(now) * 100.0 / max(1, int(top))))
        except (TypeError, ValueError):
            return None

    if os.path.isdir(_gpio_path()):
        value = _read(os.path.join(_gpio_path(), "value"))
        if value is None:
            return None
        lit = (value.strip() == "0") if ACTIVE_LOW else (value.strip() == "1")
        return 100 if lit else 0

    return None


def off():
    return set_percent(0)


def on(percent=100):
    return set_percent(percent)

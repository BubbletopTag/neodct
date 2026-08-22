"""The backlight, across the three kinds of phone that exist right now.

The hardware arrives in stages: BL soldered to 3V3 (no control at all),
BL on a GPIO (on/off), BL on a PWM with the device tree to match (real
dimming). The same build has to run on all three, because they are the
same phone on different days.

Nothing here touches a real /sys. Each test builds the sysfs tree it wants
in tmp_path and points the module at it.
"""

import os

import pytest

from System.hw import backlight


@pytest.fixture(autouse=True)
def clean(monkeypatch):
    monkeypatch.setattr(backlight, "ACTIVE_LOW", False)


@pytest.fixture
def pwm_phone(tmp_path, monkeypatch):
    """A phone whose device tree exposes pwm-backlight."""
    device = tmp_path / "backlight" / "backlight"
    device.mkdir(parents=True)
    (device / "brightness").write_text("128\n")
    (device / "max_brightness").write_text("255\n")
    monkeypatch.setattr(backlight, "BACKLIGHT_ROOT", str(tmp_path / "backlight"))
    monkeypatch.setattr(backlight, "GPIO_ROOT", str(tmp_path / "nogpio"))
    return device


@pytest.fixture
def gpio_phone(tmp_path, monkeypatch):
    """BL on gpio53, no device tree change. What the phone is today."""
    root = tmp_path / "gpio"
    pin = root / ("gpio%d" % backlight.GPIO_PIN)
    pin.mkdir(parents=True)
    (pin / "direction").write_text("out\n")
    (pin / "value").write_text("1\n")
    (root / "export").write_text("")
    monkeypatch.setattr(backlight, "GPIO_ROOT", str(root))
    monkeypatch.setattr(backlight, "BACKLIGHT_ROOT", str(tmp_path / "nobl"))
    return pin


@pytest.fixture
def bare_phone(tmp_path, monkeypatch):
    """BL soldered to 3V3, or QEMU. No control of any kind."""
    monkeypatch.setattr(backlight, "BACKLIGHT_ROOT", str(tmp_path / "nobl"))
    monkeypatch.setattr(backlight, "GPIO_ROOT", str(tmp_path / "nogpio"))


# --- which tier are we on ---

def test_a_pwm_backlight_is_preferred(pwm_phone):
    assert backlight.mode() == backlight.MODE_PWM


def test_a_gpio_is_used_when_there_is_no_pwm(gpio_phone):
    """The state of the phone right now: the wire is in, the device tree
    is not. On/off is most of the saving anyway."""
    assert backlight.mode() == backlight.MODE_GPIO


def test_no_control_is_not_an_error(bare_phone):
    """QEMU, and every phone with BL still on 3V3. Callers must not have
    to ask which phone they are on."""
    assert backlight.mode() == backlight.MODE_NONE
    assert backlight.available() is False
    assert backlight.set_percent(50) is False
    assert backlight.off() is False
    assert backlight.get_percent() is None


# --- dimming, where it exists ---

def test_percent_is_scaled_to_the_device_range(pwm_phone):
    backlight.set_percent(50)

    assert int((pwm_phone / "brightness").read_text()) == 128


def test_full_and_off_hit_the_ends(pwm_phone):
    backlight.set_percent(100)
    assert int((pwm_phone / "brightness").read_text()) == 255

    backlight.set_percent(0)
    assert int((pwm_phone / "brightness").read_text()) == 0


def test_a_device_with_its_own_range_is_respected(pwm_phone):
    (pwm_phone / "max_brightness").write_text("7\n")

    backlight.set_percent(100)

    assert int((pwm_phone / "brightness").read_text()) == 7


def test_reading_back_what_was_set(pwm_phone):
    backlight.set_percent(75)

    assert backlight.get_percent() == 75


def test_a_dim_that_looks_broken_is_lifted_to_something_visible(pwm_phone):
    """1% on a small panel is indistinguishable from a dead screen, and a
    user seeing that reaches for the reset button, not the brightness."""
    backlight.set_percent(1)

    assert int((pwm_phone / "brightness").read_text()) > 0
    assert backlight.get_percent() >= backlight.MIN_ON_PERCENT


def test_out_of_range_is_clamped_not_refused(pwm_phone):
    backlight.set_percent(500)
    assert int((pwm_phone / "brightness").read_text()) == 255

    backlight.set_percent(-20)
    assert int((pwm_phone / "brightness").read_text()) == 0


def test_nonsense_is_refused_rather_than_written(pwm_phone):
    before = (pwm_phone / "brightness").read_text()

    assert backlight.set_percent("bright") is False
    assert backlight.set_percent(None) is False
    assert (pwm_phone / "brightness").read_text() == before


# --- on and off, where that is all there is ---

def test_gpio_off_and_on(gpio_phone):
    backlight.off()
    assert (gpio_phone / "value").read_text().strip() == "0"

    backlight.on()
    assert (gpio_phone / "value").read_text().strip() == "1"


def test_any_brightness_above_zero_lights_a_gpio_backlight(gpio_phone):
    """There is no dimming here, so 30% has to mean on rather than 30% of
    nothing."""
    backlight.set_percent(30)

    assert (gpio_phone / "value").read_text().strip() == "1"
    assert backlight.get_percent() == 100


def test_an_inverted_board_is_still_driven_the_right_way(gpio_phone, monkeypatch):
    """Some boards switch the LED through a transistor that inverts the
    sense. Getting this backwards means the screen goes dark exactly when
    somebody starts using the phone."""
    monkeypatch.setattr(backlight, "ACTIVE_LOW", True)

    backlight.on()
    assert (gpio_phone / "value").read_text().strip() == "0"

    backlight.off()
    assert (gpio_phone / "value").read_text().strip() == "1"
    assert backlight.get_percent() == 0


def test_the_pin_is_exported_if_nobody_has_yet(tmp_path, monkeypatch):
    """First use after a boot. Writing the pin number to export is what
    creates the directory; neodct_displayd does the same for RST and DC."""
    root = tmp_path / "gpio"
    root.mkdir()
    (root / "export").write_text("")
    monkeypatch.setattr(backlight, "GPIO_ROOT", str(root))
    monkeypatch.setattr(backlight, "BACKLIGHT_ROOT", str(tmp_path / "nobl"))

    # Nothing creates the directory here, so this stays MODE_NONE -- what
    # matters is that it asked, and did not raise.
    assert backlight.mode() in (backlight.MODE_GPIO, backlight.MODE_NONE)
    assert (root / "export").read_text().strip() == str(backlight.GPIO_PIN)


def test_the_pin_matches_the_documented_wiring():
    """BL -> pin 11 -> GPIO1_C5 -> bank1*32 + groupC*8 + 5 = 53. The same
    arithmetic gives 56 and 57 for RST and DC, which HARDWARE_NOTES.md
    already records, so this is checkable rather than folklore."""
    assert backlight.GPIO_PIN == 1 * 32 + 2 * 8 + 5 == 53

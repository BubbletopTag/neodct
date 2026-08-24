"""neodct-play: the entry point NetSurf spawns when a <video> is clicked.

NetSurf is C and has no business knowing how mpv should be run, so it
execs this instead and gets suspended for its trouble. The keypad reaches
it as an evdev device -- the uinput keyboard the Browser app already
bridges i2c presses onto, or a real keyboard under QEMU -- because by the
time NetSurf is running, the i2c bus belongs to that bridge.
"""

import os
import signal
import struct
import subprocess
import sys
import time

import pytest

from System.core import MediaWidget

CLI = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "overlay", "NeoDCT", "System", "core", "MediaWidget", "neodct-play",
)

EV_KEY, EV_SYN = 0x01, 0x00
_EVENT = struct.Struct("llHHi")


def _event(etype, code, value):
    return _EVENT.pack(0, 0, etype, code, value)


# --- reading the keypad ---------------------------------------------------

@pytest.fixture
def evdev():
    """(write-end, keysource) over a pipe standing in for /dev/input/eventN."""
    r, w = os.pipe()
    keys = MediaWidget.EvdevKeys(fd=r)
    try:
        yield w, keys
    finally:
        keys.close()
        try:
            os.close(w)
        except OSError:
            pass          # a test may have closed it to fake an unplug


def test_a_key_press_is_reported(evdev):
    w, keys = evdev
    os.write(w, _event(EV_KEY, 106, 1))
    assert keys(0.5) == 106


def test_a_key_release_is_not_a_press(evdev):
    w, keys = evdev
    os.write(w, _event(EV_KEY, 106, 0))
    assert keys(0.2) is None


def test_autorepeat_is_ignored(evdev):
    # Holding C down must not queue a second quit for the next video.
    w, keys = evdev
    os.write(w, _event(EV_KEY, 14, 2))
    assert keys(0.2) is None


def test_sync_events_are_not_keys(evdev):
    w, keys = evdev
    os.write(w, _event(EV_SYN, 0, 0))
    assert keys(0.2) is None


def test_nothing_to_read_returns_none(evdev):
    _, keys = evdev
    started = time.monotonic()
    assert keys(0.2) is None
    assert time.monotonic() - started >= 0.15


def test_several_events_in_one_write_are_read_in_order(evdev):
    w, keys = evdev
    os.write(w, _event(EV_KEY, 105, 1) + _event(EV_KEY, 105, 0)
             + _event(EV_KEY, 103, 1))
    assert keys(0.5) == 105
    assert keys(0.5) == 103


def test_a_closed_device_stops_returning_keys(evdev):
    w, keys = evdev
    os.close(w)
    assert keys(0.2) is None


def test_a_missing_device_yields_no_keysource(tmp_path):
    assert MediaWidget.EvdevKeys.open(str(tmp_path / "event9")) is None


# --- the command-line entry point -----------------------------------------

def _run_cli(args, env=None, timeout=30):
    full = dict(os.environ)
    full.setdefault("NEODCT_MPV", "/bin/true")
    if env:
        full.update(env)
    return subprocess.run([sys.executable, CLI] + args, env=full,
                          capture_output=True, timeout=timeout, text=True)


def test_the_cli_needs_a_url():
    result = _run_cli([])
    assert result.returncode == 2               # argparse usage error
    assert "usage" in result.stderr.lower()


def test_the_cli_prints_the_command_it_would_run():
    result = _run_cli(["--dry-run", "http://host/watch.avi"])
    assert result.returncode == 0
    assert "--vo=fbdev" in result.stdout
    assert "http://host/watch.avi" in result.stdout


def test_the_dry_run_reports_the_parent_it_would_suspend():
    result = _run_cli(["--dry-run", "http://host/watch.avi"])
    assert str(os.getpid()) in result.stdout


def test_suspending_can_be_turned_off():
    result = _run_cli(["--dry-run", "--no-suspend", "http://host/x.avi"])
    assert "suspend: none" in result.stdout


def test_the_cli_runs_mpv_and_returns_its_status(tmp_path):
    failing = tmp_path / "mpv-that-fails"
    failing.write_text("#!/bin/sh\nexit 4\n")
    failing.chmod(0o755)
    result = _run_cli(["--no-suspend", "http://host/x.avi"],
                      env={"NEODCT_MPV": str(failing)})
    assert result.returncode == 4


def _state(pid):
    with open("/proc/%d/stat" % pid, "rb") as handle:
        return handle.read().rsplit(b")", 1)[1].split()[0].decode()


def test_the_application_is_stopped_while_mpv_runs_and_resumed_after(tmp_path):
    """The whole contract, both halves.

    NetSurf must be stopped for as long as mpv holds the screen, and it
    must be running again when neodct-play returns. Checking only the
    second half would pass on a build that never suspended anything.
    """
    sleeper = subprocess.Popen(["sleep", "30"])
    observed = tmp_path / "state"
    try:
        # A stand-in for mpv that records how the application looks from
        # the outside while "playback" is in progress.
        spy = tmp_path / "mpv-spy"
        spy.write_text(
            "#!%s\nimport sys\n"
            "raw = open('/proc/%d/stat','rb').read()\n"
            "open(%r,'w').write(raw.rsplit(b')',1)[1].split()[0].decode())\n"
            % (sys.executable, sleeper.pid, str(observed)))
        spy.chmod(0o755)

        result = _run_cli(["--parent", str(sleeper.pid), "http://h/x.avi"],
                          env={"NEODCT_MPV": str(spy)})

        assert result.returncode == 0
        assert observed.read_text() == "T"      # stopped during playback
        assert _state(sleeper.pid) != "T"       # running again afterwards
    finally:
        sleeper.send_signal(signal.SIGCONT)
        sleeper.kill()
        sleeper.wait()


# --- finding the keypad ---------------------------------------------------

def _fake_input_tree(tmp_path, devices):
    """Build a stand-in /sys/class/input + /dev/input from {event: name}."""
    sysfs = tmp_path / "sys"
    dev = tmp_path / "dev"
    sysfs.mkdir()
    dev.mkdir()
    for event, name in devices.items():
        node = sysfs / event / "device"
        node.mkdir(parents=True)
        (node / "name").write_text(name + "\n")
        (dev / event).write_text("")
    return str(sysfs), str(dev)


def test_the_neodct_keypad_is_preferred_over_other_devices(tmp_path):
    sysfs, dev = _fake_input_tree(tmp_path, {
        "event0": "some-mouse",
        "event1": "neodct-t9-keypad",
        "event2": "another-keyboard",
    })
    found = MediaWidget.EvdevKeys.discover(sysfs=sysfs, dev=dev)
    assert found == os.path.join(dev, "event1")


def test_a_keyboard_is_used_when_there_is_no_neodct_keypad(tmp_path):
    # QEMU: no i2c keypad and no uinput bridge, just a real keyboard.
    sysfs, dev = _fake_input_tree(tmp_path, {
        "event0": "some-mouse",
        "event1": "QEMU PS/2 Keyboard",
    })
    found = MediaWidget.EvdevKeys.discover(sysfs=sysfs, dev=dev)
    assert found == os.path.join(dev, "event1")


def test_an_explicit_device_wins(tmp_path):
    sysfs, dev = _fake_input_tree(tmp_path, {"event0": "neodct-t9-keypad"})
    chosen = tmp_path / "dev" / "event0"
    found = MediaWidget.EvdevKeys.discover(
        env={"NEODCT_KEYPAD_DEVICE": str(chosen)}, sysfs=sysfs, dev=dev)
    assert found == str(chosen)


def test_an_explicit_device_that_does_not_exist_is_ignored(tmp_path):
    sysfs, dev = _fake_input_tree(tmp_path, {"event0": "neodct-t9-keypad"})
    found = MediaWidget.EvdevKeys.discover(
        env={"NEODCT_KEYPAD_DEVICE": str(tmp_path / "gone")},
        sysfs=sysfs, dev=dev)
    assert found == os.path.join(dev, "event0")


def test_no_input_devices_at_all_is_not_an_error(tmp_path):
    sysfs, dev = _fake_input_tree(tmp_path, {})
    assert MediaWidget.EvdevKeys.discover(sysfs=sysfs, dev=dev) is None


def test_a_device_with_no_node_is_skipped(tmp_path):
    sysfs, dev = _fake_input_tree(tmp_path, {"event0": "neodct-t9-keypad"})
    os.unlink(os.path.join(dev, "event0"))
    assert MediaWidget.EvdevKeys.discover(sysfs=sysfs, dev=dev) is None

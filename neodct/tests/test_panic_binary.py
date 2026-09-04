"""nd-panic, the program that draws the screen after nd-core dies.

The pixels are `neodct/src/test/unit/test_panic.c`'s business. This drives the
BINARY, and the two things it pins are the ones the boot script depends on and
a unit test cannot see:

  * the exit status. run_neodct.sh restarts the moment nd-panic returns 0, and
    falls back to an ANSI banner (doing the waiting itself) on anything else.
    Getting that backwards gives either no pause or a double one.

  * the pacing. "Restarting in 3... 2... 1..." has to take three seconds,
    because the shell does not wait separately. There is no framebuffer on a
    build host, so the countdown is timed through --out, which draws the same
    frames to PNGs and sleeps between them exactly as the device path does.

    make -C neodct/src && python3 -m pytest neodct/tests/test_panic_binary.py
"""

import os
import subprocess
import time

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(REPO, "src", "build", "default")
PANIC = os.path.join(BUILD, "bin", "nd-panic")
LIBDIR = os.path.join(BUILD, "lib")
OVERLAY = os.path.join(REPO, "overlay")

pytestmark = pytest.mark.skipif(
    not os.path.exists(PANIC),
    reason="the C build is not present (make -C neodct/src)")


@pytest.fixture
def root(tmp_path):
    """A staged /NeoDCT: System symlinked onto the overlay, User writable.

    Same trick as nd-shoot's staged root -- nothing under neodct/overlay is
    written to, and the whole fixture costs a symlink.
    """
    (tmp_path / "NeoDCT").mkdir()
    (tmp_path / "NeoDCT" / "System").symlink_to(
        os.path.join(OVERLAY, "NeoDCT", "System"))
    (tmp_path / "NeoDCT" / "User").mkdir()
    return tmp_path


def run(root, *args, timeout=60):
    env = dict(os.environ)
    env["NEODCT_ROOT"] = str(root)
    env["LD_LIBRARY_PATH"] = LIBDIR
    return subprocess.run([PANIC, *args], capture_output=True, text=True,
                          env=env, timeout=timeout)


def frames(root):
    out = root / "frames"
    return sorted(p.name for p in out.iterdir()) if out.is_dir() else []


# --- the frames -----------------------------------------------------------


def test_a_countdown_is_one_frame_per_second_and_no_zero(root):
    result = run(root, "--status", "139", "--out", "/frames", "--no-wait")

    assert result.returncode == 0
    assert frames(root) == ["panic-1.png", "panic-2.png", "panic-3.png"]


def test_the_halt_screen_is_a_single_still_frame(root):
    result = run(root, "--status", "139", "--halt", "--out", "/frames")

    assert result.returncode == 0
    assert frames(root) == ["panic-halt.png"]


def test_the_frames_are_the_panel_size(root):
    png = pytest.importorskip("PIL.Image", reason="Pillow not installed")
    run(root, "--status", "139", "--out", "/frames", "--no-wait")

    with png.open(root / "frames" / "panic-3.png") as img:
        assert img.size == (240, 175)


# --- the pacing -----------------------------------------------------------


def test_the_countdown_takes_as_long_as_it_says(root):
    """Two seconds on the screen means two seconds of wall clock.

    run_neodct.sh has no sleep of its own; this IS the pause between the
    crash and the restart, so if it collapses the owner never reads the
    message.
    """
    started = time.monotonic()
    result = run(root, "--status", "139", "--seconds", "2", "--out", "/frames")
    elapsed = time.monotonic() - started

    assert result.returncode == 0
    assert 1.8 < elapsed < 6.0
    assert frames(root) == ["panic-1.png", "panic-2.png"]


def test_no_wait_is_for_harnesses_only(root):
    started = time.monotonic()
    run(root, "--status", "139", "--seconds", "3", "--out", "/frames", "--no-wait")
    assert time.monotonic() - started < 1.5


# --- the exit status is a contract ----------------------------------------


def test_nothing_drawn_means_a_non_zero_exit(root):
    """No framebuffer on a build host, which is the failure the shell handles."""
    result = run(root, "--status", "139", "--seconds", "0",
                 "--fb", "/dev/definitely-not-a-framebuffer")

    assert result.returncode == 1
    assert "framebuffer" in result.stderr


def test_a_bad_argument_is_refused_rather_than_guessed(root):
    """A typo in the boot script must not silently become a zero."""
    assert run(root, "--status", "not-a-number").returncode == 2
    assert run(root, "--status").returncode == 2
    assert run(root, "--seconds", "600").returncode == 2
    assert run(root, "--wat").returncode == 2


# --- the log --------------------------------------------------------------


def test_the_death_of_the_core_reaches_the_crash_log(root):
    run(root, "--status", "139", "--crash", "2", "--limit", "3",
        "--out", "/frames", "--no-wait")

    log = (root / "NeoDCT" / "User" / "logs" / "crash.log").read_text()
    assert "source: nd-core" in log
    assert "signal: 11 (SIGSEGV)" in log
    assert "consecutive crash 2 of 3" in log


def test_the_halt_is_recorded_as_a_halt(root):
    run(root, "--status", "134", "--crash", "3", "--limit", "3",
        "--halt", "--out", "/frames")

    log = (root / "NeoDCT" / "User" / "logs" / "crash.log").read_text()
    assert "consecutive crash 3 of 3 -- not restarting" in log
    assert "SIGABRT" in log


def test_the_log_can_be_turned_off(root):
    run(root, "--status", "139", "--out", "/frames", "--no-wait", "--no-log")
    assert not (root / "NeoDCT" / "User" / "logs" / "crash.log").exists()


def test_a_screen_still_appears_with_no_artwork(tmp_path):
    """A rootfs so broken that CRASH.jpg is unreadable is exactly when this
    runs. The frame must still be written."""
    (tmp_path / "NeoDCT" / "User").mkdir(parents=True)
    result = run(tmp_path, "--status", "139", "--out", "/frames", "--no-wait")

    assert result.returncode == 0
    assert "panic-3.png" in frames(tmp_path)

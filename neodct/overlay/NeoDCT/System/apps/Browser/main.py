# /NeoDCT/System/apps/Browser/main.py
#
# NetSurf framebuffer browser (netsurf-fb, NeoDCT chrome).
# Takes over /dev/fb0 and reads keys from evdev directly; on keypad
# hardware the presses are bridged to a uinput keyboard the same way
# LinuxShell does it. The launcher resumes drawing when we return.
import os
import select
import subprocess

HOME_PAGE = "file:///NeoDCT/System/apps/Browser/home.html"

# browser stderr (incl. periodic "neodct-mem:" RSS lines) goes to the
# serial console so memory pressure can be watched from the host
CONSOLE = "/dev/console"

_SIGNAL_NOTES = {
    6: "SIGABRT",
    9: "SIGKILL, possible OOM",
    11: "SIGSEGV",
}


def _describe_exit(returncode):
    """One serial-log line describing how netsurf-fb ended."""
    if returncode == 0:
        return "neodct-browser: exited normally"
    if returncode > 0:
        return "neodct-browser: exited with code %d" % returncode
    sig = -returncode
    note = _SIGNAL_NOTES.get(sig)
    if note is not None:
        return "neodct-browser: KILLED by signal %d (%s)" % (sig, note)
    return "neodct-browser: KILLED by signal %d" % sig


def _log_console(text):
    try:
        with open(CONSOLE, "wb", buffering=0) as f:
            f.write(text.encode() + b"\r\n")
    except Exception:
        pass


def _dump_dmesg_tail(lines=15):
    """After an abnormal exit, surface the kernel's view (OOM killer
    reports land in dmesg even with a quiet console)."""
    try:
        out = subprocess.run(["dmesg"], capture_output=True,
                             timeout=5).stdout
        for line in out.splitlines()[-lines:]:
            _log_console(line.decode(errors="replace"))
    except Exception:
        pass


def _start_key_bridge(ui):
    """Keypad-only hardware: mirror i2c keypad presses into a uinput
    keyboard netsurf can read. Returns None on QEMU/dev where a real
    keyboard evdev device already exists.

    The browser bridge, not the shell one: netsurf needs arrows to scroll
    and follow links, and the keypad has no Left or Right key at all, so
    2/4/6/8 stand in for the d-pad and # reaches text entry for a URL."""
    try:
        from System.hw.t9_uinput import start_browser_bridge
        return start_browser_bridge(ui)
    except Exception:
        return None


def _drain_input(ui):
    """Swallow every keypress queued up while the browser owned the
    screen, so the launcher doesn't replay them as menu actions.

    The keypad evdev fd keeps receiving events even while netsurf reads
    the same device through its own fd, and the i2c scanner queues
    presses in _pending; both must be empty before the UI resumes."""
    fd = getattr(ui, "keypad_fd", None)
    if fd is not None:
        try:
            while select.select([fd], [], [], 0)[0]:
                if not os.read(fd, 4096):
                    break
        except OSError:
            pass

    matrix = getattr(ui, "matrix_input", None)
    if matrix is not None:
        try:
            for _ in range(64):
                if matrix.read_key(0) is None:
                    break
        except Exception:
            pass


def run(ui):
    browser = "/usr/bin/netsurf-fb"
    if not os.path.exists(browser):
        return

    bridge = _start_key_bridge(ui)

    env = os.environ.copy()
    env.setdefault("HOME", "/NeoDCT/User")

    try:
        stderr = subprocess.DEVNULL
        try:
            stderr = open(CONSOLE, "wb", buffering=0)
        except Exception:
            pass

        try:
            proc = subprocess.run(
                [browser, HOME_PAGE],
                env=env,
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=stderr,
            )
            _log_console(_describe_exit(proc.returncode))
            if proc.returncode < 0:
                _dump_dmesg_tail()
        finally:
            if stderr is not subprocess.DEVNULL:
                try:
                    stderr.close()
                except Exception:
                    pass
    except Exception:
        pass
    finally:
        # Tear down the virtual keyboard before the UI resumes reading
        # the keypad, so nothing double-consumes presses.
        if bridge is not None:
            try:
                bridge.stop()
            except Exception:
                pass

        _drain_input(ui)

        # Repaint the UI over whatever the browser left on the fb.
        try:
            ui.fb.update(ui.canvas)
        except Exception:
            pass

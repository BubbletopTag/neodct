# /NeoDCT/System/apps/Browser/main.py
#
# NetSurf framebuffer browser (netsurf-fb, NeoDCT chrome).
# Takes over /dev/fb0 and reads keys from evdev directly; on keypad
# hardware the presses are bridged to a uinput keyboard the same way
# LinuxShell does it. The launcher resumes drawing when we return.
import os
import select
import subprocess
import time

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


# --- console logging ------------------------------------------------------
# netsurf writes a lot to stderr: its own warnings, SSL failures, and the
# periodic "neodct-mem:" RSS lines the NeoDCT build emits. That used to go
# straight to /dev/console untagged, so on a 64MB phone the most useful
# stream in the system was also the only one you could not tell apart from
# kernel noise. Everything now goes through here: tagged [Browser], purple,
# with the CPU figure the memory lines were missing.

TAG = "Browser"

# Substrings that mark a line as a failure rather than chatter. Kept broad
# on purpose -- a missed error is worse than a line painted too brightly.
_ERROR_HINTS = ("ssl", "tls", "certificate", "handshake", "verify",
                "error", "failed", "cannot", "refused", "timed out",
                "unable", "denied", "abort")


def _paint(text, code, bold=False):
    try:
        from System.core import logstyle
        return logstyle.paint(text, code, bold=bold)
    except Exception:
        return text


def _tagged(body, code=141):
    """One console line: a purple [Browser] and the text after it."""
    return _paint("[" + TAG + "]", code, bold=True) + " " + body


class _CpuSampler:
    """Percent CPU for one pid, between calls.

    Read from /proc/<pid>/stat rather than shelling out to top: netsurf is
    the heaviest thing this phone runs and the sampler must not be part of
    the problem.
    """

    def __init__(self, pid):
        self.pid = pid
        self._last = None

    def percent(self):
        try:
            with open("/proc/%d/stat" % self.pid, "rb") as handle:
                parts = handle.read().split()
            # utime and stime are fields 14 and 15, after the comm field
            # which may itself contain spaces -- index from the closing ")".
            busy = int(parts[13]) + int(parts[14])
            now = time.monotonic()
        except (OSError, ValueError, IndexError):
            return None
        if self._last is None:
            self._last = (busy, now)
            return None
        prev_busy, prev_now = self._last
        self._last = (busy, now)
        elapsed = now - prev_now
        if elapsed <= 0:
            return None
        ticks = os.sysconf("SC_CLK_TCK") if hasattr(os, "sysconf") else 100
        return 100.0 * (busy - prev_busy) / (ticks * elapsed)


def _classify(line):
    """(colour, body) for one line of netsurf stderr."""
    low = line.lower()
    if low.startswith("neodct-mem:"):
        return 141, line          # the memory line, handled by the caller
    if any(hint in low for hint in _ERROR_HINTS):
        return 196, line          # red: SSL, certificates, anything failing
    if "http://" in low or "https://" in low:
        return 117, line          # pale blue: a navigation
    return 141, line


def _pump_browser_log(proc, console):
    """Read netsurf's stderr to EOF, tagging every line on the way.

    Runs in the foreground because the UI is blocked on the browser anyway,
    and a thread here would outlive the process it is reading.
    """
    cpu = _CpuSampler(proc.pid)
    for raw in iter(proc.stderr.readline, b""):
        try:
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
        except Exception:
            continue
        if not line.strip():
            continue
        code, body = _classify(line)
        if body.lower().startswith("neodct-mem:"):
            # Fold CPU in beside the memory the browser already reports, so
            # one line answers "is it thrashing or is it spinning?".
            pct = cpu.percent()
            if pct is not None:
                body = "%s cpu=%.0f%%" % (body, pct)
        try:
            console.write(_tagged(body, code).encode() + b"\r\n")
        except Exception:
            return


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
            # stderr through a pipe rather than straight at the console, so
            # every line can be tagged and the memory lines can pick up a
            # CPU figure on the way past. Popen, not run(): the pipe has to
            # be drained while netsurf is alive or it fills and blocks it.
            proc = subprocess.Popen(
                [browser, HOME_PAGE],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
            _log_console(_tagged("neodct-browser: started pid %d" % proc.pid))
            if stderr is not subprocess.DEVNULL:
                _pump_browser_log(proc, stderr)
            proc.wait()
            _log_console(_tagged(_describe_exit(proc.returncode),
                                 196 if proc.returncode != 0 else 141))
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

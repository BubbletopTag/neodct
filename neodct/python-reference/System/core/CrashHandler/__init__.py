"""Crash handling UX + persistent crash logging for NeoDCT.

Behavior:
- Every crash is appended to a size-capped log at /NeoDCT/User/logs/crash.log
  (rotated once to crash.log.1, so at most ~128 KB of flash is ever used).
  Each report records timestamp, QEMU/hardware mode, source, uptime, free
  memory, and the full traceback.
- In QEMU / simulation mode (detected the same way the launcher does: no
  /dev/ttyFIQ0), a [CRASH] summary is also printed to the serial console.
- Engineering mode ON: full-screen crash image (with a one-line exception
  summary strip) and a "Continue" softkey.
- Engineering mode OFF: a simple notice dialog, including a short exception
  summary when available.

Nothing in this module may raise into the caller.
"""

from __future__ import annotations

import os
import select
import sys
import time
import traceback

from PIL import Image

from System.ui.framework import MessageDialog, SoftKeyBar

CRASH_IMAGE_PATH = "/NeoDCT/System/ui/resources/CRASH.jpg"
DEFAULT_NOTICE = "An application has crashed."
CONTINUE_KEYS = {14, 28, 46, 50, 96}

CRASH_LOG_DIR = "/NeoDCT/User/logs"
CRASH_LOG_PATH = f"{CRASH_LOG_DIR}/crash.log"
CRASH_LOG_ROTATED = f"{CRASH_LOG_PATH}.1"
CRASH_LOG_MAX_BYTES = 64 * 1024   # small flash: cap at 2x64 KB total


def is_simulation():
    """True when running in QEMU / simulation.

    Mirrors the launcher's environment detection: real Rockchip/Luckfox
    hardware exposes the FIQ debug console at /dev/ttyFIQ0; QEMU does not.
    """
    try:
        return not os.path.exists("/dev/ttyFIQ0")
    except Exception:
        return True


def _uptime():
    try:
        with open("/proc/uptime") as f:
            return f.read().split()[0] + "s"
    except Exception:
        return "?"


def _mem_available():
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith(("MemAvailable", "MemFree")):
                    return " ".join(line.split())
    except Exception:
        pass
    return "?"


def _exc_summary(exc_info):
    """One line like 'IndexError: tuple index out of range', or None."""
    if not exc_info or exc_info[0] is None:
        return None
    try:
        text = f"{exc_info[0].__name__}: {exc_info[1]}"
        return text if len(text) <= 90 else text[:87] + "..."
    except Exception:
        return None


def _rotate_if_needed():
    try:
        if (os.path.exists(CRASH_LOG_PATH)
                and os.path.getsize(CRASH_LOG_PATH) > CRASH_LOG_MAX_BYTES):
            os.replace(CRASH_LOG_PATH, CRASH_LOG_ROTATED)
    except OSError:
        pass


def log_crash(source, exc_info=None, note=None):
    """Append a crash report to the persistent log. Never raises.

    source:   short origin tag, e.g. an app name, "menu", "core-main-loop".
    exc_info: sys.exc_info() tuple; defaults to the exception currently
              being handled, if any.
    note:     optional extra context line.
    Returns the log path on success, else None.
    """
    try:
        if exc_info is None:
            exc_info = sys.exc_info()
        sim = is_simulation()

        lines = [
            "=" * 60,
            f"time:   {time.strftime('%Y-%m-%d %H:%M:%S')} (epoch {int(time.time())})",
            f"mode:   {'QEMU/simulation' if sim else 'hardware'}",
            f"source: {source}",
            f"uptime: {_uptime()}   mem: {_mem_available()}",
        ]
        if note:
            lines.append(f"note:   {note}")
        if exc_info and exc_info[0] is not None:
            lines.append("".join(traceback.format_exception(*exc_info)).rstrip())
        else:
            lines.append("(no exception info available)")
        report = "\n".join(lines) + "\n"

        os.makedirs(CRASH_LOG_DIR, exist_ok=True)
        _rotate_if_needed()
        with open(CRASH_LOG_PATH, "a", encoding="utf-8") as f:
            f.write(report)
            f.flush()
            os.fsync(f.fileno())   # survive an immediate power pull

        # fsync the parent directories too: fsync(file) persists the data,
        # but a newly created file/dir's DIRECTORY ENTRY is only durable
        # once its parent is synced -- otherwise a hard power loss right
        # after the crash loses the whole report.
        for d in (CRASH_LOG_DIR, os.path.dirname(CRASH_LOG_DIR)):
            try:
                dfd = os.open(d, os.O_RDONLY)
                try:
                    os.fsync(dfd)
                finally:
                    os.close(dfd)
            except OSError:
                pass

        if sim:
            summary = _exc_summary(exc_info) or "(no exception info)"
            print(f"[CRASH] {source}: {summary} (report -> {CRASH_LOG_PATH})")
        return CRASH_LOG_PATH
    except Exception:
        # Logging must never make a crash worse.
        return None


def _is_engineering_mode(ui, default=False):
    value = getattr(ui, "engineering_mode", None)
    if value is None:
        return default
    return bool(value)


def _flush_input(ui):
    fd = getattr(ui, "keypad_fd", None)
    if fd is None:
        return
    while True:
        r, _, _ = select.select([fd], [], [], 0.0)
        if not r:
            break
        try:
            os.read(fd, 24)
        except Exception:
            break


def _draw_engineering_crash_screen(ui, summary=None):
    screen_w = int(getattr(ui, "W", 240))
    screen_h = int(getattr(ui, "H", 175))
    softkey_h = int(getattr(ui, "SOFTKEY_H", 30))
    content_bottom = int(getattr(ui, "content_bottom", screen_h - softkey_h))

    ui.draw.rectangle((0, 0, screen_w, screen_h), fill="black")

    crash_img = None
    try:
        crash_img = Image.open(CRASH_IMAGE_PATH).convert("RGB")
    except Exception:
        crash_img = None

    if crash_img is not None:
        # Draw full frame; softkey bar intentionally overlays the bottom strip.
        crash_img = crash_img.resize((screen_w, screen_h), Image.Resampling.LANCZOS)
        ui.canvas.paste(crash_img, (0, 0))
    else:
        # Fallback if image file is missing or unreadable.
        text = "CRASH"
        font = getattr(ui, "font_xl", None) or getattr(ui, "font_n", None) or getattr(ui, "font_s", None)
        if font is not None:
            w, h = ui.get_text_size(text, font)
            x = (screen_w - w) // 2
            y = max(0, (content_bottom - h) // 2)
            ui.draw.text((x, y), text, font=font, fill="white")

    # One-line exception summary strip so the error is visible on-device.
    font_s = getattr(ui, "font_s", None)
    if summary and font_s is not None:
        try:
            _, th = ui.get_text_size(summary, font_s)
            ui.draw.rectangle((0, 0, screen_w, th + 4), fill="black")
            ui.draw.text((2, 2), summary, font=font_s, fill="white")
        except Exception:
            pass

    SoftKeyBar(ui).update("Continue", present=False)
    ui.fb.update(ui.canvas)


def _wait_for_continue(ui):
    while True:
        key = ui.wait_for_key()
        if key in CONTINUE_KEYS:
            return key


def show_app_crash(ui, message=DEFAULT_NOTICE, app_name=None, exc_info=None):
    """Log the crash, then display the crash UX.

    This function must never raise into the caller. Existing callers that
    pass only (ui) or (ui, message) keep working unchanged.
    """
    if exc_info is None:
        exc_info = sys.exc_info()
    log_crash(app_name or "app", exc_info=exc_info)

    try:
        summary = _exc_summary(exc_info)

        if _is_engineering_mode(ui, default=False):
            _flush_input(ui)
            _draw_engineering_crash_screen(ui, summary=summary)
            _wait_for_continue(ui)
            return

        MessageDialog(ui, f"{message}\n{summary}" if summary else message).show()
    except Exception:
        # Never let crash UI handling create a secondary crash.
        return

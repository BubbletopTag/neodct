# /NeoDCT/System/apps/LinuxShell/main.py
import os
import shutil
import subprocess
import time


def _which(name: str) -> str | None:
    return shutil.which(name)


def _run_quiet(args, env=None, timeout=None):
    try:
        subprocess.run(
            args,
            env=env,
            timeout=timeout,
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return True
    except Exception:
        return False


def _write_tty(tty_path: str, data: bytes) -> None:
    try:
        with open(tty_path, "wb", buffering=0) as f:
            f.write(data)
            f.flush()
    except Exception:
        pass


def run(ui):
    """
    Raw console shell on a real /dev/ttyN, shown on-device.
    No VT ioctls, no KDSETMODE, no openvt (avoids common hangs).
    Requires:
      - fbcon enabled
      - /dev/ttyN exists
      - chvt available (busybox provides it on many systems)
    """
    # Choose VTs; override via env if you want.
    shell_vt = int(os.environ.get("NEODCT_SHELL_VT", "2"))
    ui_vt = int(os.environ.get("NEODCT_UI_VT", "1"))

    tty_shell = f"/dev/tty{shell_vt}"

    chvt = _which("chvt")
    if not chvt:
        # Hard fail back to UI: without chvt we can't reliably switch the visible console.
        # (At this point, keep the UI responsive rather than trying risky ioctls.)
        return

    env = os.environ.copy()
    env["PS1"] = "NeoDCT # "
    env["TERM"] = "linux"

    # Switch to shell VT (no blocking wait)
    if not _run_quiet([chvt, str(shell_vt)], timeout=1.0):
        return

    # Show cursor + print hint
    _write_tty(tty_shell, b"\x1b[?25h")  # cursor on
    _write_tty(tty_shell, b"Type exit to go back to the NeoDCT UI\r\n\r\n")

    # Run interactive shell attached to the real tty
    try:
        with open(tty_shell, "r+b", buffering=0) as t:
            p = subprocess.Popen(
                ["/bin/sh", "-i"],
                stdin=t,
                stdout=t,
                stderr=t,
                env=env,
                close_fds=True,
            )
            p.wait()
    except Exception:
        pass
    finally:
        # Hide cursor again (your cmdline has vt.global_cursor_default=0)
        _write_tty(tty_shell, b"\x1b[?25l")  # cursor off

        # Switch back to UI VT
        _run_quiet([chvt, str(ui_vt)], timeout=1.0)

        # Small settle time before UI redraws
        time.sleep(0.05)

    return

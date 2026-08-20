"""Power menu: switch off, restart, or restart into recovery.

Recovery is not a thing the running system can enter by itself -- the
initramfs owns that decision, before any partition is mounted. The phone
asks for it by leaving a one-shot flag on the user partition
(ndsys-recovery.sh RECOVERY_FLAG) and rebooting; the applier deletes the
flag as it reads it, so a recovery boot can never repeat itself.
"""

import os
import subprocess
import sys
import time

# Apps are loaded by importlib under the name "neodct_app", so sibling
# modules need this directory on sys.path.
_APP_DIR = os.path.dirname(os.path.abspath(__file__))
if _APP_DIR not in sys.path:
    sys.path.insert(0, _APP_DIR)

from System.ui.framework import MessageDialog, SoftKeyBar, VerticalList

APP_ID = 13

# Must match staging.STATE_DIR and ndsys-recovery.sh RECOVERY_FLAG. Written
# from the running system, read by the initramfs as /mnt/user/.ndsys.
STATE_DIR = "/NeoDCT/User/.ndsys"
RECOVERY_FLAG = os.path.join(STATE_DIR, "boot_recovery")

MENU = ["Power off", "Reboot", "Recovery"]
POWER_OFF, REBOOT, RECOVERY = 0, 1, 2

# Same shape as Update/main.py _reboot: which binary exists, and where,
# differs between the qemu and luckfox images, so try in order.
_HALT_COMMANDS = (["poweroff"], ["/sbin/poweroff"], ["busybox", "poweroff"])
_REBOOT_COMMANDS = (["reboot"], ["/sbin/reboot"], ["busybox", "reboot"])

KEY_ENTER = 28


def _spawn_first(candidates):
    """Run whichever of these commands the image actually has."""
    for command in candidates:
        try:
            subprocess.Popen(command)
            return True
        except OSError:
            continue
    return False


def _confirm(ui, question):
    dialog = MessageDialog(ui, question, title="Power", button_text="Yes")
    return dialog.show() == KEY_ENTER


def _tell(ui, message):
    MessageDialog(ui, message, title="Power", button_text="OK").show()


def _go_down(ui, candidates, failure):
    subprocess.call(["sync"])
    if not _spawn_first(candidates):
        _tell(ui, failure)
        return
    # init takes a moment to bring everything down. Sit here instead of
    # returning to the launcher, which would look like the key did nothing.
    time.sleep(30)


def _request_recovery(ui):
    """Leave the one-shot flag, then reboot into it."""
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        with open(RECOVERY_FLAG, "w"):
            pass
        subprocess.call(["sync"])
    except OSError as exc:
        # A read-only or missing user partition is the interesting case:
        # without it there is nowhere to leave the flag, so say so rather
        # than rebooting into an ordinary boot and looking broken.
        _tell(ui, "Cannot ask for recovery: %s" % exc)
        return
    _go_down(ui, _REBOOT_COMMANDS, "Reboot failed.")


def run(ui):
    while True:
        menu = VerticalList(ui, "Power", MENU, app_id=APP_ID)
        SoftKeyBar(ui).update("Select", present=False)
        choice = menu.show()

        if choice < 0:
            return
        if choice == POWER_OFF:
            if _confirm(ui, "Switch the phone off?"):
                _go_down(ui, _HALT_COMMANDS, "Power off failed.")
        elif choice == REBOOT:
            if _confirm(ui, "Restart the phone?"):
                _go_down(ui, _REBOOT_COMMANDS, "Reboot failed.")
        elif choice == RECOVERY:
            if _confirm(ui, "Restart into recovery?"):
                _request_recovery(ui)

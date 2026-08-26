"""A phone with a card in it, for looking at the Update screens.

The update flow is the one part of the UI that cannot be screenshotted by
hand: every screen needs an SD card, a package on it, and a release key the
package matches. This assembles all three in a temporary tree and hands back
the real NeoDCT_UI, so the design tests next door judge genuine frames drawn
by the shipped code rather than mockups.

Set NEODCT_UI_SHOTS=/some/dir when running pytest and every screen the tests
draw is written there as a PNG, which is how the design gets audited::

    NEODCT_UI_SHOTS=/tmp/shots python -m pytest tests/test_update_ui.py
"""

import contextlib
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
NEODCT_DIR = os.path.dirname(HERE)
for _path in (os.path.join(NEODCT_DIR, "tools"),
              os.path.join(NEODCT_DIR, "python-reference")):
    if _path not in sys.path:
        sys.path.insert(0, _path)

from uistub import NEODCT_PREFIX, ScriptExhausted, StubUI, run_app  # noqa: E402
from System.core import Storage  # noqa: E402
from System.core.UpdateService import staging  # noqa: E402

from update_fixtures import PUB_PEM, make_ndsw, png  # noqa: E402

APP_NAME = "Update"
PLATFORM = "qemu-aarch64"
INSTALLED_VERSION = "0.3.1a"

UP, DOWN, ENTER, BACK = 103, 108, 28, 14

SHOT_DIR = os.environ.get("NEODCT_UI_SHOTS")


def shot(image, name):
    """Save a frame for the human doing the design review, if asked to."""
    if not SHOT_DIR or image is None:
        return None
    os.makedirs(SHOT_DIR, exist_ok=True)
    path = os.path.join(SHOT_DIR, name + ".png")
    image.save(path)
    return path


class FakeProcess:
    """Enough of Popen that the app's reboot path completes."""

    returncode = 0

    def wait(self, timeout=None):
        return 0


class Recorder:
    """Swallows sync/reboot so a test run never restarts the machine."""

    def __init__(self):
        self.commands = []

    def call(self, command, *args, **kwargs):
        self.commands.append(list(command))
        return 0

    def Popen(self, command, *args, **kwargs):
        self.commands.append(list(command))
        return FakeProcess()

    def rebooted(self):
        return any("reboot" in os.path.basename(part) for command in self.commands
                   for part in command)


class Phone:
    """The staged device: what is on its card, and what the app draws."""

    def __init__(self, ui, subprocess_recorder):
        self.ui = ui
        self.root = ui.root
        self.subprocess = subprocess_recorder
        self.mount = os.path.join(self.root, "User", "sdcard")
        self.state_file = os.path.join(self.root, "User", "sdcard.prop")

    # --- what is in the phone --------------------------------------------

    def insert_card(self, ready=True, state="mounted"):
        if ready:
            for name in Storage.FOLDERS:
                os.makedirs(os.path.join(self.mount, name), exist_ok=True)
        else:
            os.makedirs(self.mount, exist_ok=True)
        with open(self.state_file, "w") as handle:
            handle.write("state=%s\ndevice=/dev/vdc\nfstype=vfat\n"
                         "label=NEODCT\n" % state)
        return self

    def eject_card(self):
        with open(self.state_file, "w") as handle:
            handle.write("state=absent\n")
        return self

    def put_package(self, name="UPDATE.ndsw", **kwargs):
        """Write a real .ndsw into the card's update folder."""
        path = os.path.join(self.mount, "update", name)
        make_ndsw(path, **kwargs)
        return path

    def set_installed_version(self, version):
        with open(os.path.join(self.root, "System", "version.prop"), "w") as handle:
            handle.write("system.os.versionnumber=%s\n"
                         "system.os.versionname=NeoDCT System v%s\n"
                         "system.os.platform=%s\n"
                         "system.os.buildepoch=1785160800\n"
                         % (version, version, PLATFORM))
        return self

    # --- driving it -------------------------------------------------------

    def run(self, keys=(), frame_budget=240):
        """Launch Update exactly as the launcher would."""
        return run_app(self.ui, APP_NAME, keys=keys, frame_budget=frame_budget)

    @property
    def frames(self):
        return self.ui.fb.frames

    def last(self):
        return self.ui.fb.frames[-1] if self.ui.fb.frames else None


@contextlib.contextmanager
def phone(keys=(), engineering=True, installed=INSTALLED_VERSION,
          idle_budget=4, **stub_kwargs):
    """A booted phone whose card, keys and release key are all set up."""
    stub = StubUI(keys=keys, engineering=engineering, idle_budget=idle_budget,
                  **stub_kwargs)
    recorder = Recorder()
    saved = (subprocess.call, subprocess.Popen, time.sleep)
    with stub as ui:
        device = Phone(ui, recorder)
        device.set_installed_version(installed)
        # The packages the tests build are signed with the throwaway key from
        # update_fixtures, so the staged rootfs gets the matching public half.
        keys_dir = os.path.join(device.root, "System", "keys")
        os.makedirs(keys_dir, exist_ok=True)
        with open(os.path.join(keys_dir, "neodct-release.pub"), "w") as handle:
            handle.write(PUB_PEM)

        # The card lives at the device path inside the staged tree, so the
        # runtime's own constants are used -- but pinned here rather than
        # trusted, since they are module globals any other test can move.
        saved_state_file = Storage.STATE_FILE
        saved_mount_point = Storage.MOUNT_POINT
        Storage.STATE_FILE = device.state_file
        Storage.MOUNT_POINT = NEODCT_PREFIX + "/User/sdcard"
        device.eject_card()
        subprocess.call = recorder.call
        subprocess.Popen = recorder.Popen
        time.sleep = lambda seconds: None
        try:
            yield device
        finally:
            Storage.STATE_FILE = saved_state_file
            Storage.MOUNT_POINT = saved_mount_point
            subprocess.call, subprocess.Popen, time.sleep = saved


def ready_phone(**kwargs):
    """The common case: a card is in, with a good package on it."""
    return phone(**kwargs)


__all__ = ["APP_NAME", "BACK", "DOWN", "ENTER", "INSTALLED_VERSION", "PLATFORM",
           "Phone", "ScriptExhausted", "UP", "make_ndsw", "phone", "png",
           "ready_phone", "run_app", "shot", "staging"]

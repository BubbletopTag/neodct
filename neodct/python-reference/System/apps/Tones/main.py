import os
import time
import subprocess
import select

from System.ui.framework import (SoftKeyBar, MessageDialog, PagedList,
                                 VerticalList, TextScroller)
from System.core.SettingsStorage import set_setting
from System.core import Storage

ROOT_ID = 9
SYSTEM_TONES_DIR = "/NeoDCT/System/tones"
USER_TONES_DIR = "/NeoDCT/User/tones"

ADD_MORE_LABEL = "Add more..."

ADD_MORE_HELP = (
    "Add more ringtones by adding an SD card!\n"
    "\n"
    "Format a card as FAT32, make a folder called \"tones\" on it, and copy "
    "your .mp3 files into it.\n"
    "\n"
    "Put the card in the phone and the tones appear in this list next to the "
    "built-in ones. The phone can set a blank card up for you from Settings."
)

ADD_MORE_HELP_WITH_CARD = (
    "Add more ringtones from your SD card!\n"
    "\n"
    "Copy .mp3 files into the \"tones\" folder on the card that is in the "
    "phone, and they appear in this list next to the built-in ones."
)
SUPPORTED_EXTS = (".mp3")

MPV_CMD = [
    "nice", "-n", "-10",
    "mpv",
    "--no-video",
    "--audio-buffer=4",
    "--quiet"
]


class TonePreviewPlayer:
    def __init__(self):
        self.process = None

    def play(self, path):
        if not path:
            return
        self.stop()
        try:
            self.process = subprocess.Popen(
                MPV_CMD + [path],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except Exception as exc:
            print(f"[Tones] Failed to play {path}: {exc}")
            self.process = None

    def stop(self):
        if not self.process:
            return
        try:
            self.process.terminate()
            self.process.wait(timeout=0.2)
        except Exception:
            try:
                self.process.kill()
            except Exception:
                pass
        self.process = None


def _flush_input(ui):
    fd = getattr(ui, "keypad_fd", None)
    if fd is None:
        return
    while True:
        r, _, _ = select.select([fd], [], [], 0.01)
        if not r:
            break
        try:
            os.read(fd, 24)
        except Exception:
            break


def _tone_dirs():
    """Stock tones from the image, then the card's, then user-added ones."""
    dirs = list(Storage.media_dirs("tones", system_dir=SYSTEM_TONES_DIR))
    if os.path.isdir(USER_TONES_DIR) and USER_TONES_DIR not in dirs:
        dirs.append(USER_TONES_DIR)
    return dirs


def _scan_tones():
    tones = []
    for base in _tone_dirs():
        if not os.path.exists(base):
            continue
        for root, _, files in os.walk(base):
            for filename in sorted(files):
                if filename.lower().endswith(SUPPORTED_EXTS):
                    full_path = os.path.join(root, filename)
                    display = os.path.splitext(os.path.basename(filename))[0]
                    tones.append({"name": display, "path": full_path})
    tones.sort(key=lambda item: item["name"].lower())
    return tones


def _show_ringing_options(ui):
    options = ["Ring", "Vibrate"]
    vlist = VerticalList(ui, "Ringing Options", options, app_id=ROOT_ID)
    softkey = SoftKeyBar(ui)
    softkey.update("Select", present=False)

    selection = vlist.show()
    if selection == -1:
        return
    MessageDialog(ui, "Option saved (no effect yet).").show()


def _show_ringing_tones(ui):
    try:
        os.makedirs(USER_TONES_DIR, exist_ok=True)
    except Exception:
        pass

    tones = _scan_tones()
    if not tones:
        MessageDialog(ui, "No ringtones found.").show()
        return

    # A pseudo-entry at the end explains how to get more, which is the only
    # discoverable place to say "you need an SD card for this".
    tones.append({"name": ADD_MORE_LABEL, "path": None})
    names = [tone["name"] for tone in tones]
    vlist = VerticalList(ui, "Tones", names, app_id=ROOT_ID)
    softkey = SoftKeyBar(ui)
    player = TonePreviewPlayer()

    pending_index = None
    pending_time = 0.0

    def schedule_preview():
        nonlocal pending_index, pending_time
        player.stop()
        if tones[vlist.selected_index]["path"] is None:
            pending_index = None      # nothing to preview for "Add more..."
            return
        pending_index = vlist.selected_index
        pending_time = time.time()

    def redraw():
        softkey.update("Select", present=False)
        vlist.draw()

    _flush_input(ui)
    redraw()

    while True:
        if pending_index is not None and (time.time() - pending_time) >= 0.5:
            player.play(tones[pending_index]["path"])
            pending_index = None

        key = ui.read_keypress(0.05)
        if key is None:
            continue

        if key == 108:  # DOWN
            if vlist.selected_index < len(names) - 1:
                vlist.selected_index += 1
                if vlist.selected_index >= vlist.window_start + vlist.max_lines:
                    vlist.window_start += 1
                schedule_preview()
                redraw()

        elif key == 103:  # UP
            if vlist.selected_index > 0:
                vlist.selected_index -= 1
                if vlist.selected_index < vlist.window_start:
                    vlist.window_start -= 1
                schedule_preview()
                redraw()

        elif 2 <= key <= 10:  # Number shortcuts
            shortcut_idx = key - 2
            if shortcut_idx < len(names):
                vlist.selected_index = shortcut_idx
                if vlist.selected_index < vlist.window_start:
                    vlist.window_start = vlist.selected_index
                elif vlist.selected_index >= vlist.window_start + vlist.max_lines:
                    vlist.window_start = max(0, vlist.selected_index - vlist.max_lines + 1)
                schedule_preview()
                redraw()

        elif key in (28, 96):  # ENTER / center
            player.stop()
            if tones[vlist.selected_index]["path"] is None:
                TextScroller(ui, ADD_MORE_HELP_WITH_CARD if Storage.is_ready()
                             else ADD_MORE_HELP).show()
                _flush_input(ui)
                redraw()
                continue
            set_setting("system.audio.ringtone", tones[vlist.selected_index]["path"])
            MessageDialog(ui, f"Ringtone set to {names[vlist.selected_index]}.").show()
            return

        elif key == 14:  # BACKSPACE
            player.stop()
            return

def run(ui):
    while True:
        menu = PagedList(
            ui,
            "Tones",
            ["Ringing Options", "Ringing Tones"],
            root_id=ROOT_ID,
        )
        selection = menu.show()
        if selection == -1:
            return
        if selection == 0:
            _show_ringing_options(ui)
        elif selection == 1:
            _show_ringing_tones(ui)

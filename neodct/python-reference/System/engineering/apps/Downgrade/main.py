"""Downgrade: install any published release, not just the newest one.

The Update app deliberately only offers what is newer. This one lists every
release that carries a package for this phone and lets you pick, which is
what you want when a new version broke something and you need to get back
to the one that worked.

It is an engineering tool on purpose. Going backwards is a real thing to
want during development, and a genuinely dangerous thing to offer a phone's
owner by accident:

  * An update replaces the whole root filesystem. Going back to an older
    version is not "undo" -- it is installing a different system that
    happens to be older, and anything the newer one changed on the user
    partition stays changed.

  * Older packages can carry bugs that were fixed for good reasons. 0.3.4a
    and 0.3.5a leak QMI clients until the modem stops answering, which
    needs a power cycle to clear.

Everything past "pick one" is the ordinary update path: the same signature
check, the same staging, the same applier. Nothing here installs anything
itself.
"""

import os
import sys

_APP_DIR = os.path.dirname(os.path.abspath(__file__))
if _APP_DIR not in sys.path:
    sys.path.insert(0, _APP_DIR)

from System.ui.framework import (MessageDialog, ProgressScreen, VerticalList,
                                 SoftKeyBar, DetailPage)
from System.core import Storage
from System.core.SettingsStorage import get_setting

APP_ID = 9006
HEADER = "Downgrade"
APP_ICON = "/NeoDCT/System/engineering/apps/Downgrade/icon.png"
ENTER = 28

NO_NETWORK = (
    "This tool reads the release list from GitHub, so the phone needs a "
    "working data connection.\n\n"
    "Without one, an older package can still be copied onto the card by "
    "hand and installed from Update."
)


def _page(ui, title, subtitle="", body="", image=None, softkey_text="Back"):
    return DetailPage(ui, title=title, subtitle=subtitle, body=body,
                      image=image, header=HEADER,
                      softkey_text=softkey_text).show()


def _confirm(ui, message, button_text):
    return MessageDialog(ui, message, button_text=button_text).show() == ENTER


def _refuse(ui, message):
    MessageDialog(ui, message, button_text="OK", cancel_keys=()).show()


def _format_size(count):
    return "%.1f MB" % (count / 1048576.0)


def _installed():
    return get_setting("system.os.versionnumber", "") or ""


def run(ui):
    from System.core.UpdateService import remote, UpdateError

    platform = get_setting("system.os.platform", "unknown")
    installed = _installed()

    dialog = ProgressScreen(ui, "Reading releases", header=HEADER)
    dialog.draw(0, 1)
    try:
        releases = remote.all_releases(platform)
    except remote.NoRelease:
        _page(ui, "Nothing published", subtitle="for %s" % platform,
              body="No release carries a package for this phone yet.",
              image=APP_ICON)
        return
    except remote.NetworkError as exc:
        _page(ui, "No connection", subtitle="Could not reach GitHub",
              body="%s\n\n%s" % (exc, NO_NETWORK), image=APP_ICON)
        return

    # The running version is marked rather than hidden. Seeing where you
    # are in the list is most of the point of the list.
    labels = []
    for entry in releases:
        mark = ""
        if entry["version"] == installed:
            mark = "  (running)"
        elif installed and not remote.is_newer(entry["version"], installed):
            mark = "  (older)"
        labels.append("%s%s" % (entry["version"], mark))

    menu = VerticalList(ui, "Releases", labels, app_id=APP_ID)
    SoftKeyBar(ui).update("Select", present=False)
    choice = menu.show()
    if choice < 0 or choice >= len(releases):
        return
    picked = releases[choice]

    if picked["version"] == installed:
        _page(ui, "Already running", subtitle="NeoDCT %s" % installed,
              body="That is the version this phone is running.",
              image=APP_ICON)
        return

    going_back = installed and not remote.is_newer(picked["version"], installed)
    if going_back:
        # Spelled out rather than a bare "are you sure": the consequence is
        # not obvious, and this tool exists to be used deliberately.
        if not _confirm(ui, "Go back to %s?\nThis replaces the whole system. "
                        "User data is kept but stays as %s left it."
                        % (picked["version"], installed), "Downgrade"):
            return
    else:
        if not _confirm(ui, "Install %s?\n%s"
                        % (picked["version"], _format_size(picked["size"])),
                        "Install"):
            return

    folder = Storage.folder("update")
    if not folder:
        _refuse(ui, "The card has no update folder.")
        return
    destination = os.path.join(folder, remote.asset_name(platform))

    progress = ProgressScreen(ui, "Downloading %s" % picked["version"],
                              header=HEADER)
    try:
        remote.download(picked["url"], destination, size=picked["size"],
                        progress=lambda done, total: progress.draw(
                            done, total or picked["size"] or 1))
    except UpdateError as exc:
        _refuse(ui, "Download failed.\n%s\n\nNothing was installed." % exc)
        return

    # Hand off to the Update app's installer rather than reimplementing it.
    # One signature check, one staging path, one applier -- a second copy of
    # that logic is the last thing this phone needs.
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "neodct_update_app", "/NeoDCT/System/apps/Update/main.py")
        update_app = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(update_app)
        update_app._install(ui, destination)
    except Exception as exc:
        _refuse(ui, "Downloaded, but could not start the installer.\n%s\n\n"
                    "The package is on the card; install it from Update."
                    % exc)

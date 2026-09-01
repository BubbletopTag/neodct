"""What is on the SD card, as the UI needs to know it.

/bin/neodct-sdcard mounts a card and publishes what it found to
/run/neodct/sdcard.prop. This turns that into four states:

  absent       no card in the slot
  ready        a NeoDCT card: mounted, with all five folders
  needs_setup  mountable, but not laid out as a NeoDCT card yet
  unformatted  a card is there but carries no filesystem we can mount
  legacy       a NeoDCT card in the pre-0.5.0b FAT format: it mounts and the
               owner's media reads, but it cannot hold an installed app

Only "ready" hands out paths, so an app can never write into a card that is
about to be reformatted.
"""

import os
from collections import namedtuple

MOUNT_POINT = "/NeoDCT/User/sdcard"
STATE_FILE = "/run/neodct/sdcard.prop"

# The layout a NeoDCT card is expected to have. The user creates these when
# preparing a card by hand; the phone offers to create them otherwise.
FOLDERS = ("wallpapers", "tones", "backup_db", "music", "update")

UPDATE_SUFFIX = ".ndsw"
PREFERRED_UPDATE = "UPDATE.ndsw"

Card = namedtuple("Card", "state device fstype label mountpoint removable")


def _read_state():
    try:
        with open(STATE_FILE, "r", errors="replace") as handle:
            text = handle.read()
    except (OSError, UnicodeError):
        return {}
    values = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip()
    return values


def card():
    """Current card state. Never raises: a missing card is normal."""
    values = _read_state()
    reported = values.get("state", "")
    fstype = values.get("fstype", "")
    device = values.get("device", "")
    removable = fstype != "virtiofs"

    if reported in ("unmountable", "unformatted"):
        return Card("unformatted", device, fstype, values.get("label", ""),
                    MOUNT_POINT, removable)
    # A NeoDCT card in the pre-0.5.0b FAT format: mounted, readable, and
    # unable to hold an installed app because FAT records no ownership. It is
    # a state of its own because the remedy differs in kind -- NEEDS_SETUP is
    # five missing folders, this is a reformat that erases the card.
    #
    # Without this it fell through to `absent` below, so the reference
    # implementation reported "no card" for a card the C says is present and
    # usable. See nd_storage.h.
    if reported == "legacy":
        return Card("legacy", device, fstype, values.get("label", ""),
                    MOUNT_POINT, removable)

    if reported not in ("mounted", "share", "ready"):
        return Card("absent", "", "", "", MOUNT_POINT, removable)

    state = "ready" if _has_folders() else "needs_setup"
    return Card(state, device, fstype, values.get("label", ""), MOUNT_POINT,
                removable)


def _has_folders():
    return all(os.path.isdir(os.path.join(MOUNT_POINT, name))
               for name in FOLDERS)


def is_ready():
    return card().state == "ready"


def folder(name):
    """Absolute path of a folder on a ready card, else None."""
    if not is_ready():
        return None
    return os.path.join(MOUNT_POINT, name)


def setup_folders():
    """Create the NeoDCT folders on an inserted card. True if it worked."""
    try:
        for name in FOLDERS:
            os.makedirs(os.path.join(MOUNT_POINT, name), exist_ok=True)
    except OSError:
        return False
    return True


def media_dirs(kind, system_dir=None):
    """Directories to scan for media of `kind`, stock content first.

    Only existing directories come back, so callers can scan the result
    without checking each entry.
    """
    candidates = []
    if system_dir:
        candidates.append(system_dir)
    on_card = folder(kind)
    if on_card:
        candidates.append(on_card)
    return tuple(path for path in candidates if os.path.isdir(path))


def find_updates():
    """Update packages sitting in the card's update/ folder.

    UPDATE.ndsw comes first: it is the documented name, so it is what the
    user most likely just copied over.
    """
    updates = folder("update")
    if not updates:
        return []
    try:
        names = os.listdir(updates)
    except OSError:
        return []
    packages = sorted(name for name in names
                      if name.lower().endswith(UPDATE_SUFFIX)
                      and os.path.isfile(os.path.join(updates, name)))
    packages.sort(key=lambda name: (name != PREFERRED_UPDATE, name.lower()))
    return [os.path.join(updates, name) for name in packages]

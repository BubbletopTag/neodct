"""SD card state as the apps see it.

The mount helper (/bin/neodct-sdcard) publishes what it found to
/run/neodct/sdcard.prop; this module turns that into the questions the UI
actually asks: is there a card, is it one of ours, where do I look for
music, and is there an update on it.
"""

import os

import pytest

from System.core import Storage

FOLDERS = ("wallpapers", "tones", "backup_db", "music", "update")


@pytest.fixture
def card(tmp_path, monkeypatch):
    """Point Storage at a temporary mountpoint and state file."""
    mount = tmp_path / "sdcard"
    state = tmp_path / "sdcard.prop"
    monkeypatch.setattr(Storage, "MOUNT_POINT", str(mount))
    monkeypatch.setattr(Storage, "STATE_FILE", str(state))
    return tmp_path


def insert(tmp_path, state="mounted", folders=(), **fields):
    values = {"state": state, "device": "/dev/vdc", "fstype": "vfat",
              "label": "NEODCT"}
    values.update(fields)
    (tmp_path / "sdcard.prop").write_text(
        "".join("%s=%s\n" % item for item in values.items()))
    mount = tmp_path / "sdcard"
    mount.mkdir(exist_ok=True)
    for folder in folders:
        (mount / folder).mkdir(exist_ok=True)


def test_no_card_when_nothing_is_mounted(card):
    assert Storage.card().state == "absent"
    assert Storage.is_ready() is False


def test_a_neodct_card_is_ready(card):
    insert(card, folders=FOLDERS)

    assert Storage.card().state == "ready"
    assert Storage.is_ready() is True
    assert Storage.card().device == "/dev/vdc"


def test_a_plain_fat_card_needs_setting_up(card):
    """A card straight out of a camera: mountable, but not ours yet."""
    insert(card, folders=("DCIM",))

    assert Storage.card().state == "needs_setup"
    assert Storage.is_ready() is False


def test_a_card_missing_one_folder_needs_setting_up(card):
    insert(card, folders=("wallpapers", "tones", "music", "update"))

    assert Storage.card().state == "needs_setup"


def test_an_unmountable_card_wants_formatting(card):
    insert(card, state="unmountable", fstype="ext4", label="")

    assert Storage.card().state == "unformatted"
    assert Storage.card().device == "/dev/vdc"


def test_a_virtiofs_share_counts_as_a_ready_card(card):
    """The QEMU convenience path: a host folder, no label, no formatting."""
    insert(card, state="share", fstype="virtiofs", label="", folders=FOLDERS)

    assert Storage.card().state == "ready"
    assert Storage.card().removable is False


def test_folders_are_only_offered_once_the_card_is_ready(card):
    insert(card, folders=("DCIM",))

    assert Storage.folder("music") is None


def test_folder_returns_the_path_on_a_ready_card(card):
    insert(card, folders=FOLDERS)

    assert Storage.folder("music") == str(card / "sdcard" / "music")


def test_setting_up_a_card_creates_the_neodct_folders(card):
    insert(card, folders=("DCIM",))

    assert Storage.setup_folders() is True
    assert Storage.card().state == "ready"
    for folder in FOLDERS:
        assert os.path.isdir(card / "sdcard" / folder)


def test_setting_up_reports_failure_on_a_read_only_card(card, monkeypatch):
    insert(card, folders=())
    monkeypatch.setattr(Storage, "MOUNT_POINT", "/proc/nope/sdcard")

    assert Storage.setup_folders() is False


def test_media_dirs_puts_system_content_first(card):
    """Stock tones ship in the image; the card only adds to them."""
    insert(card, folders=FOLDERS)
    system = card / "system-tones"
    system.mkdir()

    dirs = Storage.media_dirs("tones", system_dir=str(system))

    assert dirs == (str(system), str(card / "sdcard" / "tones"))


def test_media_dirs_skips_directories_that_do_not_exist(card):
    dirs = Storage.media_dirs("tones", system_dir="/no/such/place")

    assert dirs == ()


def test_media_dirs_without_a_card_is_just_the_system_dir(card):
    system = card / "system-tones"
    system.mkdir()

    assert Storage.media_dirs("tones", system_dir=str(system)) == (str(system),)


def test_finds_an_update_package_on_the_card(card):
    insert(card, folders=FOLDERS)
    (card / "sdcard" / "update" / "UPDATE.ndsw").write_bytes(b"zip")

    assert Storage.find_updates() == [str(card / "sdcard" / "update" / "UPDATE.ndsw")]


def test_the_conventional_name_is_offered_first(card):
    insert(card, folders=FOLDERS)
    updates = card / "sdcard" / "update"
    (updates / "older-0.3.1a.ndsw").write_bytes(b"zip")
    (updates / "UPDATE.ndsw").write_bytes(b"zip")

    found = Storage.find_updates()

    assert os.path.basename(found[0]) == "UPDATE.ndsw"
    assert len(found) == 2


def test_non_package_files_on_the_card_are_ignored(card):
    insert(card, folders=FOLDERS)
    (card / "sdcard" / "update" / "readme.txt").write_text("hi")

    assert Storage.find_updates() == []


def test_no_updates_without_a_card(card):
    assert Storage.find_updates() == []


def test_a_corrupt_state_file_reads_as_no_card(card):
    (card / "sdcard.prop").write_bytes(b"\x00\xffgarbage")

    assert Storage.card().state == "absent"

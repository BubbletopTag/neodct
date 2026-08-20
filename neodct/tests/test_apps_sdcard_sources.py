"""Media apps look at the SD card as well as the image.

Stock content ships inside the read-only rootfs; anything the user adds
arrives on a card. These tests cover the directory selection only -- the
"Add more..." / "Get more..." screens themselves are UI flows checked in
QEMU.
"""

import os

import pytest

from System.apps.Settings import main as settings_app
from System.apps.Tones import main as tones_app
from System.core import Storage

FOLDERS = ("wallpapers", "tones", "backup_db", "music", "update")


@pytest.fixture
def card(tmp_path, monkeypatch):
    mount = tmp_path / "sdcard"
    monkeypatch.setattr(Storage, "MOUNT_POINT", str(mount))
    monkeypatch.setattr(Storage, "STATE_FILE", str(tmp_path / "sdcard.prop"))
    return tmp_path


def insert(tmp_path, folders=FOLDERS, state="mounted"):
    (tmp_path / "sdcard.prop").write_text(
        "state=%s\ndevice=/dev/vdc\nfstype=vfat\nlabel=NEODCT\n" % state)
    for folder in folders:
        (tmp_path / "sdcard" / folder).mkdir(parents=True, exist_ok=True)


def test_tones_come_from_the_image_when_there_is_no_card(card, monkeypatch):
    system = card / "system-tones"
    system.mkdir()
    monkeypatch.setattr(tones_app, "SYSTEM_TONES_DIR", str(system))
    monkeypatch.setattr(tones_app, "USER_TONES_DIR", str(card / "nope"))

    assert tones_app._tone_dirs() == [str(system)]


def test_tones_pick_up_the_card_folder(card, monkeypatch):
    system = card / "system-tones"
    system.mkdir()
    insert(card)
    monkeypatch.setattr(tones_app, "SYSTEM_TONES_DIR", str(system))
    monkeypatch.setattr(tones_app, "USER_TONES_DIR", str(card / "nope"))

    assert tones_app._tone_dirs() == [str(system), str(card / "sdcard" / "tones")]


def test_a_card_that_is_not_set_up_contributes_nothing(card, monkeypatch):
    """Half a card layout must not be scanned: it is about to be set up."""
    system = card / "system-tones"
    system.mkdir()
    insert(card, folders=("DCIM",))
    monkeypatch.setattr(tones_app, "SYSTEM_TONES_DIR", str(system))
    monkeypatch.setattr(tones_app, "USER_TONES_DIR", str(card / "nope"))

    assert tones_app._tone_dirs() == [str(system)]


def test_a_tone_on_the_card_is_listed(card, monkeypatch):
    system = card / "system-tones"
    system.mkdir()
    (system / "Low.mp3").write_bytes(b"id3")
    insert(card)
    (card / "sdcard" / "tones" / "Nokia Tune.mp3").write_bytes(b"id3")
    monkeypatch.setattr(tones_app, "SYSTEM_TONES_DIR", str(system))
    monkeypatch.setattr(tones_app, "USER_TONES_DIR", str(card / "nope"))

    names = [tone["name"] for tone in tones_app._scan_tones()]

    assert names == ["Low", "Nokia Tune"]


def test_wallpapers_come_from_the_image_by_default(card, monkeypatch):
    system = card / "system-wallpapers"
    system.mkdir()
    monkeypatch.setattr(settings_app, "SYSTEM_WALLPAPER_DIR", str(system))
    monkeypatch.setattr(settings_app, "WALLPAPER_DIR", str(card / "nope"))

    assert settings_app._wallpaper_dirs() == [str(system)]


def test_wallpapers_pick_up_the_card_folder(card, monkeypatch):
    system = card / "system-wallpapers"
    system.mkdir()
    insert(card)
    monkeypatch.setattr(settings_app, "SYSTEM_WALLPAPER_DIR", str(system))
    monkeypatch.setattr(settings_app, "WALLPAPER_DIR", str(card / "nope"))

    assert settings_app._wallpaper_dirs() == [
        str(system), str(card / "sdcard" / "wallpapers")]


def test_stock_wallpapers_are_found_in_the_image(card, monkeypatch):
    """They moved out of /NeoDCT/User when it became a separate partition."""
    system = card / "system-wallpapers"
    system.mkdir()
    (system / "Grasslands.jpg").write_bytes(b"\xff\xd8\xff")
    monkeypatch.setattr(settings_app, "SYSTEM_WALLPAPER_DIR", str(system))
    monkeypatch.setattr(settings_app, "WALLPAPER_DIR", str(card / "nope"))

    found = settings_app._scan_wallpapers()

    assert [w["name"] for w in found] == ["Grasslands"]


def test_the_shipped_wallpapers_really_are_in_the_image():
    """Guard against the move being undone: the overlay must carry them."""
    overlay = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "overlay", "NeoDCT", "System", "wallpapers")

    names = sorted(os.listdir(overlay))

    assert "Palestine.jpg" in names
    assert len([n for n in names if n.lower().endswith(".jpg")]) >= 6


def test_music_needs_a_card(card):
    from System.apps.MusicPlayer import main as music_app

    assert Storage.folder(music_app.MUSIC_FOLDER) is None


def test_music_is_found_on_a_card(card):
    from System.apps.MusicPlayer import main as music_app
    insert(card)
    (card / "sdcard" / "music" / "song.mp3").write_bytes(b"id3")

    assert Storage.folder(music_app.MUSIC_FOLDER) == \
        str(card / "sdcard" / "music")

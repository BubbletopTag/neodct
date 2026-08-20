"""System facts live in the image; user settings live on the user partition.

Before the immutable-rootfs work, system.os.versionnumber was stored in
/NeoDCT/User/settings.prop. That only appeared to work because an update
replaced the whole filesystem including settings.prop. With /NeoDCT/User on
its own partition that file survives an update, so the phone would keep
reporting the version it shipped with forever. Those keys now come from a
read-only /NeoDCT/System/version.prop written at build time.
"""

import pytest

from System.core import SettingsStorage


@pytest.fixture
def storage(tmp_path, monkeypatch):
    """Point SettingsStorage at temporary files instead of /NeoDCT."""
    monkeypatch.setattr(SettingsStorage, "SETTINGS_PATH",
                        str(tmp_path / "settings.prop"))
    monkeypatch.setattr(SettingsStorage, "VERSION_PATH",
                        str(tmp_path / "version.prop"))
    return tmp_path


def write_version(tmp_path, **values):
    body = "".join("%s=%s\n" % item for item in values.items())
    (tmp_path / "version.prop").write_text(body)


def test_version_prop_supplies_the_installed_version(storage):
    write_version(storage, **{
        "system.os.versionnumber": "0.3.2a",
        "system.os.versionname": "NeoDCT System v0.3.2a",
        "system.os.platform": "qemu-aarch64",
        "system.os.buildtime": "1785160800",
    })

    assert SettingsStorage.get_setting("system.os.versionnumber") == "0.3.2a"
    assert SettingsStorage.get_setting("system.os.platform") == "qemu-aarch64"


def test_version_prop_wins_over_a_stale_user_settings_file(storage):
    """The exact bug this split exists to prevent."""
    (storage / "settings.prop").write_text(
        "system.os.versionnumber=0.3.0a\nsystem.ui.wallpaper=/some/where.jpg\n")
    write_version(storage, **{"system.os.versionnumber": "0.3.2a"})

    assert SettingsStorage.get_setting("system.os.versionnumber") == "0.3.2a"


def test_user_settings_are_untouched_by_the_split(storage):
    write_version(storage, **{"system.os.versionnumber": "0.3.2a"})

    SettingsStorage.set_setting("system.ui.wallpaper", "/NeoDCT/User/w.jpg")

    assert SettingsStorage.get_setting("system.ui.wallpaper") == "/NeoDCT/User/w.jpg"


def test_system_facts_are_never_written_into_settings_prop(storage):
    """Otherwise they would go stale again the moment they were persisted."""
    write_version(storage, **{"system.os.versionnumber": "0.3.2a"})

    SettingsStorage.set_setting("system.ui.wallpaper", "NONE")

    assert "system.os." not in (storage / "settings.prop").read_text()


def test_stale_system_keys_are_dropped_from_an_existing_settings_file(storage):
    (storage / "settings.prop").write_text(
        "system.os.versionnumber=0.1.0a\nsystem.ui.engineering_mode=OFF\n")
    write_version(storage, **{"system.os.versionnumber": "0.3.2a"})

    SettingsStorage.set_setting("system.ui.wallpaper", "NONE")
    body = (storage / "settings.prop").read_text()

    assert "0.1.0a" not in body
    assert "system.ui.engineering_mode=OFF" in body


def test_falls_back_to_defaults_when_version_prop_is_missing(storage):
    """Host tests and pre-split images have no version.prop; don't crash."""
    assert SettingsStorage.get_setting("system.os.versionnumber") == \
        SettingsStorage.DEFAULTS["system.os.versionnumber"]


def test_a_corrupt_version_prop_does_not_break_settings(storage):
    (storage / "version.prop").write_bytes(b"\x00\xff not a prop file")

    assert SettingsStorage.get_setting("system.ui.engineering_mode") == "ON"


def test_platform_is_available_for_update_compatibility_checks(storage):
    """SystemUpdate refuses packages built for other hardware using this."""
    write_version(storage, **{"system.os.platform": "luckfox-armv7"})

    assert SettingsStorage.get_setting("system.os.platform") == "luckfox-armv7"


def test_a_read_only_settings_path_still_reads(storage, monkeypatch):
    """If the user partition is missing, the UI must still boot."""
    monkeypatch.setattr(SettingsStorage, "SETTINGS_PATH",
                        "/proc/definitely/not/writable/settings.prop")
    write_version(storage, **{"system.os.versionnumber": "0.3.2a"})

    assert SettingsStorage.get_setting("system.os.versionnumber") == "0.3.2a"
    assert SettingsStorage.get_setting("system.ui.engineering_mode") == "ON"


def test_the_boot_splash_reads_the_version_out_of_the_image(storage):
    """The splash had the number typed into it, so it drifted a release
    behind the moment anything else was bumped."""
    import launcher

    write_version(storage, **{"system.os.versionnumber": "9.9.9z"})

    assert launcher.splash_version() == "System v9.9.9z"


def test_the_splash_still_says_something_with_no_version_prop(storage):
    """An image with no version.prop is broken, but the splash is the last
    place that should be the thing to crash."""
    import launcher

    assert launcher.splash_version().startswith("System v")

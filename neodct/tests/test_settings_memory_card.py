"""Settings > Memory card: prepare a card the phone doesn't recognise.

Two very different actions hide behind one menu entry, so which one is
offered matters: a mountable card only needs folders adding (nothing is
lost), while an unreadable one can only be reformatted (everything is).
"""

import os

import pytest

from System.apps.Settings import main as settings_app
from System.core import Storage


class Recorder:
    def __init__(self):
        self.dialogs = []
        self.scrollers = []
        self.replies = []
        self.formatted = []


@pytest.fixture
def env(tmp_path, monkeypatch):
    mount = tmp_path / "sdcard"
    mount.mkdir()
    monkeypatch.setattr(Storage, "MOUNT_POINT", str(mount))
    monkeypatch.setattr(Storage, "STATE_FILE", str(tmp_path / "sdcard.prop"))

    recorder = Recorder()

    class FakeDialog:
        def __init__(self, ui, message, button_text="OK", cancel_keys=(14,),
                     **kwargs):
            self.message = message
            self.button_text = button_text

        def show(self):
            recorder.dialogs.append((self.message, self.button_text))
            return recorder.replies.pop(0) if recorder.replies else 28

    class FakeScroller:
        def __init__(self, ui, text, **kwargs):
            self.text = text

        def show(self):
            recorder.scrollers.append(self.text)

    monkeypatch.setattr(settings_app, "MessageDialog", FakeDialog)
    monkeypatch.setattr(settings_app, "TextScroller", FakeScroller)
    monkeypatch.setattr(settings_app.subprocess, "call",
                        lambda argv: recorder.formatted.append(argv) or 0)
    recorder.mount = mount
    recorder.tmp_path = tmp_path
    return recorder


def insert(env, state="mounted", fstype="vfat", folders=(), device="/dev/vdc"):
    (env.tmp_path / "sdcard.prop").write_text(
        "state=%s\ndevice=%s\nfstype=%s\nlabel=\n" % (state, device, fstype))
    for folder in folders:
        (env.mount / folder).mkdir(exist_ok=True)


def messages(env):
    return [d[0] for d in env.dialogs]


def test_no_card_explains_the_layout(env):
    settings_app._show_memory_card(None)

    assert "No memory card." in messages(env)
    assert any("wallpapers" in text for text in env.scrollers)


def test_a_ready_card_is_reported_as_ready(env):
    insert(env, folders=Storage.FOLDERS)

    settings_app._show_memory_card(None)

    assert any("ready" in m for m in messages(env))
    assert env.formatted == []


def test_a_plain_card_is_offered_folder_setup_not_formatting(env):
    """Nothing on the card should be lost to make it a NeoDCT card."""
    insert(env, folders=("DCIM",))

    settings_app._show_memory_card(None)

    offer = [d for d in env.dialogs if d[1] == "Set up"]
    assert offer, messages(env)
    assert "ERASED" not in " ".join(messages(env))
    assert env.formatted == []


def test_accepting_setup_creates_the_folders(env):
    insert(env, folders=("DCIM",))

    settings_app._show_memory_card(None)

    for folder in Storage.FOLDERS:
        assert os.path.isdir(env.mount / folder)
    assert os.path.isdir(env.mount / "DCIM")     # existing content kept
    assert Storage.is_ready() is True


def test_declining_setup_leaves_the_card_alone(env):
    insert(env, folders=("DCIM",))
    env.replies = [14]

    settings_app._show_memory_card(None)

    assert not os.path.isdir(env.mount / "music")


def test_an_unreadable_card_warns_that_formatting_erases_everything(env):
    insert(env, state="unmountable", fstype="")
    env.replies = [14]

    settings_app._show_memory_card(None)

    warning = [d for d in env.dialogs if d[1] == "Format"]
    assert warning
    assert "EVERYTHING ON IT WILL BE ERASED" in warning[0][0]


def test_declining_the_format_prompt_runs_nothing(env):
    insert(env, state="unmountable", fstype="")
    env.replies = [14]

    settings_app._show_memory_card(None)

    assert env.formatted == []


def test_accepting_the_format_prompt_calls_the_helper_with_the_device(env):
    insert(env, state="unmountable", fstype="", device="/dev/vdc")

    settings_app._show_memory_card(None)

    assert env.formatted == [[settings_app.SDCARD_HELPER, "format", "/dev/vdc"]]


def test_formatting_needs_a_device_name(env):
    insert(env, state="unmountable", fstype="", device="")

    settings_app._show_memory_card(None)

    assert env.formatted == []
    assert any("No card device" in m for m in messages(env))

"""NotifyService must not drag the audio stack in at start-up.

python-miniaudio costs 12.9 MB of RSS the moment it is imported -- more
than the whole rest of the phone's UI -- and it pulls ssl and hashlib in
with it. It exists to play a ringtone. A phone sitting on the home screen
should not be paying for one it is not playing.

Measured on the device: 27.1 MB at the home screen with the eager import,
and the audio stack is the largest single item in it.
"""

import importlib
import os
import sys

import pytest


@pytest.fixture
def fake_miniaudio(tmp_path, monkeypatch):
    """A stand-in miniaudio that records the moment it is imported.

    Without this the test is vacuous: miniaudio is a target package and is
    not installed on the host, so "was it imported?" would answer "no" on
    a build that tried its hardest to import it.
    """
    marker = tmp_path / "imported"
    (tmp_path / "miniaudio.py").write_text(
        "open(%r, 'w').write('yes')\n"
        "class SampleFormat:\n    SIGNED16 = 1\n"
        "def decode_file(*a, **k):\n    raise RuntimeError('stub')\n"
        % str(marker))

    monkeypatch.syspath_prepend(str(tmp_path))
    for name in ("miniaudio", "System.core.NotifyService"):
        sys.modules.pop(name, None)
    yield marker
    for name in ("miniaudio", "System.core.NotifyService"):
        sys.modules.pop(name, None)


def test_importing_notifyservice_does_not_load_the_audio_stack(fake_miniaudio):
    importlib.import_module("System.core.NotifyService")
    assert not fake_miniaudio.exists(), (
        "miniaudio was imported at start-up; that is 12.9 MB the home "
        "screen does not need")
    assert "miniaudio" not in sys.modules


def test_the_audio_stack_is_loaded_when_a_tone_is_actually_wanted(
        fake_miniaudio):
    notify = importlib.import_module("System.core.NotifyService")
    assert notify.miniaudio_module() is not None
    assert fake_miniaudio.exists()


def test_the_module_is_only_imported_once(fake_miniaudio):
    notify = importlib.import_module("System.core.NotifyService")
    first = notify.miniaudio_module()
    fake_miniaudio.unlink()              # a second import would recreate it
    second = notify.miniaudio_module()
    assert first is second
    assert not fake_miniaudio.exists()


def test_a_missing_audio_stack_is_not_an_error(monkeypatch, tmp_path):
    """No miniaudio at all: ringing falls back to mpv, nothing raises."""
    monkeypatch.syspath_prepend(str(tmp_path))     # empty: no miniaudio here
    for name in ("miniaudio", "System.core.NotifyService"):
        sys.modules.pop(name, None)
    monkeypatch.setitem(sys.modules, "miniaudio", None)
    sys.modules.pop("miniaudio")

    notify = importlib.import_module("System.core.NotifyService")
    real = sys.modules.pop("miniaudio", None)
    try:
        monkeypatch.setattr(notify, "_miniaudio", None, raising=False)
        monkeypatch.setattr(notify, "_miniaudio_tried", False, raising=False)
        monkeypatch.setattr(sys, "path", [p for p in sys.path
                                          if "site-packages" not in p])
        assert notify.miniaudio_module() is None
        assert notify.miniaudio_module() is None   # still None, still quiet
    finally:
        if real is not None:
            sys.modules["miniaudio"] = real

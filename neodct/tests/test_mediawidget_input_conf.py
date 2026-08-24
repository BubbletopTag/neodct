"""The two ways a key reaches mpv must mean the same thing.

Keys normally arrive over the IPC socket, translated by MediaWidget. But
mpv also reads its own input, and if the bridge never starts -- no evdev
device, a uinput bridge that failed -- that is the only way out of a
video. The two paths are written in different languages in different
files, so this checks they still agree.
"""

import os

import pytest

from System.core import MediaWidget

INPUT_CONF = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "overlay", "NeoDCT", "System", "core", "MediaWidget", "input.conf",
)


def bindings():
    """{mpv key name: [command words]} parsed out of input.conf."""
    parsed = {}
    with open(INPUT_CONF) as handle:
        for line in handle:
            line = line.strip()
            # mpv spells the '#' key "SHARP" precisely so that a binding
            # for it is not swallowed as a comment.
            if not line or line.startswith("#"):
                continue
            key, _, command = line.partition(" ")
            if command.strip():
                parsed[key] = command.split()
    return parsed


def test_the_file_exists_and_binds_things():
    assert bindings()


def test_every_neodct_key_is_bound_the_same_way_in_both_paths():
    conf = bindings()
    for keycode, command in MediaWidget._KEYMAP.items():
        name = MediaWidget.MPV_KEY_NAMES.get(keycode)
        assert name is not None, "keycode %d has no mpv key name" % keycode
        assert name in conf, "%s (keycode %d) is not bound in input.conf" % (
            name, keycode)
        assert conf[name] == command, (
            "%s: input.conf says %r, the bridge sends %r"
            % (name, conf[name], command))


def test_the_clear_key_quits():
    assert bindings()["BS"] == ["quit"]


def test_nothing_is_bound_that_the_phone_cannot_reach():
    # A binding for a key with no NeoDCT equivalent is dead weight at best
    # and, on a phone with no keyboard, a promise that cannot be kept.
    reachable = set(MediaWidget.MPV_KEY_NAMES.values())
    assert set(bindings()) <= reachable


def test_the_escape_hatch_is_documented_in_the_file():
    # Whoever edits this next needs to know why quit is not optional.
    with open(INPUT_CONF) as handle:
        assert "#" in handle.read()

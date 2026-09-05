"""The overlay is the image. What must not be in it.

BR2_ROOTFS_OVERLAY is copied over the target tree AFTER every package has
installed (buildroot/Makefile: packages, strip, then overlay, then
post-build), so a file in the overlay silently replaces whatever a package
put at the same path. That is how the browser lost its media player: the
C neodct-play was installed by the neodct package, and then the overlay's
Python neodct-play -- a #!/usr/bin/env python3 script, on an image with no
python3 -- was copied over it. The browser said "No media player" for
every video on the web, and nothing in the build said a word.

The prune script drops *.py from the image as a backstop, but a script is
only Python by its first line, and this is the check for that.
"""

import os

OVERLAY = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "overlay",
)


def _files(root):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d != "__pycache__"]
        for name in filenames:
            yield os.path.join(dirpath, name)


def _shebang(path):
    try:
        with open(path, "rb") as f:
            head = f.read(128)
    except OSError:
        return None
    if not head.startswith(b"#!"):
        return None
    return head.split(b"\n", 1)[0].decode("ascii", "replace")


def test_nothing_in_the_overlay_needs_an_interpreter_the_phone_does_not_have():
    offenders = []
    for path in _files(OVERLAY):
        line = _shebang(path)
        if line is None:
            continue
        if "python" in line:
            offenders.append(os.path.relpath(path, OVERLAY) + ": " + line)
    assert offenders == [], (
        "these overlay files ask for python, which is not on the image; "
        "the reference Python lives in neodct/python-reference:\n  "
        + "\n  ".join(offenders)
    )


def test_no_python_modules_in_the_overlay():
    offenders = [
        os.path.relpath(path, OVERLAY)
        for path in _files(OVERLAY)
        if path.endswith((".py", ".pyc", ".pyo"))
    ]
    assert offenders == [], (
        "Python in the overlay ships to the phone; it belongs in "
        "neodct/python-reference:\n  " + "\n  ".join(offenders)
    )


def test_the_media_player_the_browser_execs_is_not_in_the_overlay():
    # neodct/src installs the C neodct-play to exactly this path
    # (netsurf-neodct/.../neodct_media.h names it). Anything the overlay
    # carries there would replace it in the image.
    path = os.path.join(OVERLAY, "NeoDCT", "System", "core", "MediaWidget",
                        "neodct-play")
    assert not os.path.exists(path), (
        "the overlay carries a neodct-play; it would overwrite the C binary "
        "the neodct package installs at the same path"
    )

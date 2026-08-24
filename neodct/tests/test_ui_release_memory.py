"""The launcher should hand memory back before starting something heavy.

Measured on the device: the UI sits at 19.5 MB on the home screen, and
about 10.7 MB of that is anonymous -- the part that can only go to zram,
never simply be dropped. When the browser and then mpv start on top of it,
5.5 MB of the UI is compressed into zram, and compressing it costs the one
core the video decoder is trying to use. Memory pressure on this phone
arrives disguised as a slow decoder.

Clearing the image cache first returns about 1.2 MB of that, but only with
malloc_trim: gc.collect() on its own returns nothing at all to the OS,
because CPython hands freed blocks back to its arenas rather than to the
kernel.
"""

import pytest

pytest.importorskip("PIL")

from System.core.main import NeoDCT_UI


def _ui():
    """A UI object without a framebuffer behind it.

    __init__ opens /dev/fb0 and scans for a keypad; neither exists on the
    host and neither is what this is about.
    """
    ui = object.__new__(NeoDCT_UI)
    ui.image_cache = {}
    return ui


def test_releasing_drops_the_image_cache():
    ui = _ui()
    ui.image_cache = {"a.png": object(), "b.png": object()}
    ui.release_memory()
    assert ui.image_cache == {}


def test_releasing_twice_is_harmless():
    ui = _ui()
    ui.release_memory()
    ui.release_memory()
    assert ui.image_cache == {}


def test_releasing_survives_a_ui_with_no_cache_attribute():
    # Never let a memory optimisation be the thing that stops an app
    # from launching.
    ui = object.__new__(NeoDCT_UI)
    ui.release_memory()


def test_the_cache_still_works_afterwards():
    ui = _ui()
    ui.release_memory()
    ui.image_cache["fresh.png"] = object()
    assert "fresh.png" in ui.image_cache


def test_the_arena_trim_is_attempted():
    # gc.collect() returns nothing to the kernel on its own -- measured.
    # The trim is the part that actually hands pages back, so a build that
    # silently stopped calling it would lose the whole point.
    import System.core.main as main

    calls = []
    original = main._trim_malloc_arenas
    main._trim_malloc_arenas = lambda: calls.append(True)
    try:
        _ui().release_memory()
    finally:
        main._trim_malloc_arenas = original
    assert calls == [True]


def test_a_failed_arena_trim_is_not_fatal(monkeypatch):
    """The trim is libc-specific. A build without it should lose the
    saving, not the app launch."""
    import System.core.main as main

    def boom():
        raise OSError("no libc here")

    monkeypatch.setattr(main, "_trim_malloc_arenas", boom)
    ui = _ui()
    ui.image_cache = {"a.png": object()}
    ui.release_memory()
    assert ui.image_cache == {}

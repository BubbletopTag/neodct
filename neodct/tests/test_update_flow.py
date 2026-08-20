"""The update flow end to end, on real pixels.

Everything here launches the shipped app the way the launcher does, against
a staged rootfs with a real card and a real .ndsw on it, and then counts and
measures the frames that came out. Two things are being tested:

  * that the flow is short -- the old one made you read five screens of
    release notes and answer four questions to install an update;
  * that no frame is ugly in the ways that matter (text across the progress
    bar, content spilling into the softkey bar).

NEODCT_UI_SHOTS=/some/dir writes every frame out for a look.
"""

import pytest

from System.core.UpdateService import staging
from System.ui.framework import ProgressScreen, _content_bottom

from update_ui_fixtures import (BACK, DOWN, ENTER, INSTALLED_VERSION, UP,
                                phone, png, shot)

BLACK = (0, 0, 0)


def lit(frame, box):
    x0, y0, x1, y1 = box
    pixels = frame.load()
    return [(x, y)
            for y in range(max(0, y0), min(frame.height, y1))
            for x in range(max(0, x0), min(frame.width, x1))
            if pixels[x, y] != BLACK]


def save_all(frames, prefix):
    for index, frame in enumerate(frames):
        shot(frame, "%s-%02d" % (prefix, index))
    return frames


def bar_geometry(ui):
    """Where the progress bar lands, straight from the widget."""
    return ProgressScreen(ui, "x").bar_box


def is_progress_frame(frame, bar):
    """A frame showing the bar: its top edge is an unbroken white run."""
    left, top, right, _ = bar
    return len(lit(frame, (left, top, right, top + 1))) > (right - left) - 4


def install_run(**kwargs):
    """A phone with one good package on the card, ready to be installed."""
    return phone(**kwargs)


def test_installing_an_update_takes_two_key_presses():
    """Install on the update page, Restart on the last one. That is all."""
    with install_run() as device:
        device.insert_card()
        device.put_package()

        frames = device.run(keys=[ENTER, ENTER])
        save_all(frames, "install")

        assert staging.read_pending() is not None
        assert device.subprocess.rebooted()


def test_the_whole_install_is_two_screens_and_a_bar():
    """The old flow made you read eleven, five of them release notes.

    Repaints of the bar are not screens -- what is counted here is how many
    things there are to look at and answer.
    """
    with install_run() as device:
        device.insert_card()
        device.put_package(changelog="Added\n- A\n- B\n\nFixed\n- C\n- D\n- E")
        bar = bar_geometry(device.ui)

        frames = device.run(keys=[ENTER, ENTER])

        screens = [frame for frame in frames if not is_progress_frame(frame, bar)]
        assert len(screens) == 2, "the flow got long again"


def test_a_phone_with_no_card_gets_one_screen_not_five():
    with install_run() as device:
        frames = device.run(keys=[ENTER])
        save_all(frames, "nocard")

        assert len(frames) == 1


def test_a_phone_with_nothing_to_install_gets_one_screen():
    with install_run() as device:
        device.insert_card()

        frames = device.run(keys=[ENTER])
        save_all(frames, "uptodate")

        assert len(frames) == 1


def test_the_release_thumbnail_reaches_the_screen():
    """mkupdate put it in the zip, the manifest vouches for it, and it is
    the first thing on the page."""
    art = png(size=64, colour=(255, 0, 0))
    with install_run() as device:
        device.insert_card()
        device.put_package(thumbnail=art)

        frames = device.run(keys=[BACK])
        shot(frames[0], "page-with-release-art")

        reds = [xy for xy in lit(frames[0], (0, 0, 240, _content_bottom(device.ui)))
                if frames[0].getpixel(xy) == (255, 0, 0)]
        assert len(reds) > 1000


def test_the_release_notes_scroll_on_the_page_rather_than_paging():
    with install_run() as device:
        device.insert_card()
        device.put_package(changelog="\n".join(
            "- change number %d" % n for n in range(20)))

        frames = device.run(keys=[DOWN, DOWN, ENTER, ENTER])
        save_all(frames, "scrolled")

        assert frames[0].tobytes() != frames[1].tobytes()
        assert staging.read_pending() is not None


def test_no_frame_of_a_real_install_has_text_across_the_progress_bar():
    with install_run() as device:
        device.insert_card()
        device.put_package()
        bar = bar_geometry(device.ui)

        frames = device.run(keys=[ENTER, ENTER])

        drawn = [frame for frame in frames if is_progress_frame(frame, bar)]
        assert drawn, "the install never showed the bar"
        for index, frame in enumerate(drawn):
            assert not lit(frame, (0, bar[1] - 3, 240, bar[1])), \
                "frame %d draws over the top of the bar" % index
            assert not lit(frame, (0, bar[3] + 1, 240, bar[3] + 4)), \
                "frame %d draws over the bottom of the bar" % index


def test_nothing_in_the_flow_spills_into_the_softkey_bar():
    with install_run() as device:
        device.insert_card()
        device.put_package()
        bottom = _content_bottom(device.ui)

        frames = device.run(keys=[ENTER, ENTER])

        for index, frame in enumerate(frames):
            assert not lit(frame, (0, bottom - 2, 240, bottom)), \
                "frame %d touches the softkey bar" % index


def test_the_databases_are_backed_up_to_the_card_on_the_way_past():
    """No prompt, no extra screen: it happens while the bar is on screen."""
    import os

    with install_run() as device:
        device.insert_card()
        device.put_package()

        device.run(keys=[ENTER, ENTER])

        expected = sorted(name for name in os.listdir(
            os.path.join(device.root, "User", "db")) if name.endswith(".db"))
        backup_root = os.path.join(device.mount, "backup_db")
        copied = [name for _, _, names in os.walk(backup_root) for name in names]
        assert copied and sorted(copied) == expected


def test_backing_out_of_the_update_page_installs_nothing():
    with install_run() as device:
        device.insert_card()
        device.put_package()

        device.run(keys=[BACK])

        assert staging.read_pending() is None
        assert not device.subprocess.rebooted()


def test_an_unsigned_build_warns_once_then_asks_before_installing():
    """Engineering mode: read the warning, say you meant it, install."""
    with install_run() as device:
        device.insert_card()
        device.put_package(members=("rootfs.squashfs", "manifest.json"))

        frames = device.run(keys=[ENTER, ENTER, ENTER, ENTER])
        save_all(frames, "unsigned")

        assert staging.read_pending() is not None
        assert device.subprocess.rebooted()


def test_an_unsigned_build_is_a_dead_end_outside_engineering_mode():
    with phone(engineering=False) as device:
        device.insert_card()
        device.put_package(members=("rootfs.squashfs", "manifest.json"))

        frames = device.run(keys=[ENTER, ENTER, ENTER, ENTER])
        save_all(frames, "unsigned-blocked")

        assert len(frames) == 1, "there should be nothing after the warning"
        assert staging.read_pending() is None
        assert not device.subprocess.rebooted()

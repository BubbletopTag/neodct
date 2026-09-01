"""The Scratch port must draw what the Python draws.

`neodct/tools/scratch/build_sb3.py` turns System/ui/framework.py into a
Scratch 3 project. There is no way to look at that from a test, so
`emulator.py` runs the generated blocks and renders them at the phone's
240x175 -- and every screen below is rendered twice, once by the real
framework through `uistub` and once by the emulated blocks, and the two
images have to be identical.

That makes the port a port rather than a lookalike: a coordinate that drifts
by one pixel, a font measured with the wrong metric, an off-by-one in the
scrolling window, all fail here.
"""

import os
import sys

import pytest
from PIL import ImageChops

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(REPO, "neodct", "tools")
SCRATCH_TOOLS = os.path.join(TOOLS, "scratch")
for path in (TOOLS, SCRATCH_TOOLS):
    if path not in sys.path:
        sys.path.insert(0, path)

import uistub                                    # noqa: E402


@pytest.fixture(scope="session")
def project(tmp_path_factory):
    import build_sb3
    out = tmp_path_factory.mktemp("scratch") / "NeoDCT.sb3"
    return build_sb3.build(str(out))


@pytest.fixture(scope="session")
def ui():
    with uistub.StubUI(idle_budget=4000) as stub:
        yield stub


def emulate(project, keys=(), clock=1 / 30.0):
    import emulator
    return emulator.Emulator(project, keys=keys, clock=clock)


def python_frame(ui, draw):
    """Run `draw` against the real framework and return the 240x175 frame."""
    ui.draw.rectangle((0, 0, ui.W, ui.H), fill="black")
    draw()
    return ui.canvas.copy()


def softkey(ui, text, present=True):
    from System.ui.framework import SoftKeyBar
    SoftKeyBar(ui).update(text, present=present)


def same(expected, actual, name):
    if expected.size != actual.size:
        pytest.fail("%s: %r vs %r" % (name, expected.size, actual.size))
    diff = ImageChops.difference(expected.convert("RGB"), actual.convert("RGB"))
    box = diff.getbbox()
    if box is None:
        return
    out = os.environ.get("NEODCT_SCRATCH_DIFF_DIR")
    if out:
        os.makedirs(out, exist_ok=True)
        expected.save(os.path.join(out, name + "-python.png"))
        actual.save(os.path.join(out, name + "-scratch.png"))
    pixels = sum(1 for p in diff.convert("L").getdata() if p)
    pytest.fail("%s differs in %d pixels, first at %r%s"
                % (name, pixels, box,
                   "" if out else " (set NEODCT_SCRATCH_DIFF_DIR to dump both)"))


# --- the screens ---------------------------------------------------------

MENU = ["Phonebook", "Messages", "Games", "Settings", "Tones"]


def test_vertical_list(project, ui):
    from System.ui.framework import VerticalList

    def draw():
        widget = VerticalList(ui, "Main Menu", MENU, app_id=1)
        widget.draw()

    expected = python_frame(ui, draw)

    em = emulate(project, keys=["\\"])
    em.lists["nd items"] = list(MENU)
    em.call("list show %s app %s from %s", "Main Menu", 1, 0)
    assert float(em.variables["nd result"]) == -1
    same(expected, em.canvas.image, "vertical-list")


def test_vertical_list_scrolls(project, ui):
    from System.ui.framework import VerticalList

    def draw():
        widget = VerticalList(ui, "Main Menu", MENU, app_id=1)
        for _ in range(4):
            if widget.selected_index < len(widget.items) - 1:
                widget.selected_index += 1
                if widget.selected_index >= widget.window_start + widget.max_lines:
                    widget.window_start += 1
        widget.draw()

    expected = python_frame(ui, draw)

    em = emulate(project, keys=["down arrow"] * 4 + ["enter"])
    em.lists["nd items"] = list(MENU)
    em.call("list show %s app %s from %s", "Main Menu", 1, 0)
    assert float(em.variables["nd result"]) == 4
    same(expected, em.canvas.image, "vertical-list-scrolled")


def test_vertical_list_number_shortcut(project, ui):
    em = emulate(project, keys=["3"])
    em.lists["nd items"] = list(MENU)
    em.call("list show %s app %s from %s", "Main Menu", 1, 0)
    assert float(em.variables["nd result"]) == 2


def test_long_title_is_trimmed_around_the_breadcrumb(project, ui):
    from System.ui.framework import VerticalList
    items = ["Reboot", "Shut down"]

    def draw():
        VerticalList(ui, "Remote Shell Sessions", items, app_id=9007).draw()

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["\\"])
    em.lists["nd items"] = list(items)
    em.call("list show %s app %s from %s", "Remote Shell Sessions", 9007, 0)
    same(expected, em.canvas.image, "vertical-list-long-title")


def test_paged_list(project, ui):
    from System.ui.framework import PagedList
    items = ["Text Messages", "SMS Settings", "Voice Mailbox Number"]

    def draw():
        PagedList(ui, "Messages", items, root_id=2).draw()

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["\\"])
    em.lists["nd items"] = list(items)
    em.call("page show %s app %s hint %s", "Messages", 2, 1)
    same(expected, em.canvas.image, "paged-list")


def test_paged_list_wraps_a_long_item(project, ui):
    from System.ui.framework import PagedList
    items = ["Voice Mailbox Number And Other Very Long Settings"]

    def draw():
        PagedList(ui, "Messages", items, root_id=2).draw()

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["\\"])
    em.lists["nd items"] = list(items)
    em.call("page show %s app %s hint %s", "Messages", 2, 1)
    same(expected, em.canvas.image, "paged-list-wrapped")


def test_empty_paged_list(project, ui):
    from System.ui.framework import PagedList

    def draw():
        PagedList(ui, "Inbox", [], root_id=2).draw()

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["\\"])
    em.lists["nd items"] = []
    em.call("page show %s app %s hint %s", "Inbox", 2, 1)
    same(expected, em.canvas.image, "paged-list-empty")


def test_app_selector(project, ui):
    from System.ui.framework import AppSelector
    apps = [
        {"name": "Phonebook", "icon": "/NeoDCT/System/apps/PhoneBook/icon.png"},
        {"name": "Messages", "icon": "/NeoDCT/System/apps/Messages/icon.png"},
        {"name": "Clock", "icon": "/NeoDCT/System/apps/Clock/icon.png"},
    ]

    def draw():
        AppSelector("Main Menu", apps, ui).draw()

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["\\"])
    em.lists["nd items"] = [a["name"] for a in apps]
    em.lists["nd icons"] = ["PhoneBook", "Messages", "Clock"]
    em.call("app selector show")
    same(expected, em.canvas.image, "app-selector")


def test_app_selector_without_an_icon(project, ui):
    from System.ui.framework import AppSelector
    apps = [{"name": "Mystery", "icon": None}]

    def draw():
        AppSelector("Main Menu", apps, ui).draw()

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["\\"])
    em.lists["nd items"] = ["Mystery"]
    em.lists["nd icons"] = [""]
    em.call("app selector show")
    same(expected, em.canvas.image, "app-selector-placeholder")


def test_text_input(project, ui):
    from System.ui.framework import TextInput

    def draw():
        field = TextInput(ui, "Add Entry", "Name:", initial_text="Mom")
        from System.ui.framework import SoftKeyBar
        SoftKeyBar(ui).update("OK")
        field.draw(True)

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["enter"])
    em.call("field show %s prompt %s start %s filter %s",
            "Add Entry", "Name:", "Mom", "any")
    assert float(em.variables["nd result"]) == 0
    assert em.variables["nd text"] == "Mom"
    same(expected, em.canvas.image, "text-input")


def test_text_input_typing(project, ui):
    from System.ui.framework import TextInput
    from System.ui.framework import SoftKeyBar

    def draw():
        field = TextInput(ui, "Add Entry", "Name:")
        for key in (35, 18, 38, 38, 24):        # h e l l o
            field.handle_key(key)
        SoftKeyBar(ui).update("OK")
        field.draw(True)

    expected = python_frame(ui, draw)
    # The cursor blinks on a half-second timer, so hold the clock still:
    # this frame is the one the Python draws with the cursor showing.
    em = emulate(project, keys=["h", "e", "l", "l", "o", "enter"], clock=0)
    em.call("field show %s prompt %s start %s filter %s",
            "Add Entry", "Name:", "", "any")
    assert em.variables["nd text"] == "Hello"
    assert float(em.variables["nd result"]) == 0
    same(expected, em.canvas.image, "text-input-typed")


def test_text_input_number_filter_rejects_letters(project, ui):
    em = emulate(project, keys=["1", "a", "2", "enter"])
    em.call("field show %s prompt %s start %s filter %s",
            "Number", "Number:", "", "numbers")
    assert em.variables["nd text"] == "12"


def test_compose(project, ui):
    from System.ui.framework import TextInputLong, SoftKeyBar
    body = "Meet me at the usual place at six, and bring the thing"

    def draw():
        widget = TextInputLong(ui, "Write", initial_text=body)
        widget.draw(True)
        SoftKeyBar(ui).update("Options")

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["enter"])
    em.call("compose show %s softkey %s start %s", "Write", "Options", body)
    same(expected, em.canvas.image, "compose")


def test_message_dialog_short(project, ui):
    from System.ui.framework import MessageDialog

    def draw():
        MessageDialog(ui, "BATTERY LOW!")._draw()

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["enter"])
    em.call("dialog %s title %s icon %s button %s",
            "BATTERY LOW!", "", "warning", "OK")
    assert float(em.variables["nd result"]) == 28
    same(expected, em.canvas.image, "dialog-short")


def test_message_dialog_paragraph(project, ui):
    from System.ui.framework import MessageDialog
    text = ("The update could not be verified. Its signature does not match "
            "the key built into this phone, so it has not been installed.")

    def draw():
        MessageDialog(ui, text, title="Update")._draw()

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["enter"])
    em.call("dialog %s title %s icon %s button %s", text, "Update", "warning", "OK")
    same(expected, em.canvas.image, "dialog-paragraph")


def test_info_screen(project, ui):
    from System.ui.framework import InfoScreen

    def draw():
        try:
            InfoScreen(ui, "Top score", 385).show()
        except uistub.ScriptExhausted:
            pass

    ui.keys.push(28)
    expected = python_frame(ui, draw)
    em = emulate(project, keys=["enter"])
    em.call("info %s value %s button %s", "Top score", 385, "Back")
    same(expected, em.canvas.image, "info-screen")


def test_text_scroller(project, ui):
    from System.ui.framework import TextScroller
    text = ("Guide the snake to the food.\n\n"
            "The snake grows with every bite. Do not run into the wall, and "
            "do not run into yourself.\n\n"
            "Press any number key to steer.")

    def draw():
        TextScroller(ui, text).draw()

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["\\"])
    em.call("read text %s more %s back %s", text, "More", "Back")
    same(expected, em.canvas.image, "text-scroller")


def test_progress_screen(project, ui):
    from System.ui.framework import ProgressScreen

    def draw():
        screen = ProgressScreen(ui, "Backing up your data",
                                header="Update", hint="Do not turn off")
        screen.draw(56, 100)

    expected = python_frame(ui, draw)
    em = emulate(project)
    em.call("progress reset")
    em.call("progress %s of %s step %s header %s hint %s detail %s",
            56, 100, "Backing up your data", "Update", "Do not turn off", "")
    same(expected, em.canvas.image, "progress")


def test_detail_page(project, ui):
    from System.ui.framework import DetailPage
    body = ("Added\n\nThe serial log opens with the NeoDCT name and version, "
            "and every service now has its own colour.\n\nFixed\n\nMobile "
            "data. Each dial attempt was taking a connection slot from the "
            "modem and never giving it back.")

    def draw():
        page = DetailPage(ui, title="NeoDCT 0.4.10a",
                          subtitle="Ready to install", badge="12.4 MB",
                          body=body,
                          image="/NeoDCT/System/ui/resources/img/errorscreen/warning.png",
                          header="Update", softkey_text="Install")
        page.draw()

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["enter"])
    em.call("detail %s sub %s badge %s body %s image %s header %s softkey %s",
            "NeoDCT 0.4.10a", "Ready to install", "12.4 MB", body,
            "warning", "Update", "Install")
    same(expected, em.canvas.image, "detail-page")


def test_detail_page_plain(project, ui):
    from System.ui.framework import DetailPage

    def draw():
        DetailPage(ui, title="No updates", body="You are up to date.",
                   softkey_text="OK").draw()

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["enter"])
    em.call("detail %s sub %s badge %s body %s image %s header %s softkey %s",
            "No updates", "", "", "You are up to date.", "", "", "OK")
    same(expected, em.canvas.image, "detail-page-plain")


def test_level_selector(project, ui):
    from System.ui.framework import LevelSelector, SoftKeyBar

    def draw():
        widget = LevelSelector(ui, current=1, count=9)
        SoftKeyBar(ui).update("OK", present=False)
        widget.draw()

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["enter"])
    em.call("level select current %s of %s", 1, 9)
    assert float(em.variables["nd result"]) == 1
    same(expected, em.canvas.image, "level-selector")


# --- the text engine, against Pillow itself ------------------------------

SAMPLES = [
    "", "A", "Ag", "Hello", "Phonebook", "Remote Shell", "12:00", "1-2",
    "Text Messages", "iiiillll", "WWWWMMMM", "...", "Level 9", "0.4.10a",
    "Do not turn off", "  spaced  out  ", "j_", "?!,.-", "9007-7",
]


def test_measure_matches_pillow(project, ui):
    em = emulate(project)
    fonts = [ui.font_s, ui.font_md, ui.font_n, ui.font_xl]
    for number, font in enumerate(fonts, start=1):
        for text in SAMPLES:
            em.call("ui measure %s font %s", text, number)
            got = (float(em.variables["nd tw"]), float(em.variables["nd th"]))
            assert got == ui.get_text_size(text, font), (text, number)


def test_text_drawing_matches_pillow(project, ui):
    fonts = [ui.font_s, ui.font_md, ui.font_n, ui.font_xl]
    for number, font in enumerate(fonts, start=1):
        expected = python_frame(ui, lambda: [
            ui.draw.text((7, 20 + i * 30), text, font=font, fill="white")
            for i, text in enumerate(SAMPLES[:4])])
        em = emulate(project)
        for i, text in enumerate(SAMPLES[:4]):
            em.call("ui text %s at %s %s font %s colour %s",
                    text, 7, 20 + i * 30, number, "white")
        same(expected, em.canvas.image, "text-font-%d" % number)


def test_black_text_on_a_white_bar(project, ui):
    def draw():
        ui.draw.rectangle((0, 40, 225, 69), fill="white")
        ui.draw.text((10, 45), "Phonebook", font=ui.font_md, fill="black")

    expected = python_frame(ui, draw)
    em = emulate(project)
    em.call("ui fill %s %s to %s %s colour %s", 0, 40, 225, 69, "white")
    em.call("ui text %s at %s %s font %s colour %s", "Phonebook", 10, 45, 2, "black")
    same(expected, em.canvas.image, "inverted-text")


# --- the helpers the widgets are built from ------------------------------

def test_fit_text(project, ui):
    from System.ui.framework import fit_text
    em = emulate(project)
    cases = [("Remote Shell Sessions", 4, 120), ("Short", 4, 200),
             ("Remote Shell", 4, 10), ("", 3, 100), ("Anything", 3, 0),
             ("Voice Mailbox Number", 3, 90)]
    fonts = {1: ui.font_s, 2: ui.font_md, 3: ui.font_n, 4: ui.font_xl}
    for text, font, width in cases:
        em.call("ui fit text %s font %s width %s", text, font, width)
        assert em.variables["nd str"] == fit_text(ui, text, fonts[font], width), \
            (text, font, width)


def test_ellipsize(project, ui):
    from System.ui.framework import _ellipsize
    em = emulate(project)
    fonts = {1: ui.font_s, 3: ui.font_n}
    for text, font, width in [("Backing up your data", 3, 120),
                              ("Backing up your data", 3, 400),
                              ("Backing up your data", 1, 8),
                              ("", 1, 50)]:
        em.call("ui ellipsize %s font %s width %s", text, font, width)
        assert em.variables["nd str"] == _ellipsize(ui, text, fonts[font], width)


def test_wrap_matches_the_framework(project, ui):
    from System.ui.framework import _wrap_lines, TextInputLong
    em = emulate(project)
    texts = [
        "Guide the snake to the food.\n\nThe snake grows with every bite.",
        "One two three four five six seven eight nine ten eleven twelve",
        "Supercalifragilisticexpialidocious and then some more words",
        "", "   ", "a\n\n\nb",
    ]
    for text in texts:
        em.call("ui wrap %s font %s width %s break %s trim %s", text, 1, 220, 0, 1)
        assert list(em.lists["nd lines"]) == _wrap_lines(ui, text, ui.font_s, 220), text

    widget = TextInputLong(ui, "Write")
    for text in texts:
        em.call("ui wrap %s font %s width %s break %s trim %s", text, 1, 220, 1, 0)
        assert list(em.lists["nd lines"]) == widget._wrap_text(text, 220), text


# --- more screens ---------------------------------------------------------

def test_text_scroller_second_page(project, ui):
    from System.ui.framework import TextScroller
    text = " ".join("word%d" % n for n in range(120))

    def draw():
        widget = TextScroller(ui, text)
        widget.page = 1
        widget.draw()

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["enter", "\\"])
    em.call("read text %s more %s back %s", text, "More", "Back")
    same(expected, em.canvas.image, "text-scroller-page-2")


def test_detail_page_scrolled(project, ui):
    from System.ui.framework import DetailPage
    body = "\n\n".join("Paragraph %d, with enough words in it to wrap onto "
                       "a second line of the small font." % n for n in range(6))

    def draw():
        page = DetailPage(ui, title="Changelog", body=body, softkey_text="OK")
        page.offset = page.line_height * 2
        page.draw()

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["down arrow", "down arrow", "enter"])
    em.call("detail %s sub %s badge %s body %s image %s header %s softkey %s",
            "Changelog", "", "", body, "", "", "OK")
    same(expected, em.canvas.image, "detail-page-scrolled")


def test_progress_with_a_detail_reading(project, ui):
    from System.ui.framework import ProgressScreen

    def draw():
        screen = ProgressScreen(ui, "Downloading", header="Update",
                                detail=lambda done, total: "5.6 of 12.4 MB")
        screen.draw(45, 100)

    expected = python_frame(ui, draw)
    em = emulate(project)
    em.call("progress reset")
    em.call("progress %s of %s step %s header %s hint %s detail %s",
            45, 100, "Downloading", "Update", "", "5.6 of 12.4 MB")
    same(expected, em.canvas.image, "progress-detail")


def test_progress_only_repaints_on_a_new_percent(project, ui):
    em = emulate(project)
    em.call("progress reset")
    em.call("progress %s of %s step %s header %s hint %s detail %s",
            45, 100, "Downloading", "", "", "")
    before = em.canvas.image.copy()
    em.canvas.clear()
    em.call("progress %s of %s step %s header %s hint %s detail %s",
            451, 1000, "Downloading", "", "", "")
    assert em.canvas.image.getbbox() is None, "45.1% repainted a 45% screen"
    em.call("progress %s of %s step %s header %s hint %s detail %s",
            46, 100, "Downloading", "", "", "")
    assert em.canvas.image.getbbox() is not None
    assert before.size == em.canvas.image.size


def test_dialog_truncates_a_message_too_long_for_the_screen(project, ui):
    from System.ui.framework import MessageDialog
    text = " ".join("line%d" % n for n in range(120))

    def draw():
        MessageDialog(ui, text)._draw()

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["enter"])
    em.call("dialog %s title %s icon %s button %s", text, "", "warning", "OK")
    same(expected, em.canvas.image, "dialog-truncated")


def test_empty_app_selector(project, ui):
    from System.ui.framework import AppSelector

    def draw():
        AppSelector("Main Menu", [], ui).draw()

    expected = python_frame(ui, draw)
    em = emulate(project, keys=["\\"])
    em.lists["nd items"] = []
    em.lists["nd icons"] = []
    em.call("app selector show")
    same(expected, em.canvas.image, "app-selector-empty")


def test_app_selector_wraps_around(project, ui):
    em = emulate(project, keys=["up arrow", "enter"])
    em.lists["nd items"] = ["One", "Two", "Three"]
    em.lists["nd icons"] = ["", "", ""]
    em.call("app selector show")
    assert float(em.variables["nd result"]) == 2

"""What the update screens must look like, judged on real drawn frames.

These are design tests. They boot the actual UI (real font, real 240x175
canvas) through the headless stub and then measure the pixels: that the
progress bar never has text sitting on top of it, that nothing spills into
the softkey bar, that a page of release notes scrolls instead of throwing
five screens of huge text at you.

Run with NEODCT_UI_SHOTS=/some/dir to also get every screen as a PNG.
"""

import pytest
from PIL import Image

from System.ui.framework import DetailPage, ProgressScreen, _content_bottom

from update_ui_fixtures import BACK, DOWN, ENTER, UP, phone, shot

BLACK = (0, 0, 0)

NOTES = (
    "Added\n"
    "- Browser download manager\n"
    "- Nokia-style progress bars\n"
    "\n"
    "Changed\n"
    "- Music now lives on the SD card\n"
    "\n"
    "Fixed\n"
    "- SMS database sorting bug\n"
    "- Wallpaper picker forgetting the last choice\n"
)


@pytest.fixture(scope="module")
def ui():
    """One booted phone for the whole module: these tests only draw."""
    with phone() as device:
        yield device.ui


def lit_pixels(image, box):
    """Coordinates of everything that is not background inside `box`."""
    x0, y0, x1, y1 = box
    pixels = image.load()
    return [(x, y)
            for y in range(max(0, y0), min(image.height, y1))
            for x in range(max(0, x0), min(image.width, x1))
            if pixels[x, y] != BLACK]


def is_clear(image, box):
    return not lit_pixels(image, box)


def thumbnail(colour=(255, 255, 255), size=64):
    return Image.new("RGB", (size, size), colour)


# --- the progress bar -------------------------------------------------------


def test_no_text_ever_sits_on_top_of_the_progress_bar(ui):
    """The old screen drew "x of y MB" straight through the bar."""
    screen = ProgressScreen(ui, "Installing update")
    screen.draw(45, 100)
    frame = ui.fb.frames[-1]
    shot(frame, "progress-45")

    left, top, right, bottom = screen.bar_box
    gutter = 3
    assert is_clear(frame, (0, top - gutter, ui.W, top)), \
        "something is drawn right up against the top of the bar"
    assert is_clear(frame, (0, bottom + 1, ui.W, bottom + 1 + gutter)), \
        "something is drawn right up against the bottom of the bar"


def test_the_bar_fills_in_proportion_to_the_work_done(ui):
    screen = ProgressScreen(ui, "Installing update")

    screen.draw(50, 100)
    left, top, right, bottom = screen.bar_box
    middle = (top + bottom) // 2
    filled = len(lit_pixels(ui.fb.frames[-1], (left + 2, middle, right - 1, middle + 1)))

    assert abs(filled - (right - left) // 2) < 8


def test_the_bar_is_empty_before_anything_has_been_copied(ui):
    screen = ProgressScreen(ui, "Installing update")

    screen.draw(0, 100)
    left, top, right, bottom = screen.bar_box
    inside = (left + 2, top + 2, right - 1, bottom - 1)

    assert is_clear(ui.fb.frames[-1], inside)


def test_the_bar_is_full_when_the_copy_is_done(ui):
    screen = ProgressScreen(ui, "Installing update")

    screen.draw(100, 100)
    left, top, right, bottom = screen.bar_box
    middle = (top + bottom) // 2
    filled = len(lit_pixels(ui.fb.frames[-1], (left + 2, middle, right - 1, middle + 1)))
    shot(ui.fb.frames[-1], "progress-100")

    assert filled >= (right - left) - 6


def test_the_reading_is_underneath_the_bar_where_it_can_be_read(ui):
    screen = ProgressScreen(ui, "Installing update")

    screen.draw(45, 100)
    frame = ui.fb.frames[-1]

    assert screen.status_box[1] > screen.bar_box[3], "the reading is not below the bar"
    assert lit_pixels(frame, screen.status_box), "no reading was drawn at all"


def test_the_step_label_sits_above_the_bar(ui):
    screen = ProgressScreen(ui, "Backing up your data")

    screen.draw(10, 100)
    frame = ui.fb.frames[-1]

    assert screen.label_box[3] < screen.bar_box[1]
    assert lit_pixels(frame, screen.label_box)


def test_the_step_can_change_without_starting_a_new_screen(ui):
    """Backing up, then copying: one screen, one bar, two labels."""
    screen = ProgressScreen(ui, "Backing up your data")
    screen.draw(50, 100)
    before = ui.fb.frames[-1]

    screen.set_step("Copying update")
    screen.draw(50, 100)
    after = ui.fb.frames[-1]
    shot(after, "progress-step")

    assert lit_pixels(before, screen.label_box) != lit_pixels(after, screen.label_box)


def test_the_progress_screen_leaves_the_softkey_bar_alone(ui):
    """Its own softkey line is the only thing allowed down there."""
    screen = ProgressScreen(ui, "Installing update")

    screen.draw(45, 100)

    bottom = _content_bottom(ui)
    assert screen.bar_box[3] < bottom
    assert screen.status_box[3] <= bottom
    assert screen.hint_box[3] <= bottom


def test_redrawing_the_same_percentage_costs_nothing(ui):
    """The copy loop calls this per 256KB chunk; repainting each time would
    make the update slower than the write itself."""
    screen = ProgressScreen(ui, "Installing update")
    screen.draw(45000, 100000)
    drawn = len(ui.fb.frames)

    screen.draw(45100, 100000)   # more bytes, still 45%

    assert len(ui.fb.frames) == drawn


# --- the update page --------------------------------------------------------


def test_the_picture_from_the_package_is_shown_at_the_top(ui):
    page = DetailPage(ui, title="NeoDCT 0.3.2a", image=thumbnail((255, 0, 0)),
                      subtitle="12.4 MB", body=NOTES, softkey_text="Install")

    page.draw()
    frame = ui.fb.frames[-1]
    shot(frame, "page-thumbnail")

    reds = [xy for xy in lit_pixels(frame, page.viewport)
            if frame.getpixel(xy) == (255, 0, 0)]
    assert len(reds) > 1000, "the thumbnail is missing or tiny"
    assert min(y for _, y in reds) < page.viewport[1] + 24, \
        "the picture should be the first thing on the page"


def test_a_page_with_no_picture_still_reads_top_down(ui):
    page = DetailPage(ui, title="No SD card", subtitle="Nothing to update from",
                      body="Put a FAT32 card in the phone.", softkey_text="Back")

    page.draw()
    shot(ui.fb.frames[-1], "page-nocard")

    assert page.content_height < _content_bottom(ui) or page.scrollable


def test_blank_lines_between_paragraphs_do_not_cost_a_whole_line(ui):
    """The old scroller gave a blank line the same height as a line of text,
    which is what made five screens out of one changelog."""
    tight = DetailPage(ui, title="x", body="One.\nTwo.")
    spaced = DetailPage(ui, title="x", body="One.\n\nTwo.")

    gap = spaced.content_height - tight.content_height

    assert 0 < gap < tight.line_height, \
        "a paragraph break should be a breath, not an empty line"


def test_long_notes_scroll_instead_of_paginating(ui):
    page = DetailPage(ui, title="NeoDCT 0.3.2a", body=NOTES, softkey_text="Install")
    page.draw()
    before = ui.fb.frames[-1]

    page.handle_key(DOWN)
    after = ui.fb.frames[-1]
    shot(after, "page-scrolled")

    assert page.offset > 0
    assert page.offset < page.line_height * 2, "scrolling should be smooth, not by pages"
    assert before.tobytes() != after.tobytes()


def test_scrolling_stops_at_the_end_of_the_notes(ui):
    page = DetailPage(ui, title="NeoDCT 0.3.2a", body=NOTES)
    page.draw()

    for _ in range(200):
        page.handle_key(DOWN)

    assert page.offset == page.max_offset
    assert page.max_offset > 0


def test_scrolling_back_stops_at_the_top(ui):
    page = DetailPage(ui, title="NeoDCT 0.3.2a", body=NOTES)
    page.draw()
    page.handle_key(DOWN)

    for _ in range(50):
        page.handle_key(UP)

    assert page.offset == 0


def test_a_page_that_fits_has_no_scrollbar(ui):
    page = DetailPage(ui, title="Up to date", body="Nothing to install.")

    page.draw()
    frame = ui.fb.frames[-1]

    assert not page.scrollable
    assert is_clear(frame, (ui.W - 8, 0, ui.W, _content_bottom(ui)))


def test_a_page_with_more_to_read_shows_the_scrollbar(ui):
    page = DetailPage(ui, title="NeoDCT 0.3.2a", body=NOTES)

    page.draw()
    frame = ui.fb.frames[-1]

    assert page.scrollable
    assert lit_pixels(frame, (ui.W - 8, 0, ui.W, _content_bottom(ui)))


def test_nothing_spills_out_of_the_page_while_scrolling(ui):
    """Scrolled text must be clipped, not drawn over the header and softkey."""
    page = DetailPage(ui, title="NeoDCT 0.3.2a", body=NOTES, softkey_text="Install")
    page.draw()
    page.handle_key(DOWN)
    frame = ui.fb.frames[-1]

    left, top, right, bottom = page.viewport
    assert is_clear(frame, (0, bottom, ui.W, _content_bottom(ui)))


def test_the_header_names_the_app_on_every_page(ui):
    page = DetailPage(ui, title="NeoDCT 0.3.2a", header="SOFTWARE UPDATE", body="x")

    page.draw()
    frame = ui.fb.frames[-1]

    assert lit_pixels(frame, (0, 0, ui.W, page.viewport[1]))


def test_enter_accepts_the_page_and_back_cancels_it(ui):
    ui.keys.push(ENTER)
    assert DetailPage(ui, title="x", body="y", softkey_text="Install").show() == ENTER

    ui.keys.push(BACK)
    assert DetailPage(ui, title="x", body="y", softkey_text="Install").show() == BACK


def test_a_short_page_is_centred_rather_than_hugging_the_top(ui):
    """Half a screen of black under three words looks like a crash."""
    page = DetailPage(ui, title="Up to date", subtitle="NeoDCT 0.3.1a",
                      body="Your phone has the newest software.",
                      softkey_text="Back")

    page.draw()
    frame = ui.fb.frames[-1]
    shot(frame, "page-uptodate")

    lit = lit_pixels(frame, page.viewport)
    above = min(y for _, y in lit) - page.viewport[1]
    below = page.viewport[3] - max(y for _, y in lit)

    assert not page.scrollable
    assert abs(above - below) <= page.line_height


def test_a_page_that_scrolls_still_starts_at_the_top(ui):
    """Centring a page you have to scroll would just hide the first line."""
    page = DetailPage(ui, title="NeoDCT 0.3.2a", body=NOTES)

    page.draw()
    lit = lit_pixels(ui.fb.frames[-1], page.viewport)

    assert min(y for _, y in lit) - page.viewport[1] < page.line_height


# --- the hero row -----------------------------------------------------------


def test_a_long_step_label_is_not_cut_off_by_the_screen_edge(ui):
    """"Backing up your data" ran off both sides at full size."""
    screen = ProgressScreen(ui, "Backing up your data")

    screen.draw(30, 100)
    frame = ui.fb.frames[-1]
    shot(frame, "progress-long-label")

    drawn = lit_pixels(frame, screen.label_box)
    assert drawn, "no label was drawn"
    assert min(x for x, _ in drawn) >= 4
    assert max(x for x, _ in drawn) <= ui.W - 4


def test_the_details_sit_beside_the_picture_not_under_it(ui):
    """Stacked, the picture and four lines of detail filled the screen on
    their own and pushed the release notes below the fold."""
    page = DetailPage(ui, title="NeoDCT 0.3.2a", subtitle="12.4 MB  27 Jul 2026",
                      badge="Verified", body=NOTES, image=thumbnail((255, 0, 0)),
                      header="SOFTWARE UPDATE", softkey_text="Install")

    page.draw()
    frame = ui.fb.frames[-1]
    shot(frame, "page-hero")

    left, top, right, bottom = page.hero_box
    beside = lit_pixels(frame, (left + 64 + 4, page.viewport[1] + top,
                                ui.W - 8, page.viewport[1] + bottom))
    assert beside, "the details are not next to the picture"


def test_the_release_notes_start_on_the_first_screen(ui):
    """Version, size, whether it is signed, and the first line of what
    changed: all of it before anyone has to scroll."""
    page = DetailPage(ui, title="NeoDCT 0.3.2a", subtitle="12.4 MB  27 Jul 2026",
                      badge="Verified", body=NOTES, image=thumbnail(),
                      header="SOFTWARE UPDATE", softkey_text="Install")

    assert page.body_top + page.line_height <= page.viewport_height


def test_the_badge_is_visible_without_scrolling(ui):
    page = DetailPage(ui, title="NeoDCT 0.3.2a", subtitle="12.4 MB  27 Jul 2026",
                      badge="Not signed", body=NOTES, image=thumbnail(),
                      header="SOFTWARE UPDATE", softkey_text="Install")

    assert page.hero_box[3] <= page.viewport_height


def test_a_title_too_wide_for_the_column_gets_a_smaller_face(ui):
    page = DetailPage(ui, title="NeoDCT 0.3.2a-rc4-luckfox", subtitle="12.4 MB",
                      body=NOTES, image=thumbnail(), softkey_text="Install")

    page.draw()
    frame = ui.fb.frames[-1]
    shot(frame, "page-long-title")

    left, top, right, bottom = page.hero_box
    drawn = lit_pixels(frame, (left + 44, page.viewport[1] + top,
                               ui.W - 8, page.viewport[1] + bottom))
    assert drawn
    assert max(x for x, _ in drawn) <= ui.W - 12


def test_the_bottom_line_of_a_page_is_never_cut_in_half(ui):
    """Half a line of type at the fold looks like a rendering bug; the
    scrollbar is what says there is more."""
    page = DetailPage(ui, title="NeoDCT 0.3.2a", subtitle="12.4 MB",
                      badge="Verified", body=NOTES, image=thumbnail(),
                      header="SOFTWARE UPDATE", softkey_text="Install")

    page.draw()
    frame = ui.fb.frames[-1]
    drawn = lit_pixels(frame, (0, page.viewport[1], ui.W - 8, page.viewport[3]))

    assert page.viewport[3] - max(y for _, y in drawn) >= 3


def test_scrolling_still_shows_whole_lines(ui):
    page = DetailPage(ui, title="NeoDCT 0.3.2a", subtitle="12.4 MB",
                      body=NOTES, image=thumbnail(), header="SOFTWARE UPDATE")
    page.draw()

    page.handle_key(DOWN)
    page.handle_key(DOWN)
    frame = ui.fb.frames[-1]
    shot(frame, "page-scrolled-whole-lines")
    drawn = lit_pixels(frame, (0, page.viewport[1], ui.W - 8, page.viewport[3]))

    assert page.viewport[3] - max(y for _, y in drawn) >= 3


def test_a_long_hint_is_kept_inside_the_screen(ui):
    screen = ProgressScreen(ui, "Copying update",
                            hint="Keep the card in the phone until it restarts")

    screen.draw(30, 100)
    frame = ui.fb.frames[-1]
    drawn = lit_pixels(frame, screen.hint_box)

    assert drawn
    assert min(x for x, _ in drawn) >= 4
    assert max(x for x, _ in drawn) <= ui.W - 4


# --- the plain text scroller ------------------------------------------------
# Not part of the update app any more, but it is where the complaint started:
# every other app's help text still pages through it.


def test_a_paragraph_break_in_the_scroller_is_not_a_whole_empty_line(ui):
    from System.ui.framework import TextScroller

    text = "One.\n\nTwo.\n\nThree.\n\nFour."
    pages, _ = TextScroller(ui, text)._paginate()

    assert len(pages) == 1, "four short paragraphs should not need two screens"


def test_the_scroller_still_breaks_into_pages_when_there_is_too_much(ui):
    from System.ui.framework import TextScroller

    text = "\n".join("Line number %d." % n for n in range(1, 20))
    pages, _ = TextScroller(ui, text)._paginate()

    assert len(pages) > 1

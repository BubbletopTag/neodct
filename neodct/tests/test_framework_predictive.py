"""Predictive text in the two framework text widgets.

Multi-tap makes you spell a word out; predictive makes you press each key
once and guesses. The guess is provisional -- it is in the field, and
underlined, but the next keypress can replace the whole of it -- and these
tests pin that provisional-ness, because it is the part that goes wrong:
a word that cannot be corrected, or one that gets silently overwritten
after the user has moved on, is worse than no prediction at all.

No drawing happens in handle_key, so a FakeUI is enough (host python has
no PIL for the real one).
"""

import pytest

from System.hw import t9_dict
from System.hw import t9_engine
from System.ui import framework
from System.ui.framework import TextInput, TextInputLong

K1, K2, K3, K4, K6, K8, K0 = 2, 3, 4, 5, 7, 9, 11
STAR, HASH, ENTER, BACKSPACE = 42, 43, 28, 14


class FakeUI:
    def __init__(self):
        self.matrix_input = object()      # i2c keypad present -> T9 path
        self.font_n = object()

    def get_text_size(self, text, font):
        return (8 * len(text), 16)


class FakeDict:
    """A handful of words, keyed the way the real dictionary keys them.

    Ranked, not alphabetical: the real file is built commonest-first, and
    the first suggestion for a sequence is what the user actually sees.
    """

    WORDS = ["inn", "good", "home", "gone", "hood", "he", "if", "id"]

    def __init__(self):
        self.asked = []

    def suggest(self, digits, limit=8):
        self.asked.append(digits)
        return [w for w in self.WORDS
                if t9_dict.digits_for(w).startswith(digits)][:limit]


@pytest.fixture
def words(monkeypatch):
    fake = FakeDict()
    monkeypatch.setattr(framework.t9_dict, "shared", lambda: fake)
    return fake


def predictive(widget_cls=TextInput, **kwargs):
    """A widget already switched into predictive mode."""
    args = ("Message",) if widget_cls is TextInputLong else ("Add Entry", "Name:")
    widget = widget_cls(FakeUI(), *args, **kwargs)
    widget.t9.set_mode_index(widget.t9.modes.index(t9_engine.MODE_WORD))
    return widget


# --- one press per letter ---

def test_a_sequence_of_digits_becomes_a_word(words):
    ti = predictive()

    for key in (K4, K6, K6, K3):
        assert ti.handle_key(key) == "typed"

    assert ti.text == "good"
    assert words.asked == ["4", "46", "466", "4663"]


def test_the_word_is_marked_provisional(words):
    """The whole guess is underlined, not just the last keypress."""
    ti = predictive()
    for key in (K4, K6, K6, K3):
        ti.handle_key(key)

    assert ti.pending_word == "good"
    assert ti._pending_len == len("good")


def test_star_offers_the_next_word_the_digits_could_spell(words):
    ti = predictive()
    for key in (K4, K6, K6, K3):
        ti.handle_key(key)

    assert ti.handle_key(STAR) == "typed"
    assert ti.text == "home"
    ti.handle_key(STAR)
    assert ti.text == "gone"


def test_star_wraps_back_to_the_first_word(words):
    ti = predictive()
    ti.handle_key(K4)
    ti.handle_key(K3)                      # "43" -> he / if / id

    for _ in range(3):
        ti.handle_key(STAR)
    assert ti.text == "he"


def test_star_with_nothing_typed_does_nothing(words):
    ti = predictive()

    assert ti.handle_key(STAR) is None
    assert ti.text == ""


def test_a_longer_word_replaces_the_guess_rather_than_extending_it(words):
    """The guess for "466" is a whole word; the next digit must not leave
    its letters behind."""
    ti = predictive()
    for key in (K4, K6, K6):
        ti.handle_key(key)
    assert ti.text == "inn"

    ti.handle_key(K3)
    assert ti.text == "good"               # not "inngood" or "innd"


# --- committing ---

def test_a_space_ends_the_word_and_is_typed(words):
    ti = predictive()
    for key in (K4, K6, K6, K3):
        ti.handle_key(key)

    assert ti.handle_key(K0) == "typed"
    assert ti.text == "good "
    assert ti.pending_word == ""


def test_a_committed_word_is_not_replaced_by_the_next_one(words):
    """Once a word is finished, the digits of the next word must build a
    second word instead of overwriting the first."""
    ti = predictive()
    for key in (K4, K6, K6, K3):
        ti.handle_key(key)
    ti.handle_key(K0)                      # space commits

    for key in (K4, K3):
        ti.handle_key(key)

    assert ti.text == "good he"


def test_punctuation_ends_the_word_too(words):
    ti = predictive()
    for key in (K4, K6, K6, K3):
        ti.handle_key(key)

    ti.handle_key(K1)                      # punctuation cycle starts at "."
    assert ti.text == "good."
    assert ti.pending_word == ""


def test_changing_mode_keeps_the_word(words):
    """# to abc mid-word must leave the guess in the field, not eat it."""
    ti = predictive()
    for key in (K4, K6, K6, K3):
        ti.handle_key(key)

    assert ti.handle_key(HASH) == "mode"
    assert ti.text == "good"
    assert ti.pending_word == ""


# --- correcting ---

def test_clear_takes_a_digit_off_and_guesses_again(words):
    """Not a letter off the guessed word: the user typed digits, so that
    is what an undo has to undo."""
    ti = predictive()
    for key in (K4, K6, K6, K3):
        ti.handle_key(key)
    assert ti.text == "good"

    assert ti.handle_key(BACKSPACE) == "backspace"
    assert ti.text == "inn"                # "466" again, not "goo"


def test_clearing_the_whole_word_empties_the_field(words):
    ti = predictive()
    ti.handle_key(K4)
    ti.handle_key(K3)

    ti.handle_key(BACKSPACE)
    ti.handle_key(BACKSPACE)
    assert ti.text == ""
    assert ti.pending_word == ""


def test_clear_past_the_word_deletes_committed_text(words):
    ti = predictive()
    for key in (K4, K6, K6, K3):
        ti.handle_key(key)
    ti.handle_key(K0)                      # "good "

    ti.handle_key(BACKSPACE)               # nothing provisional left
    assert ti.text == "good"


# --- no dictionary ---

def test_without_a_dictionary_the_digits_are_shown(monkeypatch):
    """A missing or unreadable t9.dict must not turn the keypad into a
    dead pad -- the presses stay visible and the mode key still works."""
    class Empty:
        def suggest(self, digits, limit=8):
            return []
    monkeypatch.setattr(framework.t9_dict, "shared", lambda: Empty())
    ti = predictive()

    for key in (K4, K6, K6):
        assert ti.handle_key(key) == "typed"
    assert ti.text == "466"

    ti.handle_key(BACKSPACE)
    assert ti.text == "46"


# --- the long field ---

def test_the_message_field_predicts_too(words):
    ti = predictive(TextInputLong)

    for key in (K4, K6, K6, K3):
        ti.handle_key(key)

    assert ti.text == "good"
    assert ti.cursor == len("good")


def test_the_message_field_keeps_the_cursor_on_the_guess(words):
    """The cursor has to follow a guess that changes length, or the next
    character lands in the middle of the word."""
    ti = predictive(TextInputLong)
    for key in (K4, K6, K6):
        ti.handle_key(key)
    assert ti.text == "inn" and ti.cursor == 3

    ti.handle_key(K3)                      # "good" is a character longer
    assert ti.cursor == 4

    ti.handle_key(K0)
    assert ti.text == "good "


def test_replacing_the_text_drops_any_pending_guess(words):
    ti = predictive(TextInputLong)
    for key in (K4, K6, K6):
        ti.handle_key(key)

    ti.set_text("sent")
    assert ti.pending_word == ""

    ti.handle_key(K4)
    assert ti.text.startswith("sent")      # the old text was not overwritten
    assert ti.pending_word == "inn"


# --- the indicator ---

def test_predictive_shows_a_pencil_in_front_of_abc():
    """Predictive is the same alphabet as abc, so it is labelled abc with
    a pencil rather than with a fourth word to learn."""
    ui = FakeUI()
    engine = t9_engine.T9Engine()

    engine.set_mode_index(engine.modes.index(t9_engine.MODE_WORD))
    width, label, pencil = framework.t9_indicator_size(ui, engine)
    assert label == "abc"
    assert pencil > 0
    assert width > ui.get_text_size("abc", ui.font_n)[0]


def test_the_other_modes_are_plain_text():
    ui = FakeUI()
    engine = t9_engine.T9Engine()

    for mode in ("abc", "ABC", "123"):
        engine.set_mode_index(engine.modes.index(mode))
        width, label, pencil = framework.t9_indicator_size(ui, engine)
        assert (label, pencil) == (mode, 0)


def test_a_dev_keyboard_shows_no_indicator_at_all():
    """QEMU has a real keyboard, so there is no mode to report."""
    ui = FakeUI()
    ui.matrix_input = None

    assert framework.t9_indicator_size(ui, t9_engine.T9Engine()) is None


# --- the header row is shared between a title and a counter -----------------

def test_a_long_title_does_not_run_under_the_counter():
    """"Remote Shell" against a "9007-7" breadcrumb rendered as
    "Remote Sh<overlap>7-7" on the phone: the counter is right-aligned on
    the same row and nothing stopped the title reaching it."""
    ui = FakeUI()

    fitted = framework.fit_text(ui, "Remote Shell", ui.font_n, 60)

    assert ui.get_text_size(fitted, ui.font_n)[0] <= 60
    assert fitted.endswith("...")


def test_a_title_that_fits_is_left_alone():
    ui = FakeUI()

    assert framework.fit_text(ui, "Remote", ui.font_n, 200) == "Remote"


def test_no_room_means_no_title_rather_than_a_broken_one():
    ui = FakeUI()

    assert framework.fit_text(ui, "Remote", ui.font_n, 0) == ""


def test_the_ellipsis_is_three_dots_not_a_glyph():
    """This font has no U+2026 and draws a blank box for it, which is worse
    than the thing being cut off."""
    ui = FakeUI()

    fitted = framework.fit_text(ui, "Remote Shell", ui.font_n, 60)

    assert "…" not in fitted


def test_the_counter_reports_the_room_it_needs():
    """The title has no other way to know where it must stop."""
    ui = FakeUI()
    header = framework.HeaderWidget(ui, 9990)

    assert header.width(7) > ui.get_text_size("9990-7", ui.font_n)[0]

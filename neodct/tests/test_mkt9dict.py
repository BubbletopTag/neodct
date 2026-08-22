"""Building the T9 dictionary.

mkt9dict.py turns two word lists into the file System/hw/t9_dict.py
searches. The tests that matter are about what survives the budget and in
what order, because a dictionary can be perfectly well-formed and still
useless -- correctly sorted, correctly keyed, and offering "abv" where
"the" should be.

The source lists are hundreds of megabytes and not in the repo, so
--add exists to edit a built dictionary in place. That is a hand edit of
the file the binary search depends on, so it gets its own tests.
"""

import importlib.util
import os

import pytest

from System.hw import t9_dict

TOOLS = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools")


def load_builder():
    spec = importlib.util.spec_from_file_location(
        "mkt9dict", os.path.join(TOOLS, "mkt9dict.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mkt9dict = load_builder()


@pytest.fixture
def lists(tmp_path):
    """A frequency list and a bigger word list, as the builder expects."""
    freq = tmp_path / "freq.txt"
    freq.write_text("the\nof\nand\nto\nphone\nhello\n")
    words = tmp_path / "words.txt"
    words.write_text("the\nabv\nzymurgy\nmended\nphone\n")
    return str(freq), str(words)


# --- house words ---

def test_the_phone_knows_its_own_name(lists):
    freq, words = lists

    lines, _ = mkt9dict.build(freq, words)

    assert "NeoDCT" in lines


def test_the_phones_name_wins_its_own_key(lists):
    """Anything sharing 636328 must come after it: the name of the thing
    you are typing on should not be the second guess."""
    freq, words = lists

    lines, _ = mkt9dict.build(freq, words)

    key = mkt9dict.digits_for("NeoDCT")
    same = [w for w in lines if mkt9dict.digits_for(w) == key]
    assert same[0] == "NeoDCT"


def test_a_house_word_survives_a_budget_too_small_for_it(lists):
    """The budget throws away rare words. It must not throw away a name
    that exists precisely because no word list has it."""
    freq, words = lists

    lines, _ = mkt9dict.build(freq, words, budget=8)

    assert "NeoDCT" in lines


def test_house_words_keep_their_capitals(lists):
    freq, words = lists

    lines, _ = mkt9dict.build(freq, words)

    assert "neodct" not in lines


# --- the file the phone searches ---

def test_the_build_is_sorted_by_key(lists):
    freq, words = lists

    lines, _ = mkt9dict.build(freq, words)

    keys = [mkt9dict.digits_for(w) for w in lines]
    assert keys == sorted(keys)


def test_common_words_outrank_rare_ones_sharing_a_key(lists):
    """The budget is spent by rank before anything is sorted -- spend it
    the other way round and everything keyed 4-9 is cut, taking "the"
    with it."""
    freq, words = lists

    lines, _ = mkt9dict.build(freq, words, budget=24)

    assert "the" in lines
    assert "zymurgy" not in lines


# --- editing a built dictionary ---

def test_add_puts_a_word_where_the_search_will_find_it():
    lines = ["mended", "mendee", "neofascism", "nemean"]
    ordered = sorted(lines, key=mkt9dict.digits_for)

    got, added = mkt9dict.insert(list(ordered), ["NeoDCT"])

    keys = [mkt9dict.digits_for(w) for w in got]
    assert keys == sorted(keys)
    assert [w for w, _, _ in added] == ["NeoDCT"]


def test_add_puts_the_word_first_among_its_own_key():
    lines = sorted(["mended", "neodamode"], key=mkt9dict.digits_for)

    got, _ = mkt9dict.insert(lines, ["NeoDCT"])

    key = mkt9dict.digits_for("NeoDCT")
    same = [w for w in got if mkt9dict.digits_for(w) == key]
    assert same[0] == "NeoDCT"


def test_add_is_idempotent():
    """Running it twice is the obvious mistake, and a duplicated word in a
    sorted file is a duplicated suggestion."""
    lines = ["mended"]

    once, _ = mkt9dict.insert(list(lines), ["NeoDCT"])
    twice, added = mkt9dict.insert(list(once), ["NeoDCT"])

    assert once == twice
    assert added == []


def test_add_refuses_a_word_that_cannot_be_typed():
    with pytest.raises(SystemExit):
        mkt9dict.insert(["mended"], ["Neo-DCT"])


def test_an_added_word_is_findable_through_the_real_search(tmp_path):
    """The insertion point is computed by the builder and consumed by the
    phone's binary search; this is the test that they agree."""
    path = tmp_path / "t9.dict"
    base = sorted(["mended", "mendee", "neofascism", "nemean", "the", "hello"],
                  key=mkt9dict.digits_for)
    lines, _ = mkt9dict.insert(base, ["NeoDCT"])
    path.write_text("\n".join(lines) + "\n")

    found = t9_dict.T9Dictionary(str(path))
    assert found.suggest("636328") == ["NeoDCT"]
    assert "mended" in found.suggest("636333")
    assert "hello" in found.suggest("43556")

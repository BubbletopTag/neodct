"""Predictive word lookup.

The dictionary is built by neodct/tools/mkt9dict.py and searched on disk by
System/hw/t9_dict.py. These tests build tiny dictionaries rather than using
the shipped one, so they assert behaviour rather than the contents of a
word list that will change.
"""

import pytest

from System.hw import t9_dict


def make_dict(tmp_path, words):
    """A dictionary file, sorted the way the builder sorts one."""
    ordered = sorted(words, key=lambda w: (t9_dict.digits_for(w), words.index(w)))
    path = tmp_path / "t9.dict"
    path.write_text("\n".join(ordered) + "\n")
    return t9_dict.T9Dictionary(str(path))


def test_digits_match_the_keypad():
    assert t9_dict.digits_for("hello") == "43556"
    assert t9_dict.digits_for("the") == "843"


def test_a_word_with_no_key_is_rejected():
    """Punctuation and digits have no letter key, so they cannot be typed
    as a word and must not be searched for."""
    assert t9_dict.digits_for("it's") is None
    assert t9_dict.digits_for("x2") is None


def test_an_exact_sequence_finds_its_word(tmp_path):
    d = make_dict(tmp_path, ["hello", "the", "phone"])

    assert "hello" in d.suggest("43556")


def test_a_partial_sequence_suggests_while_you_type(tmp_path):
    """Pressing 4 3 5 should already offer hello, before the word is done."""
    d = make_dict(tmp_path, ["hello", "help", "the"])

    got = d.suggest("435")

    assert "hello" in got and "help" in got
    assert "the" not in got


def test_order_follows_the_file_so_the_likeliest_comes_first(tmp_path):
    """228 is cat and bat and abt. Whichever the builder put first is the
    one the phone offers first -- that ordering is the whole feature."""
    d = make_dict(tmp_path, ["cat", "bat", "abt"])

    assert d.suggest("228")[0] == "cat"


def test_a_sequence_with_nothing_behind_it_returns_nothing(tmp_path):
    d = make_dict(tmp_path, ["hello", "the"])

    assert d.suggest("2222222") == []


def test_a_one_digit_prefix_is_not_worth_answering(tmp_path):
    """One digit matches thousands of words; multi-tap is the better answer
    at that point, so the dictionary declines rather than guessing."""
    d = make_dict(tmp_path, ["hello", "help", "the"])

    assert d.suggest("4") == []


def test_non_digits_are_refused(tmp_path):
    d = make_dict(tmp_path, ["hello"])

    assert d.suggest("abc") == []
    assert d.suggest("40x") == []


def test_the_limit_is_respected(tmp_path):
    d = make_dict(tmp_path, ["cat", "bat", "abt", "act", "abu"])

    assert len(d.suggest("228", limit=2)) == 2


def test_a_missing_dictionary_is_not_an_error(tmp_path):
    """No dictionary means fall back to multi-tap, which is what the phone
    did before this existed -- not a crash on the text input screen."""
    d = t9_dict.T9Dictionary(str(tmp_path / "absent.dict"))

    assert not d.available
    assert d.suggest("43556") == []


def test_the_first_and_last_words_are_both_findable(tmp_path):
    """Binary search boundaries: a word at either end of the file is the
    easiest thing to lose and the hardest to notice."""
    words = ["cat", "bat", "abt", "the", "zoo", "you", "was"]
    d = make_dict(tmp_path, words)

    for word in words:
        digits = t9_dict.digits_for(word)
        assert word in d.suggest(digits), "%s (%s) not found" % (word, digits)


def test_every_word_in_a_larger_dictionary_is_findable(tmp_path):
    """Same boundary worry, with enough entries that the search actually
    has to work rather than landing on the answer by luck."""
    words = ["%s%s%s" % (a, b, c)
             for a in "abcdefgh" for b in "aeiou" for c in "dlnrst"]
    d = make_dict(tmp_path, words)

    for word in words:
        assert word in d.suggest(t9_dict.digits_for(word), limit=64), word


# --- words that keep their capitals ---

def test_a_capitalised_word_keys_like_any_other():
    """You press the same keys for NeoDCT as for neodct, so the key has to
    ignore case -- otherwise the word is unsearchable and, worse, the
    binary search walks straight past it while probing."""
    assert t9_dict.digits_for("NeoDCT") == t9_dict.digits_for("neodct")
    assert t9_dict.digits_for("NeoDCT") == "636328"


def test_a_capitalised_word_is_offered_with_its_capitals(tmp_path):
    """The point of storing the capitals is getting them back."""
    d = make_dict(tmp_path, ["NeoDCT", "mended", "phone"])

    assert d.suggest("636328") == ["NeoDCT"]


def test_capitals_do_not_disturb_the_words_around_them(tmp_path):
    """A mixed-case entry sits in the sorted file like any other word, so
    its neighbours must still be findable across it."""
    d = make_dict(tmp_path, ["NeoDCT", "mended", "mendee", "neofascism"])

    assert "mended" in d.suggest("636333")
    assert "neofascism" in d.suggest("6363272476")


# --- the shipped dictionary ---

def test_the_shipped_dictionary_knows_the_phone_it_runs_on():
    """NeoDCT is added by mkt9dict --add and must survive in the file that
    actually ships, not only in the builder's word list."""
    import os
    shipped = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "overlay", "NeoDCT", "System", "ui", "resources", "t9.dict")
    if not os.path.exists(shipped):
        pytest.skip("shipped dictionary not present")

    d = t9_dict.T9Dictionary(shipped)
    assert d.suggest("636328")[0] == "NeoDCT"


def test_the_shipped_dictionary_is_sorted_by_key():
    """The binary search is only correct on a sorted file, and --add is a
    hand edit of one -- so check the invariant it depends on."""
    import os
    shipped = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "overlay", "NeoDCT", "System", "ui", "resources", "t9.dict")
    if not os.path.exists(shipped):
        pytest.skip("shipped dictionary not present")

    previous = ""
    with open(shipped) as handle:
        for number, line in enumerate(handle, 1):
            key = t9_dict.digits_for(line.strip())
            assert key is not None, "line %d is not typeable: %r" % (number, line)
            assert key >= previous, "line %d is out of order: %r" % (number, line)
            previous = key

"""Tests for System.hw.t9_engine -- the shared multi-tap ("T9") engine.

The engine receives NeoDCT evdev keycodes (2-11 digits, 42 star, 43 hash)
and returns edit operations:
  ("append", ch)   add ch after the current text
  ("replace", ch)  replace the last emitted char (multi-tap cycling)
  ("mode", label)  mode changed; label is "abc" / "ABC" / "123"
  None             key not handled by the engine
"""

from System.hw.t9_engine import T9Engine, char_allowed

# NeoDCT evdev codes for the keypad
K1, K2, K3, K4, K5 = 2, 3, 4, 5, 6
K6, K7, K8, K9, K0 = 7, 8, 9, 10, 11
STAR, HASH = 42, 43
UP = 103  # a nav key the engine must not consume


class FakeClock:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t

    def advance(self, dt):
        self.t += dt


def make(input_filter="any", timeout=1.0):
    clock = FakeClock()
    return T9Engine(input_filter=input_filter, timeout=timeout, clock=clock), clock


# --- basic multi-tap cycling (abc mode) ---

def test_default_mode_is_abc():
    eng, _ = make()
    assert eng.mode == "abc"


def test_first_press_appends_first_letter():
    eng, _ = make()
    assert eng.press(K2) == ("append", "a")


def test_second_press_same_key_cycles():
    eng, _ = make()
    eng.press(K2)
    assert eng.press(K2) == ("replace", "b")
    assert eng.press(K2) == ("replace", "c")


def test_cycle_includes_digit_and_wraps():
    eng, _ = make()
    eng.press(K2)                              # a
    eng.press(K2)                              # b
    eng.press(K2)                              # c
    assert eng.press(K2) == ("replace", "2")   # digit at end of cycle
    assert eng.press(K2) == ("replace", "a")   # wrap


def test_different_key_commits_and_appends():
    eng, _ = make()
    assert eng.press(K2) == ("append", "a")
    assert eng.press(K3) == ("append", "d")


def test_press_after_timeout_starts_new_letter():
    eng, clock = make(timeout=1.0)
    eng.press(K2)
    clock.advance(1.5)
    assert eng.press(K2) == ("append", "a")


def test_press_within_timeout_cycles():
    eng, clock = make(timeout=1.0)
    eng.press(K2)
    clock.advance(0.5)
    assert eng.press(K2) == ("replace", "b")


def test_zero_key_is_space_then_zero():
    eng, _ = make()
    assert eng.press(K0) == ("append", " ")
    assert eng.press(K0) == ("replace", "0")


def test_one_key_cycles_punctuation():
    eng, _ = make()
    assert eng.press(K1) == ("append", ".")
    assert eng.press(K1) == ("replace", ",")


def test_star_not_consumed_in_abc_mode():
    eng, _ = make()
    assert eng.press(STAR) is None


def test_nav_key_returns_none_and_commits_pending():
    eng, _ = make()
    eng.press(K2)
    assert eng.press(UP) is None
    # pending cycle was committed: next press starts a fresh letter
    assert eng.press(K2) == ("append", "a")


def test_reset_clears_pending_cycle():
    eng, _ = make()
    eng.press(K2)
    eng.reset()
    assert eng.press(K2) == ("append", "a")


# --- mode cycling (# key) ---

def test_hash_cycles_modes_any_filter():
    """Typing starts in abc -- what every field has always done -- and #
    walks all the way round, predictive included, back to it."""
    eng, _ = make()
    assert eng.mode == "abc"
    assert eng.press(HASH) == ("mode", "ABC")
    assert eng.press(HASH) == ("mode", "123")
    assert eng.press(HASH) == ("mode", "word")
    assert eng.press(HASH) == ("mode", "abc")


def test_upper_mode_appends_uppercase():
    eng, _ = make()
    eng.press(HASH)  # -> ABC
    assert eng.press(K2) == ("append", "A")
    assert eng.press(K2) == ("replace", "B")


def test_mode_change_commits_pending_cycle():
    eng, _ = make()
    eng.press(K2)          # pending "a"
    eng.press(HASH)        # -> ABC
    assert eng.press(K2) == ("append", "A")


def test_123_mode_appends_digits_without_cycling():
    eng, _ = make()
    eng.press(HASH)  # ABC
    eng.press(HASH)  # 123
    assert eng.press(K2) == ("append", "2")
    assert eng.press(K2) == ("append", "2")


def test_123_mode_star_is_literal():
    eng, _ = make()
    eng.press(HASH)
    eng.press(HASH)  # 123
    assert eng.press(STAR) == ("append", "*")


# --- letters-only filter ---

def test_letters_filter_has_no_123_mode():
    """A letters-only field keeps predictive -- it is the same alphabet --
    but must never offer the digit mode."""
    eng, _ = make("letters")
    assert eng.press(HASH) == ("mode", "ABC")
    assert eng.press(HASH) == ("mode", "word")
    assert eng.press(HASH) == ("mode", "abc")
    assert "123" not in eng.modes


def test_letters_filter_cycle_has_no_digit():
    eng, _ = make("letters")
    eng.press(K2)                              # a
    eng.press(K2)                              # b
    eng.press(K2)                              # c
    assert eng.press(K2) == ("replace", "a")   # wraps, never "2"


def test_letters_filter_zero_is_space():
    eng, _ = make("letters")
    assert eng.press(K0) == ("append", " ")


# --- numbers-only filter ---

def test_numbers_filter_digits_are_literal():
    eng, _ = make("numbers")
    assert eng.mode == "123"
    assert eng.press(K2) == ("append", "2")
    assert eng.press(K2) == ("append", "2")


def test_numbers_filter_star_and_hash_are_literal():
    eng, _ = make("numbers")
    assert eng.press(STAR) == ("append", "*")
    assert eng.press(HASH) == ("append", "#")
    assert eng.mode == "123"  # hash must NOT switch modes


# --- char_allowed (shared filter check for the dev-keyboard path) ---

def test_char_allowed_any():
    assert char_allowed("a", "any")
    assert char_allowed("5", "any")


def test_char_allowed_letters_rejects_digits():
    assert char_allowed("a", "letters")
    assert char_allowed(" ", "letters")
    assert not char_allowed("5", "letters")


def test_char_allowed_numbers():
    assert char_allowed("5", "numbers")
    assert char_allowed("*", "numbers")
    assert char_allowed("#", "numbers")
    assert char_allowed("+", "numbers")
    assert not char_allowed("a", "numbers")
    assert not char_allowed(" ", "numbers")

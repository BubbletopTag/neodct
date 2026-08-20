"""
t9_engine.py -- shared multi-tap ("T9 style") text entry engine for NeoDCT.

Pure logic, no I/O: both the UI framework text widgets and the LinuxShell
uinput bridge feed it NeoDCT evdev keycodes (2-11 digits, 42 star, 43 hash)
and apply the edit operations it returns:

  ("append", ch)   add ch after the current text
  ("replace", ch)  replace the last emitted char (multi-tap cycling)
  ("mode", label)  mode changed; label is "abc" / "ABC" / "123"
  None             key not handled here (nav keys, star in letter modes)

Cycling needs no timers: a press of the same key within `timeout` seconds
of the previous press advances the cycle (replace), anything else commits
the pending letter implicitly and starts fresh.
"""

import time

MODE_ABC = "abc"
MODE_UPPER = "ABC"
MODE_123 = "123"

FILTER_ANY = "any"
FILTER_LETTERS = "letters"
FILTER_NUMBERS = "numbers"

KEY_STAR = 42
KEY_HASH = 43

# NeoDCT evdev code -> keypad digit label
CODE_TO_DIGIT = {
    2: "1", 3: "2", 4: "3", 5: "4", 6: "5",
    7: "6", 8: "7", 9: "8", 10: "9", 11: "0",
}

# Nokia-style letter groups; the digit itself is appended to the cycle
# at runtime (FILTER_ANY only).
LETTER_CYCLES = {
    "2": "abc", "3": "def", "4": "ghi", "5": "jkl",
    "6": "mno", "7": "pqrs", "8": "tuv", "9": "wxyz",
}

# Key 1 punctuation cycle ('1' included so digits stay reachable in abc
# mode; extras past ':' make the LinuxShell usable).
PUNCT_CYCLE = ".,?!'\"1-()@/:_;+#*=<>"
PUNCT_CYCLE_LETTERS = "".join(c for c in PUNCT_CYCLE if not c.isdigit())

_MODES_BY_FILTER = {
    FILTER_ANY: (MODE_ABC, MODE_UPPER, MODE_123),
    FILTER_LETTERS: (MODE_ABC, MODE_UPPER),
    FILTER_NUMBERS: (MODE_123,),
}

# What the dev-keyboard (QWERTY) path may type per filter; mirrors what
# the multi-tap cycles can produce.
_NUMBERS_CHARS = "0123456789*#+"


def char_allowed(char, input_filter):
    """True when `char` is legal for a text field with `input_filter`."""
    if input_filter == FILTER_NUMBERS:
        return char in _NUMBERS_CHARS
    if input_filter == FILTER_LETTERS:
        return not char.isdigit()
    return True


class T9Engine:
    def __init__(self, input_filter=FILTER_ANY, timeout=1.0, clock=None):
        if input_filter not in _MODES_BY_FILTER:
            raise ValueError(f"unknown input_filter: {input_filter!r}")
        self.input_filter = input_filter
        self.timeout = float(timeout)
        self._clock = clock or time.monotonic
        self._modes = _MODES_BY_FILTER[input_filter]
        self._mode_idx = 0
        self._pending_digit = None
        self._pending_idx = 0
        self._last_press = 0.0

    @property
    def mode(self):
        return self._modes[self._mode_idx]

    @property
    def modes(self):
        """The mode cycle for this filter, in the order # walks them."""
        return self._modes

    def set_mode_index(self, index):
        """Jump straight to one mode. Used by callers that fold their own
        state into the # cycle (the browser bridge adds a cursor mode)."""
        self._mode_idx = index % len(self._modes)
        self.reset()
        return self.mode

    def reset(self):
        """Commit any pending multi-tap cycle (e.g. after backspace)."""
        self._pending_digit = None

    def press(self, code):
        if code == KEY_HASH:
            self.reset()
            if len(self._modes) > 1:
                self._mode_idx = (self._mode_idx + 1) % len(self._modes)
                return ("mode", self.mode)
            return ("append", "#")  # numbers filter: literal

        if code == KEY_STAR:
            self.reset()
            if self.mode == MODE_123:
                return ("append", "*")
            return None

        digit = CODE_TO_DIGIT.get(code)
        if digit is None:
            self.reset()
            return None

        if self.mode == MODE_123:
            self.reset()
            return ("append", digit)

        cycle = self._cycle_for(digit)
        now = self._clock()
        if (self._pending_digit == digit
                and (now - self._last_press) <= self.timeout):
            self._pending_idx = (self._pending_idx + 1) % len(cycle)
            self._last_press = now
            return ("replace", cycle[self._pending_idx])

        self._pending_digit = digit
        self._pending_idx = 0
        self._last_press = now
        return ("append", cycle[0])

    def _cycle_for(self, digit):
        if digit == "0":
            return [" ", "0"] if self.input_filter == FILTER_ANY else [" "]
        if digit == "1":
            return list(PUNCT_CYCLE if self.input_filter == FILTER_ANY
                        else PUNCT_CYCLE_LETTERS)
        chars = list(LETTER_CYCLES[digit])
        if self.mode == MODE_UPPER:
            chars = [c.upper() for c in chars]
        if self.input_filter == FILTER_ANY:
            chars.append(digit)
        return chars

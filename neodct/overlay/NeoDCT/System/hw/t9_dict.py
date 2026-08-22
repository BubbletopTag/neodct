"""Predictive word lookup for T9 typing.

The dictionary is half a megabyte of words sorted by the digit sequence
that types them (built by neodct/tools/mkt9dict.py). This never loads it.
Half a megabyte of text becomes a couple of megabytes once it is Python
strings in a dict, and on a 64MB phone that is real memory taken from the
browser for the sake of a lookup that a binary search does in about
seventeen seeks.

So: seek to the middle, read the line you landed in, compute its digit
sequence, compare, halve. The file holds no digits precisely because they
are recomputable, and recomputing one is cheaper than storing 76,000.

Lookup is by prefix, because that is how typing works -- press 4, 3 and
you want "hello" offered before you have finished the word. Results come
back in file order, which the builder already sorted by frequency within
each sequence, so the first suggestion is the likeliest one.
"""

import os

DICT_PATH = "/NeoDCT/System/ui/resources/t9.dict"

# Same mapping as t9_engine.LETTER_CYCLES, inverted. Kept here rather than
# imported so a broken dictionary cannot take the multi-tap engine with it.
_LETTER_TO_DIGIT = {}
for _digit, _letters in (("2", "abc"), ("3", "def"), ("4", "ghi"),
                         ("5", "jkl"), ("6", "mno"), ("7", "pqrs"),
                         ("8", "tuv"), ("9", "wxyz")):
    for _letter in _letters:
        _LETTER_TO_DIGIT[_letter] = _digit

MAX_SUGGESTIONS = 8
# A prefix shorter than this matches thousands of words and suggests
# nothing useful; the multi-tap letters are a better answer at that point.
MIN_PREFIX = 2


def digits_for(word):
    """The key sequence for a word, or None if it contains anything else."""
    out = []
    for char in word:
        digit = _LETTER_TO_DIGIT.get(char)
        if digit is None:
            return None
        out.append(digit)
    return "".join(out)


class T9Dictionary:
    """Prefix lookup over the on-disk word list.

    Holds an open file handle and nothing else. Safe to keep for the life
    of the UI: one descriptor, no cache, no resident copy.
    """

    def __init__(self, path=DICT_PATH):
        self.path = path
        self._handle = None
        self._size = 0
        try:
            self._handle = open(path, "rb")
            self._size = os.fstat(self._handle.fileno()).st_size
        except OSError:
            # No dictionary is not an error: the phone falls back to
            # multi-tap, which is what it did before this existed.
            self._handle = None

    @property
    def available(self):
        return self._handle is not None and self._size > 0

    def close(self):
        if self._handle is not None:
            try:
                self._handle.close()
            finally:
                self._handle = None

    def _line_at(self, offset):
        """(start, word) for the line containing offset.

        Seeks back to the newline before offset, so a probe landing in the
        middle of a word still reads that whole word.
        """
        handle = self._handle
        # Walk back to the start of this line. Words are short, so this is
        # a handful of bytes, not a scan.
        back = max(0, offset - 64)
        handle.seek(back)
        chunk = handle.read(offset - back + 1) if offset > back else b""
        cut = chunk.rfind(b"\n")
        start = back + cut + 1 if cut >= 0 else back
        handle.seek(start)
        line = handle.readline()
        return start, line.strip().decode("ascii", "ignore")

    def suggest(self, digits, limit=MAX_SUGGESTIONS):
        """Words whose key starts with digits, likeliest first."""
        if not self.available or not digits or len(digits) < MIN_PREFIX:
            return []
        if not all(c in "23456789" for c in digits):
            return []

        # Binary search for the first line whose key is >= digits.
        low, high = 0, self._size
        while low < high:
            mid = (low + high) // 2
            start, word = self._line_at(mid)
            if start <= low and high - low <= 1:
                break
            key = digits_for(word) or ""
            if key < digits:
                low = start + len(word) + 1
            else:
                high = start

        results = []
        self._handle.seek(low)
        # The first line may be partial if low landed mid-word; _line_at
        # above always leaves low at a line start, so read straight on.
        while len(results) < limit:
            raw = self._handle.readline()
            if not raw:
                break
            word = raw.strip().decode("ascii", "ignore")
            if not word:
                continue
            key = digits_for(word)
            if key is None:
                continue
            if not key.startswith(digits):
                # Sorted file: the first miss after the run means the run
                # is over. Nothing further can match.
                if key > digits:
                    break
                continue
            results.append(word)
        return results


_shared = None


def shared():
    """One dictionary for the whole UI. Opened on first use."""
    global _shared
    if _shared is None:
        _shared = T9Dictionary()
    return _shared

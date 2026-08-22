#!/usr/bin/env python3
"""Build the T9 predictive dictionary.

    mkt9dict.py --freq FREQ --words WORDS --out DICT [--budget BYTES]
    mkt9dict.py --add WORD [--add WORD ...] --out DICT

The second form edits a dictionary that already exists, which is how a
word gets added without the source lists -- they are a few hundred MB of
downloads and are not in the repo, so a rebuild from scratch is not
something you can do on a whim. --add keeps the file sorted and puts the
new word first among the words sharing its key.

The output is deliberately dull: one lowercase word per line, sorted by the
digit sequence you would press to type it, and within a sequence by how
common the word is. Nothing else -- no digits, no counts, no index.

That is the whole trick. The digit sequence is a pure function of the word
("cat" is always 228), so storing it would be storing the same information
twice. The phone recomputes it while binary-searching, which costs a few
multiplications per probe and saves roughly a third of the file. On a 64MB
phone the file is never loaded: System/hw/t9_dict.py seeks around it.

Ordering within a sequence matters more than coverage. 228 is "cat", "bat",
"act" and a dozen rarer things; a predictive keyboard that offers "abt"
before "cat" is worse than no predictive keyboard, so frequency rank
decides the order, and the budget is spent on the commonest words before
anything is sorted -- see the comment on that in build().
"""

import argparse
import os
import sys

# Same mapping as System/hw/t9_engine.py LETTER_CYCLES, inverted.
LETTER_TO_DIGIT = {}
for digit, letters in (("2", "abc"), ("3", "def"), ("4", "ghi"), ("5", "jkl"),
                       ("6", "mno"), ("7", "pqrs"), ("8", "tuv"),
                       ("9", "wxyz")):
    for letter in letters:
        LETTER_TO_DIGIT[letter] = digit

MIN_LEN = 2
MAX_LEN = 12
DEFAULT_BUDGET = 512 * 1024

# Words this phone knows because it is this phone. They keep their capitals
# and outrank every word from the frequency list, so the name of the thing
# you are typing on is never the second guess. Exempt from MIN_LEN/MAX_LEN
# and from the budget -- a handful of names cannot overrun a 4MB file, and
# silently dropping one would be worse than the bytes are worth.
HOUSE_WORDS = (
    "NeoDCT",
)


def digits_for(word):
    """The key sequence for a word, or None if it is not typeable.

    Case-insensitive: house words keep their capitals (see HOUSE_WORDS)
    and must still key and sort like any other word. System/hw/t9_dict.py
    computes the key the same way while searching.
    """
    out = []
    for char in word:
        digit = LETTER_TO_DIGIT.get(char.lower())
        if digit is None:
            return None
        out.append(digit)
    return "".join(out)


def read_words(path, limit=None):
    words = []
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as handle:
            for line in handle:
                word = line.strip().lower()
                if word:
                    words.append(word)
                if limit and len(words) >= limit:
                    break
    except OSError as exc:
        sys.exit("mkt9dict: cannot read %s: %s" % (path, exc))
    return words


def build(freq_path, words_path, budget=DEFAULT_BUDGET):
    """Return the lines to write, and a short report."""
    rank = {}

    # House words before anything else, so they hold rank 0 upwards and
    # win their digit sequence outright.
    for position, word in enumerate(HOUSE_WORDS):
        if digits_for(word):
            rank[word] = position - len(HOUSE_WORDS)

    # The frequency list comes first and keeps its order: these are the
    # words people actually type, and they must win their digit sequence.
    for position, word in enumerate(read_words(freq_path)):
        if MIN_LEN <= len(word) <= MAX_LEN and word not in rank:
            if digits_for(word):
                rank[word] = position

    # Everything else fills the tail. No frequency information exists for
    # these, so they sort after every ranked word and among themselves by
    # length -- a short unknown word is likelier than a long one.
    filler = []
    if words_path:
        base = len(rank)
        for word in read_words(words_path):
            if word in rank:
                continue
            if not (MIN_LEN <= len(word) <= MAX_LEN):
                continue
            if not digits_for(word):
                continue
            filler.append(word)
        filler.sort(key=lambda w: (len(w), w))
        for offset, word in enumerate(filler):
            rank[word] = base + offset

    # Choose what to keep BEFORE sorting, and choose by rank.
    #
    # Doing it the other way round looks reasonable and is useless: sorted
    # by digit sequence, the budget runs out somewhere in the 3s and every
    # word whose sequence starts 4-9 is cut. That drops "the" (843),
    # "hello" (43556) and "phone" (74663) while keeping "abv", producing a
    # dictionary that is correctly sorted and cannot type a sentence.
    by_rank = sorted(rank.items(), key=lambda kv: kv[1])
    keep, used, dropped = [], 0, 0
    house = set(HOUSE_WORDS)
    for word, position in by_rank:
        cost = len(word) + 1
        if used + cost > budget and word not in house:
            dropped += 1
            continue
        keep.append((word, position))
        used += cost

    # Only now sort into the order the phone searches: by digit sequence,
    # and within a sequence by how common the word is.
    lines = [w for w, _ in sorted(keep, key=lambda kv: (digits_for(kv[0]), kv[1]))]
    kept = len(lines)
    report = {"kept": kept, "dropped": dropped, "bytes": used,
              "ranked": min(len(rank), len(read_words(freq_path)))}
    return lines, report


def insert(lines, words):
    """Put `words` into an already-sorted dictionary. Returns (lines, added).

    Each word goes in front of the first line whose key is not smaller
    than its own, which both keeps the file sorted for the binary search
    and makes the new word the first suggestion for its exact sequence.
    """
    added = []
    for word in words:
        key = digits_for(word)
        if key is None:
            sys.exit("mkt9dict: %r cannot be typed on a keypad" % word)
        if word in lines:
            continue
        spot = len(lines)
        for index, existing in enumerate(lines):
            other = digits_for(existing)
            if other is not None and other >= key:
                spot = index
                break
        lines.insert(spot, word)
        added.append((word, key, spot))
    return lines, added


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--freq",
                        help="frequency-ordered word list (most common first)")
    parser.add_argument("--words", help="larger word list, for coverage")
    parser.add_argument("--out", required=True)
    parser.add_argument("--budget", type=int, default=DEFAULT_BUDGET)
    parser.add_argument("--add", action="append", metavar="WORD",
                        help="insert WORD into the existing --out dictionary")
    args = parser.parse_args(argv)

    if args.add:
        with open(args.out, "r", encoding="utf-8") as handle:
            lines = handle.read().split("\n")
        lines = [w for w in lines if w]
        lines, added = insert(lines, args.add)
        # Write beside the original and rename, so an interrupted run
        # cannot leave a half-written dictionary the phone would search.
        tmp = args.out + ".tmp"
        with open(tmp, "w", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")
        os.replace(tmp, args.out)
        for word, key, spot in added:
            print("mkt9dict: added %s (keys %s) at line %d" % (word, key, spot + 1))
        if not added:
            print("mkt9dict: nothing to add; already present")
        print("mkt9dict: %s (%d words, %.1f KiB)"
              % (args.out, len(lines), os.path.getsize(args.out) / 1024.0))
        return 0

    if not args.freq:
        parser.error("--freq is required unless you are using --add")

    lines, report = build(args.freq, args.words, args.budget)
    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")

    size = os.path.getsize(args.out)
    print("mkt9dict: %s (%d words, %.1f KiB of %.0f KiB budget, %d dropped)"
          % (args.out, report["kept"], size / 1024.0,
             args.budget / 1024.0, report["dropped"]))
    if size > args.budget:
        sys.exit("mkt9dict: over budget")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Build the T9 predictive dictionary.

    mkt9dict.py --freq FREQ --words WORDS --out DICT [--budget BYTES]

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


def digits_for(word):
    """The key sequence for a word, or None if it is not typeable."""
    out = []
    for char in word:
        digit = LETTER_TO_DIGIT.get(char)
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
    for word, position in by_rank:
        cost = len(word) + 1
        if used + cost > budget:
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


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--freq", required=True,
                        help="frequency-ordered word list (most common first)")
    parser.add_argument("--words", help="larger word list, for coverage")
    parser.add_argument("--out", required=True)
    parser.add_argument("--budget", type=int, default=DEFAULT_BUDGET)
    args = parser.parse_args(argv)

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

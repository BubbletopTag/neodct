#!/usr/bin/env python3
"""mkbible.py -- build a .ndb scripture pack for apps/Bible.

The input is a verse-per-line text file, the format eBible.org publishes as
"<translation>_vpl.txt" and the one most study programs can export:

    GEN 1:1 In the beginning, God created the heavens and the earth.
    GEN 1:2 The earth was formless and empty. ...

Usage:

    tools/mkbible.py eng-web_vpl.txt web.ndb --name WEB
    tools/mkbible.py --verify web.ndb

WHY THIS IS A TOOL AND NOT A COMMITTED BLOB
-------------------------------------------

The pack that ships is the World English Bible, which is public domain. A
translation that is not -- and most modern ones are not -- cannot be checked
in here, but somebody who already owns a copy can point this at their own
export and drop the result on /NeoDCT/User. That is the only reason the reader
takes a file path rather than a compiled-in table.

WHAT IT DOES TO THE TEXT
------------------------

Two transformations, both because of the panel and neither optional:

1. Non-ASCII is transliterated. font.ttf is a 16 KB face that covers printable
   ASCII and nothing else; nd_font.c caches 64 codepoints above it so a stray
   character renders rather than crashing, but it renders as a blank box of
   advance width. The WEB text uses exactly five non-ASCII characters -- the
   four curly quotes and an em dash -- so the table below is complete for it
   and merely best-effort for anything else. --strict refuses instead.

2. Verse numbers are made contiguous. The reader indexes verse n as line n-1,
   which is arithmetic rather than a search; a source that skips a verse
   number (some do, where a manuscript tradition differs) gets an empty line
   in its place so the arithmetic stays true.

FORMAT
------

Mirrors apps/Bible/bible.h, which is the normative description. Little-endian
throughout; both targets are LE but a pack is a file people copy around.
"""

import argparse
import collections
import re
import struct
import sys
import unicodedata
import zlib

MAGIC = b"NDBIBLE\x1a"
VERSION = 1
HDR_SZ = 80
BOOK_SZ = 16
CHAP_SZ = 12
NAME_MAX = 16
F_DEFLATE = 0x0001
F_DICT = 0x0002
DICT_MAX = 32768          # zlib's window; more preset history is unreachable
OT, NT, APOC = 0, 1, 2

# Raw chapters larger than this are refused rather than allocated on the
# phone; bible.h carries the same ceiling.
RAW_MAX = 256 * 1024

VERSE_RE = re.compile(r"^\s*([0-9A-Za-z]{2,5})\s+(\d+):(\d+)\s+(.*)$")

# The canonical order and display names. A book code the input has that is not
# in here is an error, not a silent drop: a pack missing Obadiah because
# somebody's export spelled it "OBD" is worse than a build failure.
BOOKS = [
    ("GEN", "Genesis", OT),            ("EXO", "Exodus", OT),
    ("LEV", "Leviticus", OT),          ("NUM", "Numbers", OT),
    ("DEU", "Deuteronomy", OT),        ("JOS", "Joshua", OT),
    ("JDG", "Judges", OT),             ("RUT", "Ruth", OT),
    ("1SA", "1 Samuel", OT),           ("2SA", "2 Samuel", OT),
    ("1KI", "1 Kings", OT),            ("2KI", "2 Kings", OT),
    ("1CH", "1 Chronicles", OT),       ("2CH", "2 Chronicles", OT),
    ("EZR", "Ezra", OT),               ("NEH", "Nehemiah", OT),
    ("EST", "Esther", OT),             ("JOB", "Job", OT),
    ("PSA", "Psalms", OT),             ("PRO", "Proverbs", OT),
    ("ECC", "Ecclesiastes", OT),       ("SOL", "Song of Solomon", OT),
    ("ISA", "Isaiah", OT),             ("JER", "Jeremiah", OT),
    ("LAM", "Lamentations", OT),       ("EZE", "Ezekiel", OT),
    ("DAN", "Daniel", OT),             ("HOS", "Hosea", OT),
    ("JOE", "Joel", OT),               ("AMO", "Amos", OT),
    ("OBA", "Obadiah", OT),            ("JON", "Jonah", OT),
    ("MIC", "Micah", OT),              ("NAH", "Nahum", OT),
    ("HAB", "Habakkuk", OT),           ("ZEP", "Zephaniah", OT),
    ("HAG", "Haggai", OT),             ("ZEC", "Zechariah", OT),
    ("MAL", "Malachi", OT),

    ("MAT", "Matthew", NT),            ("MAR", "Mark", NT),
    ("LUK", "Luke", NT),               ("JOH", "John", NT),
    ("ACT", "Acts", NT),               ("ROM", "Romans", NT),
    ("1CO", "1 Corinthians", NT),      ("2CO", "2 Corinthians", NT),
    ("GAL", "Galatians", NT),          ("EPH", "Ephesians", NT),
    ("PHI", "Philippians", NT),        ("COL", "Colossians", NT),
    ("1TH", "1 Thessalonians", NT),    ("2TH", "2 Thessalonians", NT),
    ("1TI", "1 Timothy", NT),          ("2TI", "2 Timothy", NT),
    ("TIT", "Titus", NT),              ("PHM", "Philemon", NT),
    ("HEB", "Hebrews", NT),            ("JAM", "James", NT),
    ("1PE", "1 Peter", NT),            ("2PE", "2 Peter", NT),
    ("1JO", "1 John", NT),             ("2JO", "2 John", NT),
    ("3JO", "3 John", NT),             ("JUD", "Jude", NT),
    ("REV", "Revelation", NT),

    ("TOB", "Tobit", APOC),            ("JDT", "Judith", APOC),
    ("ESG", "Esther (Greek)", APOC),   ("WIS", "Wisdom", APOC),
    ("SIR", "Sirach", APOC),           ("BAR", "Baruch", APOC),
    ("1MA", "1 Maccabees", APOC),      ("2MA", "2 Maccabees", APOC),
    ("1ES", "1 Esdras", APOC),         ("PRM", "Prayer of Manasseh", APOC),
    ("PSX", "Psalm 151", APOC),        ("3MA", "3 Maccabees", APOC),
    ("4ES", "2 Esdras", APOC),         ("4MA", "4 Maccabees", APOC),
    ("DNG", "Daniel (Greek)", APOC),
]

BOOK_INDEX = {abbr: i for i, (abbr, _, _) in enumerate(BOOKS)}

# Alternative codes seen in the wild, mapped onto ours. Kept short on purpose:
# a code nobody has actually met is a guess, and a guess here silently
# reorders somebody's Bible.
ALIASES = {
    "SNG": "SOL", "SS": "SOL", "SONG": "SOL",
    "EZK": "EZE", "JOL": "JOE", "OBD": "OBA", "NAM": "NAH",
    "MRK": "MAR", "JHN": "JOH", "PHP": "PHI", "PHL": "PHI",
    "JAS": "JAM", "1JN": "1JO", "2JN": "2JO", "3JN": "3JO",
    "JDE": "JUD", "PSS": "PSA", "PRV": "PRO", "QOH": "ECC",
    "MAN": "PRM", "PS2": "PSX", "2ES": "4ES", "ESD": "1ES",
}

TRANSLIT = {
    "‘": "'",  "’": "'",
    "‚": ",",  "‛": "'",
    "“": '"',  "”": '"',
    "„": '"',  "‟": '"',
    "–": "-",  "—": "--",  "―": "--",
    "‐": "-",  "‑": "-",   "‒": "-",
    "…": "...",
    " ": " ",  " ": " ",   " ": " ",  " ": " ",
    "­": "",
    "¶": "",   "†": "*",   "‡": "*",
    "æ": "ae", "Æ": "AE",  "œ": "oe", "Œ": "OE",
}


def to_ascii(s, strict, where, problems):
    """Fold to printable ASCII. See the module docstring for why."""
    out = []
    for ch in s:
        if " " <= ch <= "~":
            out.append(ch)
            continue
        repl = TRANSLIT.get(ch)
        if repl is None:
            # NFKD strips the accent off a Latin letter, which is the right
            # answer for a proper name and no answer at all for anything else.
            folded = "".join(
                c for c in unicodedata.normalize("NFKD", ch)
                if " " <= c <= "~"
            )
            if folded:
                repl = folded
            else:
                problems.setdefault(ch, where)
                repl = "?" if not strict else ""
        out.append(repl)
    return "".join(out)


def read_vpl(path, strict):
    """-> {book_abbr: {chapter:int -> {verse:int -> text}}}, plus a book order."""
    books = {}
    order = []
    problems = {}
    unknown = set()
    n_lines = 0

    with open(path, "r", encoding="utf-8-sig") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.rstrip("\n\r")
            if not line.strip():
                continue
            m = VERSE_RE.match(line)
            if not m:
                raise SystemExit(
                    "%s:%d: not a verse-per-line record: %.60r" % (path, lineno, line)
                )
            code, chap, verse, text = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
            code = code.upper()
            code = ALIASES.get(code, code)
            if code not in BOOK_INDEX:
                unknown.add(code)
                continue
            text = to_ascii(text.strip(), strict, "%s %d:%d" % (code, chap, verse), problems)
            if code not in books:
                books[code] = {}
                order.append(code)
            books[code].setdefault(chap, {})[verse] = text
            n_lines += 1

    if unknown:
        raise SystemExit(
            "unknown book codes: %s\n"
            "Add them to BOOKS or ALIASES in this file -- dropping them "
            "silently would ship an incomplete Bible." % ", ".join(sorted(unknown))
        )
    if problems:
        detail = ", ".join(
            "U+%04X at %s" % (ord(ch), where) for ch, where in list(problems.items())[:8]
        )
        if strict:
            raise SystemExit("characters font.ttf cannot draw: " + detail)
        print("warning: %d character(s) with no ASCII fold: %s"
              % (len(problems), detail), file=sys.stderr)
    return books, n_lines


def build_dictionary(raws):
    """A 32 KB block of common phrasing to prime every chapter's deflater.

    Each chapter is compressed on its own so the reader can inflate one
    without touching the rest, which means each starts with an empty 32 KB
    window and re-learns from scratch that "and he said to him" is common.
    A preset dictionary is that window, pre-filled.

    Phrases are scored by (len - 3) * (count - 1): about the bytes saved by
    having the phrase available as a back-reference rather than spelling it
    out, given a match costs roughly three bytes to encode. The block is
    filled best-first.

    On the WEB text this takes the chapter blob from 2,026,131 bytes to
    1,727,653, or 14.7%. Filling it best-LAST instead -- same phrases, same
    scoring, highest value nearest the data -- gives 1,857,400, which is 7.5%
    worse. Run this with and without --no-dict to re-measure on another text.
    """
    text = b"\n".join(raws).decode("ascii", "replace")
    words = text.split(" ")
    counts = collections.Counter()
    for n in range(2, 7):
        for i in range(len(words) - n):
            phrase = " ".join(words[i:i + n])
            if 8 <= len(phrase) <= 60:
                counts[phrase] += 1

    ranked = sorted(counts.items(),
                    key=lambda kv: (len(kv[0]) - 3) * (kv[1] - 1),
                    reverse=True)
    out = bytearray()
    for phrase, n in ranked:
        if n < 3:
            break
        chunk = (" " + phrase).encode("ascii", "replace")
        # A phrase already spelled out inside the block is free; skipping it
        # leaves room for one that is not.
        if chunk in out:
            continue
        out += chunk
        if len(out) >= DICT_MAX:
            break
    return bytes(out[:DICT_MAX])


def build(books, name, use_dict=True):
    """Serialise. Returns (bytes, stats)."""
    present = [(abbr, disp, sect) for (abbr, disp, sect) in BOOKS if abbr in books]

    pool = bytearray()
    pool_off = {}
    for _, disp, _ in present:
        pool_off[disp] = len(pool)
        pool += disp.encode("ascii") + b"\x00"

    # Pass one: the raw chapter payloads, in file order.
    raws = []           # (abbr, cnum, raw bytes, n_verses)
    book_spans = []     # (abbr, disp, sect, first_chapter, n_chapters)
    gaps = []

    for abbr, disp, sect in present:
        chapters = books[abbr]
        first_chapter = len(raws)
        # Chapter numbers are made contiguous from 1 for the same reason verse
        # numbers are: the reader indexes, it does not search.
        top = max(chapters)
        for cnum in range(1, top + 1):
            verses = chapters.get(cnum, {})
            if not verses:
                gaps.append("%s %d (whole chapter)" % (abbr, cnum))
                raws.append((abbr, cnum, b"", 0))
                continue
            vtop = max(verses)
            lines = []
            for vnum in range(1, vtop + 1):
                if vnum not in verses:
                    gaps.append("%s %d:%d" % (abbr, cnum, vnum))
                lines.append(verses.get(vnum, ""))
            raw = "\n".join(lines).encode("ascii", "replace")
            if len(raw) > RAW_MAX:
                raise SystemExit("%s %d inflates to %d bytes, over the %d ceiling"
                                 % (abbr, cnum, len(raw), RAW_MAX))
            if vtop > 0xFFFF:
                raise SystemExit("%s %d has %d verses, over 65535" % (abbr, cnum, vtop))
            raws.append((abbr, cnum, raw, vtop))
        book_spans.append((abbr, disp, sect, first_chapter, top))

    zdict = build_dictionary([r for _, _, r, _ in raws]) if use_dict else b""

    # Pass two: deflate, each chapter primed with the same window.
    chap_entries = []
    blob = bytearray()
    max_raw = 0
    n_verses_total = 0
    for _, _, raw, nv in raws:
        if zdict:
            co = zlib.compressobj(9, zlib.DEFLATED, 15, 9,
                                  zlib.Z_DEFAULT_STRATEGY, zdict=zdict)
            comp = co.compress(raw) + co.flush()
        else:
            comp = zlib.compress(raw, 9)
        chap_entries.append((len(blob), len(comp), len(raw), nv))
        blob += comp
        max_raw = max(max_raw, len(raw))
        n_verses_total += nv

    book_entries = [
        (abbr, pool_off[disp], n_ch, first, sect)
        for abbr, disp, sect, first, n_ch in book_spans
    ]

    n_books = len(book_entries)
    n_chapters = len(chap_entries)
    book_off = HDR_SZ
    chap_off = book_off + n_books * BOOK_SZ
    pool_start = chap_off + n_chapters * CHAP_SZ
    dict_off = pool_start + len(pool)
    blob_off = dict_off + len(zdict)

    if max_raw > 0xFFFF:
        raise SystemExit("a chapter inflates to %d bytes; raw_len is 16-bit" % max_raw)

    flags = F_DEFLATE | (F_DICT if zdict else 0)

    out = bytearray()
    out += MAGIC
    out += struct.pack("<HHHH", VERSION, flags, n_books, 0)
    out += struct.pack("<IIIIIIIIII",
                       n_chapters, book_off, chap_off, pool_start,
                       len(pool), blob_off, len(blob), max_raw,
                       dict_off, len(zdict))
    label = name.encode("ascii", "replace")[:NAME_MAX - 1]
    out += label + b"\x00" * (NAME_MAX - len(label))
    out += b"\x00" * 8
    assert len(out) == HDR_SZ, len(out)

    for abbr, name_o, n_ch, first, sect in book_entries:
        code = abbr.encode("ascii")[:3]
        out += code + b"\x00" * (4 - len(code))
        out += struct.pack("<HHIBBH", name_o, n_ch, first, sect, 0, 0)
    assert len(out) == chap_off, (len(out), chap_off)

    for off, clen, rlen, nv in chap_entries:
        out += struct.pack("<IIHH", off, clen, rlen, nv)
    assert len(out) == pool_start

    out += pool
    assert len(out) == dict_off
    out += zdict
    assert len(out) == blob_off
    out += blob

    stats = {
        "books": n_books,
        "chapters": n_chapters,
        "verses": n_verses_total,
        "max_raw": max_raw,
        "blob": len(blob),
        "dict": len(zdict),
        "index": blob_off,
        "total": len(out),
        "gaps": gaps,
    }
    return bytes(out), stats


def verify(path):
    """Re-read a pack the way the C does and report what it found."""
    data = open(path, "rb").read()
    if data[:8] != MAGIC:
        raise SystemExit("%s: bad magic" % path)
    ver, flags, n_books, _ = struct.unpack_from("<HHHH", data, 8)
    (n_chapters, book_off, chap_off, pool_off, pool_len,
     blob_off, blob_len, max_raw, dict_off, dict_len) = struct.unpack_from("<IIIIIIIIII", data, 16)
    label = data[56:72].split(b"\x00")[0].decode("ascii")
    zdict = data[dict_off:dict_off + dict_len] if (flags & F_DICT) else b""
    print("%s: v%d flags=0x%04x %s  %d books, %d chapters, index %d B, dict %d B, "
          "blob %d B, max_raw %d"
          % (path, ver, flags, label, n_books, n_chapters,
             chap_off + n_chapters * CHAP_SZ, dict_len, blob_len, max_raw))

    total_verses = 0
    for i in range(n_books):
        base = book_off + i * BOOK_SZ
        abbr = data[base:base + 4].split(b"\x00")[0].decode("ascii")
        name_o, n_ch, first, sect, _, _ = struct.unpack_from("<HHIBBH", data, base + 4)
        disp = data[pool_off + name_o:].split(b"\x00")[0].decode("ascii")
        for c in range(n_ch):
            off, clen, rlen, nv = struct.unpack_from("<IIHH", data,
                                                     chap_off + (first + c) * CHAP_SZ)
            do = zlib.decompressobj(zdict=zdict) if zdict else zlib.decompressobj()
            raw = do.decompress(data[blob_off + off:blob_off + off + clen]) + do.flush()
            if len(raw) != rlen:
                raise SystemExit("%s %d: inflated %d, index says %d"
                                 % (abbr, c + 1, len(raw), rlen))
            lines = raw.split(b"\n") if raw else []
            if len(lines) != nv:
                raise SystemExit("%s %d: %d lines, index says %d verses"
                                 % (abbr, c + 1, len(lines), nv))
            total_verses += nv
        print("  %-4s %-22s %3d chapters  [%s]"
              % (abbr, disp, n_ch, ("OT", "NT", "Apoc")[sect]))
    print("  %d verses, every chapter inflated and checked" % total_verses)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", help="verse-per-line .txt")
    ap.add_argument("output", nargs="?", help="the .ndb to write")
    ap.add_argument("--name", default="WEB",
                    help="translation label shown on screen (max %d chars)" % (NAME_MAX - 1))
    ap.add_argument("--no-dict", action="store_true",
                    help="skip the preset dictionary (about 15%% larger; see bible.h)")
    ap.add_argument("--strict", action="store_true",
                    help="fail on a character font.ttf cannot draw instead of folding it")
    ap.add_argument("--verify", metavar="PACK",
                    help="re-read a pack, inflate every chapter, print the book table")
    args = ap.parse_args()

    if args.verify:
        verify(args.verify)
        return 0
    if not args.input or not args.output:
        ap.error("need an input and an output, or --verify")

    books, n = read_vpl(args.input, args.strict)
    data, st = build(books, args.name, use_dict=not args.no_dict)
    with open(args.output, "wb") as fh:
        fh.write(data)

    print("%s: %d books, %d chapters, %d verses from %d source lines"
          % (args.output, st["books"], st["chapters"], st["verses"], n))
    print("  index %d B + dict %d B + blob %d B = %d B (%.2f MB), "
          "largest chapter inflates to %d B"
          % (st["index"] - st["dict"], st["dict"], st["blob"], st["total"],
             st["total"] / 1048576.0, st["max_raw"]))
    if st["gaps"]:
        print("  filled %d gap(s) with empty verses: %s%s"
              % (len(st["gaps"]), ", ".join(st["gaps"][:6]),
                 " ..." if len(st["gaps"]) > 6 else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())

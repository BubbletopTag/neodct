#!/usr/bin/env python3
"""mkuifont.py -- build a theme's UI typeface from an upstream face.

The phone itself ships ONE typeface: font.ttf, Nokia Cellphone FC, 16 KB, a
bitmap-style pixel face. It is pinned by neodct/tests/golden/font/fontref.json
BY SHA-256, because that file is the evidence that the C FreeType path renders
the same pixels Pillow did, and gen_bootfont bakes the initramfs boot bar's
1-bit tables out of it. It is the built-in look's face and it may not be
disturbed by anything here.

A THEME may bring its own, as fonts/ui.ttf and fonts/ui-bold.ttf inside the
theme directory (docs/THEMES.md). That is what this script builds, and it
exists because two things have to happen together and doing either by hand
goes wrong:

  1. SUBSETTING. A full upstream face is 400-700 KB per weight. The phone has
     128 MB of NAND for the entire rootfs and 64 MB of RAM, the card is not
     much freer, and nothing on it renders Cyrillic, Greek, Hebrew, Arabic or
     CJK -- the UI is English and the user data is names and SMS. Cutting to
     Latin plus the punctuation the OS actually draws takes a pair from about
     825 KB to about 60 KB, and a theme is something an owner copies onto a
     card.

  2. RENAMING. Most of the faces worth using are under SIL OFL 1.1 with a
     Reserved Font Name, and a subset is a derivative work. Shipping a
     modified face under its original name would be a licence violation, so
     the name table is rewritten. This is the part that is easy to forget and
     impossible to notice afterwards. --family is what it is rewritten TO, and
     it must not contain the upstream's reserved name.

Build a theme's pair:

    python3 neodct/tools/mkuifont.py --family "NeoDCT Kitty Rounded" \
        --out neodct/contrib/themes/HelloKitty/fonts \
        Baloo2-Regular.ttf Baloo2-Bold.ttf

Ship the upstream licence next to it as fonts/LICENSE.txt; this script does
not copy it, because only the person who fetched the face knows where it came
from.

Needs fonttools on the build host only; nothing on the phone reads this.
"""

import os
import sys

# Where a theme's pair goes by default, and the names the loader looks for.
DEFAULT_OUT = "."
OUT_NAMES = ("ui.ttf", "ui-bold.ttf")

# What the phone can be asked to draw.
#
# Latin-1 and Latin Extended-A are user data rather than UI: a contact called
# Bjorn Ostergaard and an SMS in Polish both have to render, and a missing
# glyph on this device is not a box -- nd_draw.h says it draws NOTHING and
# still costs its advance, so the name simply comes out with holes in it.
#
# The punctuation block is the opposite: every one of these is a character
# some widget draws deliberately.
RANGES = [
    (0x0020, 0x007E),   # ASCII
    (0x00A0, 0x00FF),   # Latin-1 Supplement -- European names
    (0x0100, 0x017F),   # Latin Extended-A   -- Polish, Czech, Turkish
    (0x0192, 0x0192),   # florin, in a couple of currency strings
    (0x02C6, 0x02DC),   # spacing modifiers the above compose with
]

# Individually named, so that adding one is a decision with a reason beside it
# rather than a wider range nobody audits.
CHARS = [
    0x2010, 0x2011, 0x2012, 0x2013, 0x2014,  # hyphens and dashes
    0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E,  # smart quotes
    0x2020, 0x2021, 0x2022, 0x2026,          # dagger, bullet, ELLIPSIS
    0x2030, 0x2039, 0x203A, 0x2044,
    0x20AC,                                   # euro
    0x2122, 0x2190, 0x2191, 0x2192, 0x2193,  # trademark, arrows
    0x2212, 0x2215,                           # minus, division slash
    0x25A0, 0x25AA, 0x25B2, 0x25BA, 0x25BC, 0x25C4,  # the play/stop glyphs
    0x2600, 0x2605, 0x2606,
    0x266A, 0x266B,                           # music notes, for MusicPlayer
    0x2713, 0x2717,                           # tick and cross
]

# U+2026 is called out because its arrival is a behaviour change, not just a
# glyph. font.ttf had no ellipsis: nd_msgdialog.c's append_ellipsis() has
# always appended one and it has always drawn NOTHING, so a clipped message
# ended in eight pixels of empty space. On this face it is visible, which is
# what those call sites wanted in the first place.


def wanted_codepoints():
    cps = set()
    for lo, hi in RANGES:
        cps.update(range(lo, hi + 1))
    cps.update(CHARS)
    return cps


def build(src, dst, subfamily, family):
    from fontTools import subset
    from fontTools.ttLib import TTFont

    opts = subset.Options()
    opts.name_IDs = ["*"]
    opts.name_legacy = True
    opts.name_languages = ["*"]
    opts.notdef_outline = True
    opts.recalc_bounds = True
    # Hinting is kept. The UI draws at 14, 18, 20 and 24 px and the phone is a
    # 240x175 panel; unhinted stems at 14 px go soft enough to matter.
    opts.hinting = True
    opts.layout_features = ["*"]
    opts.drop_tables += ["DSIG"]

    font = subset.load_font(src, opts)
    subsetter = subset.Subsetter(options=opts)
    subsetter.populate(unicodes=wanted_codepoints())
    subsetter.subset(font)
    subset.save_font(font, dst, opts)

    # Rename AFTER subsetting: the subsetter copies the name table through,
    # and it is the saved file that has to carry the derivative name.
    full = family if subfamily == "Regular" else family + " " + subfamily
    psname = full.replace(" ", "")
    tt = TTFont(dst)
    for rec in tt["name"].names:
        nid = rec.nameID
        if nid == 1:
            rec.string = family
        elif nid == 2:
            rec.string = subfamily
        elif nid == 4:
            rec.string = full
        elif nid == 6:
            rec.string = psname
        elif nid in (3, 16, 17, 18, 20, 21, 22):
            # Unique ID and the typographic/compatible family names all repeat
            # the reserved name; drop rather than rewrite, since a subset has
            # no claim to a stable unique identifier anyway.
            rec.string = full if nid in (16, 18) else psname
    tt.save(dst)
    tt.close()
    return os.path.getsize(dst)


def main(argv):
    import argparse

    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("regular", help="the upstream regular weight")
    ap.add_argument("bold", nargs="?", help="the upstream bold weight; optional -- "
                                            "nd_ui_font_bold() falls back to the regular")
    ap.add_argument("--family", required=True,
                    help="the family name to rewrite the subset to; must not carry the "
                         "upstream's reserved font name")
    ap.add_argument("--out", default=DEFAULT_OUT, help="directory to write ui.ttf into")
    args = ap.parse_args(argv)

    os.makedirs(args.out, exist_ok=True)
    pairs = [(args.regular, OUT_NAMES[0], "Regular")]
    if args.bold:
        pairs.append((args.bold, OUT_NAMES[1], "Bold"))
    for src, name, sub in pairs:
        dst = os.path.join(args.out, name)
        before = os.path.getsize(src)
        after = build(src, dst, sub, args.family)
        print(f"{name}: {before:,} -> {after:,} bytes "
              f"({100 * after // before}%)  {len(wanted_codepoints())} codepoints")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

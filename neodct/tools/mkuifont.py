#!/usr/bin/env python3
"""mkuifont.py -- build the UI typeface from an upstream face.

The phone ships two typefaces and they are not interchangeable:

  font.ttf   Nokia Cellphone FC, 16 KB, a bitmap-style pixel face. It is
             pinned by neodct/tests/golden/font/fontref.json BY SHA-256,
             because that file is the evidence that the C FreeType path
             renders the same pixels Pillow did. gen_bootfont also bakes the
             initramfs boot bar's 1-bit tables out of it. Neither has anything
             to do with how the UI looks, and neither may be disturbed.

  aero.ttf   The UI face. A humanist/neo-grotesque sans, because the Frutiger
             Aero chrome in nd_theme.c is glass and gradients and a pixel
             face fights it -- letterforms with hard 90-degree corners on a
             surface that is trying to look wet.

This script produces the second from a system font, and exists because two
things have to happen together and doing either by hand goes wrong:

  1. SUBSETTING. Upstream Liberation Sans is 411 KB per weight. The phone has
     128 MB of NAND for the entire rootfs and 64 MB of RAM, and nothing on it
     renders Cyrillic, Greek, Hebrew, Arabic or CJK -- the UI is English and
     the user data is names and SMS. Cutting to Latin plus the punctuation
     the OS actually draws takes the pair from 825 KB to about 60 KB.

  2. RENAMING. Liberation is a Reserved Font Name under SIL OFL 1.1, and a
     subset is a derivative work. Shipping a modified face still called
     "Liberation Sans" would be a licence violation, so the name table is
     rewritten. This is the part that is easy to forget and impossible to
     notice afterwards.

Regenerate:

    python3 neodct/tools/mkuifont.py \
        /usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf \
        /usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf

Needs fonttools on the build host only; nothing on the phone reads this.
"""

import os
import sys

FONT_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "overlay", "NeoDCT", "System", "ui", "resources", "fonts")

# The family name the phone's face is known by. It must not contain
# "Liberation" -- see the module docstring.
FAMILY = "NeoDCT Aero Sans"

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


def build(src, dst, subfamily):
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
    full = FAMILY if subfamily == "Regular" else FAMILY + " " + subfamily
    psname = full.replace(" ", "")
    tt = TTFont(dst)
    for rec in tt["name"].names:
        nid = rec.nameID
        if nid == 1:
            rec.string = FAMILY
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
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2

    os.makedirs(FONT_DIR, exist_ok=True)
    for src, name, sub in ((argv[1], "aero.ttf", "Regular"),
                           (argv[2], "aero-bold.ttf", "Bold")):
        dst = os.path.join(FONT_DIR, name)
        before = os.path.getsize(src)
        after = build(src, dst, sub)
        print(f"{name}: {before:,} -> {after:,} bytes "
              f"({100 * after // before}%)  {len(wanted_codepoints())} codepoints")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

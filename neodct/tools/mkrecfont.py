"""Generate neodct/src/recovery/nd_recfont.h -- a 1-bit ASCII bitmap of the
phone's own font.ttf, at the two sizes the recovery UI uses.

WHY A GENERATOR WHOSE OUTPUT IS COMMITTED, RATHER THAN A BUILD STEP.

mkinitramfs.py hand-rolls its own BMP reader with a comment saying nothing
guarantees PIL is installed on a build host. That applies with equal force
here: nd-recui is built by buildroot on whatever machine somebody has. So this
is a developer-run tool whose output is checked in, exactly like fontref.py
(-> tests/golden/font/fontref.json) and mkicons.py (-> the icon PNGs).
neodct/tests/test_recui_font.py regenerates and byte-compares when PIL is
importable, so the header cannot silently drift away from font.ttf, and no
build host gains a dependency.

WHY A BITMAP RATHER THAN FREETYPE OR PRE-RENDERED SCREENS.

nd-recui runs inside the initramfs, where /NeoDCT/System is by definition not
mounted -- there is no font.ttf and no libfreetype. Pre-rendering the screens
as images cannot work either: recovery has to draw package filenames read off
an SD card, percentages and byte counts, none of which are known until it
runs. And one 240x175 XRGB8888 screen is 168,000 bytes against this table's
~5 KB, on a device whose cpio is unpacked into RAM.

WHY THE INK BOX COMES FROM THE ANTIALIASED RENDER.

The coverage is thresholded at >= 128, because antialiasing is meaningless in
a one-bit design and would cost eight times the bytes. But the ink BOX is
Pillow's own getbbox() of the greyscale render, not of the thresholded bits.
That is deliberate: nd_font.c derives ink_h/ink_dy the same way, and
nd_vlist's row centring is (item_height - ink_h) / 2. Trimming the box to the
thresholded pixels would move every menu row a pixel away from where the real
framework puts it.

LAYOUT ENGINE IS PINNED TO BASIC, for the reason fontref.py and goldenframe.py
pin it: buildroot's Pillow is built -Craqm=disable, so the phone never uses
RAQM and neither may the reference.

    python3 neodct/tools/mkrecfont.py --out neodct/src/recovery/nd_recfont.h
"""

import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
NEODCT = os.path.dirname(HERE)

FONT_PATH = os.path.join(NEODCT, "overlay", "NeoDCT", "System", "ui",
                         "resources", "fonts", "font.ttf")

DEFAULT_OUT = os.path.join(NEODCT, "src", "recovery", "nd_recfont.h")

#: 18 px carries the title and the menu items; 14 px the heading, the
#: percentage, the byte reading and package filenames. Two sizes is the
#: minimum that works and the maximum that is justified -- see
#: docs/c-rewrite/spec-recovery-ui.md section 4, which measured the real
#: strings. "NeoDCT recovery" at the framework's own title size (24) is
#: 258 px wide on a 240 px panel, so a third table could not be used anyway.
SIZES = (18, 14)

#: Printable ASCII, the same range fontref.py records. Recovery draws menu
#: labels it owns and filenames off a FAT card; anything outside this range
#: renders as the missing-glyph box nd_recdraw.c substitutes.
FIRST, LAST = 0x20, 0x7E

#: Coverage at or above this counts as ink.
THRESHOLD = 128


def load(size):
    return ImageFont.truetype(FONT_PATH, size,
                              layout_engine=ImageFont.Layout.BASIC)


def glyph(font, ch):
    """One character, rasterised exactly as draw.text() would.

    Returns (advance, w, h, dy, rows) where dy is the ink's offset below the
    "la" anchor -- the ascender line, which is the y a caller passes to
    nd_recdraw_text() -- and rows is one bytes() per ink row, MSB first.
    """
    pad = font.size * 2
    canvas = Image.new("L", (pad * 3, pad * 3), 0)
    ImageDraw.Draw(canvas).text((pad, pad), ch, font=font, fill=255)

    advance = font.getlength(ch)
    if advance != int(advance):
        raise SystemExit("mkrecfont: %r at %d px has a fractional advance "
                         "(%r); the 1-bit packing assumes whole pixels"
                         % (ch, font.size, advance))

    ink = canvas.getbbox()
    if ink is None:                       # space: an advance and no pixels
        return int(advance), 0, 0, 0, []

    x0, y0, x1, y1 = ink
    if x0 - pad != 0:
        # spec section 4 measured ink_dx as identically zero at every size,
        # which is why it is not stored. A font change could break that, and
        # silently dropping it would shift one glyph inside its own cell.
        raise SystemExit("mkrecfont: %r at %d px has ink_dx=%d; the record "
                         "has nowhere to put it"
                         % (ch, font.size, x0 - pad))

    crop = canvas.crop(ink)
    w, h = crop.size
    pixels = crop.load()
    rows = []
    for y in range(h):
        row = bytearray((w + 7) // 8)
        for x in range(w):
            if pixels[x, y] >= THRESHOLD:
                row[x // 8] |= 0x80 >> (x % 8)
        rows.append(bytes(row))
    return int(advance), w, h, y0 - pad, rows


def table(size):
    font = load(size)
    ascent = font.getmetrics()[0]
    records = []
    bits = bytearray()
    for code in range(FIRST, LAST + 1):
        advance, w, h, dy, rows = glyph(font, chr(code))
        off = len(bits)
        for row in rows:
            bits += row
        if dy < 0 or dy > 255:
            raise SystemExit("mkrecfont: %r at %d px has dy=%d, which does "
                             "not fit a uint8_t" % (chr(code), size, dy))
        if off > 0xFFFF:
            raise SystemExit("mkrecfont: bitmap for %d px exceeds 64 KiB" % size)
        records.append((advance, w, h, dy, off))
    return ascent, records, bytes(bits)


def emit_bytes(handle, name, blob):
    handle.write("static const uint8_t %s[%d] = {\n" % (name, len(blob)))
    for start in range(0, len(blob), 16):
        chunk = blob[start:start + 16]
        handle.write("    " + " ".join("0x%02X," % b for b in chunk) + "\n")
    handle.write("};\n\n")


HEADER = '''/* nd_recfont.h -- GENERATED by neodct/tools/mkrecfont.py. DO NOT EDIT.
 *
 * A 1-bit ASCII bitmap of neodct/overlay/NeoDCT/System/ui/resources/fonts/
 * font.ttf at 18 px and 14 px, so the recovery UI draws in the phone's own
 * typeface without freetype and without the squashfs the font lives on.
 *
 * Regenerate with:
 *     python3 neodct/tools/mkrecfont.py
 * neodct/tests/test_recui_font.py fails if this file and font.ttf disagree.
 *
 * Coverage is thresholded at >= 128; the ink BOX is Pillow's own antialiased
 * getbbox(), so ink_h matches what nd_font.c reports and the row centring
 * lands where nd_vlist puts it. ink_dx is identically zero at both sizes and
 * is therefore not stored. Rows are packed MSB first, (w + 7) / 8 bytes per
 * row, glyph after glyph into one array.
 */

#ifndef ND_RECFONT_H_INCLUDED
#define ND_RECFONT_H_INCLUDED

#include <stdint.h>

#define ND_RECFONT_FIRST 0x20
#define ND_RECFONT_LAST  0x7E
#define ND_RECFONT_COUNT (ND_RECFONT_LAST - ND_RECFONT_FIRST + 1)

typedef struct {
    uint8_t advance;  /* whole pixels; the pen moves by this whether or not
                       * the glyph has any ink                             */
    uint8_t w;
    uint8_t h;
    uint8_t dy;       /* ink top, below the "la" anchor the caller passes  */
    uint16_t off;     /* BYTE offset into this size's bits[] array         */
} nd_recglyph;

typedef struct {
    uint8_t size;
    uint8_t ascent;   /* getmetrics()[0]; a text box starts collapsed here */
    const nd_recglyph *glyphs;
    const uint8_t *bits;
    uint32_t n_bits;
} nd_recfont;

'''


def generate(handle):
    handle.write(HEADER)
    total = 0
    for size in SIZES:
        ascent, records, bits = table(size)
        total += len(bits) + len(records) * 6
        emit_bytes(handle, "nd_recfont_bits_%d" % size, bits)
        handle.write("static const nd_recglyph nd_recfont_glyphs_%d[%d] = {\n"
                     % (size, len(records)))
        for code, record in zip(range(FIRST, LAST + 1), records):
            advance, w, h, dy, off = record
            handle.write("    {%3d, %2d, %2d, %2d, %5d}, /* %s */\n"
                         % (advance, w, h, dy, off,
                            "space" if code == 0x20 else chr(code)))
        handle.write("};\n\n")
        handle.write("static const nd_recfont nd_recfont_%d = {\n"
                     "    %d, %d, nd_recfont_glyphs_%d, nd_recfont_bits_%d,\n"
                     "    sizeof nd_recfont_bits_%d\n};\n\n"
                     % (size, size, ascent, size, size, size))
    handle.write("/* Table size: %d bytes of .rodata. */\n" % total)
    handle.write("\n#endif /* ND_RECFONT_H_INCLUDED */\n")
    return total


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=DEFAULT_OUT)
    args = ap.parse_args(argv)

    if not os.path.exists(FONT_PATH):
        raise SystemExit("mkrecfont: font not found: %s" % FONT_PATH)

    with open(args.out, "w") as handle:
        total = generate(handle)
    print("%s (%d bytes of tables)" % (args.out, total))
    return 0


if __name__ == "__main__":
    sys.exit(main())

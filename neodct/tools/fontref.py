"""Dump Pillow's exact glyph rasterisation, so the C font engine can be
verified one character at a time instead of one screen at a time.

Text is the crux of this port. 117 of the ~260 draw calls in the shipped code
are `draw.text`, and a half-pixel disagreement in glyph placement moves every
centred, ellipsised and wrapped string on the phone. Debugging that from whole
240x175 screens is miserable: one wrong advance width shows up as "the whole
line is shifted" with no clue which glyph did it.

So this dumps, for every font size the UI uses and every character it can
draw:

  * the 8-bit antialiased coverage bitmap Pillow produces
  * the advance width, in the same fractional units Pillow reports
  * the ink box (`getbbox`) and the offset Pillow applies when compositing

plus whole-string layouts, which catch advance accumulation and any kerning
the BASIC layout engine applies. A C implementation that matches this file
matches Pillow by construction, and when it does not, the failure names the
character.

The reference is generated with the layout engine pinned to BASIC, for the
same reason `goldenframe.py` pins it: Buildroot's Pillow is built
`-Craqm=disable`, so the phone never uses RAQM.

    python3 neodct/tools/fontref.py --out neodct/tests/golden/font/
"""

import argparse
import hashlib
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
NEODCT = os.path.dirname(HERE)

FONT_PATH = os.path.join(NEODCT, "overlay", "NeoDCT", "System", "ui",
                         "resources", "fonts", "font.ttf")

#: The four sizes core/main.py loads (font_s, font_md, font_n, font_xl).
SIZES = (14, 18, 20, 24)

#: Printable ASCII. The UI is English-only; anything outside this range that
#: turns up in a contact name renders through the same path, so covering the
#: base set proves the path rather than every glyph.
CHARS = [chr(c) for c in range(0x20, 0x7F)]

#: Strings that exercise accumulation, not individual glyphs: real UI labels,
#: digits with punctuation, and the widest things the layout has to fit.
STRINGS = [
    "Menu", "Names", "Back", "Select", "Options", "Exit",
    "Messages", "Phone Book", "Settings", "Koki Mobile", "Music Player",
    "17:08", "+353 87 123 4567", "*#06#",
    "No Service", "Tello", "This application has not been implemented yet.",
    "AV To Wa Ye",                      # classic kerning pairs
    "iiiii", "WWWWW", "  leading and trailing  ",
]


def load(size):
    return ImageFont.truetype(FONT_PATH, size,
                              layout_engine=ImageFont.Layout.BASIC)


def glyph_record(font, ch):
    """Rasterise one character exactly as draw.text() would.

    Rendered onto a black canvas in white and read back as luminance, which
    is the coverage value Pillow blends with -- the same number the C
    rasteriser has to produce per pixel.
    """
    # getbbox gives the ink box relative to the text origin; it can be
    # negative above the baseline-ish origin Pillow uses, so pad generously
    # and record where the ink actually landed.
    pad = font.size * 2
    canvas = Image.new("L", (pad * 3, pad * 3), 0)
    draw = ImageDraw.Draw(canvas)
    draw.text((pad, pad), ch, font=font, fill=255)

    ink = canvas.getbbox()
    if ink is None:                      # space and friends: no ink at all
        bitmap, w, h, ox, oy = b"", 0, 0, 0, 0
    else:
        x0, y0, x1, y1 = ink
        crop = canvas.crop(ink)
        bitmap = crop.tobytes()
        w, h = crop.size
        ox, oy = x0 - pad, y0 - pad      # ink offset from the draw origin

    return {
        "char": ch,
        "codepoint": ord(ch),
        "advance": font.getlength(ch),
        "bbox": list(font.getbbox(ch)),
        "ink_w": w, "ink_h": h,
        "ink_dx": ox, "ink_dy": oy,
        "sha256": hashlib.sha256(bitmap).hexdigest() if bitmap else "",
        "coverage_sum": sum(bitmap),     # cheap smoke value for a C port
    }


def string_record(font, text):
    """Whole-string metrics, plus the running pen position per character.

    A C engine can match every glyph and still drift if it accumulates
    advances differently (rounding per glyph versus at the end). The pen
    trail catches exactly that.
    """
    pen = []
    acc = 0.0
    for ch in text:
        pen.append(round(acc, 6))
        acc += font.getlength(ch)

    # How the string actually renders, as one image -- the ground truth that
    # per-glyph data is only an explanation of.
    pad = font.size * 2
    canvas = Image.new("L", (int(font.getlength(text)) + pad * 2,
                             font.size * 4), 0)
    ImageDraw.Draw(canvas).text((pad, pad), text, font=font, fill=255)
    ink = canvas.getbbox()
    rendered = canvas.crop(ink).tobytes() if ink else b""

    return {
        "text": text,
        "length": font.getlength(text),
        "bbox": list(font.getbbox(text)),
        # The sum of per-character advances. If this differs from "length",
        # the engine applies kerning or fractional accumulation and the C
        # side must do the same.
        "sum_of_advances": round(acc, 6),
        "pen_positions": pen,
        "render_sha256": hashlib.sha256(rendered).hexdigest(),
    }


def build():
    if not os.path.exists(FONT_PATH):
        raise SystemExit(f"font not found: {FONT_PATH}")

    out = {
        "font": os.path.relpath(FONT_PATH, os.path.dirname(NEODCT)),
        "font_sha256": hashlib.sha256(open(FONT_PATH, "rb").read()).hexdigest(),
        "layout_engine": "BASIC",
        "freetype": ImageFont.core.freetype2_version,
        "pillow": __import__("PIL").__version__,
        "sizes": {},
    }

    for size in SIZES:
        font = load(size)
        ascent, descent = font.getmetrics()
        out["sizes"][str(size)] = {
            "size": size,
            "ascent": ascent,
            "descent": descent,
            "glyphs": [glyph_record(font, ch) for ch in CHARS],
            "strings": [string_record(font, s) for s in STRINGS],
        }
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True, help="directory to write into")
    args = ap.parse_args(argv)

    os.makedirs(args.out, exist_ok=True)
    data = build()

    path = os.path.join(args.out, "fontref.json")
    with open(path, "w") as fh:
        json.dump(data, fh, indent=1, sort_keys=True)

    n_glyphs = sum(len(s["glyphs"]) for s in data["sizes"].values())
    n_strings = sum(len(s["strings"]) for s in data["sizes"].values())
    print(f"{path}")
    print(f"  freetype {data['freetype']}, pillow {data['pillow']}, "
          f"layout {data['layout_engine']}")
    print(f"  {len(data['sizes'])} sizes, {n_glyphs} glyph records, "
          f"{n_strings} string records")
    return 0


if __name__ == "__main__":
    sys.exit(main())

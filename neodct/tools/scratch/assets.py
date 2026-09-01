"""Costumes for the Scratch port: one per glyph, plus the phone's own art.

Scratch has no way to draw text, so the port stamps a costume per character.
Every glyph is rendered here by the same Pillow calls the phone makes, at the
same four sizes, and the metrics that come back are what the Scratch side uses
to reproduce `ui.get_text_size` exactly -- the widget layout is almost entirely
text measurement, so a millimetre of drift there moves everything on screen.

Costumes are emitted at SCALE times their device size with nearest-neighbour
scaling, so one phone pixel is a hard-edged square on the Scratch stage rather
than a smudge. The rotation centre of every costume is its top-left corner,
which makes `go to x y` mean "put this pixel there" and keeps the coordinate
maths the same as Pillow's.
"""

import math
import os

from PIL import Image, ImageDraw, ImageFont

SCALE = 2                     # stage units per phone pixel
FONT_PATH_DEFAULT = "neodct/overlay/NeoDCT/System/ui/resources/fonts/font.ttf"

# The phone's four fonts (System/core/main.py:616). The prefix is what the
# costume name is built from on the Scratch side.
FONTS = [
    ("s", 14),
    ("m", 18),
    ("n", 20),
    ("x", 24),
]

# ASCII printables, minus space -- which has no ink, and is handled by its
# advance alone. U+2026 is on the end because MessageDialog truncates with a
# real ellipsis even though this font has no glyph for one; the port has to
# draw whatever Pillow draws there, which is nothing.
CHARSET = "".join(chr(c) for c in range(33, 127)) + "\u2026"


class Glyph:
    __slots__ = ("font", "char", "name", "advance", "dx", "dy", "width",
                 "height", "image")

    def __init__(self, font, char, name, advance, dx, dy, image):
        self.font = font
        self.char = char
        self.name = name
        self.advance = advance
        self.dx = dx
        self.dy = dy
        self.image = image
        self.width = image.width // SCALE if image else 0
        self.height = image.height // SCALE if image else 0


def _render_glyph(font, char, ascent, descent):
    """Render one character and find where its ink sits.

    Everything is measured from the origin `draw.text` is given, which Pillow
    puts on the ascender line -- so the offsets recorded here can be added to
    a caller's (x, y) with no further correction.
    """
    pad = 8
    box_w = int(font.getlength(char)) + 2 * pad + 8
    box_h = ascent + descent + 2 * pad
    canvas = Image.new("L", (max(1, box_w), max(1, box_h)), 0)
    draw = ImageDraw.Draw(canvas)
    draw.text((pad, pad), char, font=font, fill=255)
    ink = canvas.getbbox()
    if ink is None:
        return None, 0, 0
    cropped = canvas.crop(ink)
    return cropped, ink[0] - pad, ink[1] - pad


def _to_costume(mask):
    """A coverage mask becomes white pixels with the coverage as alpha.

    The phone draws white text onto black; text that has to come out black
    (a selected list row) is the same costume with Scratch's brightness
    effect turned all the way down, so only one set of glyphs is needed.
    """
    white = Image.new("RGBA", mask.size, (255, 255, 255, 0))
    white.putalpha(mask)
    return white.resize((mask.width * SCALE, mask.height * SCALE),
                        Image.Resampling.NEAREST)


def build_glyphs(font_path):
    """Every glyph of every font, in the order the costumes will be in."""
    glyphs = []
    metrics = {}
    for prefix, size in FONTS:
        font = ImageFont.truetype(font_path, size)
        ascent, descent = font.getmetrics()
        metrics[prefix] = {
            "size": size,
            "ascent": ascent,
            "descent": descent,
            "space": font.getlength(" "),
        }
        for char in CHARSET:
            mask, dx, dy = _render_glyph(font, char, ascent, descent)
            blank = mask is None
            if blank:
                # No ink -- U+2026 in this font. It still has an advance, and
                # it still needs a costume so the metrics tables stay one
                # entry per costume; it just contributes nothing to a bbox.
                mask = Image.new("L", (1, 1), 0)
                dx = dy = 0
            glyph = Glyph(prefix, char, "%s_%s" % (prefix, char),
                          font.getlength(char), dx, dy, _to_costume(mask))
            if blank:
                glyph.width = glyph.height = 0
            glyphs.append(glyph)
    return glyphs, metrics


def rhu(value):
    """Round half up -- Pillow's PIXEL() macro, and Scratch's round()."""
    return math.floor(value + 0.5)


def measure(text, prefix, index, metrics):
    """`ui.get_text_size` in pure Python (System/core/main.py:796).

    Pillow does not report the ink box: the width is the greater of the
    rounded pen advance and the rightmost glyph pixel, and the bottom edge
    never rises above the baseline even for a string of apostrophes. A
    character with no ink at all contributes neither, which is why a lone
    U+2026 measures zero high. All of it comes from how `font_getsize` seeds
    its bounds, and all of it matters -- the softkey label is centred with
    this number.

    Kept in Python so the build can assert it against Pillow itself; the
    Scratch implementation is a transcription of this function.
    """
    meta = metrics[prefix]
    pen = 0.0
    top = None
    bottom = meta["ascent"]
    right = 0
    for char in text:
        if char == " ":
            pen += meta["space"]
            continue
        glyph = index.get((prefix, char)) or index.get((prefix, "?"))
        if glyph is None:
            continue
        x = rhu(pen)
        right = max(right, x + glyph.dx + glyph.width)
        if glyph.height:
            top = glyph.dy if top is None else min(top, glyph.dy)
            bottom = max(bottom, glyph.dy + glyph.height)
        pen += glyph.advance
    return (max(rhu(pen), right), 0 if top is None else bottom - top)


# --- bitmap art -----------------------------------------------------------

def load_image(path, max_size=None):
    """`NeoDCT_UI.get_image` for the build host (System/core/main.py:750)."""
    img = Image.open(path).convert("RGBA")
    if max_size is not None and (img.width > max_size or img.height > max_size):
        img.thumbnail((max_size, max_size), Image.Resampling.LANCZOS)
    return img


def upscale(img):
    return img.resize((img.width * SCALE, img.height * SCALE),
                      Image.Resampling.NEAREST)


def save(img, directory, name):
    safe = "".join(c if c.isalnum() or c in "._-" else "%02x" % ord(c)
                   for c in name)
    path = os.path.join(directory, safe + ".png")
    img.save(path, "PNG", optimize=True)
    return path

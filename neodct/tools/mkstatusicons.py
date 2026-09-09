#!/usr/bin/env python3
"""mkstatusicons.py -- redraw the home screen's battery and signal sprites.

The home screen is DATA (nd_layout.c: "four elements in one JSON file"), and
the battery and signal meters are two icon_set elements pointing at ten PNGs.
That indirection is worth keeping -- it is what lets a layout author move or
replace a meter without touching C -- so the theme could not simply draw these
with nd_theme and be done. The sprites had to be redrawn instead.

Doing it by hand in an image editor is how ten files stop matching each other
and stop matching the chrome around them, so it is done here, from the same
palette nd_theme.h defines and with the same four construction steps
nd_theme_plate_draw() uses: gradient body, top-half sheen stopping dead at the
midpoint, white bevel under the top edge, dark border.

    python3 neodct/tools/mkstatusicons.py

Needs Pillow on the build host. Nothing on the phone reads this script; it
writes the PNGs and the PNGs ship.

============ THE GEOMETRY IS COPIED, NOT INVENTED ============

Every band below was measured off the sprites this replaces, and the new art
lands on the same rows and columns. Three things depend on that and none of
them is obvious:

  * ui_home.json places both meters by a single (x, y) and nothing else. A
    sprite whose ink started three rows lower would move the meter.
  * nd_layout.c scales these by H/240 and caches at display size, so the
    authored 36x180 is a fixed contract with the layout, not a free choice.
  * the battery's "?" label -- what a phone with no battery hardware shows --
    is positioned from nd_image_alpha_bbox() of the sprite. Changing where the
    ink stops moves the label.

============ WHY THE COLOURS DIFFER PER LEVEL ============

The old sprites were white at every level: the only thing that told you the
battery was nearly flat was counting segments. There is a palette now, so one
segment is red, two is amber and three or more is green -- the reading is
available before you have counted anything. The signal meter does NOT do this:
one bar of signal is not a fault, it is one bar.
"""

import os
import sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "overlay", "NeoDCT", "System", "ui", "resources", "img")

W, H = 36, 180
SS = 4  # supersampling factor; see render()

# ---- the palette, from nd_theme.h ----
BLUE_HI = (0x5C, 0xC3, 0xF5)
BLUE_MID = (0x0F, 0x6C, 0xC8)
BLUE_DEEP = (0x06, 0x2E, 0x63)
GLASS_TOP = (0xF2, 0xF9, 0xFF)
GLASS_BOT = (0xC6, 0xDF, 0xF2)
CHROME_HI = (0xFF, 0xFF, 0xFF)
CHROME_TOP = (0xDA, 0xE7, 0xF2)
CHROME_BOT = (0x8E, 0xA8, 0xBE)
GREEN_TOP = (0x9E, 0xE8, 0x4A)
GREEN_BOT = (0x3D, 0x9A, 0x14)
AMBER_TOP = (0xFF, 0xD9, 0x5C)
AMBER_BOT = (0xD8, 0x88, 0x0A)
RED_TOP = (0xFF, 0x8A, 0x7A)
RED_BOT = (0xB4, 0x1C, 0x14)

# ---- geometry, measured off the sprites being replaced ----
# (top, bottom, left, right), inclusive, in the 36x180 authored space.
SIG_SEGMENTS = [
    (101, 124, 12, 23),
    (72, 97, 12, 23),
    (41, 68, 12, 24),
    (7, 38, 12, 29),
]
SIG_CROSSBAR = (136, 145, 0, 35)
SIG_MAST = (146, 175, 14, 21)

BAT_SEGMENTS = [
    (101, 124, 11, 24),
    (72, 97, 11, 24),
    (41, 68, 11, 24),
    (7, 38, 11, 24),
]
BAT_NUB = (128, 134, 12, 23)
BAT_CAN = (135, 171, 7, 28)

# One segment is a warning, two is a caution, three or more is fine.
BAT_LEVEL_COLOUR = {1: (RED_TOP, RED_BOT), 2: (AMBER_TOP, AMBER_BOT)}


def _lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def plate(surf, box, top, bot, radius, sheen=90, bevel=True, border=BLUE_DEEP,
          border_a=210, body_a=255):
    """nd_theme_plate_draw(), in Pillow, in the same four steps.

    `box` is (top, bottom, left, right) inclusive -- the order the geometry
    tables above are written in, because they were transcribed from a
    row-by-row scan of the old sprites, and reading them as rows is what makes
    them checkable against it.

    EACH STAGE IS A SEPARATE LAYER, and that is not tidiness. ImageDraw on an
    RGBA image OVERWRITES the destination alpha rather than compositing over
    it -- blending is only done for an RGBA-mode Draw onto an RGB image. So a
    sheen drawn straight onto the body does not lighten it, it replaces it
    with white at the sheen's own alpha, and the plate comes out transparent
    across its top half. That is exactly what happened on the first cut of
    these sprites, and on a dark background it reads as a gradient, which is
    why it survived a look. surf.layer() is the fix: draw the stage onto a
    transparent layer and alpha_composite it down.
    """
    y0, y1, x0, x1 = box
    h = y1 - y0

    def row_inset(i):
        """How far this row is pulled in by the corner arc."""
        r = radius
        if r <= 0:
            return 0
        if i < r:
            return r - int((r * r - (r - 1 - i) ** 2) ** 0.5)
        if i > h - r:
            j = i - (h - r + 1)
            return r - int((r * r - (j + 1) ** 2) ** 0.5)
        return 0

    # 1. the body, as a stack of one-pixel rounded rows so the gradient and the
    #    rounded corner compose the way they do in C.
    with surf.layer() as draw:
        for i in range(h + 1):
            c = _lerp(top, bot, i / h if h else 0)
            inset = row_inset(i)
            draw.rectangle([x0 + inset, y0 + i, x1 - inset, y0 + i], fill=c + (body_a,))

    # 2. the sheen: white, TOP HALF ONLY, stopping dead at the midpoint.
    if sheen:
        mid = y0 + h // 2
        with surf.layer() as draw:
            for y in range(y0, mid + 1):
                i = y - y0
                a = round(sheen * (1 - (i / max(1, mid - y0)) * (2 / 3)))
                inset = row_inset(i)
                draw.rectangle([x0 + inset, y, x1 - inset, y], fill=CHROME_HI + (a,))

    # 3. the bevel.
    if bevel and h > 1:
        with surf.layer() as draw:
            draw.rectangle([x0 + radius, y0 + 1, x1 - radius, y0 + 1], fill=CHROME_HI + (150,))

    # 4. the border.
    if border_a:
        with surf.layer() as draw:
            draw.rounded_rectangle([x0, y0, x1, y1], radius=radius,
                                   outline=border + (border_a,), width=1)


def render(paint):
    """Draw at SS times scale and resample down.

    The C side antialiases its corners by area coverage; Pillow's
    rounded_rectangle does not antialias at all, and a 12 px wide capsule with
    hard corners reads as a rectangle with bites out of it. Supersampling is
    the cheapest way to get the same edge here, and it costs nothing at build
    time.
    """
    import contextlib

    from PIL import Image, ImageDraw

    big = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))

    class Scaled:
        """A draw target in authored 36x180 coordinates, one stage at a time.

        Coordinates are inclusive here (the whole project's convention) and
        half-open in Pillow, hence the +1 on the far edges.
        """

        def __init__(self, d):
            self._d = d

        def rectangle(self, box, **kw):
            self._d.rectangle([box[0] * SS, box[1] * SS,
                               (box[2] + 1) * SS - 1, (box[3] + 1) * SS - 1], **kw)

        def rounded_rectangle(self, box, radius=0, **kw):
            kw["width"] = kw.get("width", 1) * SS
            self._d.rounded_rectangle([box[0] * SS, box[1] * SS,
                                       (box[2] + 1) * SS - 1, (box[3] + 1) * SS - 1],
                                      radius=radius * SS, **kw)

    class Surface:
        @contextlib.contextmanager
        def layer(self):
            """One drawing stage, composited down on exit. See plate()."""
            nonlocal big
            lay = Image.new("RGBA", big.size, (0, 0, 0, 0))
            yield Scaled(ImageDraw.Draw(lay, "RGBA"))
            big = Image.alpha_composite(big, lay)

    paint(Surface())
    return big.resize((W, H), Image.LANCZOS)


def signal(level):
    def paint(dr):
        # The mast, in chrome: it is structure, not signal, so it never
        # changes with the level and it is deliberately a different material
        # from the bars above it.
        plate(dr, SIG_MAST, CHROME_TOP, CHROME_BOT, 2, sheen=120)
        plate(dr, SIG_CROSSBAR, CHROME_TOP, CHROME_BOT, 3, sheen=120)
        for i, box in enumerate(SIG_SEGMENTS):
            if i < level:
                plate(dr, box, BLUE_HI, BLUE_MID, 3)
            else:
                # An unlit bar is not absent -- an empty meter has to look
                # like a meter, or "no signal" and "no phone" look the same.
                plate(dr, box, BLUE_DEEP, BLUE_DEEP, 3, sheen=0, bevel=False,
                      border=CHROME_HI, border_a=70, body_a=60)
    return render(paint)


def battery(level):
    def paint(dr):
        plate(dr, BAT_NUB, CHROME_TOP, CHROME_BOT, 2, sheen=120)
        # The can is glass, and it is empty -- the charge is the segments
        # stacked above it, exactly as it was on the sprites this replaces.
        plate(dr, BAT_CAN, GLASS_TOP, GLASS_BOT, 4, sheen=70, body_a=205)
        top, bot = BAT_LEVEL_COLOUR.get(level, (GREEN_TOP, GREEN_BOT))
        for i, box in enumerate(BAT_SEGMENTS):
            if i < level:
                plate(dr, box, top, bot, 3)
            else:
                plate(dr, box, BLUE_DEEP, BLUE_DEEP, 3, sheen=0, bevel=False,
                      border=CHROME_HI, border_a=70, body_a=60)
    return render(paint)


def main():
    made = []
    for lvl in range(5):
        for sub, name, fn in (("cellsignal", "sig", signal), ("battery", "bat", battery)):
            path = os.path.join(OUT, sub, f"{name}-{lvl}.png")
            img = fn(lvl)
            img.save(path)
            made.append((path, img.size, os.path.getsize(path)))
    for path, size, n in made:
        print(f"{os.path.relpath(path, OUT):28s} {size[0]}x{size[1]}  {n:,} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())

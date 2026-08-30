#!/usr/bin/env python3
"""Redraw the Browser and Update app icons in the house pixel style.

The rest of the icon set is hand-made art from "neodct icons/"; only these
two are generated, because both were placeholders -- Browser was a byte-copy
of the Settings gear and Update was the one full-colour icon in the set.

The shipped icon set is hard-edged white line art on transparency. These are
drawn on a coarse 40x40 grid and scaled with NEAREST, so the chunky edges
survive -- anti-aliasing would make them the odd ones out.
"""
from PIL import Image

G, SCALE = 40, 3
ON, OFF = (255, 255, 255, 255), (0, 0, 0, 0)


class Canvas:
    def __init__(self):
        self.img = Image.new("RGBA", (G, G), OFF)
        self.px = self.img.load()

    def rect(self, x0, y0, x1, y1, c=ON):
        for y in range(max(0, y0), min(G - 1, y1) + 1):
            for x in range(max(0, x0), min(G - 1, x1) + 1):
                self.px[x, y] = c

    def outline(self, x0, y0, x1, y1, t=2):
        self.rect(x0, y0, x1, y1)
        self.rect(x0 + t, y0 + t, x1 - t, y1 - t, OFF)

    def mask(self, fn, c=ON):
        for y in range(G):
            for x in range(G):
                if fn(x + 0.5 - G / 2, y + 0.5 - G / 2):
                    self.px[x, y] = c

    def save(self, path):
        self.img.resize((G * SCALE, G * SCALE), Image.NEAREST).save(path)
        print("wrote", path)


def update_icon(path):
    c = Canvas()
    # phone body, corners knocked off
    c.outline(9, 2, 30, 37, t=2)
    for cx, cy in ((9, 2), (30, 2), (9, 37), (30, 37)):
        c.px[cx, cy] = OFF
    c.rect(17, 6, 22, 7)              # earpiece
    c.rect(18, 11, 21, 21)            # arrow shaft
    top, bot = 22, 30                 # head, tapering to a real point
    for i, y in enumerate(range(top, bot + 1)):
        f = i / (bot - top)
        c.rect(round(13 + 6 * f), y, round(26 - 6 * f), y)
    c.rect(17, 33, 22, 34)            # home key
    c.save(path)


def browser_icon(path):
    c = Canvas()
    R, T = 16.0, 2.0
    ring = lambda dx, dy: abs((dx * dx + dy * dy) ** 0.5 - R) <= T / 2
    inside = lambda dx, dy: (dx * dx + dy * dy) ** 0.5 <= R - T / 2

    def meridian(dx, dy):
        if not inside(dx, dy):
            return False
        a, b = 7.5, R - T / 2
        v = (dx / a) ** 2 + (dy / b) ** 2
        return 0.72 <= v <= 1.28

    c.mask(ring)
    c.mask(meridian)
    c.mask(lambda dx, dy: inside(dx, dy) and abs(dy) <= 1.0)        # equator
    c.mask(lambda dx, dy: inside(dx, dy) and 8.0 <= dy <= 9.5)      # lower lat
    c.mask(lambda dx, dy: inside(dx, dy) and -9.5 <= dy <= -8.0)    # upper lat
    c.save(path)


def calendar_icon(path):
    """A month block: two binder tabs, a filled title bar, ruled days.

    The icon this replaces was a solid white slab with the days punched out
    of it -- black on white, when every other icon in the set is white line
    art on black. Next to Calculator, which is the same idea (a box with a
    grid in it), it read as that icon's photographic negative. So the body
    is hollow here, and the only solid areas are the title bar and the days,
    which is also what tells the two apart at the 80px the selector draws.
    """
    c = Canvas()
    x0, y0, x1 = 7, 8, 32
    c.outline(x0, y0, x1, 33, t=2)
    c.rect(x0, y0, x1, y0 + 4)                 # title bar, solid
    for tx in (13, 24):                        # binder tabs, above the bar
        c.rect(tx, 3, tx + 1, y0 + 1)
    # Four by three squares, not rows of dashes: a day is a square cell in
    # the month view this opens on, and a square is also what survives the
    # selector's downscale -- a 4x1 dash blurs into a rule.
    for row in range(3):
        for col in range(4):
            dx, dy = x0 + 2 + col * 6, y0 + 7 + row * 6
            c.rect(dx, dy, dx + 3, dy + 3)
    c.save(path)


if __name__ == "__main__":
    base = "neodct/overlay/NeoDCT/System/apps"
    update_icon(f"{base}/Update/icon.png")
    browser_icon(f"{base}/Browser/icon.png")
    calendar_icon(f"{base}/Calendar/icon.png")

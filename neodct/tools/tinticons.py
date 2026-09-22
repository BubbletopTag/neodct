#!/usr/bin/env python3
"""Recolour one theme's icon set into another's palette, by hue.

    tinticons.py neodct/overlay/NeoDCT/System/themes/FrutigerAero/icons \\
                 neodct/overlay/NeoDCT/System/themes/Blossom/icons

This is how Blossom's icons are made, and the reason it is a script rather
than a folder of pictures somebody painted is the same reason mkthemeart.py
renders a theme's preview instead of mocking it up: the Aero set is the one
the OS was drawn around, every icon in it has the same light, the same gloss
and the same reflection, and a second set painted by hand drifts from it the
first time either is touched. Deriving one from the other keeps them the same
objects in two colours -- and regenerating is one command when an app gains an
icon.

WHAT MOVES. Only hue. Saturation, value and alpha are copied through, which is
what keeps the gloss, the bevel and the soft edge exactly where the Aero set
put them. Hue is remapped by BAND rather than rotated as a whole: a rotation
that takes the signature blue to pink also takes the Aero greens to navy,
which reads as a mistake beside everything else. So

    blues and cyans (165-265 deg)  -> the theme's signature pink
    greens          ( 70-165 deg)  -> orchid, the second colour
    everything else                -> kept

which leaves the two-tone icons two-tone -- the calendar's grid, the second
chat bubble, the power button -- and the warm accents (the calculator's
orange key, the controller's buttons) as they were.

Pillow only, per pixel, on 120x120 pictures: a whole set is a second or two
on a build host. Nothing on the phone runs this.
"""

import argparse
import colorsys
import os
import sys

from PIL import Image

# (lo, hi, to): hues in [lo, hi) degrees become `to`. The defaults are
# Blossom's; another theme passes its own with --band.
DEFAULT_BANDS = ((165.0, 265.0, 338.0), (70.0, 165.0, 292.0))


def remap(hue_deg, bands):
    for lo, hi, to in bands:
        if lo <= hue_deg < hi:
            return to
    return hue_deg


def tint(src, dst, bands):
    im = Image.open(src).convert("RGBA")
    px = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            hh, s, v = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
            if s == 0.0:
                continue
            nh = remap(hh * 360.0, bands) / 360.0
            nr, ng, nb = colorsys.hsv_to_rgb(nh, s, v)
            px[x, y] = (round(nr * 255), round(ng * 255), round(nb * 255), a)
    im.save(dst, optimize=True)


def parse_band(text):
    try:
        span, to = text.split(":")
        lo, hi = span.split("-")
        return float(lo), float(hi), float(to)
    except ValueError:
        raise argparse.ArgumentTypeError("a band is LO-HI:TO in degrees, e.g. 165-265:338")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("src", help="directory of <App>.png icons to recolour")
    ap.add_argument("dst", help="directory to write the recoloured set into")
    ap.add_argument("--band", action="append", type=parse_band,
                    help="LO-HI:TO in degrees; repeatable; replaces the defaults")
    args = ap.parse_args(argv)

    bands = tuple(args.band) if args.band else DEFAULT_BANDS
    os.makedirs(args.dst, exist_ok=True)
    names = sorted(n for n in os.listdir(args.src) if n.endswith(".png"))
    if not names:
        print("tinticons: no .png icons in %s" % args.src, file=sys.stderr)
        return 1
    for name in names:
        tint(os.path.join(args.src, name), os.path.join(args.dst, name), bands)
    print("tinticons: %d icons -> %s" % (len(names), args.dst))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

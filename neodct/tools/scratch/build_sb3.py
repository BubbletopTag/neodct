#!/usr/bin/env python3
"""Build NeoDCT.sb3 -- the NeoDCT UI framework as a Scratch 3 project.

    python3 neodct/tools/scratch/build_sb3.py [-o NeoDCT.sb3]

Everything the project needs is generated from the phone's own files: the
glyph costumes come from `resources/fonts/font.ttf` at the same four sizes
the UI uses, and the app icons from each app's `icon.png`, scaled by the same
Pillow call `AppSelector` makes. Nothing is hand-drawn, so a change to the
phone's assets shows up here by rebuilding.
"""

import argparse
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import assets
import port_core
import port_widgets
import port_demo
from sb3 import Project

from PIL import Image, ImageDraw

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "..", ".."))
OVERLAY = os.path.join(REPO, "neodct", "overlay", "NeoDCT")
FONT = os.path.join(OVERLAY, "System/ui/resources/fonts/font.ttf")
APPS = os.path.join(OVERLAY, "System/apps")
IMG = os.path.join(OVERLAY, "System/ui/resources/img")

# AppSelector asks get_image() for the icon at the size it will draw it:
#   icon_y   = header_y + max(24, int((content_bottom - header_y) * 0.22))
#   icon_cap = min(175, max(24, content_bottom - icon_y - 8))
ICON_Y = port_core.HEADER_Y + max(24, int((port_core.CONTENT_BOTTOM - port_core.HEADER_Y) * 0.22))
ICON_CAP = min(175, max(24, port_core.CONTENT_BOTTOM - ICON_Y - 8))


def backdrop(path):
    """The phone's screen, and a hairline where it stops.

    The pen layer is erased to clear the screen, so whatever is painted here
    is what "black" means; the bands above and below are the only thing
    telling the eye where the 240x175 panel ends.
    """
    img = Image.new("RGB", (480, 360), (10, 10, 12))
    draw = ImageDraw.Draw(img)
    draw.rectangle((0, 5, 479, 354), fill=(0, 0, 0))
    draw.line((0, 4, 479, 4), fill=(40, 40, 46))
    draw.line((0, 355, 479, 355), fill=(40, 40, 46))
    img.save(path)
    return path


def collect_images(workdir):
    """(name, width, height, path) for every picture the project ships."""
    out = []

    def add(name, image):
        path = assets.save(assets.upscale(image), workdir, "img_" + name)
        out.append((name, image.width, image.height, path))

    for entry in sorted(os.listdir(APPS)):
        icon = os.path.join(APPS, entry, "icon.png")
        if os.path.exists(icon):
            add(entry, assets.load_image(icon, max_size=ICON_CAP))
    add("warning", assets.load_image(os.path.join(IMG, "errorscreen/warning.png")))
    add("placeholder", assets.load_image(os.path.join(IMG, "appselector/placeholder_icon.png"),
                                         max_size=ICON_CAP))
    add("envelope", assets.load_image(os.path.join(IMG, "envelope.png")))
    return out


def build(output, keep=None):
    workdir = keep or tempfile.mkdtemp(prefix="neodct-sb3-")
    os.makedirs(workdir, exist_ok=True)

    glyphs, metrics = assets.build_glyphs(FONT)
    index = {(g.font, g.char): g for g in glyphs}
    images = collect_images(workdir)

    project = Project()
    stage = project.target("Stage", is_stage=True)
    stage.costume("phone", backdrop(os.path.join(workdir, "backdrop.png")),
                  rotation_x=240, rotation_y=180)

    sprite = project.target("NeoDCT")
    sprite.x = 0
    sprite.y = 0

    blank = Image.new("RGBA", (1, 1), (0, 0, 0, 0))
    sprite.costume("blank", assets.save(blank, workdir, "blank"))
    for name, _, _, path in images:
        sprite.costume(name, path)
    for glyph in glyphs:
        path = assets.save(glyph.image, workdir, "g_" + glyph.name)
        sprite.costume(glyph.name, path)
    sprite.current_costume = 0

    port = port_core.Port(sprite, stage)
    # ProgressScreen and DetailPage lay themselves out from the height of
    # "Ag" in each font, which never changes; measure it once here rather
    # than emit the blocks to work it out every frame.
    port.ag_height = {i + 1: assets.measure("Ag", prefix, index, metrics)[1]
                      for i, (prefix, _) in enumerate(assets.FONTS)}
    api = port_core.build_engine(
        port, glyphs, metrics,
        [(name, w, h) for name, w, h, _ in images], assets.CHARSET)
    api.update(port_widgets.build_widgets(port, api))
    port_demo.build_demo(port, api)

    project.write(output)
    if keep is None:
        shutil.rmtree(workdir, ignore_errors=True)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output",
                        default=os.path.join(REPO, "neodct", "tools", "scratch",
                                             "NeoDCT.sb3"))
    parser.add_argument("--keep-assets", metavar="DIR",
                        help="leave the generated costumes in DIR")
    args = parser.parse_args()
    path = build(args.output, args.keep_assets)
    print("wrote %s (%.1f KB)" % (path, os.path.getsize(path) / 1024.0))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Draw a theme's preview.png and icon.png by ACTUALLY RENDERING the theme.

    mkthemeart.py neodct/contrib/themes/HelloKitty

A theme has to carry two pictures that are not part of the look itself: a
preview for the picker and the installer, and an icon for the installer's
list. The obvious way to make them is to draw a mock-up with Pillow, and that
is the wrong way: a mock-up is a second implementation of the interface, it
drifts from the real one the moment a widget changes, and it quietly lies
about what the owner will get.

So this renders the real thing. It stages the theme into a copy of the
overlay, runs nd-shoot -- the same binary that cuts the golden frames -- with
the theme selected, and keeps one of the frames it produced. Whatever comes
out is exactly what the phone draws, because it IS what the phone draws.

    preview.png   240x175, the real menu frame, unretouched
    icon.png      120x120, that frame's own centre, for the installer list

Needs a built tree: neodct/src/build/default/bin/nd-shoot. Build first.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image

# The frame kept as the preview, in order of preference. The app selector is
# first because it is the screen a theme changes most visibly -- one big icon,
# the title plate, the wallpaper behind both -- and it is the screen an owner
# pictures when they think "theme".
PREVIEW_FRAMES = ("menu-settings", "menu-messages", "home", "app-settings")

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SHOOT = os.path.join(REPO, "neodct", "src", "build", "default", "bin", "nd-shoot")
OVERLAY = os.path.join(REPO, "neodct", "overlay")


def die(msg):
    print("mkthemeart: %s" % msg, file=sys.stderr)
    sys.exit(1)


def render(theme_dir, theme_id, keep=None):
    """Stage the theme and shoot it. Returns the directory of frames."""
    if not os.path.isfile(SHOOT):
        die("no nd-shoot at %s -- build the tree first (cd neodct/src && make)" % SHOOT)

    tmp = tempfile.mkdtemp(prefix="themeart-")
    ov = os.path.join(tmp, "overlay")
    out = keep or os.path.join(tmp, "frames")

    # A copy rather than a symlink: nd-shoot symlinks most of System into its
    # own staged root, and a theme dropped into the tree's real overlay would
    # be a build artefact left in the source.
    shutil.copytree(OVERLAY, ov, symlinks=True)
    dst = os.path.join(ov, "NeoDCT", "System", "themes", os.path.basename(theme_dir))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copytree(theme_dir, dst)
    os.makedirs(out, exist_ok=True)

    # The theme's OWN wallpaper, when it ships one.
    #
    # nd-shoot picks a wallpaper per group from the stock set, and selecting a
    # theme on a real phone writes the theme's into the setting
    # (nd_theme_select). Without this the preview shows the new chrome over
    # the old background -- which is not a screen the owner will ever see, and
    # on a theme whose wallpaper is half its character it reads as the theme
    # not working.
    cmd = [SHOOT, "--overlay", ov, "--out", out, "--set", "system.ui.theme=%s" % theme_id]
    if os.path.isfile(os.path.join(theme_dir, "wallpaper.jpg")):
        cmd += ["--set", "system.ui.wallpaper=/NeoDCT/System/themes/%s/wallpaper.jpg"
                % os.path.basename(theme_dir)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        die("nd-shoot failed (%d)" % r.returncode)

    # nd-shoot logs the theme it loaded. If it fell back, the pictures would
    # silently be of the STOCK look wearing this theme's name -- the exact
    # lie this script exists to avoid -- so it is a hard failure.
    if ("Theme: " not in r.stderr and "Theme: " not in r.stdout):
        sys.stderr.write(r.stderr)
        die("nd-shoot never loaded theme %r; the pictures would be of the "
            "built-in look" % theme_id)
    return out, tmp


def pick(frames):
    for name in PREVIEW_FRAMES:
        p = os.path.join(frames, name + ".png")
        if os.path.isfile(p):
            return p
    any_png = sorted(f for f in os.listdir(frames) if f.endswith(".png"))
    if not any_png:
        die("nd-shoot wrote no frames")
    return os.path.join(frames, any_png[0])


def make_icon(src, out):
    """The frame's centre square, scaled to 120x120.

    A crop and not a squash: the panel is 240x175 and squeezing that into a
    square makes every plate in it visibly wrong, which on an icon whose whole
    job is to say what the theme looks like is worse than showing less of it."""
    im = Image.open(src).convert("RGB")
    side = min(im.size)
    left = (im.width - side) // 2
    top = (im.height - side) // 2
    im.crop((left, top, left + side, top + side)) \
      .resize((120, 120), Image.LANCZOS).save(out)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("theme_dir", help="the theme directory, holding theme.json")
    ap.add_argument("--keep-frames", metavar="DIR",
                    help="also keep every rendered frame here, for review")
    args = ap.parse_args(argv)

    d = os.path.abspath(args.theme_dir)
    tj = os.path.join(d, "theme.json")
    if not os.path.isfile(tj):
        die("%s has no theme.json" % d)
    with open(tj, encoding="utf-8") as f:
        theme = json.load(f)
    if not theme.get("id"):
        die("%s has no \"id\"" % tj)

    frames, tmp = render(d, theme["id"], args.keep_frames)
    try:
        src = pick(frames)
        shutil.copyfile(src, os.path.join(d, "preview.png"))
        make_icon(src, os.path.join(d, "icon.png"))
        print("%s: preview.png and icon.png rendered from %s (theme %s)"
              % (os.path.relpath(d, REPO), os.path.basename(src), theme["id"]))
    finally:
        if not args.keep_frames:
            shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

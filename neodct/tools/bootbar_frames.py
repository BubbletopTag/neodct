#!/usr/bin/env python3
"""Capture what nd-bootbar puts on the panel, as PNGs.

The boot install screen is drawn by a binary that runs in the initramfs with
no libneodct, no nd-shoot and no golden-frame machinery, so `nd-shoot --out`
cannot see it. This is its equivalent: it points the real nd-bootbar at an
ordinary file instead of /dev/fb0 and converts the bytes it wrote.

That is the whole reason nd_bootfb has an nd_bootfb_open_at() -- a regular
file has no FBIOGET_VSCREENINFO to answer, and nd_bootfb refuses to invent an
answer. `--geom 240x175x32` is what the phone and QEMU both report, so these
frames are the production drawing path with the geometry handed over rather
than asked for.

    python3 neodct/tools/bootbar_frames.py --out docs/img

Frames are 1-bit by construction (nd_bootfb.h: monochrome, on purpose), so
the byte order the framebuffer wants cannot change what they look like.
"""

import argparse
import os
import subprocess
import sys

W, H, BPP = 240, 175, 32

# Every frame the applier can put up, in the order a phone would show them.
# Names match the file names, so a reviewer can follow the install by sorting
# the directory.
FRAMES = (
    ("bootbar-01-open",       ["--step", "Checking the update", "--phase", "1",
                               "--total", "0", "--at", "0"]),
    ("bootbar-02-hash-45",    ["--step", "Checking the update", "--phase", "1",
                               "--total", "53477376", "--at", "45"]),
    ("bootbar-03-write-07",   ["--step", "Installing", "--phase", "2",
                               "--total", "53477376", "--at", "7"]),
    ("bootbar-04-write-63",   ["--step", "Installing", "--phase", "2",
                               "--total", "53477376", "--at", "63"]),
    ("bootbar-05-write-100",  ["--step", "Installing", "--phase", "2",
                               "--total", "53477376", "--at", "100"]),
    ("bootbar-06-readback-28", ["--step", "Checking the phone", "--phase", "3",
                                "--total", "53477376", "--at", "28"]),
    ("bootbar-07-refused",    ["--fail", "Update refused",
                               "--reason", "Not signed by NeoDCT"]),
    ("bootbar-08-damaged",    ["--fail", "Update not installed",
                               "--reason", "The update is damaged"]),
    ("bootbar-09-unfinished", ["--fail", "Install did not finish",
                               "--reason", "It will try again"]),
)


def default_binary():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(here, "src", "build", "default", "bin", "nd-bootbar")


def capture(binary, argv, raw_path):
    # Truncate first: nd_bootfb writes only the rows it draws, so a stale
    # file would leave whatever a previous frame put below them.
    with open(raw_path, "wb") as handle:
        handle.write(b"\0" * (W * H * BPP // 8))
    subprocess.run([binary, "--fb", raw_path,
                    "--geom", "%dx%dx%d" % (W, H, BPP)] + argv,
                   check=True, stdin=subprocess.DEVNULL)
    with open(raw_path, "rb") as handle:
        return handle.read()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="docs/img", help="directory for the PNGs")
    parser.add_argument("--bin", default=None, help="path to nd-bootbar")
    args = parser.parse_args()

    binary = args.bin or default_binary()
    if not os.path.exists(binary):
        sys.exit("bootbar_frames: no nd-bootbar at %s -- build neodct/src" % binary)

    try:
        from PIL import Image
    except ImportError:
        sys.exit("bootbar_frames: needs Pillow, which is a host-side tool "
                 "dependency only -- the phone never runs this")

    os.makedirs(args.out, exist_ok=True)
    raw_path = os.path.join(args.out, ".bootbar.raw")
    written = []
    try:
        for name, argv in FRAMES:
            data = capture(binary, argv, raw_path)
            # 32bpp, and monochrome: every pixel is 00000000 or ffffffff, so
            # any channel of it is the answer and no byte order is consulted.
            image = Image.frombytes("RGBA", (W, H), data).convert("L")
            path = os.path.join(args.out, name + ".png")
            image.save(path)
            written.append(path)
    finally:
        if os.path.exists(raw_path):
            os.unlink(raw_path)

    for path in written:
        print(path)


if __name__ == "__main__":
    main()

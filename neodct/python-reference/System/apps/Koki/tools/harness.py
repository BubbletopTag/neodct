#!/usr/bin/env python3
"""Headless test harness for the Koki port (host-side).

Stubs the NeoDCT `ui` object, runs the engine for N frames with scripted
key events, and dumps PNG snapshots so the game can be verified without
QEMU or hardware.

Usage:
  python3 tools/harness.py --frames 600 \
      --press 100:enter --hold 200-260:right --shot 50 --shot 300 \
      --out /tmp/shots

Key script syntax:
  --press F:key      press+release `key` at frame F (1 frame hold)
  --hold  A-B:key    hold `key` from frame A to frame B
  --shot  F          save frame F as shot_F.png (repeatable)
  --shot-every N     save every Nth frame
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
sys.path.insert(0, APP)

from PIL import Image, ImageDraw, ImageFont  # noqa: E402

import engine  # noqa: E402
import game    # noqa: E402


class FakeFB:
    def __init__(self, outdir):
        self.outdir = outdir
        self.frame_no = 0
        self.save_set = set()
        self.save_every = 0
        self.last = None

    def update(self, canvas):
        self.last = canvas
        if self.frame_no in self.save_set or \
           (self.save_every and self.frame_no % self.save_every == 0):
            canvas.save(os.path.join(self.outdir, f"shot_{self.frame_no:05d}.png"))


class FakeUI:
    def __init__(self, outdir):
        self.W, self.H = 240, 175
        self.canvas = Image.new("RGB", (self.W, self.H), "black")
        self.draw = ImageDraw.Draw(self.canvas)
        self.fb = FakeFB(outdir)
        self.keypad_fd = None
        self.matrix_input = None
        self.font_s = ImageFont.load_default()
        self.font_n = self.font_s
        self.font_md = self.font_s
        self.font_xl = self.font_s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--press", action="append", default=[])
    ap.add_argument("--hold", action="append", default=[])
    ap.add_argument("--shot", action="append", type=int, default=[])
    ap.add_argument("--shot-every", type=int, default=0)
    ap.add_argument("--out", default="/tmp/koki_shots")
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--trace", action="store_true",
                    help="print broadcasts as they happen")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    ui = FakeUI(args.out)

    os.environ["NEODCT_KOKI_NOSOUND"] = "1"
    eng = engine.Engine(ui, APP)
    game.register_all(eng)
    eng.random.seed(args.seed)
    ui.fb.save_set = set(args.shot)
    ui.fb.save_every = args.shot_every

    # scripted input: frame -> set of held keys
    holds = {}   # key name -> list of (start, end)
    for spec in args.press:
        f, k = spec.split(":")
        holds.setdefault(k, []).append((int(f), int(f) + 1))
    for spec in args.hold:
        rng, k = spec.split(":")
        a, b = rng.split("-")
        holds.setdefault(k, []).append((int(a), int(b)))

    frame_box = {"n": 0}

    def fake_poll():
        n = frame_box["n"]
        prev = set(eng.input.held)
        cur = set()
        for k, spans in holds.items():
            for a, b in spans:
                if a <= n <= b:
                    cur.add(k)
        eng.input.held = cur
        eng.input.pressed = cur - prev
    eng.input.poll = fake_poll

    if args.trace:
        orig_bc = eng.broadcast

        def traced(msg):
            print(f"[{frame_box['n']:5d}] broadcast {msg!r}")
            orig_bc(msg)
        eng.broadcast = traced

    # headless: no sleeping, count frames via fb hook
    eng.headless_frames = args.frames

    orig_render = eng.render

    def counting_render():
        ui.fb.frame_no = frame_box["n"]
        orig_render()
        frame_box["n"] += 1
    eng.render = counting_render

    eng.run()
    print(f"ran {frame_box['n']} frames -> {args.out}")


if __name__ == "__main__":
    main()

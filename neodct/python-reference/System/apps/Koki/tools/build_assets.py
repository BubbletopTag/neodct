#!/usr/bin/env python3
"""Koki asset builder (host-side only, never runs on the phone).

Reads the extracted Scratch project from `<repo>/koki.sb3 files/` and writes
pre-rendered, pre-scaled assets into `<app>/assets/`:

- Costumes: RGBA PNGs at final on-screen pixel size (Scratch stage 480x360
  -> NeoDCT 240x175 = uniform 0.5 scale, times the sprite's default size%,
  divided by bitmapResolution). Rotation centers are scaled identically and
  recorded in assets/manifest.json so the engine never scales at runtime.
- Sounds: tracks >= 15s become mono 2205kHz 64kbps MP3 (music, looped via
  mpv); shorter ones become 22050 Hz mono 16-bit WAV (sfx).

Requires: rsvg-convert, ffmpeg, imagemagick.
Usage: python3 tools/build_assets.py
"""

import json
import re
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP_DIR = os.path.dirname(HERE)
# repo root = six levels up from the app dir
REPO = os.path.abspath(os.path.join(APP_DIR, *[".."] * 6))
# sb3 extraction was removed from the repo; look in known locations
SRC_CANDIDATES = [
    os.path.join(REPO, "koki.sb3 files"),
    os.path.expanduser("~/Downloads/Koki/resources/app/koki.sb3 files"),
]
SRC = next((p for p in SRC_CANDIDATES if os.path.isdir(p)), SRC_CANDIDATES[0])
OUT = os.path.join(APP_DIR, "assets")
IMG_OUT = os.path.join(OUT, "img")
SND_OUT = os.path.join(OUT, "snd")

STAGE_SCALE = 0.5          # 480x360 -> 240x180 (cropped to 175)
SCREEN_W, SCREEN_H = 240, 175
MUSIC_THRESHOLD_S = 15.0   # >= this = music (mp3), below = sfx (wav)

# Sprites whose size changes at runtime get baked at their largest used size
# so runtime downscale (menu-only animations) never upscales.
BAKE_SIZE_OVERRIDE = {
    "intro": 100,        # logo grows to 100%
    "StartButton": 85,   # pulses 70 -> 85
}

# Full-screen overlays that scripts never move, scale, or flip: their art is
# cropped to the visible screen window at bake time. Sprite2's six score
# screens are 633x704 (1.7MB decoded EACH) with only 240x175 ever on screen;
# uncropped they were half the game's decoded-image weight.
SCREEN_CROP_STATIC = {"Sprite2", "Dynaris Logo", "White", "GameOver"}


def run(cmd):
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)}\n{res.stderr[:500]}")


def render_costume(md5ext, scale, dst):
    """Render one costume at `scale` (multiplier on its native pixel size)."""
    src = os.path.join(SRC, md5ext)
    if md5ext.endswith(".svg"):
        try:
            run(["rsvg-convert", "--zoom", f"{scale:.6f}", "-o", dst, src])
        except RuntimeError as e:
            if "no dimensions" not in str(e):
                raise
            # empty 0x0 placeholder costume -> 1x1 transparent pixel
            run(["magick", "-size", "1x1", "xc:transparent", "PNG32:" + dst])
    else:
        run(["magick", src, "-filter", "Lanczos",
             "-resize", f"{scale * 100:.4f}%", "PNG32:" + dst])


def alpha_bbox(path):
    """Bounding box of visible (non-transparent) pixels, or None for full.

    `-alpha extract` then trim finds the alpha bbox; fully-opaque art trims
    to ~1x1 (uniform white), which we treat as "use the full image".
    """
    res = subprocess.run(
        ["magick", path, "-alpha", "extract", "-format", "%@", "info:"],
        capture_output=True, text=True)
    m = re.match(r"(\d+)x(\d+)\+(\d+)\+(\d+)", res.stdout.strip())
    if not m:
        return None
    w, h, x, y = map(int, m.groups())
    if w < 3 or h < 3:      # uniform/opaque image: trim collapsed
        return None
    return [x, y, x + w, y + h]


def crop_to_screen(path, pose_x, pose_y, cx, cy):
    """Crop a static overlay costume to the screen window (+1px slack for
    the engine's paste rounding) and return the adjusted rotation center."""
    res = subprocess.run(["magick", "identify", "-format", "%w %h", path],
                         capture_output=True, text=True)
    w, h = map(int, res.stdout.split())
    px = SCREEN_W / 2.0 + pose_x * STAGE_SCALE - cx    # paste origin on screen
    py = SCREEN_H / 2.0 - pose_y * STAGE_SCALE - cy
    lx0 = max(0, int(-px) - 1)
    ly0 = max(0, int(-py) - 1)
    lx1 = min(w, int(SCREEN_W - px) + 2)
    ly1 = min(h, int(SCREEN_H - py) + 2)
    if (lx0, ly0, lx1, ly1) == (0, 0, w, h):
        return cx, cy
    run(["magick", path, "-crop", f"{lx1 - lx0}x{ly1 - ly0}+{lx0}+{ly0}",
         "+repage", "PNG32:" + path])
    return cx - lx0, cy - ly0


def convert_sound(md5ext, duration_s):
    src = os.path.join(SRC, md5ext)
    base = os.path.splitext(md5ext)[0]
    if duration_s >= MUSIC_THRESHOLD_S:
        dst = os.path.join(SND_OUT, base + ".mp3")
        if not os.path.exists(dst):
            run(["ffmpeg", "-y", "-i", src, "-ac", "1", "-ar", "22050",
                 "-b:a", "64k", dst])
        return "snd/" + base + ".mp3"
    dst = os.path.join(SND_OUT, base + ".wav")
    if not os.path.exists(dst):
        run(["ffmpeg", "-y", "-i", src, "-ac", "1", "-ar", "22050",
             "-sample_fmt", "s16", dst])
    return "snd/" + base + ".wav"


def main():
    if not os.path.isdir(SRC):
        sys.exit(f"source not found: {SRC}")
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(IMG_OUT)
    os.makedirs(SND_OUT)

    proj = json.load(open(os.path.join(SRC, "project.json")))
    manifest = {"stage_scale": STAGE_SCALE, "targets": {}}
    rendered = {}  # (md5ext, scale_key) -> filename
    bboxes = {}    # filename -> visible-pixel bbox (or None = full)

    for t in proj["targets"]:
        name = t["name"]
        size_pct = BAKE_SIZE_OVERRIDE.get(
            name, 100 if t["isStage"] else t.get("size", 100))
        entry = {
            "size": size_pct, "costumes": [], "sounds": {},
            # editor-left pose: scripts rely on these defaults
            "default_size": 100 if t["isStage"] else t.get("size", 100),
            "x": t.get("x", 0), "y": t.get("y", 0),
            "direction": t.get("direction", 90),
            "rotation_style": t.get("rotationStyle", "all around"),
            "current_costume": t.get("currentCostume", 0),
        }

        for c in t.get("costumes", []):
            res = c.get("bitmapResolution", 1) or 1
            scale = STAGE_SCALE * (size_pct / 100.0) / res
            cx = c.get("rotationCenterX", 0) * scale
            cy = c.get("rotationCenterY", 0) * scale
            if name in SCREEN_CROP_STATIC:
                # per-target file (crop depends on this sprite's pose)
                fname = (f"{os.path.splitext(c['md5ext'])[0]}"
                         f"@{round(scale*10000)}c.png")
                dst = os.path.join(IMG_OUT, fname)
                try:
                    render_costume(c["md5ext"], scale, dst)
                    cx, cy = crop_to_screen(
                        dst, entry["x"], entry["y"], cx, cy)
                except Exception as e:
                    print(f"  !! {name}/{c['name']}: {e}")
                    continue
                bboxes[fname] = alpha_bbox(dst)
            else:
                key = (c["md5ext"], round(scale, 4))
                if key not in rendered:
                    fname = (f"{os.path.splitext(c['md5ext'])[0]}"
                             f"@{round(scale*10000)}.png")
                    dst = os.path.join(IMG_OUT, fname)
                    try:
                        render_costume(c["md5ext"], scale, dst)
                    except Exception as e:
                        print(f"  !! {name}/{c['name']}: {e}")
                        continue
                    rendered[key] = fname
                    bboxes[fname] = alpha_bbox(dst)
                fname = rendered[key]
            entry["costumes"].append({
                "name": c["name"],
                "img": "img/" + fname,
                "cx": cx,
                "cy": cy,
                "bbox": bboxes.get(fname),
            })

        for s in t.get("sounds", []):
            dur = s.get("sampleCount", 0) / max(1, s.get("rate", 1))
            try:
                rel = convert_sound(s["md5ext"], dur)
            except Exception as e:
                print(f"  !! {name}/sound {s['name']}: {e}")
                continue
            entry["sounds"][s["name"]] = {"file": rel, "dur": round(dur, 2)}

        manifest["targets"][name] = entry
        print(f"{name}: {len(entry['costumes'])} costumes, "
              f"{len(entry['sounds'])} sounds")

    with open(os.path.join(OUT, "manifest.json"), "w") as f:
        json.dump(manifest, f)

    rewrite_controls_panel(manifest)

    total = subprocess.run(["du", "-sh", OUT], capture_output=True, text=True)
    print("assets total:", total.stdout.strip())


# NeoDCT system font, for native-looking replacement text
NEODCT_FONT = os.path.abspath(os.path.join(
    APP_DIR, "..", "..", "ui", "resources", "fonts", "font.ttf"))

CONTROL_LINES = [
    (22, 15, "NeoDCT Controls:"),
    (50, 13, "Walk: 4 / 6"),
    (70, 13, "Up: 2   Down: 8"),
    (90, 13, "Jump: 5  (or *)"),
    (110, 13, "Interact: 0  (or #)"),
    (130, 13, "OK: NaviKey  Pause: C"),
]


def rewrite_controls_panel(manifest):
    """The original 'how to play' panel names Scratch keys (arrows/Z/X);
    on the phone keypad those don't exist. Repaint the text area with
    NeoDCT keypad controls, keeping the panel art and the 'Okay' prompt."""
    if not os.path.exists(NEODCT_FONT):
        print(f"  !! controls panel: font not found: {NEODCT_FONT}")
        return
    for c in manifest["targets"]["Sprite1"]["costumes"]:
        path = os.path.join(OUT, c["img"])
        # sample the panel gray so the cover rectangle matches exactly
        res = subprocess.run(
            ["magick", path, "-format", "%[pixel:p{120,90}]", "info:"],
            capture_output=True, text=True)
        gray = res.stdout.strip() or "gray(60%)"
        cmd = ["magick", path,
               "-fill", gray, "-draw", "rectangle 4,4 239,152"]
        for y, size, text in CONTROL_LINES:
            cmd += ["-font", NEODCT_FONT, "-pointsize", str(size),
                    "-fill", "white", "-annotate", f"+12+{y}", text]
        if c["name"] == "costume2":
            # the original 'Okay' hugs the asset's right edge and clips
            # off-screen; redraw it a few px left (costume2 only, so the
            # NaviKey confirm flash keeps blinking it)
            cmd += ["-fill", gray, "-draw", "rectangle 180,153 243,178",
                    "-font", NEODCT_FONT, "-pointsize", "14",
                    "-fill", "white", "-annotate", "+192+172", "Okay"]
        cmd.append("PNG32:" + path)
        run(cmd)
    print("controls panel: rewritten with NeoDCT keypad controls")


if __name__ == "__main__":
    main()

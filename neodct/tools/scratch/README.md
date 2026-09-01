# NeoDCT in Scratch

`NeoDCT.sb3` is the NeoDCT UI framework — `System/ui/framework.py`, the last
Python one — as a Scratch 3 project. Open it in the Scratch editor or
TurboWarp and the phone's widgets are custom blocks: a vertical list, a paged
list, the text fields, the message dialog, the app selector, the progress
screen, the detail page. You build a NeoDCT app by dragging them together.

    python3 neodct/tools/scratch/build_sb3.py        # rebuilds NeoDCT.sb3

Nothing in the project is hand-drawn. The glyphs are rendered from
`resources/fonts/font.ttf` at the same four sizes the phone uses, and the app
icons are each `icon.png` scaled by the same Pillow call `AppSelector` makes,
so changing the phone's assets and rebuilding changes the project.

## How it draws

The screen is 240×175 with a 30-pixel softkey bar, at two stage units per
phone pixel, which is why the phone fills the stage almost exactly. Every
block takes phone coordinates, so a line that reads
`(0, header_y, screen_w, header_y)` in the Python reads the same here.

Rectangles are one pen stroke each and text is a stamped costume per
character — Scratch has no way to draw a letter, so the metrics tables in the
project stand in for `ImageFont`, and `ui measure` reproduces
`ui.get_text_size` exactly, including the parts of it that are Pillow quirks
rather than decisions.

## Writing an app

Custom blocks in Scratch belong to the sprite that defines them, so an app
written with this framework lives in the **NeoDCT sprite**, alongside the
framework. The green-flag script already there is a demo; replace it.

1. `NeoDCT: start`
2. `items: clear`, then `items: add` for each row
3. call a widget
4. read the answer and loop

A custom block cannot return a value, so a widget leaves its answer in a
variable:

| variable      | what it holds                                          |
| ------------- | ------------------------------------------------------ |
| `nd result`   | the chosen index, or -1 for back; for a dialog, the key |
| `nd text`     | what a text field or the composer collected             |
| `nd items`    | the rows a list widget shows — fill this first          |
| `nd icons`    | a costume name per row, for the app selector            |
| `nd tw`/`nd th` | the last `ui measure`                                |
| `nd str`      | the last string helper's answer                         |
| `nd key`      | the last key, as an evdev code                          |
| `nd wallpaper`| a costume name, or empty for a black background         |

Everything named `nd~…` is the widgets' own scratch space. Leave it alone.

Fonts are numbered 1 = 14px, 2 = 18px, 3 = 20px, 4 = 24px — `font_s`,
`font_md`, `font_n` and `font_xl`. Colours are `white`, `black` and `gray`,
which are the three the framework uses.

## Keys

| phone        | keyboard                    | evdev |
| ------------ | --------------------------- | ----- |
| navi / OK    | Enter                       | 28    |
| Clear / back | `\` or `/`                  | 14    |
| up/down      | arrow keys                  | 103/108 |
| left/right   | arrow keys                  | 105/106 |
| typing       | letters, digits, space . , - | DEV_KEYMAP |

Backspace is the one key a browser will not report to Scratch, so Clear had
to move; everything else is the phone's own mapping.

## What is not here

- **T9** — multi-tap, predictive text and the mode indicator. Not a
  simplification: `_t9_active()` is false without the i2c matrix keypad, so a
  keyboard takes the `DEV_KEYMAP` path on the phone too. This is what QEMU
  shows.
- **The home screen, the app registry, telephony.** This is the UI framework,
  not the OS.
- **`DetailPage`'s shrinking hero image.** The loop that steps a picture down
  in 8-pixel jumps until the body fits cannot trigger at 240×175 with images
  capped at 64 pixels, and Scratch cannot resample a costume the way Pillow
  does, so the picture is drawn at the size it was registered.
- **Wallpaper behind a transparent softkey bar.** `softkey … opaque (0)`
  draws no background at all rather than cutting the strip out of the
  wallpaper again, which comes to the same thing as long as the wallpaper was
  stamped for this frame.

One thing differs from Pillow by design rather than omission: Scratch
antialiases the ends of a pen stroke, so the leftmost and rightmost column of
a filled rectangle is drawn at about 80% alpha instead of 100%. It is a
property of Scratch's pen, it is a fraction of one stage pixel, and there is
no primitive that avoids it.

## Is it really the same?

`neodct/tests/test_scratch_port.py` renders every screen twice — once through
the real framework with `uistub`, once by running the generated blocks in
`emulator.py` — and requires the two images to be identical, pixel for pixel.
A coordinate that drifts by one, a font measured with the wrong metric, an
off-by-one in a scrolling window: all of it fails there.

    python3 -m pytest neodct/tests/test_scratch_port.py -q

`emulator.py` is a small interpreter for the opcodes this generator emits. It
is not a Scratch implementation and is not shipped; it exists so the port can
be checked without opening a browser.

## The files

    sb3.py          writes a .sb3: blocks, costumes, the zip
    blocks.py       one factory per Scratch opcode, plus arithmetic sugar
    assets.py       glyph costumes and font metrics, from font.ttf
    port_core.py    the drawing engine: fills, text, images
    port_widgets.py input, string helpers, the three wrapping algorithms
    port_screens.py the widget classes
    port_demo.py    `NeoDCT: start`, the item helpers, and the demo app
    build_sb3.py    puts it together
    emulator.py     runs the result, for the test

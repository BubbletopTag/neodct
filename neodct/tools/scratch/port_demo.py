"""The blocks an app author actually starts from, and one app built on them.

Custom blocks in Scratch belong to the sprite that defines them, so an app
written with this framework has to live in the same sprite as the framework.
The demo script below is therefore not a sample to copy elsewhere -- it is
the place to start typing.
"""

from blocks import (Block, NOT, eq, forever, goto, gt, if_, join, lt,
                    pen_clear, pen_up, set_size, show as show_sprite,
                    switch_costume, when_flag_clicked)
from port_core import BLANK
from sb3 import Lit

HELP = """NeoDCT for Scratch

This sprite is the phone. Its custom blocks are the UI framework from
System/ui/framework.py, and the screen is drawn with the pen at two stage
units per phone pixel, so all the coordinates below are the phone's own
240x175.

Writing an app
  1. Start with `NeoDCT: start`.
  2. Fill `nd items` with `items: clear` and `items: add`.
  3. Call a widget. Blocks cannot return values, so a widget leaves its
     answer in `nd result` (the chosen index, or -1 for back) and, for the
     text fields, in `nd text`.
  4. Loop.

Keys
  arrows      up / down / left / right
  enter       the middle softkey (28)
  \\ or /      Clear, which is Back (14)
  letters,
  digits,
  space . , - typing, exactly as QEMU's keyboard does it

Fonts are numbered: 1 = 14px, 2 = 18px, 3 = 20px, 4 = 24px -- font_s,
font_md, font_n and font_xl on the phone.

Colours are "white", "black" and "gray", which are the three the framework
uses.

T9 -- multi-tap, predictive text and the mode indicator -- is not here,
because it is not there either: it runs only on the i2c matrix keypad, and a
keyboard takes the DEV_KEYMAP path instead."""


def build_demo(port, api):
    proc, define, v = port.proc, port.define, port.var
    items, icons = port.lists["nd items"], port.lists["nd icons"]
    wallpaper = v("nd wallpaper")

    start = proc("NeoDCT: start", [])
    define(start, [
        show_sprite(),
        set_size(100),
        switch_costume(BLANK),
        pen_up(),
        goto(0, 0),
        Block("looks_seteffectto", {"VALUE": Lit(4, 0)},
              {"EFFECT": ["brightness", None]}),
        pen_clear(),
        wallpaper.set(""),
        items.clear(),
        icons.clear(),
        api["flush"].call(),
    ], "Blanks the screen and puts the sprite back where the drawing blocks "
       "expect it. The sprite has to stay visible -- Scratch will not stamp a "
       "hidden one -- so it wears an empty costume instead.")

    clear_items = proc("items: clear", [])
    define(clear_items, [items.clear(), icons.clear()])

    add_item = proc("items: add %s", ["name"])
    define(add_item, [items.add(add_item.arg("name")), icons.add("")])

    add_icon = proc("items: add %s icon %s", ["name", "icon"])
    define(add_icon, [items.add(add_icon.arg("name")),
                      icons.add(add_icon.arg("icon"))],
           "The icon is a costume name. Every app's icon.png is already a "
           "costume here, at the size AppSelector draws it; your own art needs "
           "an `ui register image` so the framework knows how big it is.")

    _demo_script(port, api)


def _demo_script(port, api):
    v = port.var
    result, text_out = v("nd result"), v("nd text")
    procs = port.procs
    start = procs["NeoDCT: start"]
    clear_items = procs["items: clear"]
    add_item = procs["items: add %s"]
    add_icon = procs["items: add %s icon %s"]

    menu = [
        "Say hello",
        "A warning",
        "Instructions",
        "The app grid",
        "Pick a level",
        "Write a message",
        "About this port",
    ]

    def build_menu():
        return [clear_items.call()] + [add_item.call(name) for name in menu]

    def choice(index, body):
        return if_(eq(result.get(), index), body)

    about = ("NeoDCT for Scratch\n\n"
             "Every screen here is drawn by the blocks in this sprite, which "
             "are System/ui/framework.py from the phone, block for block.\n\n"
             "Press Clear -- backslash -- to go back.")

    instructions = ("Move with the up and down arrows.\n\n"
                    "Enter is the softkey under the screen. Backslash is the "
                    "C key: it deletes a character, and on an empty field it "
                    "backs out of the screen.\n\n"
                    "That is the whole 5190 interface. There is nothing else "
                    "to learn.")

    script = [
        when_flag_clicked(),
        start.call(),
        forever(build_menu() + [
            procs["list show %s app %s from %s"].call("NeoDCT", 1, 0),
            choice(0, [
                procs["field show %s prompt %s start %s filter %s"].call(
                    "Say hello", "Name:", "", "letters"),
                if_(eq(result.get(), 0), [
                    procs["dialog %s title %s icon %s button %s"].call(
                        join("Hello, ", join(text_out.get(), "!")), "", "", "OK"),
                ]),
            ]),
            choice(1, [
                procs["dialog %s title %s icon %s button %s"].call(
                    "BATTERY LOW!", "", "warning", "OK"),
            ]),
            choice(2, [
                procs["read text %s more %s back %s"].call(
                    instructions, "More", "Back"),
            ]),
            choice(3, [
                clear_items.call(),
                add_icon.call("Phonebook", "PhoneBook"),
                add_icon.call("Messages", "Messages"),
                add_icon.call("Clock", "Clock"),
                add_icon.call("Games", "Games"),
                add_icon.call("Settings", "Settings"),
                procs["app selector show"].call(),
                if_(NOT(lt(result.get(), 0)), [
                    procs["dialog %s title %s icon %s button %s"].call(
                        "Not written yet.", "", "", "OK"),
                ]),
            ]),
            choice(4, [
                procs["level select current %s of %s"].call(1, 9),
                if_(gt(result.get(), 0), [
                    procs["info %s value %s button %s"].call(
                        "Level", result.get(), "Back"),
                ]),
            ]),
            choice(5, [
                procs["compose show %s softkey %s start %s"].call(
                    "Write", "Options", ""),
                if_(eq(result.get(), 0), [
                    procs["dialog %s title %s icon %s button %s"].call(
                        "Saved!", "", "", "OK"),
                ]),
            ]),
            choice(6, [
                procs["detail %s sub %s badge %s body %s image %s header %s "
                      "softkey %s"].call(
                    "NeoDCT", "A Nokia 5190, rebuilt", "0.4.10a", about,
                    "warning", "About", "OK"),
            ]),
        ]),
    ]
    port.sprite.script(script, 60, 60)
    port.sprite.comment(HELP, 500, 60, width=380, height=560)

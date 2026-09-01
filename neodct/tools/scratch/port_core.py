"""The NeoDCT drawing engine, expressed as Scratch custom blocks.

Everything below works in *phone* pixels -- a 240x175 screen with the origin
at the top left, exactly as Pillow sees it -- and converts to Scratch's
centre-origin stage only at the moment something is drawn. Widget code ported
from `System/ui/framework.py` therefore keeps its coordinates unchanged, which
is the whole point: a line that reads `(0, header_y, screen_w, header_y)` in
Python reads the same here.

Two Scratch limitations shape the rest of it. There is no text, so a costume
per glyph is stamped and the metrics tables in `assets.py` stand in for
`ImageFont`. And a custom block cannot return a value, so blocks that would
have returned one leave it in a variable -- `nd result` for a chosen index,
`nd text` for typed text, `nd tw`/`nd th` for a measurement.
"""

from blocks import (AND, Block, Ex, NOT, Proc, ScratchList, Var,
                    costume_number, eq, floor, goto, gt, if_, if_else, join,
                    length_of, letter_of, lt, pen_clear, pen_colour, pen_down,
                    pen_size, pen_stamp, pen_up, repeat, switch_costume)
from sb3 import Lit

W = 240
H = 175
SOFTKEY_H = 30
CONTENT_BOTTOM = H - SOFTKEY_H          # 145
HEADER_Y = max(30, int(H * 0.11))       # 30
SCALE = 2

# The phone's fonts, as the numbers the Scratch blocks take.
FONT_S, FONT_MD, FONT_N, FONT_XL = 1, 2, 3, 4

BLANK = "blank"


def sx(x):
    """Stage x of the left edge of phone column `x`."""
    return 2 * Ex(x) - W


def sy(y):
    """Stage y of the top edge of phone row `y`."""
    return H - 2 * Ex(y)


# The variables and lists an app author uses. Everything else is scratch
# space for the widgets, and gets a "nd~" prefix so the editor's palette
# sorts it out of the way -- there are a lot of them, and only these fifteen
# are worth reading.
PUBLIC = {
    "nd result", "nd text", "nd key", "nd items", "nd icons", "nd wallpaper",
    "nd newline", "nd tw", "nd th", "nd str", "nd font", "nd iw", "nd ih",
    "nd hw", "nd lines", "nd image name", "nd image w", "nd image h",
    "nd keys",
}


def public_name(name):
    if name in PUBLIC or not name.startswith("nd "):
        return name
    return "nd~" + name[3:]


class Port:
    """Builds the custom blocks onto one sprite."""

    def __init__(self, sprite, stage):
        self.sprite = sprite
        self.stage = stage
        self.vars = {}
        self.lists = {}
        self.procs = {}
        self._column = 0
        self._row = 0

    # --- data -------------------------------------------------------------

    def var(self, name, value=0, local=False):
        if name in self.vars:
            return self.vars[name]
        owner = self.sprite if local else self.stage
        shown = public_name(name)
        v = Var(shown, owner.variable(shown, value))
        self.vars[name] = v
        return v

    def lst(self, name, items=(), local=False):
        if name in self.lists:
            return self.lists[name]
        owner = self.sprite if local else self.stage
        shown = public_name(name)
        l = ScratchList(shown, owner.list(shown, items))
        self.lists[name] = l
        return l

    # --- procedures -------------------------------------------------------

    def proc(self, proccode, argnames=(), warp=True, argtypes=None):
        p = Proc(proccode, list(argnames), warp=warp, argtypes=argtypes)
        self.procs[proccode] = p
        return p

    def define(self, proc, body, comment=None):
        x, y = self._place()
        script = proc.definition(body)
        self.sprite.script(script, x, y)
        if comment:
            self.sprite.comment(comment, x + 480, y, width=340, height=120)
        return proc

    def _place(self):
        # The demo app sits at the origin; definitions start to the right of
        # it, so opening the project lands on the thing worth reading first.
        x = 900 + self._column * 720
        y = 60 + self._row * 900
        self._row += 1
        if self._row >= 6:
            self._row = 0
            self._column += 1
        return x, y


def build_engine(port, glyphs, metrics, images, charset):
    """Define the engine blocks. Returns a dict of the ones widgets call."""
    v = port.var
    lst = port.lst

    # --- tables -----------------------------------------------------------
    #
    # One entry per costume, so a glyph's metrics are found by switching to
    # its costume and reading the costume number back: Scratch's own name
    # lookup is the only case-sensitive string comparison it has, and 'a'
    # and 'A' are different glyphs.
    n_costumes = 1 + len(images) + len(glyphs)
    adv = [0] * n_costumes
    gdx = [0] * n_costumes
    gdy = [0] * n_costumes
    gw = [0] * n_costumes
    gh = [0] * n_costumes
    first_glyph = 1 + len(images)
    for i, glyph in enumerate(glyphs):
        slot = first_glyph + i
        adv[slot] = glyph.advance
        gdx[slot] = glyph.dx
        gdy[slot] = glyph.dy
        gw[slot] = glyph.width
        gh[slot] = glyph.height

    lst("nd glyph adv", adv)
    lst("nd glyph dx", gdx)
    lst("nd glyph dy", gdy)
    lst("nd glyph w", gw)
    lst("nd glyph h", gh)
    lst("nd font prefix", [p + "_" for p, _ in
                           [("s", 14), ("m", 18), ("n", 20), ("x", 24)]])
    lst("nd font space", [metrics[p]["space"] for p in ("s", "m", "n", "x")])
    lst("nd font ascent", [metrics[p]["ascent"] for p in ("s", "m", "n", "x")])
    lst("nd ascii", list(charset))

    lst("nd image name", [name for name, _, _ in images])
    lst("nd image w", [w for _, w, _ in images])
    lst("nd image h", [h for _, _, h in images])

    lst("nd items", [])
    lst("nd icons", [])
    lst("nd lines", [])

    # --- variables --------------------------------------------------------
    v("nd tw", 0)          # width of the last measured text
    v("nd th", 0)          # its height
    v("nd str", "")        # result of the string helpers
    v("nd result", -1)     # what a widget's show() would have returned
    v("nd text", "")       # what a text field returned
    v("nd key", 0)         # the last key press, as an evdev code
    v("nd wallpaper", "")  # costume name, or empty for a black background
    v("nd newline", "\n")  # so app code can build multi-line strings
    v("nd iw", 0)
    v("nd ih", 0)

    api = {}

    # --- colour -----------------------------------------------------------

    pen = port.proc("ui pen %s", ["colour"])
    port.define(pen, [
        if_else(eq(pen.arg("colour"), "black"),
                [pen_colour("#000000")],
                [if_else(eq(pen.arg("colour"), "gray"),
                         [pen_colour("#808080")],
                         [pen_colour("#ffffff")])]),
    ], "Only three colours are ever asked for; anything else is white.")
    api["pen"] = pen

    # --- filled rectangle -------------------------------------------------
    #
    # Pillow's rectangle is inclusive of both corners, so these arguments are
    # x0 y0 x1 y1 and not x y w h. One pen stroke does the whole rectangle:
    # a stroke of thickness 2h covers h phone rows exactly, and its round
    # caps only cost a fraction of an alpha at the two end pixels.

    fx0, fy0, fx1, fy1 = v("nd fx0"), v("nd fy0"), v("nd fx1"), v("nd fy1")
    fill = port.proc("ui fill %s %s to %s %s colour %s",
                     ["x0", "y0", "x1", "y1", "colour"])
    port.define(fill, [
        fx0.set(floor(fill.arg("x0"))),
        fy0.set(floor(fill.arg("y0"))),
        fx1.set(floor(fill.arg("x1"))),
        fy1.set(floor(fill.arg("y1"))),
        if_(lt(fx0.get(), 0), [fx0.set(0)]),
        if_(lt(fy0.get(), 0), [fy0.set(0)]),
        if_(gt(fx1.get(), W - 1), [fx1.set(W - 1)]),
        if_(gt(fy1.get(), H - 1), [fy1.set(H - 1)]),
        if_(AND(NOT(gt(fx0.get(), fx1.get())), NOT(gt(fy0.get(), fy1.get()))), [
            pen.call(fill.arg("colour")),
            pen_size(2 * (fy1 - fy0 + 1)),
            goto(2 * fx0 - (W - 1), H - 1 - fy0 - fy1),
            pen_down(),
            goto(2 * fx1 - (W - 1), H - 1 - fy0 - fy1),
            pen_up(),
        ]),
    ], "Pillow's rectangle(): x0 y0 x1 y1, both corners included, clipped to "
       "the screen the way drawing into a 240x175 image is.")
    api["fill"] = fill

    box = port.proc("ui outline %s %s to %s %s colour %s",
                    ["x0", "y0", "x1", "y1", "colour"])
    port.define(box, [
        fill.call(box.arg("x0"), box.arg("y0"), box.arg("x1"), box.arg("y0"),
                  box.arg("colour")),
        fill.call(box.arg("x0"), box.arg("y1"), box.arg("x1"), box.arg("y1"),
                  box.arg("colour")),
        fill.call(box.arg("x0"), box.arg("y0"), box.arg("x0"), box.arg("y1"),
                  box.arg("colour")),
        fill.call(box.arg("x1"), box.arg("y0"), box.arg("x1"), box.arg("y1"),
                  box.arg("colour")),
    ])
    api["box"] = box

    clear = port.proc("ui clear screen", [])
    port.define(clear, [pen_clear()],
                "The backdrop is the phone's black screen, so erasing the pen "
                "layer is the whole-screen black fill.")
    api["clear"] = clear

    clear_content = port.proc("ui clear content", [])
    port.define(clear_content, [fill.call(0, 0, W - 1, CONTENT_BOTTOM, "black")])
    api["clear_content"] = clear_content

    # --- glyph lookup -----------------------------------------------------

    t_pre, t_pen, t_i, t_ch = v("nd t pre", ""), v("nd t pen", 0), v("nd t i", 0), v("nd t ch", "")
    t_gi, t_top, t_bot, t_right = v("nd t gi", 0), v("nd t top", 0), v("nd t bot", 0), v("nd t right", 0)
    t_ink = v("nd t ink", 0)
    ascii_list = port.lists["nd ascii"]
    prefix_list = port.lists["nd font prefix"]
    space_list = port.lists["nd font space"]
    ascent_list = port.lists["nd font ascent"]
    adv_list = port.lists["nd glyph adv"]
    dx_list = port.lists["nd glyph dx"]
    dy_list = port.lists["nd glyph dy"]
    gw_list = port.lists["nd glyph w"]
    gh_list = port.lists["nd glyph h"]

    def lookup_glyph():
        """Leave the costume for `nd t ch` selected and its index in nd t gi.

        The membership test is Scratch's, so it ignores case -- which is
        exactly what is wanted here: it is only asked whether the character
        is one the font has at all, and the costume lookup that follows is
        the case-sensitive half.
        """
        return [
            if_else(ascii_list.contains(t_ch.get()),
                    [switch_costume(join(t_pre.get(), t_ch.get()))],
                    [switch_costume(join(t_pre.get(), "?"))]),
            t_gi.set(costume_number()),
        ]

    # --- measurement ------------------------------------------------------

    t_x, t_y = v("nd t x", 0), v("nd t y", 0)

    measure = port.proc("ui measure %s font %s", ["text", "font"])
    port.define(measure, [
        t_pre.set(prefix_list.item(measure.arg("font"))),
        t_pen.set(0),
        t_right.set(0),
        t_ink.set(0),
        t_top.set(0),
        t_bot.set(ascent_list.item(measure.arg("font"))),
        t_i.set(1),
        repeat(length_of(measure.arg("text")), [
            t_ch.set(letter_of(t_i.get(), measure.arg("text"))),
            if_else(eq(t_ch.get(), " "),
                    [t_pen.change(space_list.item(measure.arg("font")))],
                    lookup_glyph() + [
                        t_x.set(rounded(t_pen) + dx_list.item(t_gi.get())
                                + gw_list.item(t_gi.get())),
                        if_(gt(t_x.get(), t_right.get()), [t_right.set(t_x.get())]),
                        t_y.set(dy_list.item(t_gi.get())),
                        if_(gt(gh_list.item(t_gi.get()), 0), [
                            if_else(eq(t_ink.get(), 0),
                                    [t_ink.set(1), t_top.set(t_y.get())],
                                    [if_(lt(t_y.get(), t_top.get()),
                                         [t_top.set(t_y.get())])]),
                            if_(gt(t_y + gh_list.item(t_gi.get()), t_bot.get()),
                                [t_bot.set(t_y + gh_list.item(t_gi.get()))]),
                        ]),
                        t_pen.change(adv_list.item(t_gi.get())),
                    ]),
            t_i.change(1),
        ]),
        v("nd tw").set(rounded(t_pen)),
        if_(gt(t_right.get(), v("nd tw").get()), [v("nd tw").set(t_right.get())]),
        if_else(eq(t_ink.get(), 0),
                [v("nd th").set(0)],
                [v("nd th").set(t_bot - t_top)]),
        switch_costume(BLANK),
    ], "ui.get_text_size(). Pillow reports the greater of the rounded pen "
       "advance and the rightmost lit pixel, and never lets the bottom edge "
       "rise above the baseline -- both matter, because every widget centres "
       "something with this number.")
    api["measure"] = measure

    # --- drawing text -----------------------------------------------------

    write = port.proc("ui text %s at %s %s font %s colour %s",
                      ["text", "x", "y", "font", "colour"])
    port.define(write, [
        Block("looks_seteffectto", {"VALUE": inp_num(0)},
              {"EFFECT": ["brightness", None]}),
        if_(eq(write.arg("colour"), "black"),
            [Block("looks_seteffectto", {"VALUE": inp_num(-100)},
                   {"EFFECT": ["brightness", None]})]),
        if_(eq(write.arg("colour"), "gray"),
            [Block("looks_seteffectto", {"VALUE": inp_num(-50)},
                   {"EFFECT": ["brightness", None]})]),
        t_pre.set(prefix_list.item(write.arg("font"))),
        t_pen.set(0),
        t_i.set(1),
        repeat(length_of(write.arg("text")), [
            t_ch.set(letter_of(t_i.get(), write.arg("text"))),
            if_else(eq(t_ch.get(), " "),
                    [t_pen.change(space_list.item(write.arg("font")))],
                    lookup_glyph() + [
                        t_x.set(Ex(write.arg("x")) + rounded(t_pen)
                                + dx_list.item(t_gi.get())),
                        t_y.set(Ex(write.arg("y")) + dy_list.item(t_gi.get())),
                        if_(AND(lt(t_x.get(), W),
                                gt(t_x + gw_list.item(t_gi.get()), 0),
                                lt(t_y.get(), H),
                                gt(t_y + gh_list.item(t_gi.get()), 0)), [
                            goto(2 * t_x - W, H - 2 * t_y),
                            pen_stamp(),
                        ]),
                        t_pen.change(adv_list.item(t_gi.get())),
                    ]),
            t_i.change(1),
        ]),
        switch_costume(BLANK),
    ], "draw.text(). The (x, y) given is Pillow's: the top-left of the "
       "ascender box, not of the ink.")
    api["write"] = write

    # --- stamped art ------------------------------------------------------

    img_name = port.lists["nd image name"]
    img_w = port.lists["nd image w"]
    img_h = port.lists["nd image h"]

    image_size = port.proc("ui image size %s", ["name"])
    port.define(image_size, [
        t_gi.set(img_name.index_of(image_size.arg("name"))),
        if_else(eq(t_gi.get(), 0),
                [v("nd iw").set(0), v("nd ih").set(0)],
                [v("nd iw").set(img_w.item(t_gi.get())),
                 v("nd ih").set(img_h.item(t_gi.get()))]),
    ], "Scratch cannot report a costume's size, so pictures are registered "
       "in a table when the project is built. Add your own with "
       "'ui register image'.")
    api["image_size"] = image_size

    register = port.proc("ui register image %s w %s h %s", ["name", "w", "h"])
    port.define(register, [
        if_(eq(img_name.index_of(register.arg("name")), 0), [
            img_name.add(register.arg("name")),
            img_w.add(register.arg("w")),
            img_h.add(register.arg("h")),
        ]),
    ])
    api["register_image"] = register

    stamp_image = port.proc("ui image %s at %s %s", ["name", "x", "y"])
    port.define(stamp_image, [
        Block("looks_seteffectto", {"VALUE": inp_num(0)},
              {"EFFECT": ["brightness", None]}),
        switch_costume(stamp_image.arg("name")),
        goto(sx(stamp_image.arg("x")), sy(stamp_image.arg("y"))),
        pen_stamp(),
        switch_costume(BLANK),
    ], "canvas.paste(img, (x, y), img) -- the picture's own alpha is kept, so "
       "a transparent icon does not punch a black square in the wallpaper.")
    api["stamp_image"] = stamp_image

    return api


def rounded(value):
    from blocks import rnd
    return rnd(value)


def inp_num(value):
    return Lit(4, value)

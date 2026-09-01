"""The widget screens themselves. Split from port_widgets.py for length."""

from blocks import (AND, Ex, NOT, OR, eq, floor, gt, if_, if_else, join,
                    length_of, letter_of, lt, repeat, repeat_until, timer)
from port_core import (CONTENT_BOTTOM, FONT_MD, FONT_N, FONT_S, FONT_XL, H,
                       HEADER_Y, SOFTKEY_H, W)

MARGIN = 8
LADDER = "321"          # _font_ladder(ui, "font_n", "font_md", "font_s")


def half(value):
    return floor(Ex(value) / 2)


def clamp0(var):
    """`max(0, v)` applied in place."""
    return if_(lt(var.get(), 0), [var.set(0)])


def build(port, api):
    v, proc, define = port.var, port.proc, port.define
    fill, box = api["fill"], api["box"]
    write, measure = api["write"], api["measure"]
    tw, th = v("nd tw"), v("nd th")
    nd_str = v("nd str")

    # widget-local scratch space; widgets are not reentrant, which the
    # framework's own blocking show() loops are not either
    d = {name: v("nd d " + name, 0) for name in
         "i x y w h n t idx sel win max notch top run done page".split()}
    ds = {name: v("nd d " + name, "") for name in ("s", "line")}

    # What the screen builders below share. Spelled out rather than handed
    # locals(), so that a name going missing is a NameError here and not a
    # KeyError three functions away.
    shared = {
        "fill": fill, "box": box, "write": write, "measure": measure,
        "stamp_image": api["stamp_image"], "image_size": api["image_size"],
        "tw": tw, "th": th, "nd_str": nd_str,
        "result": v("nd result"), "text_out": v("nd text"), "key": v("nd key"),
        "font_out": v("nd font"), "iw": v("nd iw"), "ih": v("nd ih"),
        "items": port.lists["nd items"], "icons": port.lists["nd icons"],
        "lines": port.lists["nd lines"], "wallpaper": v("nd wallpaper"),
        "poll": api["poll"], "flush": api["flush"], "wait_key": api["wait_key"],
        "d": d, "ds": ds,
    }

    # ------------------------------------------------------------------
    # SoftKeyBar (framework.py:447)
    # ------------------------------------------------------------------
    softkey = proc("softkey %s opaque %s", ["text", "opaque"])
    define(softkey, [
        if_(eq(softkey.arg("opaque"), 1),
            [fill.call(0, H - SOFTKEY_H, W, H, "black")]),
        if_(NOT(eq(softkey.arg("text"), "")), [
            measure.call(softkey.arg("text"), FONT_N),
            write.call(softkey.arg("text"), half(W - tw.get()),
                       (H - SOFTKEY_H) + half(SOFTKEY_H - th.get()),
                       FONT_N, "white"),
        ]),
    ], "SoftKeyBar.update(). opaque=0 is the home screen's transparent bar: it "
       "leaves whatever is already there, which is the wallpaper, instead of "
       "cutting a black strip out of it.")
    api["softkey"] = softkey

    # ------------------------------------------------------------------
    # HeaderWidget (framework.py:502) -- the "2-1" breadcrumb, top right
    # ------------------------------------------------------------------
    header_text = proc("ui header text %s sub %s", ["root", "sub"])
    define(header_text, [
        if_else(eq(header_text.arg("sub"), ""),
                [nd_str.set(header_text.arg("root"))],
                [nd_str.set(join(join(header_text.arg("root"), "-"),
                                 header_text.arg("sub")))]),
    ])

    hw = v("nd hw", 0)
    header_width = proc("ui header width %s sub %s", ["root", "sub"])
    define(header_width, [
        header_text.call(header_width.arg("root"), header_width.arg("sub")),
        measure.call(nd_str.get(), FONT_N),
        hw.set(5 + tw.get()),
    ], "What the breadcrumb costs a title sharing its row: a 'Remote Shell' "
       "against a '9007-7' counter used to come out overlapped.")

    header = proc("header %s sub %s", ["root", "sub"])
    define(header, [
        header_text.call(header.arg("root"), header.arg("sub")),
        measure.call(nd_str.get(), FONT_N),
        write.call(nd_str.get(), W - 5 - tw.get(), 5, FONT_N, "white"),
    ])
    api["header"] = header

    _app_selector(port, api, shared)
    _vertical_list(port, api, shared)
    _paged_list(port, api, shared)
    _text_fields(port, api, shared)
    _dialogs(port, api, shared)
    _pages(port, api, shared)
    return api


# ----------------------------------------------------------------------
# AppSelector (framework.py:307)
# ----------------------------------------------------------------------

ICON_Y = HEADER_Y + max(24, int((CONTENT_BOTTOM - HEADER_Y) * 0.22))
ICON_CAP = min(175, max(24, CONTENT_BOTTOM - ICON_Y - 8))


def _app_selector(port, api, s):
    proc, define = port.proc, port.define
    fill, box, write, measure = s["fill"], s["box"], s["write"], s["measure"]
    stamp_image, image_size = s["stamp_image"], s["image_size"]
    tw, th, iw = s["tw"], s["th"], s["iw"]
    items, icons = s["items"], s["icons"]
    wallpaper, key = s["wallpaper"], s["key"]
    d, ds = s["d"], s["ds"]
    flush, wait_key = s["flush"], s["wait_key"]

    place_x = (W - ICON_CAP) // 2

    draw = proc("app selector draw", [])
    define(draw, [
        if_else(eq(wallpaper.get(), ""),
                [fill.call(0, 0, W, H, "black")],
                [stamp_image.call(wallpaper.get(), 0, 0)]),
        if_else(eq(items.length(), 0), [
            measure.call("No Apps", FONT_N),
            d["y"].set(HEADER_Y + half(CONTENT_BOTTOM - HEADER_Y - th.get())),
            if_(lt(d["y"].get(), HEADER_Y), [d["y"].set(HEADER_Y)]),
            write.call("No Apps", half(W - tw.get()), d["y"].get(), FONT_N, "white"),
        ], [
            ds["s"].set(items.item(d["sel"] + 1)),
            measure.call(ds["s"].get(), FONT_XL),
            write.call(ds["s"].get(), half(W - tw.get()), HEADER_Y - 16,
                       FONT_XL, "white"),
            ds["line"].set(icons.item(d["sel"] + 1)),
            # An item with no icon at all draws nothing here; the outlined
            # box with a "?" is for an icon that was named and could not be
            # loaded, which is what get_image() returning None means.
            if_(NOT(eq(ds["line"].get(), "")), [
                image_size.call(ds["line"].get()),
                if_else(gt(iw.get(), 0), [
                    stamp_image.call(ds["line"].get(), half(W - iw.get()), ICON_Y),
                ], [
                    box.call(place_x, ICON_Y, place_x + ICON_CAP,
                             ICON_Y + ICON_CAP, "white"),
                    measure.call("?", FONT_XL),
                    write.call("?", place_x + half(ICON_CAP - tw.get()),
                               ICON_Y + half(ICON_CAP - th.get()), FONT_XL,
                               "white"),
                ]),
            ]),
            measure.call("Select", FONT_N),
            d["y"].set(half(SOFTKEY_H - th.get())),
            clamp0(d["y"]),
            write.call("Select", half(W - tw.get()), CONTENT_BOTTOM + d["y"].get(),
                       FONT_N, "white"),
            # the Nokia scrollbar down the right edge
            fill.call(W - 8, HEADER_Y + 6, W - 7, CONTENT_BOTTOM - 10, "white"),
            if_else(gt(items.length(), 1),
                    [d["notch"].set((HEADER_Y + 6)
                                    + d["sel"] * ((CONTENT_BOTTOM - 10 - HEADER_Y - 6)
                                                  / (items.length() - 1)))],
                    [d["notch"].set(HEADER_Y + 6)]),
            d["notch"].set(floor(d["notch"].get())),
            fill.call(W - 12, d["notch"] - 3, W - 6, d["notch"] + 3, "white"),
            measure.call(d["sel"] + 1, FONT_N),
            write.call(d["sel"] + 1, W - 5 - tw.get(), 10, FONT_N, "white"),
        ]),
    ], "AppSelector.draw(): one app per screen, its name above the icon and "
       "the page number in the corner.")

    show = proc("app selector show", [], warp=False)
    define(show, [
        s["d"]["sel"].set(0),
        flush.call(),
        draw.call(),
        s["result"].set(-1),
        d["done"].set(0),
        repeat_until(eq(d["done"].get(), 1), [
            wait_key.call(),
            if_else(eq(items.length(), 0), [
                if_(OR(eq(key.get(), 14), eq(key.get(), 28)),
                    [s["result"].set(-1), d["done"].set(1)]),
            ], [
                if_(eq(key.get(), 108), [
                    d["sel"].set((d["sel"] + 1) % items.length()),
                    draw.call(),
                ]),
                if_(eq(key.get(), 103), [
                    d["sel"].set((d["sel"] - 1) % items.length()),
                    draw.call(),
                ]),
                if_(eq(key.get(), 28), [s["result"].set(d["sel"].get()),
                                        d["done"].set(1)]),
                if_(eq(key.get(), 14), [s["result"].set(-1), d["done"].set(1)]),
            ]),
        ]),
    ], "Leaves the chosen index in `nd result`, or -1 for back. An empty list "
       "only lets you back out -- navigating it would divide by zero.")
    api["app_selector"] = show


# ----------------------------------------------------------------------
# VerticalList (framework.py:539)
# ----------------------------------------------------------------------

VL_Y_START = HEADER_Y + 10                       # 40
VL_CONTENT_H = max(1, CONTENT_BOTTOM - VL_Y_START - 4)
VL_LINE_H = max(28, VL_CONTENT_H // 3)
VL_ITEM_H = max(24, VL_LINE_H - 4)
VL_MAX_LINES = min(3, max(1, VL_CONTENT_H // VL_LINE_H))
VL_BAR_X = W - 5
VL_SEL_RIGHT = max(20, VL_BAR_X - 10)


def _vertical_list(port, api, s):
    proc, define, v = port.proc, port.define, port.var
    fill, write, measure = s["fill"], s["write"], s["measure"]
    fit_text, header = api["fit_text"], api["header"]
    th, nd_str, hw = s["th"], s["nd_str"], v("nd hw")
    items, key, result = s["items"], s["key"], s["result"]
    d, ds = s["d"], s["ds"]
    header_width_proc = port.procs["ui header width %s sub %s"]

    draw = proc("list draw %s app %s", ["title", "app"])
    define(draw, [
        fill.call(0, 0, W, CONTENT_BOTTOM, "black"),
        header_width_proc.call(draw.arg("app"), d["sel"] + 1),
        fit_text.call(draw.arg("title"), FONT_XL, W - 5 - hw.get() - 6),
        write.call(nd_str.get(), 5, 0, FONT_XL, "white"),
        header.call(draw.arg("app"), d["sel"] + 1),
        fill.call(0, HEADER_Y, W, HEADER_Y, "white"),
        if_(lt(d["sel"].get(), d["win"].get()), [d["win"].set(d["sel"].get())]),
        d["max"].set(items.length() - VL_MAX_LINES),
        clamp0(d["max"]),
        if_(gt(d["win"].get(), d["max"].get()), [d["win"].set(d["max"].get())]),
        d["i"].set(0),
        repeat(VL_MAX_LINES, [
            d["idx"].set(d["win"] + d["i"]),
            if_(lt(d["idx"].get(), items.length()), [
                d["y"].set(VL_Y_START + d["i"] * VL_LINE_H),
                ds["line"].set(items.item(d["idx"] + 1)),
                measure.call(ds["line"].get(), FONT_MD),
                d["t"].set(half(VL_ITEM_H - th.get())),
                clamp0(d["t"]),
                d["t"].set(d["y"] + d["t"]),
                if_else(eq(d["idx"].get(), d["sel"].get()), [
                    fill.call(0, d["y"].get(), VL_SEL_RIGHT,
                              d["y"] + VL_ITEM_H, "white"),
                    write.call(ds["line"].get(), 10, d["t"].get(), FONT_MD, "black"),
                ], [
                    write.call(ds["line"].get(), 10, d["t"].get(), FONT_MD, "white"),
                ]),
            ]),
            d["i"].change(1),
        ]),
        fill.call(VL_BAR_X, VL_Y_START, VL_BAR_X, CONTENT_BOTTOM - 5, "gray"),
        if_else(gt(items.length(), 1),
                [d["notch"].set(VL_Y_START + d["sel"]
                                * ((CONTENT_BOTTOM - 5 - VL_Y_START)
                                   / (items.length() - 1)))],
                [d["notch"].set(VL_Y_START)]),
        d["notch"].set(floor(d["notch"].get())),
        fill.call(VL_BAR_X - 2, d["notch"] - 3, VL_BAR_X + 2, d["notch"] + 3,
                  "white"),
    ], "VerticalList.draw(): three rows of a scrolling window, the selected "
       "one inverted, and the title trimmed to what the breadcrumb leaves.")

    show = proc("list show %s app %s from %s", ["title", "app", "from"],
                warp=False)
    define(show, [
        d["sel"].set(show.arg("from")),
        d["win"].set(0),
        draw.call(show.arg("title"), show.arg("app")),
        result.set(-1),
        d["done"].set(0),
        repeat_until(eq(d["done"].get(), 1), [
            s["wait_key"].call(),
            if_(eq(key.get(), 108), [
                if_(lt(d["sel"].get(), items.length() - 1), [
                    d["sel"].change(1),
                    if_(NOT(lt(d["sel"].get(), d["win"] + VL_MAX_LINES)),
                        [d["win"].change(1)]),
                ]),
                draw.call(show.arg("title"), show.arg("app")),
            ]),
            if_(eq(key.get(), 103), [
                if_(gt(d["sel"].get(), 0), [
                    d["sel"].change(-1),
                    if_(lt(d["sel"].get(), d["win"].get()), [d["win"].change(-1)]),
                ]),
                draw.call(show.arg("title"), show.arg("app")),
            ]),
            if_(AND(NOT(lt(key.get(), 2)), NOT(gt(key.get(), 10))), [
                if_(lt(key - 2, items.length()),
                    [result.set(key - 2), d["done"].set(1)]),
            ]),
            if_(eq(key.get(), 28), [result.set(d["sel"].get()), d["done"].set(1)]),
            if_(eq(key.get(), 14), [result.set(-1), d["done"].set(1)]),
        ]),
    ], "Digits 1-9 jump straight to that row, like the 5190's menus.")
    api["vertical_list"] = show
    api["vertical_list_start"] = show
    api["vertical_list_draw"] = draw


# ----------------------------------------------------------------------
# PagedList (framework.py:1175) and LevelSelector (framework.py:1452)
# ----------------------------------------------------------------------

PL_TOP = HEADER_Y + 8                    # 38
PL_BOTTOM = CONTENT_BOTTOM - 10          # 135
PL_BAR_X = W - 5                         # 235
PL_MAX_W = max(20, PL_BAR_X - 12)        # 223


def _paged_list(port, api, s):
    proc, define, v, lst = port.proc, port.define, port.var, port.lst
    fill, write, measure = s["fill"], s["write"], s["measure"]
    softkey, header = api["softkey"], api["header"]
    slice_ = api["slice"]
    tw, th, nd_str = s["tw"], s["th"], s["nd_str"]
    items, key, result, lines = s["items"], s["key"], s["result"], s["lines"]
    d, ds = s["d"], s["ds"]
    newline = v("nd newline")

    words = lst("nd words", [])
    p_word, p_i = v("nd pw word", ""), v("nd pw i", 0)
    p_cur, p_cand, p_t = v("nd pw cur", ""), v("nd pw cand", ""), v("nd pw t", "")
    tr_t, tr_done = v("nd tr t", ""), v("nd tr done", 0)

    trim = proc("ui trim %s font %s width %s", ["text", "font", "width"])
    define(trim, [
        tr_t.set(trim.arg("text")),
        tr_done.set(0),
        repeat_until(eq(tr_done.get(), 1), [
            if_else(eq(tr_t.get(), ""), [tr_done.set(1)], [
                measure.call(join(tr_t.get(), "..."), trim.arg("font")),
                if_else(NOT(gt(tw.get(), trim.arg("width"))),
                        [tr_done.set(1)],
                        [slice_.call(tr_t.get(), 1, length_of(tr_t.get()) - 1),
                         tr_t.set(nd_str.get())]),
            ]),
        ]),
        nd_str.set(tr_t.get()),
    ], "while trimmed and width(trimmed + \"...\") > max_w: drop a character. "
       "Comes back empty when even one character will not fit.")

    split = proc("ui split words %s", ["text"])
    define(split, [
        words.clear(),
        p_word.set(""),
        p_i.set(1),
        repeat(length_of(split.arg("text")) + 1, [
            ds["s"].set(letter_of(p_i.get(), split.arg("text"))),
            if_else(OR(eq(ds["s"].get(), " "), eq(ds["s"].get(), newline.get()),
                       gt(p_i.get(), length_of(split.arg("text")))),
                    [if_(NOT(eq(p_word.get(), "")), [words.add(p_word.get())]),
                     p_word.set("")],
                    [p_word.set(join(p_word.get(), ds["s"].get()))]),
            p_i.change(1),
        ]),
    ], "str.split(): any run of whitespace, and no empty words.")

    pwrap = proc("ui page wrap %s font %s width %s lines %s",
                 ["text", "font", "width", "lines"])
    define(pwrap, [
        lines.clear(),
        split.call(pwrap.arg("text")),
        if_else(eq(words.length(), 0), [lines.add("")], [
            p_cur.set(""),
            p_i.set(1),
            repeat_until(OR(gt(p_i.get(), words.length()),
                            NOT(lt(lines.length(), pwrap.arg("lines")))), [
                if_else(eq(p_cur.get(), ""),
                        [p_cand.set(words.item(p_i.get()))],
                        [p_cand.set(join(join(p_cur.get(), " "),
                                         words.item(p_i.get())))]),
                measure.call(p_cand.get(), pwrap.arg("font")),
                if_else(NOT(gt(tw.get(), pwrap.arg("width"))),
                        [p_cur.set(p_cand.get()), p_i.change(1)],
                        [if_else(NOT(eq(p_cur.get(), "")),
                                 [lines.add(p_cur.get()), p_cur.set("")],
                                 [
                                     trim.call(words.item(p_i.get()),
                                               pwrap.arg("font"),
                                               pwrap.arg("width")),
                                     p_t.set(nd_str.get()),
                                     if_else(NOT(eq(p_t.get(), "")),
                                             [if_else(lt(p_i.get(), words.length()),
                                                      [lines.add(join(p_t.get(), "..."))],
                                                      [lines.add(p_t.get())])],
                                             [lines.add("...")]),
                                     p_i.change(1),
                                 ])]),
            ]),
            if_(AND(lt(lines.length(), pwrap.arg("lines")),
                    NOT(eq(p_cur.get(), ""))), [lines.add(p_cur.get())]),
            if_(NOT(gt(p_i.get(), words.length())), [
                ds["s"].set(lines.item(lines.length())),
                slice_.call(ds["s"].get(), length_of(ds["s"].get()) - 2, 3),
                if_(NOT(eq(nd_str.get(), "...")), [
                    trim.call(ds["s"].get(), pwrap.arg("font"),
                              pwrap.arg("width")),
                    p_t.set(nd_str.get()),
                    if_else(NOT(eq(p_t.get(), "")),
                            [lines.replace(lines.length(), join(p_t.get(), "..."))],
                            [lines.replace(lines.length(), "...")]),
                ]),
            ]),
        ]),
    ], "PagedList._wrap_to_lines(): at most two lines, and whatever is left "
       "over turns the last one into an ellipsis.")

    draw = proc("page draw %s app %s hint %s", ["title", "app", "hint"])
    define(draw, [
        fill.call(0, 0, W, H, "black"),
        write.call(draw.arg("title"), 5, 5, FONT_XL, "white"),
        fill.call(0, HEADER_Y, W, HEADER_Y, "white"),
        if_else(eq(items.length(), 0), [
            header.call(draw.arg("app"), ""),
            measure.call("No Items", FONT_N),
            d["y"].set(half((PL_BOTTOM - PL_TOP) - th.get())),
            clamp0(d["y"]),
            write.call("No Items", half(W - tw.get()), PL_TOP + d["y"].get(),
                       FONT_N, "white"),
            if_(eq(draw.arg("hint"), 1), [softkey.call("", 1)]),
        ], [
            header.call(draw.arg("app"), d["sel"] + 1),
            pwrap.call(items.item(d["sel"] + 1), FONT_XL, PL_MAX_W, 2),
            measure.call("Ag", FONT_XL),
            d["h"].set(th.get()),
            d["t"].set(lines.length() * (d["h"] + 6) - 6),
            d["y"].set(half((PL_BOTTOM - PL_TOP) - d["t"].get())),
            clamp0(d["y"]),
            d["top"].set(PL_TOP + d["y"].get()),
            d["i"].set(1),
            repeat(lines.length(), [
                ds["line"].set(lines.item(d["i"].get())),
                measure.call(ds["line"].get(), FONT_XL),
                d["x"].set(half(PL_MAX_W - tw.get())),
                if_(lt(d["x"].get(), 5), [d["x"].set(5)]),
                write.call(ds["line"].get(), d["x"].get(),
                           d["top"] + (d["i"] - 1) * (d["h"] + 6), FONT_XL, "white"),
                d["i"].change(1),
            ]),
            fill.call(PL_BAR_X, PL_TOP, PL_BAR_X + 1, PL_BOTTOM, "white"),
            if_else(gt(items.length(), 1),
                    [d["notch"].set(PL_TOP + d["sel"] * ((PL_BOTTOM - PL_TOP)
                                                         / (items.length() - 1)))],
                    [d["notch"].set(PL_TOP)]),
            d["notch"].set(floor(d["notch"].get())),
            fill.call(PL_BAR_X - 4, d["notch"] - 3, PL_BAR_X + 2, d["notch"] + 3,
                      "white"),
            if_(eq(draw.arg("hint"), 1), [softkey.call("Select", 1)]),
        ]),
    ], "PagedList.draw(): one item per screen in the big font, wrapped to two "
       "lines.")

    show = proc("page show %s app %s hint %s", ["title", "app", "hint"],
                warp=False)
    define(show, [
        d["sel"].set(0),
        s["flush"].call(),
        draw.call(show.arg("title"), show.arg("app"), show.arg("hint")),
        result.set(-1),
        d["done"].set(0),
        repeat_until(eq(d["done"].get(), 1), [
            s["wait_key"].call(),
            if_(AND(eq(key.get(), 108), gt(items.length(), 0)), [
                d["sel"].set((d["sel"] + 1) % items.length()),
                draw.call(show.arg("title"), show.arg("app"), show.arg("hint")),
            ]),
            if_(AND(eq(key.get(), 103), gt(items.length(), 0)), [
                d["sel"].set((d["sel"] - 1) % items.length()),
                draw.call(show.arg("title"), show.arg("app"), show.arg("hint")),
            ]),
            if_(eq(key.get(), 28), [result.set(d["sel"].get()), d["done"].set(1)]),
            if_(eq(key.get(), 14), [result.set(-1), d["done"].set(1)]),
        ]),
    ])
    api["paged_list"] = show

    level_items = proc("ui level items %s", ["count"])
    define(level_items, [
        items.clear(),
        d["i"].set(1),
        repeat(level_items.arg("count"), [
            items.add(join("Level ", d["i"].get())),
            d["i"].change(1),
        ]),
    ])

    level = proc("level select current %s of %s", ["current", "count"],
                 warp=False)
    define(level, [
        level_items.call(level.arg("count")),
        d["n"].set(Ex(level.arg("current")) - 1),
        if_(gt(d["n"].get(), Ex(level.arg("count")) - 1),
            [d["n"].set(Ex(level.arg("count")) - 1)]),
        clamp0(d["n"]),
        api["softkey"].call("OK", 1),
        api["vertical_list_start"].call("Level", 6, d["n"].get()),
        if_(NOT(lt(result.get(), 0)), [result.change(1)]),
    ], "LevelSelector: a VerticalList of 'Level n' with an OK softkey. Leaves "
       "the level (1-based) in `nd result`, or -1 for back. Note it starts "
       "with the window at the top even when the current level is below it -- "
       "that is what the phone does.")
    api["level"] = level


# ----------------------------------------------------------------------
# TextInput (framework.py:663) and TextInputLong (framework.py:800)
# ----------------------------------------------------------------------

TI_PROMPT_Y = HEADER_Y + 20                                   # 50
TI_BOX_Y = TI_PROMPT_Y + 30                                   # 80
TI_BOX_H = max(24, min(40, CONTENT_BOTTOM - TI_BOX_Y - 10))   # 40
TI_BOX_RIGHT = max(20, W - 10)                                # 230
TL_TOP = HEADER_Y + 10                                        # 40
TL_BOTTOM = CONTENT_BOTTOM - 4                                # 141
TL_WIDTH = max(20, W - 20)                                    # 220
DIGITS = "0123456789"


def _text_fields(port, api, s):
    proc, define, v = port.proc, port.define, port.var
    fill, box = s["fill"], s["box"]
    write, measure = s["write"], s["measure"]
    wrap, slice_, softkey = api["wrap"], api["slice"], api["softkey"]
    tw, th, nd_str = s["tw"], s["th"], s["nd_str"]
    key, result, text_out, lines = (s["key"], s["result"], s["text_out"],
                                    s["lines"])
    d = s["d"]
    map_code = port.lists["nd map code"]
    map_char = port.lists["nd map char"]
    map_upper = port.lists["nd map upper"]

    blink, last_blink = v("nd blink", 1), v("nd blink at", 0)
    ti_disp, ti_ch = v("nd ti display", ""), v("nd ti ch", "")
    ti_slot, ti_action = v("nd ti slot", 0), v("nd ti action", "")

    # The typing half of handle_key, minus every branch that needs the i2c
    # keypad: with no matrix input `_t9_active` is false, so multi-tap,
    # predictive text and the mode indicator are all unreachable here, which
    # is exactly what QEMU does with a QWERTY keyboard.
    typed = proc("ui type key filter %s", ["filter"])
    define(typed, [
        ti_action.set(""),
        ti_slot.set(map_code.index_of(key.get())),
        if_(NOT(eq(ti_slot.get(), 0)), [
            ti_ch.set(map_char.item(ti_slot.get())),
            d["run"].set(1),
            if_(AND(eq(typed.arg("filter"), "numbers"),
                    NOT(_contains_char("0123456789*#+", ti_ch))), [d["run"].set(0)]),
            if_(AND(eq(typed.arg("filter"), "letters"),
                    _contains_char(DIGITS, ti_ch)), [d["run"].set(0)]),
            if_(eq(d["run"].get(), 1), [
                if_(eq(length_of(text_out.get()), 0),
                    [ti_ch.set(map_upper.item(ti_slot.get()))]),
                text_out.set(join(text_out.get(), ti_ch.get())),
                ti_action.set("typed"),
            ]),
        ]),
    ], "TextInput.DEV_KEYMAP, and t9_engine.char_allowed() for the field's "
       "filter. The first character of an empty field is capitalised.")

    draw = proc("field draw %s prompt %s", ["title", "prompt"])
    define(draw, [
        fill.call(0, 0, W, CONTENT_BOTTOM, "black"),
        write.call(draw.arg("title"), 5, 5, FONT_XL, "white"),
        fill.call(0, HEADER_Y, W, HEADER_Y, "white"),
        write.call(draw.arg("prompt"), 10, TI_PROMPT_Y, FONT_N, "white"),
        box.call(10, TI_BOX_Y, TI_BOX_RIGHT, TI_BOX_Y + TI_BOX_H, "white"),
        if_else(eq(blink.get(), 1),
                [ti_disp.set(join(text_out.get(), "_"))],
                [ti_disp.set(text_out.get())]),
        if_else(eq(ti_disp.get(), ""),
                [measure.call("A", FONT_N)],
                [measure.call(ti_disp.get(), FONT_N)]),
        d["y"].set(half(TI_BOX_H - th.get())),
        clamp0(d["y"]),
        write.call(ti_disp.get(), 15, TI_BOX_Y + d["y"].get(), FONT_N, "white"),
    ], "TextInput.draw(): the box, and the text with a blinking underscore "
       "for a cursor.")

    show = proc("field show %s prompt %s start %s filter %s",
                ["title", "prompt", "start", "filter"], warp=False)
    define(show, [
        text_out.set(show.arg("start")),
        blink.set(1),
        last_blink.set(timer()),
        softkey.call("OK", 1),
        draw.call(show.arg("title"), show.arg("prompt")),
        result.set(-1),
        d["done"].set(0),
        repeat_until(eq(d["done"].get(), 1), [
            # The Python blinks here and then blocks in wait_for_key, so the
            # cursor only moves when a key is pressed. Ported as written.
            if_(gt(timer() - last_blink, 0.5), [
                blink.set(1 - blink.get()),
                last_blink.set(timer()),
                draw.call(show.arg("title"), show.arg("prompt")),
            ]),
            s["wait_key"].call(),
            if_else(OR(eq(key.get(), 28), eq(key.get(), 96)),
                    [result.set(0), d["done"].set(1)],
                    [if_else(eq(key.get(), 14), [
                        if_else(gt(length_of(text_out.get()), 0), [
                            slice_.call(text_out.get(), 1,
                                        length_of(text_out.get()) - 1),
                            text_out.set(nd_str.get()),
                            draw.call(show.arg("title"), show.arg("prompt")),
                        ], [
                            result.set(-1), d["done"].set(1),
                        ]),
                    ], [
                        typed.call(show.arg("filter")),
                        if_(eq(ti_action.get(), "typed"),
                            [draw.call(show.arg("title"), show.arg("prompt"))]),
                    ])]),
        ]),
    ], "TextInput.show(). `nd text` is what was typed; `nd result` is 0 for OK "
       "and -1 when Clear was pressed on an empty field. filter is any / "
       "letters / numbers.")
    api["text_input"] = show

    # --- TextInputLong ---------------------------------------------------

    ldraw = proc("compose draw %s", ["title"])
    define(ldraw, [
        fill.call(0, 0, W, CONTENT_BOTTOM, "black"),
        write.call(ldraw.arg("title"), 5, 5, FONT_XL, "white"),
        measure.call(length_of(text_out.get()), FONT_N),
        write.call(length_of(text_out.get()), W - 5 - tw.get(), 5, FONT_N, "white"),
        fill.call(0, HEADER_Y, W, HEADER_Y, "white"),
        if_else(eq(blink.get(), 1),
                [ti_disp.set(join(text_out.get(), "_"))],
                [ti_disp.set(text_out.get())]),
        wrap.call(ti_disp.get(), FONT_S, TL_WIDTH, 1, 0),
        measure.call("Ag", FONT_S),
        d["h"].set(th + 3),
        d["max"].set(floor((TL_BOTTOM - TL_TOP) / d["h"].get())),
        if_(lt(d["max"].get(), 1), [d["max"].set(1)]),
        d["n"].set(lines.length() - d["max"]),
        clamp0(d["n"]),
        d["y"].set(TL_TOP),
        d["i"].set(d["n"] + 1),
        repeat(d["max"].get(), [
            if_(NOT(gt(d["i"].get(), lines.length())), [
                write.call(lines.item(d["i"].get()), 10, d["y"].get(),
                           FONT_S, "white"),
                d["y"].set(d["y"] + d["h"]),
            ]),
            d["i"].change(1),
        ]),
    ], "TextInputLong.draw(): the small font, wrapped hard, scrolled so the "
       "end of what you are typing is always the last line.")

    lshow = proc("compose show %s softkey %s start %s", ["title", "softkey", "start"],
                 warp=False)
    define(lshow, [
        text_out.set(lshow.arg("start")),
        blink.set(1),
        last_blink.set(timer()),
        ldraw.call(lshow.arg("title")),
        softkey.call(lshow.arg("softkey"), 1),
        result.set(-1),
        d["done"].set(0),
        repeat_until(eq(d["done"].get(), 1), [
            if_(gt(timer() - last_blink, 0.5), [
                blink.set(1 - blink.get()),
                last_blink.set(timer()),
                ldraw.call(lshow.arg("title")),
                softkey.call(lshow.arg("softkey"), 1),
            ]),
            s["wait_key"].call(),
            if_else(OR(eq(key.get(), 28), eq(key.get(), 96)),
                    [result.set(0), d["done"].set(1)],
                    [if_else(eq(key.get(), 14), [
                        if_else(eq(length_of(text_out.get()), 0),
                                [result.set(-1), d["done"].set(1)],
                                [slice_.call(text_out.get(), 1,
                                             length_of(text_out.get()) - 1),
                                 text_out.set(nd_str.get()),
                                 ldraw.call(lshow.arg("title")),
                                 softkey.call(lshow.arg("softkey"), 1)]),
                    ], [
                        typed.call("any"),
                        if_(eq(ti_action.get(), "typed"),
                            [ldraw.call(lshow.arg("title")),
                             softkey.call(lshow.arg("softkey"), 1)]),
                    ])]),
        ]),
    ], "The message composer. TextInputLong itself has no show() -- the loop "
       "here is the one Messages runs around it (apps/Messages/main.py:413): "
       "the softkey confirms, and Clear on an empty message leaves.")
    api["compose"] = lshow


def _contains_char(haystack, var):
    from blocks import contains
    return contains(haystack, var.get())


# ----------------------------------------------------------------------
# MessageDialog (framework.py:990), InfoScreen (1473), TextScroller (1364)
# ----------------------------------------------------------------------

def _dialogs(port, api, s):
    proc, define, v, lst = port.proc, port.define, port.var, port.lst
    fill, write, measure = s["fill"], s["write"], s["measure"]
    stamp_image, image_size = s["stamp_image"], s["image_size"]
    wrap, slice_, softkey = api["wrap"], api["slice"], api["softkey"]
    tw, th, nd_str = s["tw"], s["th"], s["nd_str"]
    iw, ih = s["iw"], s["ih"]
    key, result, lines = s["key"], s["result"], s["lines"]
    d, ds = s["d"], s["ds"]

    centred = v("nd md centred", 1)
    body_font = v("nd md font", FONT_N)

    dialog_draw = proc("dialog draw %s title %s icon %s button %s",
                       ["message", "title", "icon", "button"])
    define(dialog_draw, [
        fill.call(0, 0, W, H, "black"),
        image_size.call(dialog_draw.arg("icon")),
        if_(gt(iw.get(), 0),
            [stamp_image.call(dialog_draw.arg("icon"), MARGIN, MARGIN)]),
        d["y"].set(MARGIN),
        if_(NOT(eq(dialog_draw.arg("title"), "")), [
            d["x"].set(MARGIN),
            if_(gt(iw.get(), 0), [d["x"].set(MARGIN + iw + 6)]),
            write.call(dialog_draw.arg("title"), d["x"].get(), MARGIN, FONT_MD,
                       "white"),
            measure.call(dialog_draw.arg("title"), FONT_MD),
            if_(gt(MARGIN + th + 6, d["y"].get()), [d["y"].set(MARGIN + th + 6)]),
        ]),
        if_(gt(iw.get(), 0), [
            # The body has to clear the triangle even when the title is
            # shorter than it is, or the first line lands on the icon.
            if_(gt(MARGIN + ih + 6, d["y"].get()), [d["y"].set(MARGIN + ih + 6)]),
        ]),
        # A short notice gets the Nokia alert look: normal font, centred.
        # Anything longer keeps the small left-aligned paragraph form.
        wrap.call(dialog_draw.arg("message"), FONT_N, W - MARGIN * 2, 1, 1),
        if_else(NOT(gt(lines.length(), 2)),
                [body_font.set(FONT_N), centred.set(1)],
                [body_font.set(FONT_S), centred.set(0),
                 wrap.call(dialog_draw.arg("message"), FONT_S, W - MARGIN * 2,
                           1, 1)]),
        measure.call("Ag", body_font.get()),
        d["h"].set(th + 3),
        d["max"].set(floor((CONTENT_BOTTOM - d["y"] - MARGIN) / d["h"])),
        if_(lt(d["max"].get(), 1), [d["max"].set(1)]),
        if_(gt(lines.length(), d["max"].get()), [
            repeat_until(NOT(gt(lines.length(), d["max"].get())),
                         [lines.delete(lines.length())]),
            if_(gt(lines.length(), 0), [
                ds["s"].set(lines.item(lines.length())),
                slice_.call(ds["s"].get(), length_of(ds["s"].get()), 1),
                if_(NOT(eq(nd_str.get(), "…")),
                    [lines.replace(lines.length(), join(ds["s"].get(), " …"))]),
            ]),
        ]),
        d["t"].set(half(CONTENT_BOTTOM - MARGIN - d["y"] - lines.length() * d["h"])),
        clamp0(d["t"]),
        d["y"].set(d["y"] + d["t"]),
        d["i"].set(1),
        repeat(lines.length(), [
            ds["line"].set(lines.item(d["i"].get())),
            if_else(eq(centred.get(), 1), [
                measure.call(ds["line"].get(), body_font.get()),
                d["x"].set(half(W - tw.get())),
                if_(lt(d["x"].get(), MARGIN), [d["x"].set(MARGIN)]),
            ], [
                d["x"].set(MARGIN),
            ]),
            write.call(ds["line"].get(), d["x"].get(), d["y"].get(),
                       body_font.get(), "white"),
            d["y"].set(d["y"] + d["h"]),
            d["i"].change(1),
        ]),
        softkey.call(dialog_draw.arg("button"), 1),
    ], "MessageDialog._draw(). A short notice gets the Nokia alert look -- "
       "normal font, centred; a paragraph keeps the small left-aligned form.")

    dialog = proc("dialog %s title %s icon %s button %s",
                  ["message", "title", "icon", "button"], warp=False)
    define(dialog, [
        s["flush"].call(),
        dialog_draw.call(dialog.arg("message"), dialog.arg("title"),
                         dialog.arg("icon"), dialog.arg("button")),
        result.set(0),
        d["done"].set(0),
        repeat_until(eq(d["done"].get(), 1), [
            s["wait_key"].call(),
            if_(OR(eq(key.get(), 28), eq(key.get(), 14)),
                [result.set(key.get()), d["done"].set(1)]),
        ]),
    ], "MessageDialog: a full-screen notice with the warning triangle. Pass "
       "\"\" for the icon to leave it off. `nd result` is the key that "
       "dismissed it -- 28 for the softkey, 14 for Clear.")
    api["dialog"] = dialog

    info_draw = proc("info draw %s value %s button %s",
                     ["title", "value", "button"])
    define(info_draw, [
        fill.call(0, 0, W, CONTENT_BOTTOM, "black"),
        measure.call(info_draw.arg("title"), FONT_N),
        d["w"].set(tw.get()),
        d["h"].set(th.get()),
        if_else(eq(info_draw.arg("value"), ""), [
            d["y"].set(half(CONTENT_BOTTOM - d["h"])),
            clamp0(d["y"]),
            write.call(info_draw.arg("title"), half(W - d["w"]), d["y"].get(),
                       FONT_N, "white"),
        ], [
            measure.call(info_draw.arg("value"), FONT_XL),
            d["y"].set(half(CONTENT_BOTTOM - (d["h"] + 10 + th))),
            clamp0(d["y"]),
            write.call(info_draw.arg("title"), half(W - d["w"]), d["y"].get(),
                       FONT_N, "white"),
            write.call(info_draw.arg("value"), half(W - tw.get()),
                       d["y"] + d["h"] + 10, FONT_XL, "white"),
        ]),
        softkey.call(info_draw.arg("button"), 1),
    ], "InfoScreen: a label and a reading, centred.")

    info = proc("info %s value %s button %s", ["title", "value", "button"],
                warp=False)
    define(info, [
        info_draw.call(info.arg("title"), info.arg("value"),
                       info.arg("button")),
        result.set(0),
        d["done"].set(0),
        repeat_until(eq(d["done"].get(), 1), [
            s["wait_key"].call(),
            if_(OR(eq(key.get(), 28), eq(key.get(), 14)),
                [result.set(key.get()), d["done"].set(1)]),
        ]),
    ], "Not a warning -- no icon, and nothing to decide. `nd result` is the "
       "key that dismissed it. An empty value means there is no value: the "
       "Python distinguishes None from \"\" and Scratch has no None, so a "
       "genuinely empty reading gets the one-line layout here.")
    api["info"] = info

    # --- TextScroller -----------------------------------------------------
    pg_line = lst("nd page line", [])
    pg_h = lst("nd page h", [])
    pg_start = lst("nd page start", [])
    used, curlen = v("nd pg used", 0), v("nd pg len", 0)

    paginate = proc("ui paginate %s font %s", ["text", "font"])
    define(paginate, [
        wrap.call(paginate.arg("text"), paginate.arg("font"), W - 20, 0, 1),
        if_(eq(lines.length(), 0), [lines.add("")]),
        measure.call("Ag", paginate.arg("font")),
        d["h"].set(th + 4),
        d["t"].set(floor(d["h"] / 3)),
        if_(lt(d["t"].get(), 4), [d["t"].set(4)]),
        d["max"].set(CONTENT_BOTTOM - 8 - 4),
        pg_line.clear(),
        pg_h.clear(),
        pg_start.clear(),
        used.set(0),
        curlen.set(0),
        d["i"].set(1),
        repeat(lines.length(), [
            ds["line"].set(lines.item(d["i"].get())),
            if_else(eq(ds["line"].get(), ""),
                    [d["n"].set(d["t"].get())],
                    [d["n"].set(d["h"].get())]),
            if_(AND(gt(curlen.get(), 0), gt(used + d["n"], d["max"].get())),
                [curlen.set(0), used.set(0)]),
            if_(NOT(AND(eq(ds["line"].get(), ""), eq(curlen.get(), 0))), [
                if_(eq(curlen.get(), 0), [pg_start.add(pg_line.length() + 1)]),
                pg_line.add(ds["line"].get()),
                pg_h.add(d["n"].get()),
                used.set(used + d["n"]),
                curlen.change(1),
            ]),
            d["i"].change(1),
        ]),
        if_(eq(pg_start.length(), 0), [
            pg_start.add(1), pg_line.add(""), pg_h.add(d["h"].get()),
        ]),
    ], "TextScroller._paginate(). A paragraph break costs a third of a line, "
       "not a whole one -- giving it a full line of 20px type is what turned "
       "one changelog into five screens of paging.")

    sdraw = proc("read page more %s back %s", ["more", "back"])
    define(sdraw, [
            if_(gt(d["page"].get(), pg_start.length() - 1),
                [d["page"].set(pg_start.length() - 1)]),
            if_(lt(d["page"].get(), 0), [d["page"].set(0)]),
            fill.call(0, 0, W, CONTENT_BOTTOM, "black"),
            d["y"].set(8),
            d["i"].set(pg_start.item(d["page"] + 1)),
            if_else(lt(d["page"] + 1, pg_start.length()),
                    [d["n"].set(pg_start.item(d["page"] + 2) - 1)],
                    [d["n"].set(pg_line.length())]),
            repeat_until(gt(d["i"].get(), d["n"].get()), [
                if_(NOT(eq(pg_line.item(d["i"].get()), "")), [
                    write.call(pg_line.item(d["i"].get()), 10, d["y"].get(),
                               FONT_N, "white"),
                ]),
                d["y"].set(d["y"] + pg_h.item(d["i"].get())),
                d["i"].change(1),
            ]),
            if_else(NOT(lt(d["page"].get(), pg_start.length() - 1)),
                    [d["run"].set(1), softkey.call(sdraw.arg("back"), 1)],
                    [d["run"].set(0), softkey.call(sdraw.arg("more"), 1)]),
    ], "One page of a TextScroller, and whether it is the last one.")

    scroller = proc("read text %s more %s back %s", ["text", "more", "back"],
                    warp=False)
    define(scroller, [
        d["page"].set(0),
        d["done"].set(0),
        paginate.call(scroller.arg("text"), FONT_N),
        repeat_until(eq(d["done"].get(), 1), [
            sdraw.call(scroller.arg("more"), scroller.arg("back")),
            s["wait_key"].call(),
            if_(OR(eq(key.get(), 28), eq(key.get(), 108)), [
                if_else(eq(d["run"].get(), 1),
                        [d["done"].set(1)],
                        [d["page"].change(1)]),
            ]),
            if_(eq(key.get(), 103), [
                if_(gt(d["page"].get(), 0), [d["page"].change(-1)]),
            ]),
            if_(eq(key.get(), 14), [d["done"].set(1)]),
        ]),
    ], "TextScroller: the instructions reader. The softkey says More until the "
       "last page, where it becomes Back.")
    api["scroller"] = scroller


# ----------------------------------------------------------------------
# ProgressScreen (framework.py:1522) and DetailPage (framework.py:1644)
# ----------------------------------------------------------------------

def _pages(port, api, s):
    proc, define, v, lst = port.proc, port.define, port.var, port.lst
    fill, box, write, measure = s["fill"], s["box"], s["write"], s["measure"]
    stamp_image, image_size = s["stamp_image"], s["image_size"]
    wrap, ellipsize, fit_font = api["wrap"], api["ellipsize"], api["fit_font"]
    softkey = api["softkey"]
    tw, th, nd_str, font_out = s["tw"], s["th"], s["nd_str"], s["font_out"]
    iw, ih = s["iw"], s["ih"]
    key, result, lines = s["key"], s["result"], s["lines"]
    d, ds = s["d"], s["ds"]
    ag = port.ag_height

    # ProgressScreen precomputes every box in __init__; the sizes it uses are
    # the height of "Ag" in each font, which does not change at runtime.
    step_h, small_h = ag[FONT_N], ag[FONT_S]
    divider_y = 4 + small_h + 5                     # 24
    bar_top = int(CONTENT_BOTTOM * 0.55)            # 79
    bar_left, bar_right = 20, W - 20
    bar_bottom = bar_top + 14
    label_y = bar_top - 14 - step_h
    status_y = bar_bottom + 9
    hint_y = CONTENT_BOTTOM - small_h - 6
    span = (bar_right - 2) - (bar_left + 2)

    pr_pct = v("nd pr percent", -1)

    reset = proc("progress reset", [])
    define(reset, [pr_pct.set(-1)],
           "ProgressScreen only repaints when the whole percentage changes, so "
           "a copy loop can call it per chunk. This is what set_step() does to "
           "force the next call through.")
    api["progress_reset"] = reset

    progress = proc("progress %s of %s step %s header %s hint %s detail %s",
                    ["done", "total", "step", "header", "hint", "detail"])
    define(progress, [
        if_else(eq(progress.arg("total"), 0),
                [d["n"].set(100)],
                [d["n"].set(floor(Ex(progress.arg("done")) * 100
                                  / Ex(progress.arg("total"))))]),
        if_(lt(d["n"].get(), 0), [d["n"].set(0)]),
        if_(gt(d["n"].get(), 100), [d["n"].set(100)]),
        if_(NOT(eq(d["n"].get(), pr_pct.get())), [
            pr_pct.set(d["n"].get()),
            fill.call(0, 0, W, CONTENT_BOTTOM, "black"),
            if_(NOT(eq(progress.arg("header"), "")), [
                write.call(progress.arg("header"), 10, 4, FONT_S, "white"),
                fill.call(10, divider_y, W - 10, divider_y, "white"),
            ]),
            if_(NOT(eq(progress.arg("step"), "")), [
                fit_font.call(progress.arg("step"), W - 16, LADDER),
                ellipsize.call(progress.arg("step"), font_out.get(), W - 16),
                ds["s"].set(nd_str.get()),
                measure.call(ds["s"].get(), font_out.get()),
                d["x"].set(half(W - tw.get())),
                clamp0(d["x"]),
                write.call(ds["s"].get(), d["x"].get(), label_y, font_out.get(),
                           "white"),
            ]),
            box.call(bar_left, bar_top, bar_right, bar_bottom, "white"),
            d["w"].set(floor(span * d["n"] / 100)),
            if_(gt(d["w"].get(), 0), [
                fill.call(bar_left + 2, bar_top + 2, bar_left + 2 + d["w"],
                          bar_bottom - 2, "white"),
            ]),
            ds["s"].set(join(d["n"].get(), "%")),
            if_else(eq(progress.arg("detail"), ""), [
                measure.call(ds["s"].get(), FONT_S),
                d["x"].set(half(bar_left + bar_right - tw.get())),
                clamp0(d["x"]),
                write.call(ds["s"].get(), d["x"].get(), status_y, FONT_S, "white"),
            ], [
                write.call(ds["s"].get(), bar_left, status_y, FONT_S, "white"),
                measure.call(progress.arg("detail"), FONT_S),
                write.call(progress.arg("detail"), bar_right - tw.get(), status_y,
                           FONT_S, "white"),
            ]),
            if_(NOT(eq(progress.arg("hint"), "")), [
                ellipsize.call(progress.arg("hint"), FONT_S, W - 16),
                ds["s"].set(nd_str.get()),
                measure.call(ds["s"].get(), FONT_S),
                d["x"].set(half(W - tw.get())),
                clamp0(d["x"]),
                write.call(ds["s"].get(), d["x"].get(), hint_y, FONT_S, "white"),
            ]),
            softkey.call("", 1),
        ]),
    ], "ProgressScreen.draw(). Nothing is ever drawn on top of the bar: a "
       "percentage sitting across its own fill is the one thing that makes a "
       "progress bar look broken. `detail` is the string the Python's detail "
       "callback would have returned, e.g. \"5.6 of 12.4 MB\".")
    api["progress"] = progress

    # --- DetailPage -------------------------------------------------------

    DP_MARGIN = 10
    DP_SCROLLBAR = 8
    line_h = ag[FONT_S] + 3
    title_h = ag[FONT_N]

    kind = lst("nd block kind", [])
    bh = lst("nd block h", [])
    btext = lst("nd block text", [])
    bfont = lst("nd block font", [])
    tb_kind, tb_h, tb_text = lst("nd tb kind", []), lst("nd tb h", []), lst("nd tb text", [])
    hr_text, hr_font, hr_h = (lst("nd hero text", []), lst("nd hero font", []),
                              lst("nd hero h", []))
    hero_img, hero_iw, hero_ih = v("nd hero img", ""), v("nd hero iw", 0), v("nd hero ih", 0)
    hero_inner, hero_stack, hero_tx = (v("nd hero inner", 0), v("nd hero stack", 0),
                                       v("nd hero tx", 0))
    dp_top, dp_vh, dp_ch = v("nd dp top", 0), v("nd dp vh", 0), v("nd dp height", 0)
    dp_off, dp_max = v("nd dp offset", 0), v("nd dp max", 0)
    bw, pass_done = v("nd dp bw", 0), v("nd dp pass", 0)

    def add_block(k, height, text="", font=FONT_S):
        return [kind.add(k), bh.add(height), btext.add(text), bfont.add(font)]

    layout = proc("detail layout %s sub %s badge %s body %s image %s header %s",
                  ["title", "sub", "badge", "body", "image", "header"])
    define(layout, [
        if_else(eq(layout.arg("header"), ""),
                [dp_top.set(4)],
                [dp_top.set(4 + small_h + 5 + 6)]),
        dp_vh.set((CONTENT_BOTTOM - 2) - dp_top.get()),
        kind.clear(), bh.clear(), btext.clear(), bfont.clear(),
        hr_text.clear(), hr_font.clear(), hr_h.clear(),
        image_size.call(layout.arg("image")),
        hero_img.set(layout.arg("image")),
        hero_iw.set(iw.get()),
        hero_ih.set(ih.get()),
        if_else(AND(gt(iw.get(), 0), OR(NOT(eq(layout.arg("sub"), "")),
                                        NOT(eq(layout.arg("badge"), "")))), [
            # Picture on the left, everything it is on the right. Stacking the
            # two used up the whole screen before a word of the body got a
            # look in.
            hero_tx.set(DP_MARGIN + hero_iw + 8),
            d["w"].set(W - hero_tx - DP_MARGIN - DP_SCROLLBAR),
            fit_font.call(layout.arg("title"), d["w"].get(), LADDER),
            d["t"].set(font_out.get()),
            ellipsize.call(layout.arg("title"), d["t"].get(), d["w"].get()),
            ds["s"].set(nd_str.get()),
            if_(NOT(eq(layout.arg("title"), "")), [
                if_(NOT(eq(ds["s"].get(), "")), [
                    measure.call("Ag", d["t"].get()),
                    hr_text.add(ds["s"].get()),
                    hr_font.add(d["t"].get()),
                    hr_h.add(th + 5),
                ]),
            ]),
            if_(NOT(eq(layout.arg("sub"), "")), [
                wrap.call(layout.arg("sub"), FONT_S, d["w"].get(), 0, 1),
                d["i"].set(1),
                repeat(lines.length(), [
                    if_(NOT(eq(lines.item(d["i"].get()), "")), [
                        hr_text.add(lines.item(d["i"].get())),
                        hr_font.add(FONT_S),
                        hr_h.add(line_h),
                    ]),
                    d["i"].change(1),
                ]),
            ]),
            if_(NOT(eq(layout.arg("badge"), "")), [
                ellipsize.call(layout.arg("badge"), FONT_S, d["w"].get()),
                if_(NOT(eq(nd_str.get(), "")), [
                    hr_text.add(nd_str.get()),
                    hr_font.add(FONT_S),
                    hr_h.add(line_h),
                ]),
            ]),
            hero_stack.set(0),
            d["i"].set(1),
            repeat(hr_h.length(), [
                hero_stack.set(hero_stack + hr_h.item(d["i"].get())),
                d["i"].change(1),
            ]),
            hero_inner.set(hero_ih.get()),
            if_(gt(hero_stack.get(), hero_inner.get()),
                [hero_inner.set(hero_stack.get())]),
        ] + add_block(5, hero_inner.get() + 6), [
            if_else(gt(iw.get(), 0),
                    add_block(4, ih + 8, layout.arg("image"))
                    + [if_(NOT(eq(layout.arg("title"), "")),
                           add_block(1, title_h + 6, layout.arg("title"), FONT_N))],
                    [
                        # Nothing to sit beside: centre the type instead.
                        if_(NOT(eq(layout.arg("title"), "")),
                            add_block(1, title_h + 6, layout.arg("title"), FONT_N)),
                        if_(NOT(eq(layout.arg("sub"), "")), [
                            wrap.call(layout.arg("sub"), FONT_S,
                                      W - DP_MARGIN * 2, 0, 1),
                            d["i"].set(1),
                            repeat(lines.length(),
                                   add_block(1, line_h, "", FONT_S)
                                   + [btext.replace(btext.length(),
                                                    lines.item(d["i"].get())),
                                      d["i"].change(1)]),
                        ]),
                        if_(NOT(eq(layout.arg("badge"), "")),
                            add_block(1, line_h + 4, layout.arg("badge"), FONT_S)),
                    ]),
        ]),
        d["top"].set(0),
        d["i"].set(1),
        repeat(bh.length(), [
            d["top"].set(d["top"] + bh.item(d["i"].get())),
            d["i"].change(1),
        ]),
        if_(NOT(eq(layout.arg("body"), "")), [
            # The rule is the first thing to go when the page is tight: it
            # separates, but it does not say anything.
            if_(AND(gt(bh.length(), 0),
                    NOT(gt(d["top"] + 10 + line_h, dp_vh.get()))),
                add_block(3, 10) + [d["top"].set(d["top"] + 10)]),
            bw.set(W - DP_MARGIN * 2),
            pass_done.set(0),
            repeat_until(eq(pass_done.get(), 1), [
                wrap.call(layout.arg("body"), FONT_S, bw.get(), 0, 1),
                tb_kind.clear(), tb_h.clear(), tb_text.clear(),
                d["i"].set(1),
                d["n"].set(0),
                repeat(lines.length(), [
                    if_else(eq(lines.item(d["i"].get()), ""), [
                        # A paragraph break is a breath, not an empty line.
                        tb_kind.add(0), tb_h.add(floor(line_h / 2)), tb_text.add(""),
                        d["n"].set(d["n"] + floor(line_h / 2)),
                    ], [
                        tb_kind.add(2), tb_h.add(line_h),
                        tb_text.add(lines.item(d["i"].get())),
                        d["n"].set(d["n"] + line_h),
                    ]),
                    d["i"].change(1),
                ]),
                if_else(OR(NOT(gt(d["top"] + d["n"], dp_vh.get())),
                           eq(bw.get(), W - DP_MARGIN * 2 - DP_SCROLLBAR)),
                        [pass_done.set(1)],
                        [bw.set(bw - DP_SCROLLBAR)]),
            ]),
            d["i"].set(1),
            repeat(tb_kind.length(), [
                kind.add(tb_kind.item(d["i"].get())),
                bh.add(tb_h.item(d["i"].get())),
                btext.add(tb_text.item(d["i"].get())),
                bfont.add(FONT_S),
                d["i"].change(1),
            ]),
        ]),
        dp_ch.set(0),
        d["i"].set(1),
        repeat(bh.length(), [
            dp_ch.set(dp_ch + bh.item(d["i"].get())),
            d["i"].change(1),
        ]),
        dp_max.set(dp_ch - dp_vh),
        clamp0(dp_max),
    ], "DetailPage._layout(): the page as a list of blocks and their heights. "
       "The body is wrapped twice because whether a scrollbar takes room "
       "depends on the height wrapping produces.")

    draw = proc("detail draw %s header %s", ["softkey", "header"])
    define(draw, [
        fill.call(0, 0, W, CONTENT_BOTTOM, "black"),
        d["y"].set(dp_top - dp_off),
        if_(NOT(gt(dp_ch.get(), dp_vh.get())), [
            # A page that fits is centred: a few words pinned to the top of an
            # otherwise black screen reads as a crash rather than as an answer.
            d["y"].set(d["y"] + half(dp_vh - dp_ch)),
        ]),
        d["i"].set(1),
        repeat(kind.length(), [
            d["h"].set(bh.item(d["i"].get())),
            # Blocks sliced by the bottom edge are left for the next scroll:
            # half a line of type at the fold reads as a bug.
            if_(AND(NOT(eq(kind.item(d["i"].get()), 0)),
                    gt(d["y"] + d["h"], dp_top.get()),
                    NOT(gt(d["y"] + d["h"], dp_top + dp_vh))), [
                ds["line"].set(btext.item(d["i"].get())),
                if_(eq(kind.item(d["i"].get()), 1), [
                    measure.call(ds["line"].get(), bfont.item(d["i"].get())),
                    write.call(ds["line"].get(), half(W - tw.get()), d["y"].get(),
                               bfont.item(d["i"].get()), "white"),
                ]),
                if_(eq(kind.item(d["i"].get()), 2), [
                    write.call(ds["line"].get(), DP_MARGIN, d["y"].get(),
                               FONT_S, "white"),
                ]),
                if_(eq(kind.item(d["i"].get()), 3), [
                    fill.call(DP_MARGIN * 3, d["y"] + 4, W - DP_MARGIN * 3,
                              d["y"] + 4, "white"),
                ]),
                if_(eq(kind.item(d["i"].get()), 4), [
                    stamp_image.call(ds["line"].get(), half(W - hero_iw),
                                     d["y"].get()),
                ]),
                if_(eq(kind.item(d["i"].get()), 5), [
                    stamp_image.call(hero_img.get(), DP_MARGIN,
                                     d["y"] + 3 + half(hero_inner - hero_ih)),
                    d["t"].set(d["y"] + 3 + half(hero_inner - hero_stack)),
                    d["n"].set(1),
                    repeat(hr_h.length(), [
                        write.call(hr_text.item(d["n"].get()), hero_tx.get(),
                                   d["t"].get(), hr_font.item(d["n"].get()),
                                   "white"),
                        d["t"].set(d["t"] + hr_h.item(d["n"].get())),
                        d["n"].change(1),
                    ]),
                ]),
            ]),
            d["y"].set(d["y"] + d["h"]),
            d["i"].change(1),
        ]),
        # The Python paints into a viewport-sized image and pastes it, so a
        # block hanging off the top is clipped. Stamped glyphs cannot be, so
        # the strip above the viewport is repainted instead.
        fill.call(0, 0, W, dp_top - 1, "black"),
        if_(NOT(eq(draw.arg("header"), "")), [
            write.call(draw.arg("header"), DP_MARGIN, 4, FONT_S, "white"),
            fill.call(DP_MARGIN, divider_y, W - DP_MARGIN, divider_y, "white"),
        ]),
        if_(gt(dp_ch.get(), dp_vh.get()), [
            d["top"].set(dp_top + 2),
            fill.call(W - 5, d["top"].get(), W - 4, CONTENT_BOTTOM - 4, "white"),
            d["n"].set((CONTENT_BOTTOM - 4) - d["top"] - 10),
            d["y"].set(dp_max.get()),
            if_(lt(d["y"].get(), 1), [d["y"].set(1)]),
            d["t"].set(d["top"] + floor(d["n"] * dp_off / d["y"])),
            fill.call(W - 8, d["t"].get(), W - 2, d["t"] + 10, "white"),
        ]),
        softkey.call(draw.arg("softkey"), 1),
    ], "DetailPage.draw(): an optional picture, a title, a line or two of "
       "detail, then body text, scrolled a line at a time.")

    show = proc("detail %s sub %s badge %s body %s image %s header %s softkey %s",
                ["title", "sub", "badge", "body", "image", "header", "softkey"],
                warp=False)
    define(show, [
        dp_off.set(0),
        layout.call(show.arg("title"), show.arg("sub"), show.arg("badge"),
                    show.arg("body"), show.arg("image"), show.arg("header")),
        draw.call(show.arg("softkey"), show.arg("header")),
        result.set(0),
        d["done"].set(0),
        repeat_until(eq(d["done"].get(), 1), [
            s["wait_key"].call(),
            if_else(OR(eq(key.get(), 28), eq(key.get(), 14)),
                    [result.set(key.get()), d["done"].set(1)], [
                        d["t"].set(dp_off.get()),
                        if_(eq(key.get(), 108), [
                            dp_off.set(dp_off + line_h),
                            if_(gt(dp_off.get(), dp_max.get()),
                                [dp_off.set(dp_max.get())]),
                        ]),
                        if_(eq(key.get(), 103), [
                            dp_off.set(dp_off - line_h),
                            if_(lt(dp_off.get(), 0), [dp_off.set(0)]),
                        ]),
                        if_(NOT(eq(dp_off.get(), d["t"].get())),
                            [draw.call(show.arg("softkey"), show.arg("header"))]),
                    ]),
        ]),
    ], "A page you read. Pass \"\" for anything you do not want -- no image, "
       "no header, no badge. `nd result` is the key that left the page.")
    api["detail"] = show

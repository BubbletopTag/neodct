"""The widget classes of System/ui/framework.py, as Scratch custom blocks.

Each class becomes a pair of blocks: a `draw` that paints one frame, and a
`show` that runs the class's blocking loop and leaves what `show()` would
have returned in `nd result` (and `nd text`, for the fields). The arithmetic
is transcribed rather than reinterpreted -- where the Python says
`max(24, line_height - 4)` so does this, including the places where the
result is a rounding quirk rather than a decision.

The screen is 240x175 with a 30-pixel softkey bar, so `content_bottom` is 145
and the header divider sits at 30. Those are constants here because they are
constants on the phone; the Python computes them from `ui.H` for the sake of
a display that never shipped.
"""

from blocks import (AND, Ex, NOT, OR, eq, floor, gt, if_, if_else, join,
                    length_of, letter_of, lt, repeat, repeat_until)
from port_core import FONT_N

# Keys, as the phone's evdev codes. A Scratch project has no keypad, so this
# is the DEV_KEYMAP path of TextInput -- the one QEMU uses -- and T9 never
# runs. Backspace cannot be detected from a browser, so Clear is a backslash
# (or a forward slash, whichever the keyboard has to hand).
KEYS = [
    ("up arrow", 103), ("down arrow", 108),
    ("left arrow", 105), ("right arrow", 106),
    ("enter", 28), ("\\", 14), ("/", 14),
    ("1", 2), ("2", 3), ("3", 4), ("4", 5), ("5", 6),
    ("6", 7), ("7", 8), ("8", 9), ("9", 10), ("0", 11),
    ("q", 16), ("w", 17), ("e", 18), ("r", 19), ("t", 20),
    ("y", 21), ("u", 22), ("i", 23), ("o", 24), ("p", 25),
    ("a", 30), ("s", 31), ("d", 32), ("f", 33), ("g", 34),
    ("h", 35), ("j", 36), ("k", 37), ("l", 38),
    ("z", 44), ("x", 45), ("c", 46), ("v", 47), ("b", 48),
    ("n", 49), ("m", 50),
    ("space", 57), (".", 52), (",", 51), ("-", 12),
]

# TextInput.DEV_KEYMAP (System/ui/framework.py:675).
DEV_KEYMAP = [
    (2, "1"), (3, "2"), (4, "3"), (5, "4"), (6, "5"),
    (7, "6"), (8, "7"), (9, "8"), (10, "9"), (11, "0"),
    (16, "q"), (17, "w"), (18, "e"), (19, "r"), (20, "t"),
    (21, "y"), (22, "u"), (23, "i"), (24, "o"), (25, "p"),
    (30, "a"), (31, "s"), (32, "d"), (33, "f"), (34, "g"),
    (35, "h"), (36, "j"), (37, "k"), (38, "l"),
    (44, "z"), (45, "x"), (46, "c"), (47, "v"), (48, "b"),
    (49, "n"), (50, "m"),
    (57, " "), (52, "."), (51, ","), (12, "-"),
]

NUMBER_CHARS = "0123456789*#+"


def half(value):
    """Python's `// 2` on a value that may be negative."""
    return floor(Ex(value) / 2)


def build_widgets(port, api):
    v, lst, proc, define = port.var, port.lst, port.proc, port.define
    measure = api["measure"]
    tw = v("nd tw")
    nd_str = v("nd str")
    key = v("nd key")
    lines = port.lists["nd lines"]
    api = dict(api)

    # ------------------------------------------------------------------
    # input
    # ------------------------------------------------------------------
    lst("nd keys", [name for name, _ in KEYS])
    lst("nd key codes", [code for _, code in KEYS])
    lst("nd key down", [0] * len(KEYS))
    lst("nd map code", [code for code, _ in DEV_KEYMAP])
    lst("nd map char", [char for _, char in DEV_KEYMAP])
    lst("nd map upper", [char.upper() for _, char in DEV_KEYMAP])
    keys, codes, down = (port.lists["nd keys"], port.lists["nd key codes"],
                         port.lists["nd key down"])

    p_i = v("nd p i", 0)

    poll = proc("ui poll keys", [])
    define(poll, [
        key.set(0),
        p_i.set(1),
        repeat(keys.length(), [
            if_else(_key_pressed(keys.item(p_i.get())), [
                if_(eq(down.item(p_i.get()), 0), [
                    down.replace(p_i.get(), 1),
                    if_(eq(key.get(), 0), [key.set(codes.item(p_i.get()))]),
                ]),
            ], [
                down.replace(p_i.get(), 0),
            ]),
            p_i.change(1),
        ]),
    ], "read_keypress(): one press per key-down, never a repeat, because the "
       "phone only looks at evdev value==1.")
    api["poll"] = poll

    flush = proc("ui flush keys", [])
    define(flush, [
        p_i.set(1),
        repeat(keys.length(), [
            if_else(_key_pressed(keys.item(p_i.get())),
                    [down.replace(p_i.get(), 1)],
                    [down.replace(p_i.get(), 0)]),
            p_i.change(1),
        ]),
        key.set(0),
    ], "The input flush AppSelector and MessageDialog do before drawing, so a "
       "key still held from the last screen does not dismiss this one.")
    api["flush"] = flush

    wait_key = proc("ui wait for key", [], warp=False)
    define(wait_key, [
        key.set(0),
        repeat_until(NOT(eq(key.get(), 0)), [poll.call()]),
    ], "wait_for_key(). Not 'run without screen refresh' -- this is where the "
       "project gets its frames.")
    api["wait_key"] = wait_key

    # ------------------------------------------------------------------
    # string helpers
    # ------------------------------------------------------------------
    s_i, s_out, s_n = v("nd s i", 0), v("nd s out", ""), v("nd s n", 0)

    slice_ = proc("ui slice %s from %s length %s", ["text", "start", "count"])
    define(slice_, [
        s_out.set(""),
        s_i.set(slice_.arg("start")),
        repeat(slice_.arg("count"), [
            s_out.set(join(s_out.get(), letter_of(s_i.get(), slice_.arg("text")))),
            s_i.change(1),
        ]),
        nd_str.set(s_out.get()),
    ])
    api["slice"] = slice_

    rstrip = proc("ui rstrip %s", ["text"])
    define(rstrip, [
        s_n.set(length_of(rstrip.arg("text"))),
        repeat_until(OR(eq(s_n.get(), 0),
                        NOT(eq(letter_of(s_n.get(), rstrip.arg("text")), " "))),
                     [s_n.change(-1)]),
        slice_.call(rstrip.arg("text"), 1, s_n.get()),
    ])

    f_end, f_cand, f_done = v("nd f end", 0), v("nd f cand", ""), v("nd f done", 0)

    fit_text = proc("ui fit text %s font %s width %s", ["text", "font", "width"])
    define(fit_text, [
        f_done.set(0),
        if_(OR(NOT(gt(fit_text.arg("width"), 0)), eq(fit_text.arg("text"), "")), [
            nd_str.set(""), f_done.set(1),
        ]),
        if_(eq(f_done.get(), 0), [
            measure.call(fit_text.arg("text"), fit_text.arg("font")),
            if_(NOT(gt(tw.get(), fit_text.arg("width"))), [
                nd_str.set(fit_text.arg("text")), f_done.set(1),
            ]),
        ]),
        if_(eq(f_done.get(), 0), [
            f_end.set(length_of(fit_text.arg("text")) - 1),
            repeat_until(OR(eq(f_done.get(), 1), lt(f_end.get(), 1)), [
                slice_.call(fit_text.arg("text"), 1, f_end.get()),
                rstrip.call(nd_str.get()),
                f_cand.set(join(nd_str.get(), "...")),
                measure.call(f_cand.get(), fit_text.arg("font")),
                if_(NOT(gt(tw.get(), fit_text.arg("width"))), [
                    nd_str.set(f_cand.get()), f_done.set(1),
                ]),
                f_end.change(-1),
            ]),
            if_(eq(f_done.get(), 0), [nd_str.set("")]),
        ]),
    ], "fit_text(): three dots, not an ellipsis -- this font has no U+2026 "
       "and would draw an empty box.")
    api["fit_text"] = fit_text

    e_t, e_go = v("nd e t", ""), v("nd e go", 0)
    ellipsize = proc("ui ellipsize %s font %s width %s", ["text", "font", "width"])
    define(ellipsize, [
        measure.call(ellipsize.arg("text"), ellipsize.arg("font")),
        if_else(NOT(gt(tw.get(), ellipsize.arg("width"))),
                [nd_str.set(ellipsize.arg("text"))],
                [
                    e_t.set(ellipsize.arg("text")),
                    e_go.set(1),
                    repeat_until(eq(e_go.get(), 0), [
                        if_else(eq(e_t.get(), ""),
                                [e_go.set(0)],
                                [
                                    measure.call(join(e_t.get(), "..."),
                                                 ellipsize.arg("font")),
                                    if_else(NOT(gt(tw.get(), ellipsize.arg("width"))),
                                            [e_go.set(0)],
                                            [slice_.call(e_t.get(), 1,
                                                         length_of(e_t.get()) - 1),
                                             e_t.set(nd_str.get())]),
                                ]),
                    ]),
                    if_else(eq(e_t.get(), ""),
                            [nd_str.set(ellipsize.arg("text"))],
                            [nd_str.set(join(e_t.get(), "..."))]),
                ]),
    ], "_ellipsize(): unlike fit_text this keeps the text when nothing fits.")
    api["ellipsize"] = ellipsize

    ff_i, ff_done = v("nd ff i", 0), v("nd ff done", 0)
    font_out = v("nd font", FONT_N)
    fit_font = proc("ui fit font %s width %s ladder %s", ["text", "width", "ladder"])
    define(fit_font, [
        font_out.set(letter_of(length_of(fit_font.arg("ladder")),
                               fit_font.arg("ladder"))),
        ff_done.set(0),
        ff_i.set(1),
        repeat_until(OR(eq(ff_done.get(), 1),
                        gt(ff_i.get(), length_of(fit_font.arg("ladder")))), [
            measure.call(fit_font.arg("text"),
                         letter_of(ff_i.get(), fit_font.arg("ladder"))),
            if_(NOT(gt(tw.get(), fit_font.arg("width"))), [
                font_out.set(letter_of(ff_i.get(), fit_font.arg("ladder"))),
                ff_done.set(1),
            ]),
            ff_i.change(1),
        ]),
    ], "_fit_font() over a ladder written biggest-first, e.g. 321 for "
       "font_n, font_md, font_s. Falls back to the smallest.")
    api["fit_font"] = fit_font

    # --- wrapping ---------------------------------------------------------
    w_cur, w_word, w_i, w_ch = (v("nd w cur", ""), v("nd w word", ""),
                                v("nd w i", 0), v("nd w ch", ""))
    w_cand, w_done = v("nd w cand", ""), v("nd w done", 0)
    b_cur, b_i, b_next = v("nd b cur", ""), v("nd b i", 0), v("nd b next", "")
    newline = v("nd newline")

    break_word = proc("ui break word %s font %s width %s", ["word", "font", "width"])
    define(break_word, [
        b_cur.set(""),
        b_i.set(1),
        repeat(length_of(break_word.arg("word")), [
            b_next.set(join(b_cur.get(), letter_of(b_i.get(), break_word.arg("word")))),
            measure.call(b_next.get(), break_word.arg("font")),
            if_else(AND(NOT(eq(b_cur.get(), "")), gt(tw.get(), break_word.arg("width"))),
                    [lines.add(b_cur.get()),
                     b_cur.set(letter_of(b_i.get(), break_word.arg("word")))],
                    [b_cur.set(b_next.get())]),
            b_i.change(1),
        ]),
        if_(NOT(eq(b_cur.get(), "")), [lines.add(b_cur.get())]),
    ], "break_long_word(): a word wider than the column is cut wherever it "
       "stops fitting, one character at a time.")

    flush_word = proc("ui wrap flush font %s width %s break %s",
                      ["font", "width", "break"])
    define(flush_word, [
        w_done.set(0),
        if_(NOT(eq(w_word.get(), "")), [
            if_(eq(flush_word.arg("break"), 1), [
                measure.call(w_word.get(), flush_word.arg("font")),
                if_(gt(tw.get(), flush_word.arg("width")), [
                    if_(NOT(eq(w_cur.get(), "")), [lines.add(w_cur.get()),
                                                   w_cur.set("")]),
                    break_word.call(w_word.get(), flush_word.arg("font"),
                                    flush_word.arg("width")),
                    w_done.set(1),
                ]),
            ]),
            if_(eq(w_done.get(), 0), [
                if_else(eq(w_cur.get(), ""),
                        [w_cand.set(w_word.get())],
                        [w_cand.set(join(join(w_cur.get(), " "), w_word.get()))]),
                measure.call(w_cand.get(), flush_word.arg("font")),
                if_else(OR(NOT(gt(tw.get(), flush_word.arg("width"))),
                           AND(eq(w_cur.get(), ""), eq(flush_word.arg("break"), 0))),
                        [w_cur.set(w_cand.get())],
                        [if_(NOT(eq(w_cur.get(), "")), [lines.add(w_cur.get())]),
                         w_cur.set(w_word.get())]),
            ]),
            w_word.set(""),
        ]),
    ])

    wrap = proc("ui wrap %s font %s width %s break %s trim %s",
                ["text", "font", "width", "break", "trim"])
    define(wrap, [
        lines.clear(),
        w_cur.set(""),
        w_word.set(""),
        w_i.set(1),
        repeat(length_of(wrap.arg("text")) + 1, [
            w_ch.set(letter_of(w_i.get(), wrap.arg("text"))),
            if_else(OR(eq(w_ch.get(), " "), eq(w_ch.get(), newline.get()),
                       gt(w_i.get(), length_of(wrap.arg("text")))), [
                flush_word.call(wrap.arg("font"), wrap.arg("width"),
                                wrap.arg("break")),
                if_(OR(eq(w_ch.get(), newline.get()),
                       gt(w_i.get(), length_of(wrap.arg("text")))), [
                    lines.add(w_cur.get()),
                    w_cur.set(""),
                ]),
            ], [
                w_word.set(join(w_word.get(), w_ch.get())),
            ]),
            w_i.change(1),
        ]),
        if_(eq(wrap.arg("trim"), 1), [
            repeat_until(OR(eq(lines.length(), 0),
                            NOT(eq(lines.item(lines.length()), ""))),
                         [lines.delete(lines.length())]),
        ]),
    ], "_wrap_lines() with break=0, and the _wrap_text() of TextInputLong and "
       "MessageDialog with break=1. trim=1 drops the trailing blank lines, "
       "which _wrap_lines does and TextInputLong does not. A blank line "
       "survives as \"\" so the caller can decide what a paragraph break is "
       "worth on a screen this size.")
    api["wrap"] = wrap

    import port_screens
    return port_screens.build(port, api)


def _key_pressed(name):
    from blocks import key_pressed
    return key_pressed(name)

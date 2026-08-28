# UI framework (widgets) — C port specification

> **⚠ SUPERSEDED IN ONE PLACE: the background.** Every `fill="black"` clear
> below is now a call to `nd_ui_paint_chrome()`, `nd_ui_paint_chrome_full()` or
> `nd_ui_paint_chrome_content()` over the *same rectangle*. Those paint the
> configured wallpaper — dimmed a second time by
> `system.ui.wpeverywhere_dim`, on top of the 0.3 the home screen uses — or
> flat black when there is no wallpaper, when `system.ui.wpeverywhere` is off,
> or when the running app's `manifest.json` says `"useWallpaper": false`. The
> rectangles, the partial-versus-full clears and everything drawn on top are
> unchanged, including the off-by-one in `(0, 0, 240, 145)`; read every
> `fill="black"` below as "the background", not as "black". See `nd_ui.h`.

> **⚠ SUPERSEDED IN ONE MORE PLACE: "draws once, then blocks".** That is still
> the shape of every `show()` below, but the background underneath it can now
> move. A widget registers a redraw with `nd_ui_set_repaint()` before its loop
> and restores the previous one after, and `nd_ui_wait_for_key()` advances an
> animated wallpaper and calls it. The widget's own `draw()` is unchanged —
> it is simply called again, and must therefore stay safe to call at any
> moment with no argument but its own state. `system.ui.wpanimate=HOME` or
> `=OFF` turns this off; the still-wallpaper case is unaffected either way,
> because there is nothing to advance. See `nd_ui.h`.

> **⚠ STALE: measured on a raqm host.** Any font advance / text-width table in this
> document is wrong. Pillow on the phone is built `-Craqm=disable` and uses BASIC
> layout: `getlength("Menu")@20` is **68**, not 65. Take all text metrics from
> `neodct/tests/golden/font/fontref.json`, which was captured with BASIC forced.
> See decision C-1 in `OPEN-QUESTIONS.md`.

Source of truth: `neodct/overlay/NeoDCT/System/ui/framework.py` (1937 LOC),
`neodct/overlay/NeoDCT/System/ui/Dialer/call_screen.py` (220 LOC),
`neodct/overlay/NeoDCT/System/ui/Dialer/incoming_screen.py` (130 LOC),
`neodct/overlay/NeoDCT/System/ui/__init__.py` (0 LOC).

Every number in this document was either read out of the Python or **measured** by
booting the real UI headlessly through `neodct/tools/uistub.py` and dumping the actual
values and pixels. Where a value is measured it is marked *(measured)*.

---

## What this does (plain English)

This is the box of screen parts the whole phone is built from. Nothing here knows what a
phonebook or a text message is. It knows how to draw a menu, a text box, a warning, a
progress bar, and how to wait for you to press a key.

There are thirteen parts. Each one is a small object you create, hand some text to, and
then call `.show()` on. `.show()` paints the screen, sits in a loop waiting for keys, and
does not come back until you press something that ends it. Then it hands back an answer —
which item you picked, what you typed, or "you pressed Back".

The parts:

- **AppSelector** — the main menu. One big app icon at a time with its name above it, and
  a Nokia scrollbar down the right. Up/Down flick between apps, Enter launches one.
- **SoftKeyBar** — the strip along the bottom that says what the middle button does right
  now: "Select", "OK", "Back", "End". Thirty pixels tall.
- **HeaderWidget** — the tiny "1-4" breadcrumb in the top-right corner. "1" is which app
  you are in, "4" is which item in its list you are on.
- **VerticalList** — the ordinary three-item menu with a white highlight bar on the
  selected row. Pressing a number key jumps straight to that row.
- **TextInput** — a one-line box for a name, a number, a host name. Has a blinking
  underscore for a cursor.
- **TextInputLong** — the message composer. Wraps text over five lines and counts the
  characters in the corner.
- **MessageDialog** — a full-screen warning with a triangle icon. "LOW BATTERY!" is one.
- **PagedList** — a menu that shows one item per screen in big type, the way the 5190's
  main menus worked.
- **TextScroller** — a help/instructions reader. The softkey says "More" until the last
  page, where it becomes "Back".
- **LevelSelector** — the game difficulty picker. A VerticalList of "Level 1" … "Level 9"
  with an OK softkey.
- **InfoScreen** — a centred label with a big number under it. "Top score / 385".
- **ProgressScreen** — a step name, a progress bar, and a percentage underneath. Never
  draws text on top of the bar.
- **DetailPage** — a page you read: a small picture on the left, its title and details
  beside it, a rule, then body text that scrolls a line at a time.
- **PredictiveText** — not a screen. It is the shared brain behind the two text boxes
  that lets the phone guess the word you are typing, and underline its guess until you
  accept it.

Plus two call screens under `Dialer/` — the "Calling…" screen and the ringing screen.

There are also a handful of small helper functions used everywhere: shorten this text
until it fits and put "..." on the end; word-wrap this paragraph to this many pixels;
pick the biggest of these three type sizes that fits; draw a little pencil.

**Two things about how the drawing works that decide whether the C version looks
identical.**

First, every widget draws onto one shared 240×175 picture held by the core (`ui.canvas`),
and then calls `ui.fb.update(ui.canvas)` to push it to the screen. Nobody clears the whole
picture unless they say so — most widgets clear only the top 145 rows and leave the
softkey strip alone, which is exactly how a caller can set the softkey text first and then
draw a list over it. Get that ordering wrong and the softkey vanishes.

Second, when the code asks "how big is this text?", the answer it gets back is the size of
the **ink**, not the size of a line of type. The string `"_"` is 3 pixels tall at the
normal size; `"Ag"` is 21. Widgets centre things using that number, so text visibly shifts
by a few pixels depending on which letters are in it. That is not a bug to fix — it is
what the current screens look like, and the C version has to reproduce it exactly.

---

## Files and where they go in C

| Python file | LOC | What is in it | C destination |
| --- | --- | --- | --- |
| `System/ui/framework.py` | 1937 | 13 widget classes, the predictive-text mixin, 8 module helpers, the T9 mode indicator | split across `nd_ui_*.c` in `libneodct.so` (see **Proposed C modules**) |
| `System/ui/Dialer/call_screen.py` | 220 | in-call screen + its own evdev reader | `nd_dialer_call.c` — **core process only**, not in the app-facing header |
| `System/ui/Dialer/incoming_screen.py` | 130 | ringing screen, phonebook name lookup, blink | `nd_dialer_incoming.c` — core process only |
| `System/ui/__init__.py` | 0 | empty package marker | nothing |
| `System/ui/resources/fonts/font.ttf` | — | **Nokia Cellphone FC (Small)** *(measured)* | copied verbatim; FreeType loads it at 14/18/20/24 px |
| `System/ui/resources/img/errorscreen/warning.png` | — | 24×24 RGBA *(measured)* | copied verbatim |
| `System/ui/resources/img/appselector/placeholder_icon.png` | — | 50×50 RGBA *(measured)* | copied verbatim (currently unreferenced by framework.py) |

Everything in `framework.py` belongs in `libneodct.so`: both the core process and every
app process draw with it, so it must be one shared mapping.

---

## The `ui` context object — the contract every widget depends on

Widgets never touch hardware. They reach through a single object (`self.ui`), which in
Python is `NeoDCT_UI` from `System/core/main.py`. The C equivalent is one struct,
`nd_ui`, passed to every widget. This is the complete list of what the widget code in this
subsystem reads off it:

| Python attribute | Value on the device | Used by | C member |
| --- | --- | --- | --- |
| `ui.W` | `240` | every widget via `_ui_width()` | `int32_t w` |
| `ui.H` | `175` | every widget via `_ui_height()` | `int32_t h` |
| `ui.SOFTKEY_H` | `30` | `_softkey_height()` | `int32_t softkey_h` |
| `ui.content_bottom` | `145` | Dialer screens only (framework recomputes it) | `int32_t content_bottom` |
| `ui.canvas` | PIL `Image` RGB 240×175 | `paste()` calls | `nd_image *canvas` |
| `ui.draw` | PIL `ImageDraw` bound to canvas | every drawing call | `nd_draw *draw` |
| `ui.fb` | `Framebuffer`; `.update(img)` presents | every widget | `nd_fb *fb` |
| `ui.font_s` | truetype(font.ttf, **14**) | body text, hints, long input | `nd_font *font_s` |
| `ui.font_md` | truetype(font.ttf, **18**) | VerticalList rows, dialog titles | `nd_font *font_md` |
| `ui.font_n` | truetype(font.ttf, **20**) | the default face | `nd_font *font_n` |
| `ui.font_xl` | truetype(font.ttf, **24**) | screen titles, PagedList items | `nd_font *font_xl` |
| `ui.get_text_size(text, font)` | `(bbox[2]-bbox[0], bbox[3]-bbox[1])` of `draw.textbbox((0,0), …)` | everywhere | `nd_text_size()` |
| `ui.get_image(path, max_size=None, scale=None)` | cached RGBA `Image` or `None` | AppSelector, MessageDialog, DetailPage | `nd_ui_image()` |
| `ui.wait_for_key()` | blocks forever until a key code arrives | every `show()` | `nd_ui_wait_key()` |
| `ui.read_keypress(timeout)` | polls; returns code or `None` | Dialer screens | `nd_ui_read_key()` |
| `ui.keypad_fd` | evdev fd or `None` | input-flush in AppSelector / PagedList / MessageDialog | `int keypad_fd` |
| `ui.matrix_input` | i2c keypad object or `None` | `_t9_active()` — decides T9 vs QWERTY | `bool has_matrix_keypad` (in an app: from `NEODCT_KEYPAD_MATRIX`, not from `ui->input` — BR-3) |
| `ui.wallpaper` | dimmed 240×175 RGB `Image` or `None` | SoftKeyBar transparent mode | `nd_image *wallpaper` |
| `ui.softkey` | the core's own `SoftKeyBar` | **its mere existence** decides softkey transparency | see §SoftKeyBar |
| `ui.modem` | ModemService | Dialer screens | `nd_modem *modem` |
| `ui.home_layout` | parsed `ui_home.json` | Dialer screens (status icons) | `nd_home_layout *` |
| `ui.render_element(el)` | draws one home-layout element | Dialer screens | `nd_home_render_element()` |

**Fallback rule to preserve.** `_ui_width`/`_ui_height`/`_softkey_height` use
`int(getattr(ui, "X", DEFAULT))` with `DEFAULT_UI_W = 240`, `DEFAULT_UI_H = 175`,
`DEFAULT_SOFTKEY_H = 30`. In C these are struct fields, but keep the same three
constants named `ND_UI_W`, `ND_UI_H`, `ND_SOFTKEY_H` so a future panel change has one
place to edit. `APP_SELECTOR_ICON_MAX = 175` is the fourth module constant.

### Derived geometry — one table, used by everything

```
_ui_width(ui)          = 240
_ui_height(ui)         = 175
_softkey_height(ui)    = 30
_content_bottom(ui)    = 175 - 30            = 145
_header_divider_y(ui)  = max(30, int(175*0.11)) = max(30, 19) = 30   (measured: 30)
```

`_header_divider_y` is `30` on this hardware because the `max(30, …)` floor wins. Port the
formula, not just the constant.

---

## Behaviour that must be reproduced exactly

### 0. Drawing primitive semantics (verified against Pillow 12.3.0)

The rasterizer agent owns these, but every coordinate below assumes them, so they are
pinned here:

| Primitive | Exact behaviour *(measured)* |
| --- | --- |
| `draw.rectangle((x0,y0,x1,y1), fill=c)` | fills **inclusive** of both corners: `(2,3,6,8)` lights x∈[2,6], y∈[3,8] |
| `draw.rectangle(..., outline=c)` | 1-px border on the inclusive rectangle; interior untouched |
| `draw.line((x,y0,x,y1), fill=c)` | 1-px column at `x`, rows `y0..y1` inclusive |
| `draw.line(..., width=2)` | grows by **+1 in the minor axis only**: a vertical line at x=10 lights columns 10 **and 11**; a horizontal line at y=10 lights rows 10 **and 11** |
| float coordinates | truncated toward zero, **not rounded**: `3.6 → 3`, `8.5 → 8`, `2.5 → 2` |
| `"white"` / `"black"` / `"gray"` | `(255,255,255)`, `(0,0,0)`, **`(128,128,128)`** |
| `draw.text((x,y), s, font)` | anchor is `"la"`: `y` is the **ascender line**, not the ink top. Ink begins at `y + bbox_top`, where `bbox_top` is 1 (14 px), 3 (18 px), 2 (20 px), 3 (24 px) *(measured)* |
| text rendering | **antialiased** — a white glyph on black produces intermediate values (76, 128, 152 observed) *(measured)*. FreeType `FT_RENDER_MODE_NORMAL` 8-bit, composited with Pillow's `MULDIV255` blend |
| `canvas.paste(img, box, img)` | alpha-composite using the RGBA image's own alpha as mask |
| `canvas.paste(img, box)` | straight copy, alpha ignored |
| `image.thumbnail((w,h), LANCZOS)` | in-place, aspect-preserving, never upscales |

`ui.get_text_size(text, font)` returns **ink extent**, not line metrics. Concrete values
for this font *(all measured)*:

| string | 14 px | 18 px | 20 px | 24 px |
| --- | --- | --- | --- | --- |
| `"Ag"` | (21, 15) | (27, 17) | (30, 21) | (36, 24) |
| `"A"` | (11, 13) | (14, 15) | (15, 18) | (18, 21) |
| `"abc"` | (30, 13) | (38, 15) | (43, 18) | (51, 21) |
| `"ABC"` | (32, 13) | (41, 15) | (45, 18) | (54, 21) |
| `"123"` | (28, 13) | (36, 15) | (40, 18) | (48, 21) |
| `"Select"` | (51, 13) | (65, 15) | (73, 18) | (87, 21) |
| `"_"` | (11, 2) | (14, 3) | (15, 3) | (18, 3) |
| `"..."` | (16, 4) | (20, 4) | (23, 5) | (27, 6) |

`font.getmetrics()` (ascender, descender) is (14,4) / (18,5) / (20,5) / (24,6)
*(measured)* — the framework never reads these, but the text renderer needs them for the
`"la"` anchor.

**The font has no glyph for U+2026 (…) or U+270F (✏).** `getbbox("…")` returns
`(0, 20, 8, 20)` — zero height, 8 px advance *(measured)*. That is why the code writes
`"..."` everywhere. `MessageDialog` is the one place that still emits a literal `" …"`
(§10), and it therefore draws **an invisible 8-px gap**. Reproduce it; do not "fix" it to
three dots.

### 1. Module-level helpers

```python
T9_PENCIL_GAP      = 4
DEFAULT_UI_W       = 240
DEFAULT_UI_H       = 175
DEFAULT_SOFTKEY_H  = 30
APP_SELECTOR_ICON_MAX = 175
DEFAULT_WARNING_ICON = "/NeoDCT/System/ui/resources/img/errorscreen/warning.png"
```

**`fit_text(ui, text, font, max_width) -> str`**
1. `max_width <= 0` or empty text → `""`.
2. Fits → return unchanged.
3. Otherwise, for `end` from `len(text)-1` down to `1`: candidate = `text[:end].rstrip() + "..."`;
   first candidate that fits is returned. `rstrip()` is Python's — strips all Unicode
   whitespace from the right.
4. Nothing fits → `""`.

Only `VerticalList` calls it, to keep the title clear of the breadcrumb.

**`_ellipsize(ui, text, font, max_w) -> str`**
1. Fits → return unchanged.
2. `trimmed = text`; while `trimmed` non-empty and `width(trimmed + "...") > max_w`: drop
   the last character.
3. Return `trimmed + "..."` if `trimmed` is non-empty, else the **original untrimmed
   `text`**. (Note the asymmetry with `fit_text`, which returns `""`. Port both.)

**`_font_ladder(ui, *names) -> [font]`** — for each name in order, `getattr(ui, name, None)`;
keep it if non-`None` **and not already in the list** (identity/equality dedupe). On the
device `_font_ladder(ui, "font_n", "font_md", "font_s")` = `[20 px, 18 px, 14 px]`.

**`_fit_font(ui, text, max_w, fonts) -> font`** — the first font whose ink width for `text`
is `<= max_w`; if none fits, `fonts[-1]`.

**`_wrap_lines(ui, text, font, max_w) -> [str]`** — the plain wrapper (no long-word
breaking):
```
for raw in (text or "").splitlines() or [""]:
    if raw.strip() == "":  emit ""  and continue        # blank line preserved
    current = ""
    for word in raw.split(" "):
        if word == "": continue                          # collapses runs of spaces
        candidate = word if not current else current + " " + word
        if width(candidate) <= max_w or not current: current = candidate
        else: emit(current); current = word
    emit(current)
while lines and lines[-1] == "": lines.pop()             # trailing blanks dropped
```
A word wider than `max_w` is emitted on its own over-wide line (`or not current`). Python's
`splitlines()` splits on `\n`, `\r`, `\r\n`, `\v`, `\f`, `\x1c`–`\x1e`, `\x85`, U+2028,
U+2029 — the C version must at minimum handle `\n` and `\r\n`; the exotic separators are
not reachable from any current caller.

**`_wrap_text` (three near-duplicate copies!)** — `TextInputLong._wrap_text` and
`MessageDialog._wrap_text` are the same algorithm as `_wrap_lines` **plus** hard breaking
of over-long words, via a nested `break_long_word(word)` that accumulates characters until
one more would exceed `max_w`. Differences to preserve:

| | `_wrap_lines` | `TextInputLong._wrap_text` | `MessageDialog._wrap_text` |
| --- | --- | --- | --- |
| blank source line | emits `""` | emits `""` (falls out of the empty `words` loop) | emits `""` |
| long word | left over-wide | hard-broken into pieces | hard-broken into pieces |
| trailing blanks | popped | **kept** | popped |
| empty result | `[]` possible | returns `[""]` | `[]` possible |

`PagedList._wrap_to_lines` is a fourth, different wrapper — see §11.

**`_underline_tail(ui, x, y, line, tail_len, font)`**
```
if not tail_len or len(line) < tail_len: return
head_w = width(line[:-tail_len]) if len(line) > tail_len else 0
full_w, height = get_text_size(line, font)
draw.line((x + head_w, y + height + 1, x + full_w, y + height + 1), fill="white")
```
A 1-px horizontal rule one pixel below the ink box. Note `height` is the **ink** height of
the whole line, so the underline's vertical position depends on whether the line contains
descenders. Port as-is.

### 2. The T9 mode indicator and the pencil

**`_t9_active(ui)`** = `getattr(ui, "matrix_input", None) is not None`. True only on the
real i2c keypad; QEMU (dev keyboard) is False and the indicator is never drawn there.

**`_draw_pencil(draw, x, y, size, fill="white")`** — plotted per pixel, deliberately not
with `polygon()`:
```
end   = size - 1
half  = max(1, round(size / 4.0))     # Python round() = banker's rounding
point = max(4, round(size * 0.55))    # Python round() = banker's rounding
gap   = 2
for row in 0..size-1:
    for col in 0..size-1:
        along  = col + (end - row)
        across = abs(col - (end - row))
        if along > 2*end:            continue
        if along <= point:           width = along * half // point   # floor div
        elif along <= point + gap:   continue
        else:                        width = half
        if across <= width: draw.point((x+col, y+row), fill)
```
**`round()` here is Python's banker's rounding (half-to-even), not C's `round()`.**
`round(3.5) == 4`, `round(4.5) == 4`, `round(2.5) == 2`. Sizes where this bites:
`size=14 → 14/4=3.5 → 4`; `size=18 → 4.5 → 4`; `size=22 → 5.5 → 6`. Write a helper
`nd_round_half_even(double)` and use it here.

At the size the phone actually renders (15 px, see below) the bitmap is exactly this
*(measured — reproduce this pattern byte for byte)*:
```
..........#####.
.........######.
........#######.
.......########.
......#########.
.....#########..
....#########...
....########....
..#..######.....
..##..####......
..###..##.......
.#####..........
.######.........
.###............
#...............
```
(15 rows × 15 columns, drawn inside a 15×15 box; the 16th column/row shown as `.` is
outside the box.)

**`t9_indicator_size(ui, t9) -> (width, label, pencil_size) | None`**
```
if not _t9_active(ui):            return None
mode = t9.mode                                   # "word" | "abc" | "ABC" | "123"
if mode != MODE_WORD:             return (width(mode, font_n), mode, 0)
label = "abc"
tw, th = get_text_size("abc", font_n)            # (43, 18) on device
size   = max(8, int(th * 0.85))                  # int(15.3) = 15
return (size + T9_PENCIL_GAP + tw, label, size)  # (15 + 4 + 43) = 62
```
Non-predictive widths on the device: `"abc"` 43, `"ABC"` 45, `"123"` 40.

**`draw_t9_indicator(ui, right, y, t9) -> int`** — draws with the indicator's **right
edge** at `right`, returns the width used (0 when inactive).
```
x = right - width
if pencil:
    text_h = get_text_size(label, font_n)[1]          # 18
    _draw_pencil(ui.draw, x, y + max(0, text_h - pencil), pencil)   # y + 3
    x += pencil + T9_PENCIL_GAP                       # x += 19
draw.text((x, y), label, font=font_n, fill="white")
return width
```

### 3. `PredictiveText` (mixin, not a screen)

Mixed into `TextInput` and `TextInputLong`. State: `self._pending_len` (how many
characters at the insertion point are provisional), `self._candidates`,
`self._candidate_idx`, plus the widget's own `self.text` / `self.t9`. In C this becomes a
struct embedded in both widgets.

| Method | Behaviour |
| --- | --- |
| `_predict_reset()` | `_pending_len = 0`, `_candidates = []`, `_candidate_idx = 0`, **and** `t9.clear_word()`. Both halves or neither — leaving digits in the engine makes the next key continue a word the field already threw away. |
| `_insert_at()` | `getattr(self, "cursor", len(self.text))` — `TextInputLong` has a cursor, `TextInput` does not. |
| `pending_word` (property) | `self.text[end - _pending_len : end]` where `end = _insert_at()`; `""` when `_pending_len == 0`. |
| `_show_word(word)` | Splices `word` over the provisional tail: `start = end - _pending_len`; `text = text[:start] + word + text[end:]`; `_pending_len = len(word)`; if the widget has a cursor, `cursor = start + len(word)`. |
| `_commit_word()` | Just calls `_predict_reset()`. The characters stay in `.text`. |
| `_predict(digits)` | Empty digits → `_show_word("")`, `_candidates = []`. Else `_candidates = t9_dict.shared().suggest(digits)`, `_candidate_idx = 0`, `_show_word(candidates[0] if candidates else digits)` — **with no dictionary the raw digits are shown**, so the keypad is never dead. |
| `_next_candidate()` | `False` if fewer than 2 candidates; else advance `(idx+1) % n`, `_show_word(candidates[idx])`, `True`. |
| `_predict_key(op)` | `op` is the `(kind, value)` from the T9 engine. `kind == "word"` → `_predict(value)`, return `"typed"`. `kind == "next"` → `"typed"` if `_next_candidate()` else `None`. **Anything else** → `_commit_word()` then return `None`, meaning "handle it as an ordinary key". |
| `_predict_backspace()` | `False` if `_pending_len == 0`. Else `left = t9.pop_word_digit()`; `False` if that returned `None`; else `_predict(left)`, and if `left == ""` also `_predict_reset()`; return `True`. So Clear removes **a typed digit**, not a guessed letter — `"good"` becomes `"inn"` (the guess for `"466"`), not `"goo"`. |

The dictionary is `System.hw.t9_dict.shared()` — a binary search over
`/NeoDCT/System/core/t9.dict` (≈3 MB, never loaded into RAM; one open fd, ~17 seeks per
lookup, `MAX_SUGGESTIONS = 8`, `MIN_PREFIX = 2`, digits must all be in `"23456789"`). That
module belongs to the T9 subsystem; this spec only needs `suggest(digits, limit=8) -> [str]`.

### 4. `AppSelector`

```python
AppSelector(title, items, ui, background=None)
```
**Note the argument order** — `ui` is third here, unlike every other widget. `items` is a
list of dicts with at least `"name"` and `"icon"` (an absolute path or falsy).
`self.selected_index = 0`. `title` is stored but **never drawn** — the header shows the
current app's name instead.

`draw()`, in order, with device values:

| # | What | Exact call |
| --- | --- | --- |
| 1 | background | `background` set → `canvas.paste(background, (0,0))`; else `draw.rectangle((0,0,240,175), fill="black")` |
| 2a | **empty list** → `"No Apps"` | `w,h = size("No Apps", font_n)`; `y = max(30, 30 + ((145-30-h)//2))` = `30 + (115-h)//2`; `draw.text(((240-w)//2, y), …, font_n, white)`; `fb.update(canvas)`; **return** |
| 2b | app name | `w,h = size(name, font_xl)`; `title_y = 30 - 16 = 14`; `draw.text(((240-w)//2, 14), name, font_xl, white)` |
| 3 | icon | `icon_y = 30 + max(24, int((145-30)*0.22))` = `30 + max(24, 25)` = **55**. `icon_cap = min(175, max(24, 145-55-8))` = `min(175, 82)` = **82**. `img = ui.get_image(icon_path, max_size=82)`, falling back to `ui.get_image(icon_path)` on `TypeError`. If `img`: `canvas.paste(img, ((240-img.width)//2, 55), img)` — **alpha mask**. |
| 3b | icon missing | `draw.rectangle((79, 55, 161, 137), outline="white")` (`px = (240-82)//2 = 79`); then `"?"` in `font_xl`: `qw,qh = size("?", font_xl)`; `draw.text((79 + (82-qw)//2, 55 + (82-qh)//2), "?", font_xl, white)` |
| 4 | footer | `w,h = size("Select", font_n)` = `(73,18)`; `footer_y = 145 + max(0, (30-18)//2)` = **151**; `draw.text((83, 151), "Select", font_n, white)` — x = `(240-73)//2` |
| 5 | scrollbar track | `bar_x = 240-8 = 232`; `track_top = 30+6 = 36`; `track_bottom = max(36, 145-10) = 135`; `draw.line((232,36,232,135), fill="white", width=2)` → **columns 232 and 233** |
| 5b | scrollbar notch | `len>1` → `step = (135-36)/(n-1)` (float); `notch_y = 36 + selected*step` (float); else `notch_y = 36`. `draw.rectangle((228, notch_y-3, 234, notch_y+3), fill="white")` — floats truncate |
| 6 | page number | `page_num = str(selected+1)`; `w,h = size(page_num, font_n)`; `draw.text((240-5-w, 10), page_num, font_n, white)` |
| 7 | present | `fb.update(canvas)` |

The 82×82 cap means the shipped 120×120 icons are thumbnailed once and cached at that
size; `APP_SELECTOR_ICON_MAX = 175` only bites on a taller panel.

`show()`:
1. **Input flush** — if `ui.keypad_fd` is not `None`, loop `select([fd], [], [], 0.01)`;
   while readable, `os.read(fd, 24)` (errors swallowed); stop on the first idle poll.
2. `draw()`.
3. Loop on `ui.wait_for_key()`:
   - **empty `items`**: only `14` or `28` return `-1`; everything else is ignored (guards
     the modulo-by-zero and the out-of-range index).
   - `108` (DOWN) → `selected = (selected+1) % n`; `draw()`.
   - `103` (UP) → `selected = (selected-1) % n`; `draw()`.
   - `28` (ENTER) → return `selected_index`.
   - `14` (BACKSPACE) → return `-1`.
   - anything else ignored, **no redraw**.

### 5. `SoftKeyBar`

```python
SoftKeyBar(ui)
```
- `self.height = _softkey_height(ui)` = 30
- `self.y_start = _ui_height(ui) - height` = **145**
- `self.current_text = None`
- `self.is_transparent = not hasattr(ui, 'softkey')`

That last line is a **construction-order trick**: `core/main.py` does
`self.softkey = SoftKeyBar(self)` at line 596, so the core's own bar is built while
`ui.softkey` does not yet exist → transparent. Every later bar (apps, dialogs, lists) sees
`ui.softkey` already set → opaque. **In C, replace this with an explicit boolean** —
`nd_softkey_init(&bar, ui, /*transparent=*/true)` for the core's home-screen bar, `false`
everywhere else — and set `ui->softkey_exists = true` at the same point in core startup so
any code that still asks gets the same answer. Do not try to emulate `hasattr`.

`update(new_text, present=True)`:
```
if is_transparent and ui.wallpaper:
    box = (0, 145, 240, 175)
    try:  canvas.paste(wallpaper.crop(box), box)
    except: draw.rectangle((0,145,240,175), fill="black")
else:
    draw.rectangle((0,145,240,175), fill="black")

if new_text:                                    # note: "" is falsy -> no text drawn
    w,h = get_text_size(new_text, font_n)
    draw.text(((240-w)//2, 145 + ((30-h)//2)), new_text, font_n, fill="white")

current_text = new_text
if present: fb.update(canvas)
```
`update("")` and `update(None)` both clear the strip and draw nothing — `ProgressScreen`
and `PagedList`'s empty state rely on that. `paste(crop_result, box)` with a 4-tuple box is
a straight copy of the same-sized region.

### 6. `HeaderWidget`

```python
HeaderWidget(ui, root_id)      # root_id may be an int OR a string ("1-6", "5-5")
```
- `text_for(sub_index=None)` → `"%s-%s" % (root_id, sub_index)` when `sub_index is not
  None`, else `"%s" % root_id`. Python `%s` on an int gives no padding.
- `width(sub_index=None)` → `5 + get_text_size(text_for(sub_index), font_n)[0]` — the `5`
  is the right margin, and callers subtract this to know where a title must stop.
- `draw(sub_index=None)` → `draw.text((240 - 5 - w, 5), text, font_n, fill="white")`.

### 7. `VerticalList`

```python
VerticalList(ui, title, items, app_id=99)     # items: list[str]
```
State: `header = HeaderWidget(ui, app_id)`, `selected_index = 0`, `window_start = 0`,
`max_lines = 3` (recomputed in `draw`).

`draw()`:

| # | What | Exact call (device values) |
| --- | --- | --- |
| 1 | clear | `draw.rectangle((0, 0, 240, 145), fill="black")` — **softkey strip untouched** |
| 2 | title | `reserved = header.width(selected+1)`; `title = fit_text(ui, self.title, font_xl, 240 - 5 - reserved - 6)`; `draw.text((5, 0), title, font_xl, white)` — **y = 0**, unlike every other widget's `y = 5` |
| 2b | breadcrumb | `header.draw(selected_index + 1)` → `(240-5-w, 5)` |
| 3 | divider | `draw.line((0, 30, 240, 30), fill="white")` → row 30 *(measured: the only row lit at both x=0 and x=239)* |
| 4 | metrics | `y_start = 40`; `content_height = max(1, 145-40-4) = 101`; `target_lines = 3`; `line_height = max(28, 101//3) = max(28,33) = 33`; `item_height = max(24, 33-4) = 29`; `max_lines = min(3, max(1, 101//33)) = 3` *(measured: 3)*; `item_font = ui.font_md` (18 px), falling back to `font_n` |
| 4b | window clamp | `if selected < window_start: window_start = selected`; `max_start = max(0, len-3)`; `if window_start > max_start: window_start = max_start` |
| 4c | rows | `bar_x = 235`; `selected_right = max(20, 225) = 225`. For `i` in 0,1,2: `item_idx = window_start+i`, break past the end. `y = 40 + i*33` → **40, 73, 106**. `text_h = size(item, item_font)[1]`; `text_y = y + max(0, (29 - text_h)//2)`. Selected: `draw.rectangle((0, y, 225, y+29), fill="white")` *(measured: rows 40–69, x 0–225 inclusive)* then `draw.text((10, text_y), item, item_font, fill="black")`. Unselected: `draw.text((10, text_y), item, item_font, fill="white")` |
| 5 | scrollbar | `track_top = 40`; `track_bottom = max(40, 140) = 140`; `draw.line((235, 40, 235, 140), fill="gray", width=1)` — **grey (128,128,128), width 1** *(measured: column 235 only)*. This is the only grey pixel in the framework. |
| 5b | notch | `len>1` → `step = (140-40)/(n-1)`; `notch_y = 40 + selected*step`; else 40. `draw.rectangle((233, notch_y-3, 237, notch_y+3), fill="white")` *(measured: x 233–237, y 37–43 at selected=0)* |
| 6 | present | `fb.update(canvas)` |

`show()` — draw once, then loop `wait_for_key()`:
- `108` DOWN: if `selected < len-1`: `selected += 1`; if `selected >= window_start + max_lines`: `window_start += 1`. **`draw()` is called unconditionally**, even at the end of the list.
- `103` UP: if `selected > 0`: `selected -= 1`; if `selected < window_start`: `window_start -= 1`. `draw()` unconditionally.
- `2 <= key <= 10` (digits 1–9): `shortcut_idx = key - 2`; **return it immediately** if `< len(items)`; otherwise fall through and ignore (no redraw).
- `28`: return `selected_index`.
- `14`: return `-1`.

Callers routinely do `SoftKeyBar(ui).update("Select", present=False)` **before** `show()`,
which works only because `draw()` clears just rows 0–145.

### 8. `TextInput`

```python
TextInput(ui, title, prompt, initial_text="", input_filter="any")
```
`input_filter` ∈ `"any" | "letters" | "numbers"`; it is passed straight to
`t9_engine.T9Engine(input_filter=…)` and also gates the QWERTY path via
`t9_engine.char_allowed()`. `self.t9 = T9Engine(...)`, then `self._predict_reset()`.

`DEV_KEYMAP` (identical copy in `TextInputLong`):
```
 2:"1"  3:"2"  4:"3"  5:"4"  6:"5"  7:"6"  8:"7"  9:"8" 10:"9" 11:"0"
16:"q" 17:"w" 18:"e" 19:"r" 20:"t" 21:"y" 22:"u" 23:"i" 24:"o" 25:"p"
30:"a" 31:"s" 32:"d" 33:"f" 34:"g" 35:"h" 36:"j" 37:"k" 38:"l"
44:"z" 45:"x" 46:"c" 47:"v" 48:"b" 49:"n" 50:"m"
57:" " 52:"." 51:"," 12:"-"
```

`draw(blink_state=True)`:

| # | What | Exact call |
| --- | --- | --- |
| 1 | clear | `draw.rectangle((0, 0, 240, 145), fill="black")` |
| 2 | title | `draw.text((5, 5), self.title, font_xl, white)` — **no `fit_text`; a long title runs off the right edge.** Port the bug. |
| 2b | divider | `draw.line((0, 30, 240, 30), fill="white")` |
| 3 | prompt | `prompt_y = 30 + 20 = 50`; `draw.text((10, 50), self.prompt, font_n, white)` |
| 3b | T9 indicator | `draw_t9_indicator(ui, 240-12 = 228, 50, self.t9)` — right edge at x=228 |
| 4 | box | `box_y = 50 + 30 = 80`; `box_h = max(24, min(40, 145-80-10)) = max(24, min(40,55)) = 40`; `box_right = max(20, 230) = 230`; `draw.rectangle((10, 80, 230, 120), outline="white")` |
| 5 | text | `display_text = self.text + ("_" if blink_state else "")`; `text_h = size(display_text or "A", font_n)[1]`; `text_y = 80 + max(0, (40 - text_h)//2)`; `draw.text((15, text_y), display_text, font_n, white)` |
| 5b | underline | `_underline_tail(ui, 15, text_y, self.text, self._pending_len, font_n)` — measured on `self.text`, **not** on `display_text`, so the cursor is excluded |
| 6 | present | `fb.update(canvas)` |

Because `text_h` is an ink height, the baseline moves as you type: empty + cursor → `"_"`
is 3 px → `text_y = 98`; `"Ag"` is 21 px → `text_y = 89`; empty with the cursor off falls
back to `"A"` (18 px) → `text_y = 91`. **The text visibly jumps.** Reproduce it.

`handle_key(key) -> "confirm" | "cancel" | "typed" | "backspace" | "mode" | None`
(does **no** drawing):
1. `key in (28, 96)` → `"confirm"`. (96 is KP_ENTER.)
2. `key == 14`:
   - `_predict_backspace()` returned `True` → `"backspace"`.
   - else `_commit_word()`, `t9.reset()`; if `len(text) > 0`: drop the last character, `"backspace"`; else `"cancel"`.
3. `_t9_active(ui)` (i2c keypad):
   - `op = t9.press(key)`; `None` → `None`.
   - `handled = _predict_key(op)`; not `None` → return it.
   - `kind == "append"` → `text += value`, `"typed"`.
   - `kind == "replace"` → `text = text[:-1] + value`, `"typed"`.
   - `kind == "mode"` → `"mode"`.
   - otherwise `None`.
4. Dev keyboard: `char = DEV_KEYMAP.get(key)`; `None` or `not char_allowed(char, filter)` →
   `None`. **If `len(text) == 0`, `char = char.upper()`** (legacy auto-caps). Append,
   return `"typed"`.

`show() -> str | None`:
```
softkey = SoftKeyBar(self.ui);  softkey.update("OK")        # presents
cursor_on = True;  last_blink = time.time()
self.draw(cursor_on)
loop:
    if time.time() - last_blink > 0.5:
        cursor_on = not cursor_on;  last_blink = time.time();  self.draw(cursor_on)
    key = ui.wait_for_key()          # BLOCKS until a key arrives
    if key is None: continue
    action = self.handle_key(key)
    "confirm"  -> return self.text
    "cancel"   -> return None
    "typed" | "backspace" | "mode" -> self.draw(cursor_on)
```
**The cursor does not actually blink while the phone is idle.** `wait_for_key()` blocks,
so the 0.5 s check only runs after a key arrives. A C implementation that uses a `select()`
timeout would blink — and would then differ from the reference frames. Keep the blocking
wait.

### 9. `TextInputLong`

```python
TextInputLong(ui, title, initial_text="", on_empty_backspace=None, input_filter="any")
```
State: `text`, `cursor = len(text)`, `on_empty_backspace` (callback), `t9`,
`_predict_reset()`, `font = getattr(ui, "font_s", None) or ui.font_n` (**14 px**),
`text_area_top = _header_divider_y(ui) + 10 = 40`,
`text_area_bottom = _content_bottom(ui) - 4 = 141`. Same `DEV_KEYMAP` as `TextInput`.

Accessors: `get_text()`, `set_text(t)` (sets `cursor = len`, `_predict_reset()`),
`clear_text()` (`text=""`, `cursor=0`, `_predict_reset()`),
`set_on_empty_backspace(cb)`.

`_wrap_text(text, max_w)` — the long-word-breaking wrapper described in §1; **trailing
blank lines are kept**, and an empty result returns `[""]`.

`_current_lines(blink_state)` → `_wrap_text(self.text + ("_" if blink_state else ""),
max(20, 240-20) = 220)`.

`draw(blink_state=True)`:

| # | What | Exact call |
| --- | --- | --- |
| 1 | clear | `draw.rectangle((0, 0, 240, 145), fill="black")` |
| 2 | title | `draw.text((5, 5), title, font_xl, white)` — again no `fit_text` |
| 3 | char count | `char_count = str(len(self.text))`; `w,_ = size(char_count, font_n)`; `draw.text((240-5-w, 5), char_count, font_n, white)` |
| 4 | T9 indicator | `draw_t9_indicator(ui, 240 - 5 - w - 10, 5, self.t9)` — right edge 10 px left of the counter |
| 5 | divider | `draw.line((0, 30, 240, 30), fill="white")` |
| 6 | metrics | `_, line_h = size("Ag", self.font)` = 15; **`line_h += 3` → 18**; `max_lines = max(1, int((141-40)/18)) = int(5.61) = 5`; `start = max(0, len(lines) - 5)` — **the last five lines are shown; it scrolls with the cursor, it never pages** |
| 7 | lines | `y = 40`; for each of `lines[start:start+5]`: `draw.text((10, y), line, self.font, white)`; then `y += 18` → rows **40, 58, 76, 94, 112** |
| 7b | underline | only on the **last shown line** and only when `_pending_len`: `body = line[:-1] if (blink_state and line.endswith("_")) else line`; `_underline_tail(ui, 10, y, body, min(_pending_len, len(body)), self.font)` |
| 8 | present | `fb.update(canvas)` |

`handle_key(key) -> "backspace" | "empty_backspace" | "typed" | "mode" | None`:
1. `key == 14`:
   - `_predict_backspace()` → `"backspace"`.
   - else `_commit_word()`, `t9.reset()`; if `len(text) == 0`: call `on_empty_backspace()`
     when callable, return `"empty_backspace"`; else if `cursor > 0`: delete the character
     before the cursor and decrement it; return `"backspace"`.
2. `_t9_active(ui)`: `op = t9.press(key)`; `None` → `None`; `_predict_key(op)` if it
   handled it; `"append"` → insert `value` at `cursor`, `cursor += 1`, `"typed"`;
   `"replace"` → if `cursor > 0` replace the character before the cursor (cursor
   unchanged), `"typed"`; `"mode"` → `"mode"`; else `None`.
3. Dev keyboard: `key in DEV_KEYMAP` → `char`; `not char_allowed` → `None`; **auto-caps
   when `len(text) == 0`**; insert at `cursor`, `cursor += 1`, `"typed"`.
4. Otherwise `None`.

**`TextInputLong` has no `show()`.** The composing loop lives in the calling app
(`System/apps/Messages/main.py`), which owns the blink timer and the softkey. The C widget
must therefore expose `draw()` and `handle_key()` as public API with no loop of its own.

### 10. `MessageDialog`

```python
MessageDialog(ui, message, *, title=None, icon_path=None, button_text="OK",
              accept_keys=(28,), cancel_keys=(14,), margin=8)
```
- `icon_path = icon_path or DEFAULT_WARNING_ICON` (`/NeoDCT/System/ui/resources/img/errorscreen/warning.png`, **24×24 RGBA**).
  Passing `icon_path=""` or `None` both give the warning triangle; there is no way to ask
  for no icon short of a path that fails to load.
- `accept_keys`/`cancel_keys` are coerced with `tuple(x or ())` — callers pass
  `cancel_keys=()` for un-cancellable notices.
- `font_title = font_md or font_n or font_s` → **18 px**; `font_body = font_s or font_n` → **14 px**.

`_flush_input()` — `select([keypad_fd], [], [], 0.0)` in a loop, `os.read(fd, 24)`, stop on
the first idle poll or on any exception. **Timeout 0.0**, unlike AppSelector's 0.01.

`_wrap_text(text, font, max_w)` — the long-word-breaking wrapper; trailing blanks popped.

`_draw()`:

| # | What | Exact call (device values) |
| --- | --- | --- |
| 1 | clear | `draw.rectangle((0, 0, 240, 175), fill="black")` — **the whole screen, softkey strip included** |
| 2 | icon | `icon = ui.get_image(icon_path)` (no `max_size`; exceptions → `None`). `canvas.paste(icon, (8, 8), icon)` *(measured: rows 8–31)* |
| 3 | title | `y = 8`. If `title` and `font_title`: `title_x = 8 + (icon.width + 6 if icon else 0)` = **38**; `draw.text((38, 8), title, font_title, white)`; `_, th = size(title, font_title)`; `y = max(8, 8 + th + 6)` |
| 3b | icon clearance | if `icon`: `y = max(y, 8 + icon.height + 6)` = `max(y, 38)` |
| 4 | choose the look | `max_w = 240 - 16 = 224`; `alert_lines = _wrap_text(message, font_n, 224)`. **If `len(alert_lines) <= 2`**: `font_body = font_n` (20 px), `lines = alert_lines`, `centered = True` — the Nokia alert look. **Else**: `font_body = self.font_body` (14 px), `lines = _wrap_text(message, font_s, 224)`, `centered = False` |
| 5 | line height | `line_h = size("Ag", font_body)[1] + 3` → **24** (alert) or **18** (paragraph) |
| 5b | clip | `max_lines = max(1, int((145 - y - 8) / line_h))`; if `len(lines) > max_lines`: truncate, then `lines[-1] += " …"` unless it already ends with `…`. **U+2026 has no glyph in this font: it draws nothing and costs 8 px of advance** *(measured)*. Reproduce exactly — same codepoint, same invisible result. |
| 6 | vertical centring | `y += max(0, (145 - 8 - y - len(lines)*line_h) // 2)` |
| 7 | lines | for each: `x = max(8, (240 - lw)//2)` when centred, else `x = 8`; `draw.text((x, y), line, font_body, white)`; `y += line_h` |
| 8 | softkey | `SoftKeyBar(ui).update(button_text, present=False)` — a **fresh** bar, so its transparency is re-decided here |
| 9 | present | `fb.update(canvas)` |

Worked example, `MessageDialog(ui, "LOW BATTERY!")`: `y = 38` after the icon; one alert
line at 20 px; `line_h = 24`; `max_lines = int(99/24) = 4`; `y += (145-8-38-24)//2 = 37` →
the line is drawn at **y = 75**, horizontally centred.

`render()` — `_flush_input()` then `_draw()`; returns `None`. Used by the low-battery
shutdown notice, which the user cannot dismiss.

`show()` — `_flush_input()`, `_draw()`, then loop `wait_for_key()` and **return the key**
as soon as it is in `accept_keys` or `cancel_keys`. Callers compare against `28` to tell
Yes from No. Any other key is ignored with no redraw.

### 11. `PagedList`

```python
PagedList(ui, title, items, root_id=99, show_select_hint=True)
```
`items` may be a list of strings **or** a list of dicts with a `"name"` key —
`_get_item_name(idx)` handles both (`str(item.get("name",""))` / `str(item)`), and returns
`""` for an empty list. `root_id` is often a string like `"5-5"`.

Precomputed in `__init__`: `header = HeaderWidget(ui, root_id)`;
`softkey = SoftKeyBar(ui) if show_select_hint else None`;
`_content_top = 30 + 8 = 38`; `_content_bottom = 145 - 10 = 135`; `_bar_x = 240 - 5 = 235`
*(measured: 38 / 135 / 235)*.

`_wrap_to_lines(text, font, max_width, max_lines=2)` — **a fourth wrapper, different from
the other three.** It splits on `text.split()` (any whitespace, no empty tokens), so
newlines and multiple spaces collapse:
```
words = text.split();  if not words: return [""]
lines = [];  cur = "";  i = 0
while i < len(words) and len(lines) < max_lines:
    candidate = (cur + " " + words[i]).strip() if cur else words[i]
    if fits(candidate): cur = candidate; i += 1; continue
    if cur: lines.append(cur); cur = ""; continue
    trimmed = words[i]
    while trimmed and not fits(trimmed + "..."): trimmed = trimmed[:-1]
    lines.append((trimmed + "...") if (i < len(words)-1) else trimmed) if trimmed else lines.append("...")
    i += 1
if len(lines) < max_lines and cur: lines.append(cur)
if i < len(words):                      # words left over
    last = lines[-1] if lines else ""
    if last.endswith("..."): return lines
    trimmed = last
    while trimmed and not fits(trimmed + "..."): trimmed = trimmed[:-1]
    lines[-1] = (trimmed + "...") if trimmed else "..."
return lines[:max_lines]
```
Note the quirk on the single-over-long-word branch: the `"..."` is appended only when this
is **not** the last word. And `lines[-1] = …` will raise `IndexError` if `lines` is empty
at that point — unreachable today because the loop always appends something before `i`
advances, but do not "simplify" it into something that changes when it fires.

`draw()`:

| # | What | Exact call |
| --- | --- | --- |
| 1 | clear | `draw.rectangle((0, 0, 240, 175), fill="black")` — **full screen** |
| 2 | title | `draw.text((5, 5), title, font_xl, white)` |
| 2b | divider | `draw.line((0, 30, 240, 30), fill="white")` |
| 3 | **empty** | `header.draw(None)`; `w,h = size("No Items", font_n)`; `y = 38 + max(0, ((135-38) - h)//2)` = `38 + (97-h)//2` = **77** for h=18; `draw.text(((240-w)//2, 77), "No Items", font_n, white)`; if the softkey exists, `softkey.update(None, present=False)`; `fb.update`; **return** |
| 4 | breadcrumb | `header.draw(selected_index + 1)` |
| 5 | item | `name = _get_item_name(selected)`; `max_w = max(20, 235-12) = 223`; `lines = _wrap_to_lines(name, font_xl, 223, max_lines=2)` |
| 6 | placement | `_, line_h = size("Ag", font_xl)` = **24**; `total_h = len(lines)*(24+6) - 6` → 24 (1 line) or 54 (2 lines); `y0 = 38 + max(0, ((135-38) - total_h)//2)` → **74** (1 line) or **59** (2 lines) |
| 7 | lines | for `li`, `line`: `w,_ = size(line, font_xl)`; **`x = max(5, (max_w - w)//2)`** — centred inside **223**, not 240, so the text sits ~8 px left of the true centre. Port the quirk. `y = y0 + li*30` |
| 8 | scrollbar | `draw.line((235, 38, 235, 135), fill="white", width=2)` → columns **235 and 236** |
| 8b | notch | `len>1` → `step = (135-38)/(n-1)`; `notch_y = 38 + selected*step`; else 38. `draw.rectangle((231, notch_y-3, 237, notch_y+3), fill="white")` |
| 9 | hint | if `softkey` and `show_select_hint`: `softkey.update("Select", present=False)` |
| 10 | present | `fb.update(canvas)` |

`show()`:
1. Input flush, `select(..., 0.01)`, same as `AppSelector`.
2. `if selected_index >= len(items): selected_index = 0`.
3. `draw()`.
4. Loop: `108` → wrap forward **and redraw, only if `items` is non-empty**; `103` → wrap
   backward likewise; `28` → return `selected_index` (**even for an empty list, returning
   0**); `14` → return `-1`.

### 12. `TextScroller`

```python
TextScroller(ui, text, more_text="More", back_text="Back")
```
`font = getattr(ui, "font_n", None) or ui.font_md` → **20 px**; `margin = 10`; `top = 8`;
`page = 0`.

`_wrap_text(text, max_w)` = `_wrap_lines(ui, text, self.font, max_w) or [""]`.

`_paginate() -> (pages, line_h)`:
```
lines  = _wrap_text(text, 240 - 20 = 220)
line_h = size("Ag", font_n)[1] + 4 = 21 + 4 = 25          (measured: 25)
gap_h  = max(4, 25 // 3) = 8
budget = 145 - 8 - 4 = 133
pages, current, used = [], [], 0
for line in lines:
    height = line_h if line else gap_h                     # blank line = 8 px, not 25
    if current and used + height > budget:
        pages.append(current); current, used = [], 0
    if not line and not current: continue                  # never start a page on a gap
    current.append((line, height)); used += height
if current: pages.append(current)
return (pages or [[("", line_h)]]), line_h
```
*(measured: `TextScroller(ui, "One.\n\nTwo.\n\nThree.")._paginate()` →
`[[('One.',25), ('',8), ('Two.',25), ('',8), ('Three.',25)]]`, `line_h = 25`.)*

`draw() -> bool`:
1. `pages, line_h = _paginate()` — **recomputed from scratch on every draw**; `page` is
   then clamped to `0 .. len(pages)-1`.
2. `draw.rectangle((0, 0, 240, 145), fill="black")`.
3. `y = 8`; for `(line, height)` in `pages[page]`: if `line` non-empty,
   `draw.text((10, y), line, self.font, white)`; `y += height` either way.
4. `last_page = page >= len(pages)-1`; `SoftKeyBar(ui).update(back_text if last_page else
   more_text)` — **`present=True`, so this is what pushes the frame**. There is no separate
   `fb.update`.
5. Return `last_page`.

`show()`:
- `28` (ENTER) or `108` (DOWN): if on the last page, **return**; else `page += 1`, redraw.
- `103` (UP): if `page > 0`, `page -= 1`, redraw.
- `14`: return immediately.
- Returns `None` in all cases.

### 13. `LevelSelector(VerticalList)`

```python
LevelSelector(ui, current=1, count=9, title="Level", app_id=6)
```
- `items = ["Level 1", … "Level {count}"]` (f-string `f"Level {n}"`, `n` from 1 to `count`).
- `super().__init__(ui, title, items, app_id=app_id)`.
- `self.selected_index = max(0, min(count-1, int(current) - 1))`.

`show()`:
```
SoftKeyBar(self.ui).update("OK", present=False)     # drawn first, NOT presented
choice = VerticalList.show(self)                    # its draw() clears only rows 0..145,
                                                    # so "OK" survives and is presented
return None if choice < 0 else choice + 1
```
The inherited digit shortcut means pressing `5` returns level 5 directly.

### 14. `InfoScreen`

```python
InfoScreen(ui, title, value=None, softkey_text="Back")
```
`title = title or ""`; `value` is used verbatim (`str(value)` at draw time), so `0` renders
as `"0"` but `None` means "no value".

`show()` draws inline (there is no separate `draw()`):
1. `draw.rectangle((0, 0, 240, 145), fill="black")`.
2. `title_font = ui.font_n` (20), `value_font = ui.font_xl` (24).
3. `tw, th = size(title, font_n)`.
4. **No value**: `ty = max(0, (145 - th)//2)`; `draw.text(((240-tw)//2, ty), title, font_n, white)`.
5. **With value**: `value_text = str(value)`; `vw, vh = size(value_text, font_xl)`;
   `gap = 10`; `total = th + 10 + vh`; `ty = max(0, (145 - total)//2)`;
   title at `((240-tw)//2, ty)`; value at `((240-vw)//2, ty + th + 10)`.
6. `SoftKeyBar(ui).update(softkey_text)` — **presents**.
7. Loop `wait_for_key()`; **return the key** when it is `28` or `14`.

### 15. `ProgressScreen`

```python
ProgressScreen(ui, step, header=None, hint=None, detail=None)
BAR_HEIGHT = 14;  BAR_MARGIN = 20;  INSET = 2
```
`detail` is `callable(done, total) -> str`, drawn right-aligned beside the percentage (the
Update app puts `"5.6 of 12.4 MB"` there). `self._percent = None`.

Fonts: `font_step = font_n or font_md` → 20; `font_small = font_s or font_step` → 14;
`_label_fonts = _font_ladder(ui, "font_n", "font_md", "font_s")` → `[20, 18, 14]`.

Boxes, all computed once in `__init__` and **all measured**:
```
step_h  = size("Ag", font_n)[1]  = 21
small_h = size("Ag", font_s)[1]  = 15

header_box = (0, 4, 240, 19)             # (0, 4, width, 4 + small_h)
divider_y  = 19 + 5 = 24
bar_top    = int(145 * 0.55) = 79
bar_box    = (20, 79, 220, 93)           # (BAR_MARGIN, bar_top, W-BAR_MARGIN, bar_top+14)
label_y    = 79 - 14 - 21 = 44
label_box  = (0, 44, 240, 65)
status_y   = 93 + 9 = 102
status_box = (20, 102, 220, 117)
hint_y     = 145 - 15 - 6 = 124
hint_box   = (0, 124, 240, 139)
```

`set_step(step)` — `self.step = step or ""`, `self._percent = None` (forces a repaint).

`_centered(text, font, box)` → `(max(0, (box[0] + box[2] - text_w) // 2), box[1])`. Note it
uses `box[0] + box[2]`, i.e. **left + right**, not left + width.

`draw(done, total) -> bool`:
1. `percent = int(done * 100 / total) if total else 100`, clamped to `0..100`.
   `int()` truncates; `done*100/total` is float division.
2. **If `percent == self._percent`: return `False` and draw nothing.** The copy loop calls
   this per 256 KB chunk; repainting each time would make the update slower than the write.
3. `self._percent = percent`.
4. `draw.rectangle((0, 0, 240, 145), fill="black")`.
5. `header` set → `draw.text((10, 4), header, font_small, white)` and
   `draw.line((10, 24, 230, 24), fill="white")`.
6. `step` non-empty → `room = 240 - 16 = 224`; `font = _fit_font(ui, step, 224, _label_fonts)`;
   `label = _ellipsize(ui, step, font, 224)`;
   `draw.text(_centered(label, font, label_box), label, font, white)` → y = 44.
7. Bar: `draw.rectangle((20, 79, 220, 93), outline="white", width=1)`;
   `span = (220-2) - (20+2) = 196`; `filled = int(196 * percent / 100.0)`;
   if `filled > 0`: `draw.rectangle((22, 81, 22 + filled, 91), fill="white")`.
8. `reading = "%d%%" % percent`; `detail_text = self.detail(done, total) if self.detail else ""`.
   - With detail: `draw.text((20, 102), reading, font_small, white)`; then
     `detail_w = width(detail_text, font_small)`;
     `draw.text((220 - detail_w, 102), detail_text, font_small, white)`.
   - Without: `draw.text(_centered(reading, font_small, status_box), reading, font_small, white)`
     → x = `(20+220-w)//2` = `(240-w)//2`, y = 102.
9. `hint` set → `hint = _ellipsize(ui, hint, font_small, 224)`;
   `draw.text(_centered(hint, font_small, hint_box), hint, font_small, white)` → y = 124.
10. `SoftKeyBar(ui).update("", present=False)` — clears the strip to black with no label.
11. `fb.update(canvas)`; return `True`.

`ProgressScreen` has **no `show()`** and reads no keys; the caller drives it.

### 16. `DetailPage`

```python
DetailPage(ui, title="", subtitle=None, body="", image=None, badge=None, header=None,
           softkey_text="OK", accept_keys=(28,), cancel_keys=(14,))
MARGIN = 10;  IMAGE_MAX = 64;  MIN_IMAGE = 40;  SCROLLBAR_W = 8
```
The most complex widget. It builds the page once, in the constructor, as a list of
`(paint_callable, height)` blocks, then scrolls that list inside a viewport.

Constructor:
```
font_title  = font_n or font_md          -> 20 px
font_small  = font_s or font_title       -> 14 px
line_height = size("Ag", font_s)[1] + 3  = 18            (measured: 18)
small_h     = size("Ag", font_s)[1]      = 15
top = 4
if header:  header_box = (0, 4, 240, 19);  divider_y = 24;  top = 30
else:       header_box = None;  divider_y = None
viewport = (0, top, 240, 145 - 2 = 143)
            -> with header    (0, 30, 240, 143), viewport_height = 113   (measured)
            -> without header (0,  4, 240, 143), viewport_height = 139
image  = _prepare_image(image)
_blocks = _layout()
content_height = sum(height for _, height in _blocks)
```
Properties: `viewport_height = viewport[3] - viewport[1]`;
`scrollable = content_height > viewport_height`;
`max_offset = max(0, content_height - viewport_height)`.

`_prepare_image(image)`:
- `None` → `None`.
- `str` → `ui.get_image(path, max_size=64)`, falling back to `ui.get_image(path)` on `TypeError`.
- otherwise an in-memory image: if either side exceeds 64, `image = image.copy()` then
  `image.thumbnail((64, 64), Image.Resampling.LANCZOS)`.

`_text_width()` = `240 - 20 = 220`, minus `SCROLLBAR_W = 8` when `scrollable`. (Defined but
only used indirectly — the body wrap loop recomputes it.)

**`_hero_block(image) -> (paint, height)`** — picture left, text column right:
```
text_x = 10 + image.width + 8
column = 240 - text_x - 10 - 8
title_font = _fit_font(ui, title, column, _font_ladder(ui,"font_n","font_md","font_s"))
title      = _ellipsize(ui, title, title_font, column)
title_h    = size("Ag", title_font)[1] + 5   if title else 0
subtitle_lines = _wrap_lines(ui, subtitle, font_small, column) if subtitle else []
rows = [(title, title_font, title_h)]
     + [(line, font_small, line_height) for line in subtitle_lines]
     + ([(_ellipsize(ui, badge, font_small, column), font_small, line_height)] if badge else [])
rows = [r for r in rows if r[0]]              # drop empty strings
stack_h = sum(h for _,_,h in rows)
inner   = max(image.height, stack_h)
height  = inner + 6

paint(canvas, draw, y):
    box = (10, y + 3 + (inner - image.height)//2)
    canvas.paste(image, box, image) if image.mode == "RGBA" else canvas.paste(image, box)
    row_y = y + 3 + (inner - stack_h)//2
    for text, font, row_h in rows:
        draw.text((text_x, row_y), text, font=font, fill="white")
        row_y += row_h
```
For a 64×64 picture: `text_x = 82`, `column = 140`.

**`_fitted_hero()`** — shrinks the picture until the first body line fits on screen:
```
image = self.image
loop:
    paint, height = _hero_block(image)
    if height + line_height <= viewport_height: return (paint, height, image)
    if image.height <= MIN_IMAGE (40):          return (paint, height, image)
    smaller = image.copy()
    side = max(40, image.height - 8)
    smaller.thumbnail((side, side), LANCZOS)
    image = smaller
```
The 8-px step and the 40-px floor are load-bearing: the picture gives ground so the notes
start above the fold.

**`_layout() -> [(paint|None, height)]`** — sets `self.hero_box` and `self.body_top`:
```
width = 240;  hero_box = None

centered(text, font) -> paint that draws at (( 240 - width(text) )//2, y)

CASE A: image is not None and (subtitle or badge)
    paint_hero, hero_h, self.image = _fitted_hero()
    blocks += [(paint_hero, hero_h)];  hero_box = (0, 0, 240, hero_h)

CASE B: image is not None (no subtitle, no badge)
    paint_image: canvas.paste(image, ((240-image.width)//2, y), image if RGBA else no mask)
    blocks += [(paint_image, image.height + 8)]
    if title:
        title_h = size("Ag", font_title)[1]              # 21
        blocks += [(centered(title, font_title), title_h + 6)]     # 27

CASE C: no image
    if title:    blocks += [(centered(title, font_title), size("Ag",font_title)[1] + 6)]   # 27
    if subtitle: for line in _wrap_lines(ui, subtitle, font_small, 240 - 20 = 220):
                     blocks += [(centered(line, font_small), line_height)]                 # 18
    if badge:    blocks += [(centered(badge, font_small), line_height + 4)]                # 22

body_top = sum of the heights so far

if body:
    rule_h = 10
    if blocks and body_top + 10 + line_height <= viewport_height:
        paint_rule: draw.line((30, y+4, 210, y+4), fill="white")   # MARGIN*3 .. W - MARGIN*3
        blocks += [(paint_rule, 10)];  body_top += 10

    body_width = 240 - 20 = 220
    repeat at most twice:
        lines = _wrap_lines(ui, body, font_small, body_width)
        text_blocks = []
        for line in lines:
            if line == "": text_blocks += [(None, line_height // 2)]     # 9 -- a breath
            else:          text_blocks += [(paint_line at (10, y), line_height)]   # 18
        height = sum(blocks heights) + sum(text_blocks heights)
        narrowed = body_width - 8
        if height <= viewport_height or body_width == narrowed: break
        body_width = narrowed                                            # 212, then stop
    blocks += text_blocks
```
The two-pass body wrap exists because whether a scrollbar steals 8 px depends on the
height, which depends on the wrap. Measure without it, redo once if the page turns out to
need one. A blank source line costs **9 px**, half a line — the test
`test_blank_lines_between_paragraphs_do_not_cost_a_whole_line` pins `0 < gap < line_height`.

*(measured: `DetailPage(ui, title="NeoDCT 0.3.2a", subtitle="12.4 MB", body="One.\n\nTwo.",
header="SOFTWARE UPDATE")` → viewport `(0,30,240,143)`, `viewport_height` 113,
`line_height` 18, `content_height` 100, `body_top` 55, `scrollable` False.)*

`draw()`:
1. `draw.rectangle((0, 0, 240, 145), fill="black")`.
2. `header` → `draw.text((10, 4), header, font_small, white)` and
   `draw.line((10, 24, 230, 24), fill="white")` (`MARGIN` … `W - MARGIN`).
3. **`column = Image.new("RGB", (240, max(1, viewport_height)), "black")`** with its own
   `ImageDraw`. Scrolled text is clipped by construction, not by arithmetic. On the device
   this is a 240×113 RGB buffer = **81,360 bytes allocated on every draw** — see Risks.
4. `y = -self.offset`; if **not** `scrollable`, `y += (viewport_height - content_height) // 2`
   (a short page is vertically centred; a scrolling one starts at the top).
5. For each `(paint, height)`: draw it only when `paint is not None` **and**
   `y + height > 0` **and** `y + height <= viewport_height`. A block that would be sliced by
   the bottom edge is skipped entirely — half a line at the fold reads as a bug.
   `y += height` always.
6. `canvas.paste(column, (0, viewport[1]))`.
7. `if scrollable: _draw_scrollbar()`.
8. `SoftKeyBar(ui).update(softkey_text, present=False)`; `fb.update(canvas)`.

`_draw_scrollbar()`:
```
x    = 240 - 5 = 235
top  = viewport[1] + 2          # 32 with a header, 6 without
base = viewport[3] - 2 = 141
draw.line((235, top, 235, 141), fill="white", width=2)      # columns 235 and 236
travel   = base - top - 10
position = top + int(travel * (offset / float(max_offset or 1)))
draw.rectangle((232, position, 238, position + 10), fill="white")
```
`max_offset or 1` guards the divide when the page is not scrollable (the scrollbar is not
drawn then anyway).

`handle_key(key) -> bool`:
- `108` → `new = min(max_offset, offset + line_height)` (+18).
- `103` → `new = max(0, offset - line_height)`.
- anything else → `False`.
- `new == offset` → `False` (no redraw at either end).
- else `offset = new`, `draw()`, `True`.

`show()` — `draw()`, then loop: `key in accept_keys or key in cancel_keys` → **return the
key**; otherwise `handle_key(key)` and keep looping.

### 17. `Dialer/call_screen.py` — the in-call screen

Module constants: `KEYPAD_PATH = "/dev/input/event0"`, `WIDTH = 240`, `HEIGHT = 175`.

Local helpers (these duplicate core functionality and should collapse onto the shared C
input layer, but the **behaviour** must stay):
- `_flush_input(fd)` — `select(..., 0.0)` + `os.read(fd, 24)` until idle.
- `_read_keypress(fd, timeout=0.10)` — `select`, then `os.read(fd, 24)`; unpack `'llHHI'`
  for a 24-byte event or `'IIHHI'` for a 16-byte one; return `code` when `etype == 1` and
  `val == 1`, else `None`. (Same dual-layout handling as `core/main.py`.)
- `_draw_handset_icon(draw, x, y)` — a hand-drawn 19×9 glyph:
  ```
  draw.rectangle((x,   y+2, x+18, y+10), outline="white")
  draw.rectangle((x+1, y+3, x+5,  y+5),  fill="white")
  draw.rectangle((x+13,y+7, x+17, y+9),  fill="white")
  ```
- `_fit_text(ui, text, max_width, prefer_font)` — **a fifth text-fitting function**, and the
  only one that uses a real ellipsis: try `prefer_font`; else `ui.font_s`; else **binary
  search** `lo=0, hi=len(text)`, `mid=(lo+hi)//2`, candidate `text[:mid] + "…"`, keeping the
  widest that fits; returns `(best or "…", small_font)`. Because the font has no U+2026,
  the marker is invisible — reproduce it anyway.

`draw_call_screen(ui, number, name=None)`:
```
screen_w = ui.W (240);  screen_h = ui.H (175)
softkey_h = ui.SOFTKEY_H (30);  content_bottom = ui.content_bottom (145)

draw.rectangle((0, 0, 240, 175), fill="black")
_draw_handset_icon(ui.draw, 8, 10)

status, secs = ui.modem.call_status() if the modem exposes it, else ("CONNECTED", None)
label = {"CALLING": "Calling...", "RINGING": "Ringing..."}.get(status, "Call 1")
label_font = ui.font_n or ui.font_s
label_x = max(34, int(240 * 0.23)) = max(34, 55) = 55
label_y = max(50, int(145 * 0.20)) = max(50, 29) = 50
draw.text((55, 50), label, label_font, white)

num_text = number or ""
fitted_num, num_font = _fit_text(ui, num_text, max_width = 240 - 55 - 10 = 175, label_font)
num_y = 50 + 26 = 76
draw.text((55, 76), fitted_num, num_font, white)

if secs is not None:                       # connected
    timer = "%02d:%02d" % (secs // 60, secs % 60)
    draw.text((55, 100), timer, ui.font_s or num_font, fill="gray")   # num_y + 24

# reuse the home layout's clock and status icons, at their exact home placement
for el in (ui.home_layout or {}).get("elements", []):
    if el["type"] == "text" and el["text"] == "12:00": ui.render_element(el); break
for el in (ui.home_layout or {}).get("elements", []):
    if el["type"] == "icon_set" and el["prefix"] in ("bat","sig"): ui.render_element(el)
```
The commented-out clock block at the top of the function is dead code; do not port it.
There is **no** `fb.update` inside `draw_call_screen` — the caller presents.

`show_calling(ui, number, name=None)`:
```
softkey = SoftKeyBar(ui)
use_ui_reader = hasattr(ui, "read_keypress")
if not use_ui_reader:                    # legacy standalone path
    fd = ui.keypad_fd if hasattr(ui,"keypad_fd") else os.open(KEYPAD_PATH, O_RDONLY|O_NONBLOCK)
    (and ui.keypad_fd = fd);  _flush_input(fd)

last_draw = 0.0
loop:
    now = time.time()
    if now - last_draw >= 0.25:                     # 4 fps
        draw_call_screen(ui, number, name)
        ui.fb.update(ui.canvas)
        last_draw = now
        softkey.update("End")                       # presents a SECOND time
    key = ui.read_keypress(0.10) or _read_keypress(fd, 0.10)
    if ui.modem is not None and ui.modem.state == "IDLE": return    # remote hangup
    if key is None: continue
    if key in (14, 28):
        try: ui.modem.hangup()
        except: pass
        return
```
Note the **two presents per frame** (once for the screen, once inside `softkey.update`).
That is visible as a two-stage repaint on the panel; keep it.

### 18. `Dialer/incoming_screen.py` — the ringing screen

```
KEY_ANSWER = 28;  KEY_DECLINE = 14;  BLINK_S = 0.5;  REFRESH_S = 0.1
```

`_lookup_contact_name(number)` — imports `System.apps.PhoneBook.shared.list_ui.get_all_contacts`
lazily, strips everything but digits from the caller ID and from each stored number
(`row[2]`), and matches when **the last 10 digits are equal** or the full strings are
equal; returns `row[1]` (the name). All exceptions swallowed → `None`. In C this becomes a
call into the phonebook database module — keep the last-10-digits rule exactly.

`_fit_caller_text(ui, text, max_width)` — **a sixth fitter**: try `font_n`, then `font_s`;
if neither fits, trim from the right with `font_s` while `width(trimmed + "...") > max_width`
and return `(trimmed + "...")`, or `("?", font)` when nothing is left.

`draw_incoming_screen(ui, caller_text, blink_on)`:
```
draw.rectangle((0, 0, 240, 175), fill="black")
text, font = _fit_caller_text(ui, caller_text, 240 - 16 = 224)
w = width(text, font)
draw.text(((240 - w)//2, max(18, int(145 * 0.18)) = max(18, 26) = 26), text, font, white)

if blink_on:
    calling_x = 7 + int(36 * (175 / 240.0)) + 6 = 7 + int(26.25) + 6 = 7 + 26 + 6 = 39
    draw.text((39, 145 - 26 = 119), "calling", ui.font_n or font, fill="white")

for el in (ui.home_layout or {}).get("elements", []):
    if el["type"] == "icon_set" and el["prefix"] in ("bat","sig"): ui.render_element(el)
```
The `36` is the width of the signal-strength sprite sheet; `H/240.0` is the same scale
`render_element` applies to status icons.

`show_incoming(ui, number, name=None) -> "answered" | "declined" | "gone"`:
```
softkey = SoftKeyBar(ui);  modem = ui.modem
caller_label(num) = name or _lookup_contact_name(num) or num or "Unknown"
caller_text = caller_label(number);  blink_on = True;  last_blink = 0.0
loop:
    now = time.monotonic()                       # monotonic here, time.time() in call_screen
    if modem and modem.caller_id and modem.caller_id != number:   # late +CLIP
        number = modem.caller_id;  caller_text = caller_label(number);  last_blink = 0.0
    if now - last_blink >= 0.5:
        blink_on = not blink_on;  last_blink = now
        draw_incoming_screen(ui, caller_text, blink_on)
        softkey.update("Answer", present=False)
        ui.fb.update(ui.canvas)
    key = ui.read_keypress(0.1)                  # pumps the modem URCs
    if modem and modem.state not in ("RINGING","INCOMING"):
        return "gone" if modem.state == "IDLE" else "answered"
    if key == 28: return "answered"
    if key == 14: return "declined"
```
The ringer itself is owned by `core/main.py`, not by this screen.

---

## Public interface (the functions other parts call)

Every shipped app and the core reach this subsystem only through these names. Grepped call
sites are listed so the C headers can be shaped to match.

| Python name | Callers | Proposed C |
| --- | --- | --- |
| `AppSelector(title, items, ui, background=)` `.show()` | `core/main.py:938`; `tests/test_uistub.py` | `nd_appsel_init/show` |
| `SoftKeyBar(ui)` `.update(text, present=True)` | 25+ sites: core, every app, both Dialer screens, and 6 widgets internally | `nd_softkey_init/update` |
| `HeaderWidget(ui, root_id)` `.text_for/.width/.draw` | `VerticalList`, `PagedList` only (never called directly by apps) | `nd_header_*` |
| `VerticalList(ui, title, items, app_id=)` `.show()`, `.selected_index` | PhoneBook, Messages, Settings, Power, CallLog, Games, Tones, MusicPlayer, Calculator, Update, Downgrade, RemoteShell, Crash | `nd_vlist_*` |
| `TextInput(ui, title, prompt, initial_text=, input_filter=)` `.show()`, `.handle_key()`, `.text` | PhoneBook (5×), RemoteShell (3×), Messages (subclassed as `ContactNumberInput`) | `nd_textinput_*` |
| `TextInputLong(ui, title, …)` `.draw/.handle_key/.get_text/.set_text/.clear_text/.set_on_empty_backspace/.cursor` | Messages | `nd_textlong_*` |
| `MessageDialog(ui, msg, title=, icon_path=, button_text=, accept_keys=, cancel_keys=, margin=)` `.show()`, `.render()` | ErrorScreen, CrashHandler, Power, Update, Downgrade, Tones, Clock, Messages, MusicPlayer, RemoteShell, Modem, FuelGauge, KeypadMapper×2, TestsApp | `nd_msgdialog_*` |
| `PagedList(ui, title, items, root_id=, show_select_hint=)` `.show()`, `.selected_index` | CallLog (3×), Messages, Tones | `nd_pagedlist_*` |
| `TextScroller(ui, text, more_text=, back_text=)` `.show()`, `._paginate()` | Games, Settings, Tones, MusicPlayer | `nd_scroller_*` |
| `LevelSelector(ui, current=, count=, title=, app_id=)` `.show()` | Games | `nd_levelsel_*` |
| `InfoScreen(ui, title, value=, softkey_text=)` `.show()` | Games, CallLog | `nd_infoscreen_show` |
| `ProgressScreen(ui, step, header=, hint=, detail=)` `.draw(done,total)`, `.set_step()`, `.bar_box/.label_box/.status_box/.hint_box` | Update (4×), Downgrade (2×) | `nd_progress_*` |
| `DetailPage(ui, title=, subtitle=, body=, image=, badge=, header=, softkey_text=, accept_keys=, cancel_keys=)` `.show()`, `.draw()`, `.handle_key()`, `.offset/.max_offset/.scrollable/.content_height/.line_height/.viewport/.hero_box/.body_top` | Update, Downgrade, `tests/test_update_ui.py` | `nd_detailpage_*` |
| `fit_text(ui, text, font, max_width)` | `VerticalList`; `tests/test_framework_predictive.py` | `nd_text_fit()` |
| `t9_indicator_size(ui, t9)` / `draw_t9_indicator(ui, right, y, t9)` | both text widgets; tests | `nd_t9ind_size/draw` |
| `_content_bottom(ui)` | imported by `tests/test_update_ui.py` — an underscore name crossing a module boundary | `nd_ui_content_bottom()` |
| `dialer_ui.show_calling(ui, number, name=)` | `core/main.py` | `nd_dialer_show_calling()` |
| `incoming_ui.show_incoming(ui, number, name=)` | `core/main.py` | `nd_dialer_show_incoming()` |
| `incoming_ui.draw_incoming_screen(ui, text, blink_on)` | `core/main.py` (redraw during ring) | `nd_dialer_draw_incoming()` |

**Subclassing.** Two shipped classes subclass framework widgets:
`LevelSelector(VerticalList)` inside the framework, and
`Messages.ContactNumberInput(TextInput)` in the app. C has no inheritance — give
`nd_vlist` and `nd_textinput` an optional key-hook function pointer, or expose
`draw()`/`handle_key()` publicly so a caller can compose its own loop. `ContactNumberInput`
only pins `input_filter="numbers"`, so a constructor argument covers it.

---

## External dependencies and their C replacements

| Dependency | Used for | C replacement |
| --- | --- | --- |
| `PIL.ImageDraw.text` | every label, in 13 widgets | `nd_draw_text(nd_draw*, x, y, s, nd_font*, nd_color)` — FreeType, anchor `"la"`, 8-bit AA, `MULDIV255` blend |
| `PIL.ImageDraw.rectangle(fill=)` | screen clears, selection bar, scrollbar notches, progress fill, handset icon | `nd_draw_rect_fill()` — inclusive corners |
| `PIL.ImageDraw.rectangle(outline=, width=1)` | TextInput box, progress bar frame, AppSelector icon placeholder, handset outline | `nd_draw_rect_outline()` |
| `PIL.ImageDraw.line(width=1)` | header dividers, VerticalList scrollbar (grey), DetailPage rule, underline tail | `nd_draw_line()` |
| `PIL.ImageDraw.line(width=2)` | AppSelector / PagedList / DetailPage scrollbar tracks | same, `width` argument; **+1 in the minor axis** |
| `PIL.ImageDraw.point` | `_draw_pencil` only (≈100 calls per indicator) | `nd_draw_point()`, or write straight into the pixel buffer |
| `PIL.ImageDraw.textbbox` | `ui.get_text_size` — the single most-used measurement in the file | `nd_text_size()`; must return **ink extents**, matching the table in §0 |
| `PIL.Image.new("RGB", …)` | `DetailPage.draw` scratch column (240×113) | preallocate one scratch surface on the `nd_ui` context at startup — **no per-frame allocation** |
| `PIL.ImageDraw.Draw(img)` | binding a draw context to that scratch column | `nd_draw_bind(&ctx, surface)` |
| `Image.paste(img, box)` | SoftKeyBar wallpaper slice, DetailPage column blit, AppSelector background | `nd_blit()` |
| `Image.paste(img, box, mask)` | icon draws in AppSelector, MessageDialog, DetailPage | `nd_blit_alpha()` |
| `Image.crop(box)` | SoftKeyBar transparent mode | `nd_blit()` with a source rectangle — no copy needed |
| `Image.copy()` + `Image.thumbnail((w,h), LANCZOS)` | `DetailPage._prepare_image`, `_fitted_hero` | `nd_image_scale_lanczos()`; **must match Pillow's LANCZOS** or icons differ pixel-by-pixel |
| `Image.Resampling.LANCZOS` | as above | same filter, same support radius, same coefficient rounding |
| `ui.get_image()` → `Image.open` + `.convert("RGBA")` | PNG icons | libpng or a minimal PNG decoder + the existing FIFO cache (`IMAGE_CACHE_MAX = 32`) |
| `ui.fb.update(canvas)` → `Image.tobytes` | presenting a frame | framebuffer subsystem; unchanged contract |
| `select.select` | input flush in AppSelector, PagedList, MessageDialog, call_screen | `poll()` or `select()` — keep the exact timeouts: **0.01** in AppSelector/PagedList, **0.0** in MessageDialog and `_flush_input` |
| `os.read(fd, 24)` | draining evdev | `read(fd, buf, 24)` |
| `struct.unpack('llHHI' / 'IIHHI')` | `call_screen._read_keypress` | `struct input_event` — note the 24- vs 16-byte handling |
| `time.time()` | TextInput blink, call_screen redraw pacing | `clock_gettime(CLOCK_REALTIME)` — **keep the same clock**, the Python uses wall time here |
| `time.monotonic()` | incoming_screen blink | `clock_gettime(CLOCK_MONOTONIC)` |
| `math` | imported, **never used** | drop |
| `System.hw.t9_engine` | `T9Engine`, `MODE_WORD`, `MODE_ABC`, `char_allowed` | T9 subsystem; interface: `press(code) -> (kind,value)|None`, `.mode`, `.modes`, `set_mode_index()`, `reset()`, `clear_word()`, `pop_word_digit()`, `word_digits` |
| `System.hw.t9_dict` | `shared().suggest(digits, limit=8)` | T9 subsystem; binary search over the on-disk word list, **never loaded into RAM** |
| `System.apps.PhoneBook.shared.list_ui.get_all_contacts` | `incoming_screen` caller-name lookup | phonebook/database subsystem |
| Python `round()` | `_draw_pencil` | `nd_round_half_even()` — **banker's rounding, not C `round()`** |
| Python `str.rstrip()` | `fit_text` | ASCII-space strip is sufficient for current callers; document the narrowing |
| Python `str.splitlines()` | `_wrap_lines` and friends | handle `\n` and `\r\n`; the exotic separators are unreachable today |
| Python `%` int/float division | notch positions, percentages | `int()` truncates toward zero; `//` on positive ints is floor — they agree here, but write it explicitly |

**Nothing in this subsystem needs sqlite, zlib, ssl or lzma.** The only file it opens
directly is the T9 dictionary, through `t9_dict`.

---

## Proposed C modules

All in `libneodct.so`, headers under `include/neodct/`.

| File | Contents | est. LOC |
| --- | --- | --- |
| `nd_ui.h` | the `nd_ui` context struct, `nd_key` codes, `nd_widget_result` enum, `ND_UI_W/H/SOFTKEY_H`, `APP_SELECTOR_ICON_MAX` | 140 |
| `nd_ui_metrics.c/.h` | `nd_ui_width/height/softkey_height/content_bottom/header_divider_y` | 70 |
| `nd_text.c/.h` | `nd_text_fit` (fit_text), `nd_text_ellipsize`, `nd_text_wrap` (`_wrap_lines`), `nd_text_wrap_break` (the long-word variant used by TextInputLong and MessageDialog), `nd_font_ladder`, `nd_fit_font`, `nd_underline_tail`; a reusable `nd_lines` growable line buffer with a hard cap | 420 |
| `nd_t9ind.c/.h` | `T9_PENCIL_GAP`, `nd_draw_pencil`, `nd_t9ind_size`, `nd_t9ind_draw`, `nd_round_half_even` | 150 |
| `nd_predictive.c/.h` | the `PredictiveText` state struct and its eight methods, embedded by both text widgets | 220 |
| `nd_softkey.c/.h` | `SoftKeyBar` — with an explicit `transparent` flag replacing the `hasattr` trick | 110 |
| `nd_header.c/.h` | `HeaderWidget`; `root_id` as a `char[16]` so the string forms (`"1-6"`, `"5-5"`) work | 90 |
| `nd_appsel.c/.h` | `AppSelector` incl. the input flush | 260 |
| `nd_vlist.c/.h` | `VerticalList` **and** `LevelSelector` (a thin constructor plus a `show` wrapper) | 330 |
| `nd_textinput.c/.h` | `TextInput`: draw, handle_key, show, the shared `DEV_KEYMAP` table | 340 |
| `nd_textlong.c/.h` | `TextInputLong`: draw, handle_key, accessors (no `show`) | 340 |
| `nd_msgdialog.c/.h` | `MessageDialog`: `_draw`, `render`, `show`, its own wrapper | 300 |
| `nd_pagedlist.c/.h` | `PagedList` incl. `_wrap_to_lines` | 320 |
| `nd_scroller.c/.h` | `TextScroller` incl. `_paginate` | 220 |
| `nd_infoscreen.c/.h` | `InfoScreen` | 90 |
| `nd_progress.c/.h` | `ProgressScreen` incl. the box table and the percent gate | 260 |
| `nd_detailpage.c/.h` | `DetailPage`: block list, hero fitting, two-pass body wrap, scrolling, scrollbar. The `(paint, height)` blocks become a tagged union (`ND_BLOCK_TEXT / IMAGE / HERO / RULE / GAP`) with a fixed-capacity array, **not** function pointers with captured state | 620 |
| `nd_dialer_call.c/.h` | `show_calling`, `draw_call_screen`, `_draw_handset_icon`, `_fit_text` (binary-search variant) — **core process only** | 260 |
| `nd_dialer_incoming.c/.h` | `show_incoming`, `draw_incoming_screen`, `_fit_caller_text`, the contact lookup — **core process only** | 220 |
| `nd_keymap.c/.h` | the shared `DEV_KEYMAP` (evdev code → char) as a 128-entry lookup table | 60 |
| **total** | | **≈ 5,020** |

Add roughly 600 LOC of unit tests per the coding standards, and a golden-frame harness
shared with the other rendering subsystems.

**Design notes for the implementer**

1. **No `.show()` in the library allocates.** Give every widget an `init` that takes a
   caller-owned struct plus caller-owned string pointers. `nd_vlist_init(&list, ui, "Menu",
   items, n, app_id)` where `items` is `const char *const *`. The Python objects are
   short-lived; C should keep them on the stack of the calling function.
2. **`DetailPage` is the only widget that needs heap.** Its block array is bounded by the
   wrapped body line count. Cap it (e.g. 256 blocks) and truncate beyond that, and put the
   scratch column surface on the `nd_ui` context, allocated once at startup.
3. **Strings.** Everything drawn is `const char *` UTF-8. The two text widgets mutate a
   caller-provided buffer with an explicit capacity (`char *text, size_t cap`) so no
   allocation happens on a keypress. `TextInput` today has no length limit at all —
   `TextInputLong` neither — so pick a cap (the SMS composer implies ≥ 480 bytes) and
   document it in OPEN-QUESTIONS.
4. **Character indexing.** Python indexes `self.text` by *characters*; the pending-word
   splice and the cursor arithmetic are character counts. All characters the keypad and
   `DEV_KEYMAP` can produce are ASCII, and `t9_dict` decodes `ascii` with `errors="ignore"`,
   so byte indexing is safe **for typed text**. Initial text from the phonebook could in
   principle be non-ASCII; treat `_pending_len` and `cursor` as byte offsets and note the
   restriction.

---

## Risks

| # | Risk | Severity | Mitigation |
| --- | --- | --- | --- |
| 1 | **`get_text_size` returns ink extents, not line metrics.** 40-odd call sites centre and stack things using it, so a C `nd_text_size` built on ascender/descender instead of the glyph bounding box shifts nearly every label by a few pixels — and differently per string. | **high** | Implement `nd_text_size` as an exact port of Pillow's `textbbox` → `FreeTypeFont.getbbox` → `font.getsize` path (ink bbox of the laid-out run, exclusive right/bottom). Pin the table in §0 as a unit test before anything else is written. |
| 2 | **Text is antialiased and blended with Pillow's `MULDIV255` rounding.** Grey values 76/128/152 appear in a plain white-on-black label *(measured)*. Any other blend gives near-miss pixels everywhere. | **high** | FreeType `FT_RENDER_MODE_NORMAL`, then `out = MULDIV255(src, a) + MULDIV255(dst, 255-a)` with `MULDIV255(a,b) = ((t = a*b + 128), (t + (t>>8)) >> 8)`. Golden-frame test on `VerticalList` (black text on the white selection bar is the case that exposes the wrong direction). |
| 3 | **Python `round()` is banker's rounding**, and `_draw_pencil` uses it twice. C's `round()` rounds half away from zero, changing the pencil's barrel width at several sizes. | medium | `nd_round_half_even()`; unit-test the 15×15 bitmap in §2 exactly. |
| 4 | **Pillow LANCZOS resampling** for `thumbnail()` on app icons and `DetailPage` images. A different filter or coefficient rounding shifts every icon pixel. | **high** | Port Pillow's `ImagingResample` LANCZOS (support 3.0, the same horizontal-then-vertical two-pass with `precision_bits` fixed-point) rather than writing a "good enough" one. Golden-frame the AppSelector on all shipped icons. |
| 5 | **`DetailPage.draw` allocates a 240×113 RGB scratch buffer (81,360 bytes) every frame**, plus a `copy()` per hero-shrink iteration. On a 53 MB phone this is the worst allocation pattern in the subsystem. | medium | One preallocated scratch surface on `nd_ui`, reused. Hero shrinking scales into a second preallocated surface, or computes the final size arithmetically and scales once. |
| 6 | **`SoftKeyBar.is_transparent` is decided by `hasattr(ui, 'softkey')`** — a construction-order side effect that has no C equivalent. Getting it wrong makes the home screen's softkey opaque black over the wallpaper, or every app's softkey transparent over stale pixels. | medium | Explicit boolean argument. Exactly one bar in the whole system is transparent: the one built at `core/main.py:596`. |
| 7 | **The blinking cursor does not blink.** `TextInput.show()` checks the 0.5 s timer only after `wait_for_key()` returns, and that blocks. A C loop with a `poll()` timeout would start blinking and diverge from the golden frames. | medium | Keep a blocking key wait in `nd_textinput_show`. Note it in OPEN-QUESTIONS as a candidate fix **after** the port is verified identical. |
| 8 | **`MessageDialog` emits a literal `" …"` (U+2026) when it truncates**, which this font renders as nothing plus 8 px of advance *(measured)*. Anyone "fixing" it to `"..."` changes the pixels. | low | Reproduce the codepoint; the C text layer must produce the same `.notdef` advance for a missing glyph. |
| 9 | **Six different text-fitting/wrapping routines** with subtly different behaviour (`fit_text`, `_ellipsize`, `_wrap_lines`, `TextInputLong._wrap_text` = `MessageDialog._wrap_text`, `PagedList._wrap_to_lines`, `call_screen._fit_text`, `incoming_screen._fit_caller_text`). Merging them "because they're the same" silently changes several screens. | medium | Port all of them, separately, with the differences table in §1 as the test matrix. Deduplicate only after golden frames pass. |
| 10 | **Float scrollbar notch positions truncate, not round.** `notch_y = track_top + selected*step` is a float and PIL truncates toward zero *(measured: 8.5 → 8)*. Rounding instead moves the notch one pixel on most list lengths. | medium | Compute in double, truncate with a `(int32_t)` cast, exactly as Pillow does. |
| 11 | **Widgets construct a fresh `SoftKeyBar` inside `draw()`** — `MessageDialog`, `TextScroller`, `InfoScreen`, `ProgressScreen`, `DetailPage`, `LevelSelector`. Six places re-decide transparency per frame and (in three of them) present a second time. | low | Stack-allocate; keep the double `fb.update` in `TextScroller`/`InfoScreen`/`call_screen` because it is observable as a two-stage repaint. |
| 12 | **Partial-screen clears are load-bearing.** Most widgets clear rows 0–145 only, so a caller's earlier `SoftKeyBar.update(..., present=False)` survives. `MessageDialog` and `PagedList` clear the full 0–175 instead. Getting either wrong loses or double-draws the softkey. | medium | The per-widget clear rectangle is in the tables above; assert it in golden frames. |
| 13 | **Unbounded text buffers.** Neither text widget caps input length; the SMS composer can grow `self.text` indefinitely, and `TextInputLong._wrap_text` rewraps the whole string on every keypress (O(n²) over a long message). | medium | Explicit capacity with a documented limit; incremental rewrap is a later optimisation and must not change the wrap result. |
| 14 | **`PagedList._wrap_to_lines` writes `lines[-1]` on a possibly empty list**, and `PagedList.show()` returns index 0 for `ENTER` on an empty list even though `draw()` short-circuits the empty case. Latent, but both are reachable if items change between construction and `show`. | low | Port the behaviour; add a bounds check that cannot alter the observable result, and log it. |
| 15 | **App icons are decoded through `ui.get_image` with a FIFO cache of 32 entries**, and `MessageDialog` asks for its icon with **no** `max_size`, so the full-size art is cached. Today that is only a 24×24 PNG, but a bigger icon would be held at full RGBA size. | low | Keep the FIFO cap; consider a byte budget per the coding standards' cache rule. |
| 16 | **The `ui` object is a duck-typed grab-bag.** Tests construct a `FakeUI` with only `matrix_input` and `font_n`, and the framework's `getattr(..., default)` calls tolerate that. A C struct cannot. | low | The struct in §"The `ui` context object" is the contract; the host test harness fills it with stubs. |
| 17 | **`_header_divider_y` is `max(30, int(H*0.11))`** — on a 175-px band the floor wins and the value is 30. Anyone hard-coding 30 breaks a future panel; anyone recomputing it wrong breaks today's. | low | Port the formula; assert `== 30` in a unit test. |
| 18 | **`AppSelector` takes `ui` as its third argument**, unlike every other widget. Easy to transpose in C where the types are all pointers. | low | Keep the Python argument order in `nd_appsel_init` so the correspondence is obvious, and take `nd_ui *` first if the reviewer prefers — but then say so loudly in the header. |
| 19 | **`call_screen` presents twice per 0.25 s frame** and `incoming_screen` once per 0.5 s blink. Both drive the SPI panel; the framebuffer subsystem's dirty-rect diffing absorbs it today. | low | Keep the call pattern; measure on hardware. |
| 20 | **`math` is imported and never used**; `title` in `AppSelector` is stored and never drawn; `placeholder_icon.png` is shipped and never referenced. | low | Drop the import; keep the unused `title` field only if a caller reads it (none do — drop it and note the change). |

---

## Tests that cover this

| File | Tests | What it pins | Usable as a port oracle? |
| --- | --- | --- | --- |
| `neodct/tests/test_framework_predictive.py` | 25 | `PredictiveText` end to end through `TextInput` and `TextInputLong`: digit→word, `*` cycling and wrap, commit on space/punctuation/mode-change, clear-removes-a-digit, no-dictionary fallback, cursor tracking in the long field, `set_text` dropping a pending guess. Also `t9_indicator_size` for all four modes and the dev-keyboard `None`, and four `fit_text` cases plus `HeaderWidget.width`. | **Yes, directly.** Pure logic, a `FakeUI` with `get_text_size = (8*len(text), 16)`. Reimplement verbatim as C unit tests — the fake metric makes every expected value hand-checkable. |
| `neodct/tests/test_framework_text_input.py` | 19 | T9 multi-tap through both text widgets: append, in-place cycling, mode `#`, backspace resetting the cycle, backspace-on-empty → `"cancel"` / `"empty_backspace"` callback, `input_filter` behaviour on both the keypad and QWERTY paths, first-letter auto-caps. | **Yes, directly.** Same `FakeUI`. |
| `neodct/tests/test_update_ui.py` | 32 | **Real drawn frames** at 240×175 with the real font, through `uistub`. 12 tests on `ProgressScreen` (nothing within 3 px of the bar, fill proportionality within 8 px, empty at 0%, full at 100%, reading below the bar, label above it, `set_step`, softkey untouched, the percent gate suppressing a redraw, long labels inside x∈[4,236], long hints likewise); 18 on `DetailPage` (thumbnail present and first, paragraph gap `0 < gap < line_height`, smooth scrolling by less than 2 lines, clamping at both ends, scrollbar presence/absence in the `x ≥ 232` strip, nothing below `viewport[3]`, header row lit, ENTER/BACK returns, short pages centred within one line-height, scrolling pages starting at the top, hero details beside the picture, notes visible on the first screen, badge above the fold, long title shrinking, no half-line at the fold); 2 on `TextScroller._paginate`. | **Yes — the strongest oracle in the repo.** These assert on real pixels and already encode the geometry invariants. Port them as C golden-frame tests. |
| `neodct/tests/test_uistub.py` | 37 | Two `AppSelector` tests driving it to a choice and to `-1` with scripted keys; one `SoftKeyBar` test inside an installed third-party app; the 240×175 band and its letterbox into 240×240 at **row 32** (`(240-175)//2`); the shipped font loading. | Partly — mostly harness tests, but the AppSelector key-walk and the band offset are worth keeping. |
| `neodct/tests/test_messages_number_field.py` | 3 | `Messages.ContactNumberInput(TextInput)` is `input_filter="numbers"` and types `2,2,*,#` → `"22*#"`. | Yes — pins the one subclass in the app tree. |
| `neodct/tests/test_linuxshell_t9.py` | — | The LinuxShell T9 bridge; touches `t9_engine`, not the widgets. | Indirect. |

**Golden-frame capture.** `neodct/tools/uistub.py` gives frames as PIL images with no
hardware: `CapturingFramebuffer.update()` copies each frame,
`device_frame()` letterboxes it into 240×240, and `KeyScript` replays evdev codes.
`update_ui_fixtures.py` already writes every frame to `$NEODCT_UI_SHOTS` as a PNG. Before
any C is written, capture a reference set covering **every widget in every state** listed
here — empty lists, 1/2/3+ items, selected first/middle/last, all four T9 modes, both
`MessageDialog` looks (≤2 alert lines and the paragraph form), truncated dialogs, all three
`DetailPage` layout cases, `ProgressScreen` at 0/45/100 with and without a detail callback,
both `TextScroller` softkey states. That set is the pass/fail definition of "one-to-one".

**Not covered by any test today** (write C tests for these from this spec):
`SoftKeyBar` transparent mode over a wallpaper; `AppSelector` icon placeholder and empty
state; `VerticalList` window scrolling past three items and the digit shortcuts;
`PagedList` two-line wrap, empty state and the 223-px centring quirk; `InfoScreen`;
`LevelSelector`; `_draw_pencil` at sizes other than 15; both Dialer screens.

---

## How this could be split across agents

The subsystem parallelises cleanly along a single hard dependency: **everything waits on
the text layer.** `nd_text_size` and `nd_draw_text` must be pixel-exact before any widget
can be verified, because every widget's geometry is expressed in terms of measured text.

**Wave 0 — blocking, one agent, no parallelism.**
`nd_text_size` / `nd_draw_text` (rasterizer subsystem) plus `nd_text.c` (the wrapping and
fitting helpers) and `nd_ui_metrics.c`. Gate: the §0 metric table and the six wrapper
behaviours pass as unit tests. Nothing else starts until this is green.

**Wave 1 — three agents in parallel, once Wave 0 is green.**

| Agent | Modules | Why grouped |
| --- | --- | --- |
| **A — chrome and lists** | `nd_softkey`, `nd_header`, `nd_vlist` (+`LevelSelector`), `nd_appsel`, `nd_pagedlist` | All five share the scrollbar/notch idiom and the header row; one agent keeps the notch arithmetic consistent. `nd_softkey` is a dependency of the other four, so it must be inside this group. |
| **B — text entry** | `nd_keymap`, `nd_predictive`, `nd_textinput`, `nd_textlong`, `nd_t9ind` | One coherent story (T9, predictive, the two fields, the indicator) with 44 existing tests that can be ported first, TDD-style. Depends on the T9 subsystem's `nd_t9_engine` interface — agree that header up front. |
| **C — pages and dialogs** | `nd_msgdialog`, `nd_scroller`, `nd_infoscreen`, `nd_progress`, `nd_detailpage` | The screens driven by `test_update_ui.py`; one agent can port those 32 pixel assertions as a set. `nd_detailpage` is the single largest module — if C needs splitting further, give `nd_detailpage` its own agent and leave B with the other four. |

**Wave 2 — one agent, after A and the modem/phonebook subsystems land.**
`nd_dialer_call`, `nd_dialer_incoming`. They need `nd_softkey`, `ui->modem`,
`ui->home_layout` + `render_element`, and the phonebook lookup, so they cannot start with
Wave 1. They are also core-only, so they do not block any app.

**Shared contracts to freeze before Wave 1 starts**, or the three agents will diverge:
1. `nd_ui.h` — the context struct exactly as tabulated above.
2. The drawing-primitive signatures in `nd_draw.h`, including the width-2 line rule.
3. `nd_key` codes: `ND_KEY_CLEAR 14`, `ND_KEY_ENTER 28`, `ND_KEY_KPENTER 96`,
   `ND_KEY_UP 103`, `ND_KEY_DOWN 108`, `ND_KEY_STAR 42`, `ND_KEY_HASH 43`,
   digits `2..11` = `1,2,3,4,5,6,7,8,9,0`.
4. The `nd_lines` line-buffer type returned by every wrapper.
5. The T9 engine's C interface (owned by the T9 subsystem, consumed here).

**Do not split** `nd_vlist` from `LevelSelector`, `nd_textinput` from `nd_predictive`, or
`nd_detailpage` internally — each of those pairs shares state that would have to be
exposed only to be shared back.

---

## Open questions for the project owner

Raise these in `docs/c-rewrite/OPEN-QUESTIONS.md` rather than guessing:

1. **Text-field capacity.** Neither `TextInput` nor `TextInputLong` limits length today.
   What cap should the C buffers use? (SMS implies ≥ 480 bytes for the composer; 64 for a
   name field?) — `framework.py:663` (`TextInput`) and `framework.py:800` (`TextInputLong`).
2. **The non-blinking cursor** (`framework.py:766–789`). Reproduce the current behaviour
   (no blink while idle) or fix it? The spec assumes reproduce.
3. **`MessageDialog`'s invisible `" …"`** (`framework.py:1123`). Reproduce, or switch to
   `"..."` like the rest of the codebase? The spec assumes reproduce.
4. **`AppSelector.title`** is accepted and never drawn (`framework.py:308`). Drop the
   parameter, or keep it for source-level correspondence?
5. **Six wrappers.** Confirm they must all be ported separately before any deduplication.

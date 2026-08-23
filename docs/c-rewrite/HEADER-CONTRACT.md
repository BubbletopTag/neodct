# The header contract, in plain English

*For the project owner. Written assuming you know Python and not C.*

---

## What a header is, and why these came first

In Python, `from System.ui.framework import VerticalList` gets you both the
description of `VerticalList` and its code, because they live in the same file.

C separates them. A **header** (`.h`) says *what exists* — the names, what
arguments they take, what shape the data is. A **source file** (`.c`) says *how
it works*. The header is the part other people read and write code against.

That separation is the only reason ten agents can work at once. The moment the
header for the drawing code exists, the person writing the widgets can write
every widget against it — before a single pixel has actually been drawn. When
the two halves meet, they fit, because both were written against the same
description.

Which is also why these were done first and why they are now frozen. Changing a
header after people have started writing against it breaks their work
retroactively.

**28 headers, all in `neodct/src/include/`.** Everything else in the port is an
implementation of something described in one of them.

---

## The ones you would recognise from the Python

| Header | What it is |
| --- | --- |
| `nd_ui.h` | `NeoDCT_UI` — the object every screen and every app receives. Canvas, fonts, framebuffer, keypad, wallpaper, the app list, the services. The biggest single contract in the project. |
| `nd_widgets.h` | All fourteen parts of `framework.py`: the menus, the text boxes, the warning dialog, the progress bar, the app selector, plus the two Dialer screens. |
| `nd_image.h` | The fifteen Pillow operations the project actually uses. Not a Pillow port — I counted every call in the whole overlay and it comes to fifteen. |
| `nd_draw.h` | `ImageDraw`: rectangle, line, text, point, polygon, ellipse. Seven functions, 255 call sites. |
| `nd_font.h` | FreeType on `font.ttf` at 14/18/20/24, and the text measurement everything centres itself with. |
| `nd_settings.h` | `get_setting` / `set_setting`, and every key the overlay uses, listed. |
| `nd_storage.h` | Is there an SD card, is it one of ours, where are the update files. |
| `nd_db.h` | The four sqlite databases — contacts, inbox, outbox, call log — with their schemas copied byte for byte so an existing phone's data keeps opening. |
| `nd_modem.h` `nd_battery.h` `nd_clock.h` `nd_notify.h` | The four services, one header each. |
| `nd_log.h` | The colourful serial log. Byte for byte. |
| `nd_t9.h` | The T9 engine and the on-disk dictionary. |
| `nd_crash.h` | What you see, and what gets written down, when something breaks. |
| `nd_update.h` | The update system's error list and the exact words the phone puts on screen for each. |

## The ones with no Python equivalent

These exist because C makes explicit what Python did invisibly.

| Header | What it is |
| --- | --- |
| `nd_types.h` | The vocabulary: what a rectangle is, what a colour is, and the one way a function reports failure. Python raises exceptions; C returns an error code, and this is the list. |
| `nd_paths.h` | Every `/NeoDCT/...` path in one place. Plus one switch that lets the tests point the whole system at a scratch directory, so a test can never write to a real phone's files. |
| `nd_keycodes.h` | The keypad numbers — 28 is Enter, 14 is Clear — in one place instead of scattered through twelve files. |
| `nd_fb.h` | The framebuffer. Two kernel calls, a memory mapping, and the two ways of packing a pixel. |
| `nd_input.h` | The keypad, including **the new bit**: apps now get told when a key goes *up* as well as down, so every app can tell a key is being held. That was your ask about NetSurf navigation. |
| `nd_app.h` | What an app *is*: which functions it must provide, and exactly what has to happen when a call comes in while it is running. |
| `nd_proc.h` | Starting an app as its own program and finding out how it ended. (`nd_child.h` is the same file under the name the port plan used.) |
| `nd_json.h` | One JSON reader for manifests, layouts and update files. Python had `json` built in; C does not. |
| `nd_props.h` | `key=value` files. There are **three** slightly different parsers in the Python and they are not interchangeable, so there are three here. |
| `nd_text.h` | The word-wrapping and text-shortening helpers. There are **six** of them in `framework.py`, all slightly different; merging them would quietly change several screens. |
| `nd_layout.h` | `ui_home.json`, turned into something the C can read. |

---

## Three things worth knowing about

**The softkey bar's transparency was an accident.** In `framework.py`, a softkey
bar decides whether to show the wallpaper through itself by asking "does the UI
object have a `softkey` attribute yet?". The core builds its own bar one line
*before* that attribute is created, so the core's bar — and only the core's —
comes out transparent. Every bar an app makes later sees the attribute and is
opaque.

That is a real behaviour that shows on screen, so it is preserved. But it is now
a plain true/false you pass in, rather than something re-derived from the order
the code happens to run in.

**Text is measured by its ink, not its line height.** `get_text_size("_")`
returns 3 pixels tall at the normal size; `get_text_size("Ag")` returns 21. So
text that is "centred" visibly shifts depending on which letters are in it. It
does that on the phone today, roughly forty places rely on it, and the C does
the same thing. It looks like a bug and it is load-bearing.

**Two blend formulas that disagree, on purpose.** Text is composited one way and
the wallpaper is dimmed another, and Pillow really does use different rounding
for the two. I measured both across all 227,328 possible inputs. Anyone who
"tidies" them into one breaks all thirty wallpapered reference screenshots, so
both formulas are written into `nd_image.h` with that warning attached.

---

## The build

`neodct/src/Makefile`. On a developer's machine:

```
make            build everything that exists yet
make test       build and run the unit tests
make ASAN=1     build with the memory-error detector switched on
```

Two things about it are deliberate and worth knowing:

**It globs.** It does not have a list of source files; it builds whatever is in
the directories. With ten people adding modules at once, a hand-written list
would be a merge conflict on every commit and would silently drop the module
whose author forgot to add themselves. A component nobody has started yet prints
`SKIP` rather than failing.

**The warnings are errors.** All eight flags from the coding standards, with
`-Werror`, so a warning stops the build rather than scrolling past. The
acceptance gate (`neodct/tools/verify-c-build.sh`) checks the flags are still
*in* the Makefile, because "it compiles clean" is easy to achieve by deleting
the flag that complained.

---

## What is actually running right now

`nd_log.c` — the colourful logging — is complete, and `neodct/src/core/nd_main.c`
is a deliberately tiny program that prints the boot banner and stops. It exists
so the headers, the library and the build are proved end to end before anyone
writes a widget:

```
$ make && ./build/default/bin/nd-core
========================================================================
[Launcher] Initializing Hardware...
[FB] no framebuffer yet -- walking skeleton
[Launcher] Starting UI...
[CORE] Entering Main Loop...
```

in the right colours — `Launcher` light green, `FB` pale cyan, `CORE` green.

The logging is checked against `neodct/tests/golden/log/logref.json`, which was
generated by running the real `logstyle.py`. Eighty checks: all twenty-two named
tags, all eleven app tags, twelve tags nobody has registered (those get a colour
computed from the tag's own letters, which is the half a rewrite usually gets
wrong), and the fourteen edge cases around deciding what counts as a tag at all.
All eighty pass.

Everything else in the port is now unblocked.

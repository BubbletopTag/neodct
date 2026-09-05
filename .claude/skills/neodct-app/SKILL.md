---
name: neodct-app
description: Build, change or test an app for NeoDCT OS -- the Nokia-5190-style feature phone OS in this repo (C, 240x175 panel, 64 MB RAM, process-per-app). Use this whenever the work touches neodct/src/apps/, a new or existing phone app, the home screen, NotifyService or its banner, the i2c keypad or T9, nd_widgets/nd_ui/nd_draw, the app manifest or app.so, the golden frames, or booting the image under QEMU. Also use it for questions like "how do I add a screen", "why does my app look wrong", "how do I test this without hardware", or any request to add a feature to the phone even when no file is named -- the constraints here (sixteen keys, no left/right, a read-only rootfs, one process per app) break naive designs, so read this before writing the first line.
---

# NeoDCT app development

NeoDCT is a feature-phone OS: Buildroot Linux, a C UI drawn straight to
`/dev/fb0`, on a 240x175 panel with 64 MB of RAM. `AGENTS.md` at the repo root
is the ground truth for building and running it; this skill is what `AGENTS.md`
does not say -- how to write an *app* that looks and behaves like it belongs.

Read `AGENTS.md` and `docs/c-rewrite/CODING-STANDARDS.md` before starting. They
are short and they are not optional: the coding standards are enforced by
`-Werror -Wconversion` and by review.

## The five facts that break naive designs

Get these wrong and the work has to be redone, so they come first.

**1. Sixteen keys, and no left or right.** The keypad is NaviKey, C, Up, Down,
1-9, 0, `*`, `#` -- that is the complete list, and you can confirm it in
`nd_kpsetup_targets[]` in `lib/nd_keypadsetup.c`. There is no left and no
right arrow on the phone. `ND_KEY_LEFT` and `ND_KEY_RIGHT` exist in
`nd_keycodes.h` and are reachable *only* from a development QWERTY keyboard.
Any screen needing a second axis takes it from the number pad, which is what
MusicPlayer and Messages already do. See `references/keypad.md`.

**2. One process per app.** The core `fork`s and `execve`s `nd-apprun`, which
`dlopen`s your `app.so` and calls one entry point. Your app gets no modem, no
battery and no NotifyService handle -- those live in the core and are NULL in
your `nd_ui`. A null dereference kills your app and nothing else. `nd_app.h`
is the contract and it is worth reading in full.

**3. The rootfs is read-only.** `/` and `/NeoDCT/System` are squashfs under
dm-verity. `/NeoDCT/User` is the only writable storage, and an update destroys
everything else. Persistent state goes in `/NeoDCT/User` (usually sqlite under
`/NeoDCT/User/db`), never `/etc`.

**4. Every path goes through `nd_path_resolve()`.** Absolute `/NeoDCT/...`
paths are load-bearing, and the host tests redirect them by setting `ND_ROOT`.
Open files through the helpers in `nd_paths.h` or your code will not be
testable.

**5. Screens are built from `nd_widgets.h`, not from scratch.** Fourteen
widgets cover almost everything. Drawing your own screen is occasionally right
-- but then you inherit every framework rule by hand. See "Making a new screen
look native" below.

## Anatomy of an app

An app is a directory in two places, and both halves are required:

```
neodct/src/apps/<Name>/          the code
  main.c                         app_run() and friends
  <name>_app.h                   constants and the testable surface
neodct/overlay/NeoDCT/System/apps/<Name>/
  manifest.json                  {"name","id","icon","exec"}
  icon.png                       120x120 RGBA, white line art, transparent
```

The Makefile globs `apps/*/` so a new directory just builds. The manifest's
`exec` field still says `"main.py"` in every shipped app and is never read --
leave it, for consistency.

Exported symbols (`nd_app.h`):

| symbol | required | purpose |
| --- | --- | --- |
| `app_run(nd_ui *)` | yes | the app; return 0, non-zero gets the crash screen |
| `app_shutdown(void)` | yes | teardown, even if empty -- see below |
| `app_open_message` / `app_open_inbox` | Messages | banner entry points |
| `app_open_event` | Calendar | banner entry point |

`app_shutdown()` is mandatory even when there is nothing to release, so that
"missing" always means the author forgot. It runs on SIGTERM, which is how an
incoming call reclaims the sound card. **Any loop that outlives a frame must
poll `nd_app_should_exit()`** -- otherwise the core waits, then SIGKILLs you,
and the phone rings silently in the meantime.

Pick an app id that is free; `neodct/overlay/NeoDCT/System/apps/*/manifest.json`
lists what is taken. The id orders the menu and appears in the breadcrumb.

## Making a new screen look native

Most screens should be a widget. When you genuinely need to draw one -- a grid,
a game board -- these are the rules that decide whether it looks like it came
from this OS or from somewhere else. All are in `nd_widgets.h`; the short
version:

- **Clear rows `0..content_bottom` only**, not the full height. That is what
  lets a caller do `nd_softkey_update(&bar, "Options", false)` before your draw
  and get one framebuffer write instead of two.
- **Position text by its INK extents** (`nd_text_size`), never by the font's
  line height. A row of "17" and a row of "8" genuinely do not sit on the same
  pixel here, and that is the house look rather than a bug.
- **Title at y=0 in `font_xl`**, trimmed against the breadcrumb's reserved
  width (`nd_header_width`). When the string can be long, step down a font size
  with `nd_fit_font` instead of ellipsizing -- twelve month names of which four
  do not fit is a case where "September 2..." is simply wrong.
- **Divider** is one white pixel row at `nd_ui_header_divider_y()`.
- **Selection is white fill, black text**, matching every list in the OS.
- **Derive geometry from the panel** (`nd_ui_width`, `nd_ui_content_bottom`)
  rather than hard-coding 240 and 145.
- **Rects are inclusive of both corners** and floats truncate -- `nd_trunc32`,
  never `round`. `nd_draw.h` explains why.

Measure before you commit to a layout. Ink height for a digit at 14 px is 13
rows, not 10; a "small marker under the number" that assumes otherwise will
strike through the glyph. Render it and look at it (see Testing).

## Where state lives

Small preferences go in `nd_settings.h` (`nd_settings_get`/`_set`) with a key
named `<app>.<thing>`. Structured data goes in sqlite under
`/NeoDCT/User/db/`, opened through `nd_db.h`'s helpers: **open, one statement,
close, per query** -- a held connection costs page cache this device cannot
spare. Guard every read with `nd_path_is_file()` so that merely *looking* at an
empty feature does not create its database.

If the core also needs to read your data -- because a notification has to
appear while your app is closed -- the store belongs in `neodct/src/lib/` as a
library module, not inside the app. The app is then just one of its callers.
`lib/nd_calendar.c` is the worked example.

## Notifications

The home screen has exactly one banner idiom: two lines mid-left, the carrier
line hidden behind it, the softkey becoming a verb, C dismissing it. Reuse it
rather than inventing a second one. `references/notifications.md` has the full
recipe for adding a new kind; the shape is:

1. a kind string and a `post_*` function in `nd_notify.h`/`.c`
2. a tick in `lib/nd_ui.c` beside `battery_tick`/`modem_tick` that notices the
   thing and posts it -- **the core polls, because your app is not running**
3. a dispatch in `open_notification()` so pressing the softkey opens your app
   at the right entry point

## Testing

Run the suite before and after every change:

```sh
cd neodct/src && make && make test       # sandboxed -- AGENTS.md's Tests section
make test-one T=test_modem               # one binary, the same way
```

`make ASAN=1 test` before pushing -- `AGENTS.md` asks for it and it catches
real leaks. Both targets run inside `test/harness/sandbox.sh` (bubblewrap: no
D-Bus, no network, a minimal `/dev`) with fake system verbs on `$PATH`,
because the suite once reached `poweroff(8)` and switched off the developer's
machine. A test binary started by hand refuses before `main()`; use
`make test-one`. If a test hangs, `make test-one T=...` it under a timeout;
`test_bluetooth` blocks in containers with no bluetooth stack and that is
pre-existing, not something you broke.

Write two tests for a new app, following the existing pairs:

- **`test/unit/test_<thing>.c`** for library logic, using `platform_test.h`
  (scratch `ND_ROOT` per case, so a test that writes settings cannot touch a
  real `/NeoDCT`).
- **`test/unit/test_<name>_app.c`** for the app, using `smallapp_test.h`, which
  `dlopen`s the **built `app.so`** and `dlsym`s its symbols. Testing the shipped
  artefact rather than a recompile is the point.

Declare anything a test needs in your `<name>_app.h` rather than leaving it
`static`. Pure functions -- a key map, a layout, a formatter -- are worth far
more than UI tests here, because a blocking widget drains the key channel
before its first draw and a scripted keypress never arrives. `sa_hold()` is the
workaround for the one key you can inject.

**Golden frames are a regression net, not a gate.** `CODING-STANDARDS.md`
section 7 is explicit. A frame that changes because you deliberately changed a
screen gets re-cut, not argued with:

```sh
./build/default/bin/nd-shoot --out /tmp/frames
python3 neodct/tools/goldenframe.py --compare neodct/tests/golden /tmp/frames
```

then copy the changed PNGs over and update `manifest.json`'s sha256 entries.
Say so in the commit. Adding an app moves the app selector's scrollbar notch
and the breadcrumb index, so expect the `menu-*` frames to shift, and expect
`test_appreg`/`test_appsel` to need their app counts updated. Do **not** cut a
new frame for a new screen -- its test is its unit test.

## Seeing it actually run

Unit tests do not tell you whether a screen looks right. Two options:

- **Headless render** -- build a small harness that makes an `nd_ui` with real
  fonts and an `nd_capture` framebuffer, call your draw function, save a PNG.
  This is the fast loop and it produces genuine output, not a mockup.
  `test/unit/smallapp_test.h` shows how to construct the context.
- **The real image under QEMU** -- `neodct/tools/cloud-test.sh` brings up an X
  server, a window manager and VNC so a headless agent can boot the phone and
  screenshot it. Read `references/cloud-testing.md` before trying to expose it
  over a network; the egress rules in a cloud session are unintuitive and cost
  hours if you rediscover them.

## Commit conventions

Short, imperative, scope-prefixed: `calendar: a diary, and a second thing the
banner can say`. The body explains *why*, at length, in prose -- this codebase
documents reasoning unusually well and a terse commit is out of place. Comments
follow the same rule: explain why, never what.

Update `neodct/overlay/NeoDCT/CHANGELOG.txt` under `Unreleased` for anything a
phone owner would notice. Do not bump `VERSION_ID`.

## Reference files

- `references/keypad.md` -- the sixteen keys, codes, and d-pad conventions
- `references/widgets.md` -- the fourteen widgets and when each one is right
- `references/notifications.md` -- adding a banner kind, end to end
- `references/testing.md` -- fixtures, golden frames, ASan, rendering a screen
- `references/cloud-testing.md` -- QEMU over VNC, and the egress reality
- `scripts/new-app.sh` -- scaffold an app's eight files in the right places

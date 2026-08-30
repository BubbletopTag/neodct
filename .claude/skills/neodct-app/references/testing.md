# Testing without hardware

## The loop

    cd neodct/src
    make                 # -Werror -Wconversion; it will not let much through
    make test            # ~70 binaries, one per test file
    make ASAN=1 test     # before pushing; AGENTS.md asks for it

If `make test` seems to hang, run the binaries individually with a timeout
rather than assuming you broke something:

    for t in build/default/test/test_*; do
        root=$(mktemp -d)
        timeout 120 env NEODCT_ROOT=$root NEODCT_GOLDEN=$PWD/../tests/golden \
            LD_LIBRARY_PATH=build/default/lib "$t" >/dev/null 2>&1 \
            && echo "ok   $(basename $t)" || echo "FAIL $(basename $t)"
        rm -rf "$root"
    done

`test_bluetooth` blocks indefinitely in a container with no bluetooth stack.
That is pre-existing. Verify against a stashed clean tree before blaming a
change for a hang.

## Two fixtures

**`platform_test.h`** -- for library modules. Gives `CHECK`/`CHECK_INT`/
`CHECK_STR`, and a fresh scratch directory per case pointed at by `ND_ROOT`.
That is why modules under test need no test-only branches: every path goes
through `nd_path_resolve()`, so redirecting the root redirects everything.

**`smallapp_test.h`** -- for apps. Builds a minimal `nd_ui` with real fonts, a
real canvas and an `nd_capture` framebuffer, plus a key channel you can write
scripted presses into. It `dlopen`s the **built `app.so`** and `dlsym`s the
symbols, so the test exercises the artefact that ships rather than a second
copy compiled with different flags.

    void *h = sa_begin("Calendar", "ndcal");
    api.run = sa_sym(h, "app_run");
    ...
    return sa_end(h, "test_calendar_app");

`sa_send()` queues a press and release. `sa_hold()` presses without releasing,
with repeat enabled -- needed for `MessageDialog` and `PagedList`, which drain
the channel before their first draw and would eat a queued press.

If a test needs the font or the warning triangle, point `ND_ROOT` at the
overlay with `sa_overlay_root()`. **The overlay is read-only in tests**: any
case that writes a setting or a database must restore the scratch root first,
or it leaves files in the source tree.

## What is worth testing

Pure functions, heavily. A blocking widget drains the key channel before its
first draw, so a scripted "Down, Enter" never arrives and only a single held
key survives -- which means most UI paths are simply not reachable from a
test. Expose the decision instead of trying to drive the screen:

- a key map as `f(key, cursor) -> enum`
- a layout as `f(year, month) -> cells[]`
- a formatter as `f(value) -> string`

Then the test is exhaustive and fast, and the header says out loud which call
sites are *not* covered. `apps/Clock/clock_app.h` and
`apps/Calendar/calendar_app.h` both do this.

The highest-value test in the calendar work walks only the sixteen real
keycodes and asserts every direction is still reachable -- because every other
test runs on a QWERTY dev keyboard that has arrows the phone does not.

Declare the surface in `<name>_app.h` rather than leaving it `static`, so the
test can reach it.

## Golden frames

**They are a regression net, not a gate.** `CODING-STANDARDS.md` section 7 is
explicit: applications are being deliberately redesigned, and a rule that fails
a build for changing pixels is a rule against doing the work. A frame is not a
reason to leave a screen alone and not something to ask permission about.

What they are still good for: when you change screen A, the frames for B..Z are
a cheap check that you did not disturb them.

Re-cut a frame you changed on purpose:

    ./build/default/bin/nd-shoot --out /tmp/frames
    python3 neodct/tools/goldenframe.py --compare neodct/tests/golden /tmp/frames

then copy the changed PNGs into `neodct/tests/golden/` and update the matching
`sha256` in its `manifest.json`. Say so in the commit message.

**Adding an app moves frames you did not touch.** The app selector's scrollbar
notch is `(track_bottom - track_top) / (n_apps - 1)`, so every `menu-*` frame
shifts, and apps sorting after yours have their breadcrumb index bumped.
`test_appreg` and `test_appsel` hard-code the app counts and the notch position
and will need updating. This is expected -- do not treat it as breakage.

Do **not** cut a new frame for a new screen. Its test is its unit test, not a
picture of itself that can only ever agree with it.

## Seeing a screen

Unit tests do not tell you whether a layout looks right, and this is a UI
project. The fast loop is a throwaway harness: construct an `nd_ui` the way
`smallapp_test.h` does, call your draw function, `nd_capture_save()` a PNG,
and look at it. Compile it against `build/default/lib`:

    cc -std=c11 -D_GNU_SOURCE -Iinclude $(pkg-config --cflags freetype2 libpng libjpeg sqlite3) \
       -o /tmp/shoot /tmp/shoot.c -Lbuild/default/lib -lneodct -ldl -lm \
       $(pkg-config --libs freetype2 libpng libjpeg sqlite3)

Do this *before* declaring a screen finished. Measuring beats assuming: a digit
at 14 px has 13 rows of ink, not 10, and a marker placed on the assumption of
10 draws a line through the glyph. That bug survived a clean build, a clean
test run, and a confident description of the design -- and died the moment
someone looked at a 4x zoom of the grid.

# Testing without hardware

## The loop

    cd neodct/src
    make                 # -Werror -Wconversion; it will not let much through
    make test            # ~70 binaries, one per test file, in the sandbox
    make ASAN=1 test     # before pushing; AGENTS.md asks for it

Both test targets run inside `test/harness/sandbox.sh` -- bubblewrap with no
D-Bus, no network and a minimal `/dev` -- with fake `poweroff`, `reboot` and
`systemctl` first on `$PATH`, because the suite once reached `poweroff(8)`
and switched off the machine running it. A test binary started by hand
refuses before `main()`, so there is no loop over `build/default/test/test_*`
to write any more; the one-binary form is:

    make test-one T=test_modem

If `make test` seems to hang, run the binaries one at a time that way, under
a timeout, rather than assuming you broke something:

    for t in build/default/test/test_*; do
        timeout 120 make test-one T=$(basename $t) >/dev/null 2>&1 \
            && echo "ok   $(basename $t)" || echo "FAIL $(basename $t)"
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


## Fixtures and the privilege drop

Apps no longer run as root. `nd_proc_launch_app()` drops an ordinary app to
`ndusr` (see `nd_proc_app_needs_root()`), and that quietly changed what a test
fixture has to look like. It has now cost three separate debugging sessions,
so it is written down.

**A fixture stands in for `/NeoDCT`, so it has to be shaped like `/NeoDCT`.**
Two properties matter and neither is obvious:

1. **The staged root must be traversable — 0711, not `mkdtemp`'s 0700.** The
   app the test launches lives *under* the fixture, so a dropped child cannot
   resolve the path to `nd-apprun` and dies before running anything. On the
   phone this never happens because `/` and `/NeoDCT/System` are 0755.
   `platform_test.h`'s `pt_new_case()` does this for you; a test that stages
   its own root with `mkdtemp` must do it itself.

2. **`/NeoDCT/User` must be owned by `ndusr`.** `S00userdata` takes ownership
   on the first boot, so on a phone an app can write its own partition. A
   fixture that creates the directory root-owned gives the app a partition it
   cannot write. `test_svc.c` and `test_t9_app.c` have a `stage_user_owner()`
   helper; copy it.

**How it fails is the reason this is worth reading.** None of these produce an
error mentioning permissions:

- `test_proc` gave thirteen assertion failures about crash reports and exit
  codes.
- `test_svc` and `test_t9_app` **hung**, because the parent waited for a reply
  from a child that had already exited 1.
- `test_browser` reported "console does not contain [Browser] ..." fourteen
  times — the browser had produced no output at all.

**And it only bites on some machines.** A build host normally has no `ndusr`,
so `run_as.valid` is false, the drop is a documented no-op, and everything
passes. Create the users to run `nd-selftest` and the same commit starts
failing. If a test suddenly breaks or hangs after you touched nothing relevant,
check `getent passwd ndusr` before looking anywhere else.

The flip side is worth knowing too: **on a machine that does have the users and
is running as root, those tests exercise the real drop** — a real `fork`, a
real `nd_priv_become()`, a real `execve`. That is the only way to cover the
privilege path from a host at all, and `test_browser`'s
`t_run_with_the_untrusted_user_really_drops` exists for exactly that.

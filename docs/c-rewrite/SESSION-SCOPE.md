# What we are building right now

Scope set by the project owner on 2026-08-23. Every agent working on the C port
reads this to know what is in and what is out. **In scope means fully working,
not stubbed.**

---

## In scope — must actually work

**The whole of `System/core`.** The main loop, the framebuffer, `NeoDCT_UI`, the
home screen, the app selector, the settings and storage layers, the crash screens,
and **the Dialer** (the owner called this out specifically).

**The whole UI framework.** All 14 widgets, pixel-identical to the Python.

**The services.** Modem, notify, clock, battery — running for real, not simulated
away. The modem gets its own thread, so a call interrupts any app, including Koki.

**The keypad, improved.** The core owns it and sends press *and* release events to
apps over an inherited pipe, so every app gets held-key state and key repeat. This
was the owner's explicit ask: NetSurf navigation is annoying today because nothing
can tell a key is being held.

**CubeBench, ported for real.** Not stubbed. The owner wants it as a visible
demonstration of the speed difference — a wireframe cube spinning at the Python's
frame rate versus C's.

**The browser launcher, ported for real.** `netsurf-neodct` is already a standalone
program, so the app is a launcher: build the command line, hand over the screen,
run it, take the screen back. The browser engine itself is untouched.

**The colourful serial logging, byte for byte.** The owner asked for this
specifically. It is not just a palette — app tags and unknown tags get colours
derived arithmetically from the tag's characters. `neodct/tests/golden/log/logref.json`
has the exact bytes for every case, including 14 `_split_tag` edge cases.

**musl instead of glibc**, as far as it can be taken without a full image build.
See `MUSL.md` — `neodct_displayd` must be built from source first, because the
committed binary is glibc-linked and will not exec at all under musl.

---

## Stubbed for now — but present

**All 25 apps stay in place**, with their real `manifest.json` and their real
`icon.png`, so the menu looks complete and every icon shows. Their `app.so` just
opens a dialog:

> This application has not been implemented yet.

**There is already a pixel-exact target for this.** `golden/widget-messagedialog.png`
renders that precise string through the real Python `MessageDialog` — warning
triangle, two lines of text, OK button. The stub app must match it.

The 25, with their real IDs, all of which keep their icons:

| Stock | | Engineering | |
| --- | --- | --- | --- |
| Phonebook | 1 | LinuxShell | 999 |
| Messages | 2 | LCDTest | 9001 |
| CallLog | 3 | KeyMap | 9002 |
| Settings | 4 | KeyMapI2C | 9003 |
| Games | 6 | FuelGauge | 9004 |
| Calculator | 7 | ModemInfo | 9005 |
| Clock | 8 | Downgrade | 9006 |
| Tones | 9 | RemoteShell | 9990 |
| KokiMobile | 10 | Crash | 9997 |
| Browser | 11 | **CubeBench** | **9998** |
| Update | 12 | Tests | 9999 |
| Music | 970 | | |
| Power | 971 | | |

Two exceptions to the stubbing: **CubeBench (9998)** and **Browser (11)** are real.

---

## Out of scope this session

- **Koki.** 11,500 lines, its own sub-project. Stubbed like the rest.
- **NetSurf itself.** Only its launcher is ported.
- **The update system and its crypto.** Security-critical; it gets its own pass
  with the RSA verifier reviewed properly, not rushed alongside everything else.
- **Removing Python from the defconfigs.** The image carries both until every app
  is real. Note `BR2_PACKAGE_SQLITE` is only reachable through
  `BR2_PACKAGE_PYTHON3_SQLITE` — dropping Python without adding sqlite explicitly
  ships a phone with no phonebook.
- **T9 predictive text** beyond what the framework needs to compile and render its
  mode indicator.

---

## How we know it worked

The owner pulls the branch and runs it. Before that, screenshots from the C build
compared against `neodct/tests/golden/`.

The bar for "done" this session:

1. `make` builds clean, `-Werror`, and clean under `musl-gcc` too
2. `make test` passes, including under AddressSanitizer
3. `nd-shoot` renders the home screen, the app selector and the stub dialog
4. Those frames match the golden reference pixel for pixel
   — *superseded 2026-08-26: the port is done and screens are now being
   deliberately redesigned, so this is no longer a bar for anything. See
   CODING-STANDARDS.md section 7.*
5. Idle RSS measured and reported against the ~15–17 MB Python baseline

Frames that are allowed not to match exactly are listed in `OPEN-QUESTIONS.md`
under the frame tolerance policy — currently CubeBench only, and only by a few
pixels along the wireframe, from `sin`/`cos` differing by one unit in the last
place between libcs.

# Open questions for the C port

Anything an agent cannot determine from the code goes here rather than being guessed.
Add an entry with the Python file and line you were reading, then carry on with the rest
of your work.

**The four Wave-0 blocking questions were answered by the project owner on 2026-08-23.**
They are settled — implement them as written, do not re-litigate them.

---

## ANSWERED

### 1. Should an incoming call interrupt an app that never calls `read_keypress()`?

**Found in:** `System/core/main.py:1073`; `uistub.py` notes Koki reads the evdev fd directly.

**What the Python does:** `poll_modem()` raises `IncomingCall` from inside
`read_keypress()`. Koki never calls it, so a call arriving during Koki is silently missed.

> **ANSWER: Fix it.** The modem gets its own thread in the core process, and calls
> always interrupt whatever is running.

**Implementation:** modem thread in `nd-core`. On `RING`, the core signals the app child
(`SIGTERM`), the child runs its cleanup and exits, the core reclaims the screen and shows
the call UI. This mirrors how `IncomingCall` unwinds today, but does not depend on the app
cooperating.

**This is a deliberate deviation from 1:1.** No golden frame covers it, so nothing in the
oracle changes. Note it in the changelog when the port ships.

---

### 2. How does an app in a separate process learn which keys are *held*?

**Found in:** Koki's input handling; `System/core/main.py` `MatrixKeypadInput`.

**What the Python does:** Koki reaches into the core keypad driver's private state to read
currently-held keys. Impossible once apps are separate processes.

> **ANSWER: The core sends press *and* release records to the app.**
>
> The owner's reasoning goes further than the original recommendation and should be
> honoured: *"the keypad handling should be in core, but it should be more robust in
> general — apps like NetSurf can't tell when you're holding a key down, which makes
> navigation annoying."*

**Implementation:** the core owns the keypad. It synthesises evdev-format press **and**
release records onto a pipe the child inherits as `keypad_fd`. Apps read ordinary evdev
and derive held state themselves.

Three consequences, all good:

- Koki's entire matrix-scanner branch disappears from app code — it reads keys like every
  other app.
- **Key repeat and held-key state become available to every app, not just Koki.** This is
  the owner's actual goal: it fixes NetSurf navigation, and scrolling in `VerticalList` and
  `PagedList` can hold-to-repeat. Treat "every app can see held keys" as a requirement of
  this work package, not a side effect of it.
- Apps need no keypad device permission at all, which is what makes the sandboxing in
  `SECURITY.md` possible later.

**Caveat to carry forward:** the i2c matrix keypad reports one key at a time (no chords) —
see `docs/KOKI_PORT_NOTES.md`. The pipe protocol must not assume multi-key is available on
hardware even though QEMU keyboards provide it.

---

### 3. When does a wallpaper change made in Settings become visible?

**Found in:** `System/apps/Settings/main.py` — assigns `ui.wallpaper`, flips
`ui.engineering_mode`, and rewrites `ui.apps` in the core's live memory.

> **ANSWER: When you leave Settings.**

**Implementation:** Settings writes only the setting. After every app exit the core
re-reads `system.ui.wallpaper` and `system.ui.engineering_mode` and rescans the app
directories — exactly the pattern `launch_app()` already uses for the unread-SMS count
(`self._unread_sms = self._count_unread_sms()`).

**Verify before implementing:** if the Python currently repaints the wallpaper *while
still inside* Settings, this is a visible change and the affected golden frames must be
re-captured. Check `app-settings-wallpaper.png` against the running app and say so in the
work package rather than quietly re-cutting the reference.

---

### 4. Snake and Memory seed `random` from the clock

**Found in:** `System/apps/Games/snake.py:42`, `memory.py:113`.

> **ANSWER: Use a normal C generator and re-capture those two reference frames.**
> The owner's framing: *"you may not get a perfect 100% match on apps like that, so take
> it with a grain of salt if there isn't a perfect match."*

**Implementation:** one small pinned PRNG in `libneodct.so`, seeded from the deterministic
clock in capture mode and from real time otherwise. Do **not** reimplement CPython's
MT19937. Re-capture `game-snake.png` and `game-memory.png` from the C build once the games
are ported, and mark them `tolerance: recut` in the manifest (see below).

---

## Frame tolerance policy

The owner also flagged CubeBench as unlikely to match. **They are right, but for a
different reason than stated, and it needs recording.**

CubeBench is no longer time-nondeterministic — `goldenframe.py` patches `perf_counter`,
so the cube's rotation angle is now fixed for a given frame number. The remaining risk is
**`libm`**: the cube's vertices go through `sin()` and `cos()`, and glibc, uClibc-ng and
musl do not agree to the last bit on transcendental functions. A one-ULP difference in a
rotation matrix can move a vertex by a pixel.

So frames fall into three classes, and the manifest should say which:

| Class | Rule | Frames |
| --- | --- | --- |
| `exact` | zero differing pixels; a regression fails the build | the other 46 |
| `recut` | reference re-captured from C; exact thereafter | `game-snake`, `game-memory` |
| `tolerance` | small bounded pixel delta permitted, with a stated cap | `eng-cubebench` |

**A tolerance is a budget, not an excuse.** Set the cap from a measured libm delta, not
from whatever the port happens to produce. If CubeBench comes out 3 % different, that is a
bug in the port, not floating-point noise — the honest cap here is single-digit pixels
along the wireframe edges. Any frame wanting `tolerance` status needs a written
justification in its work package, approved before the reference is relaxed.

---

## DECISIONS on C-1 … C-6 — all answered, none blocking

Answered 2026-08-23. Implement as written; the questions are kept below for the
reasoning behind each.

**C-1 — `fontref.json` wins. The spec tables are stale; treat them as wrong.**
Measured directly on the project's own `font.ttf` at size 20:

| engine | `getlength("Menu")` | pen positions |
| --- | --- | --- |
| **BASIC** (what the phone has) | **68.0** | `[0, 20, 36, 52]` |
| RAQM (what a desktop Pillow defaults to) | 65.0 | `[0, 20, 35, 50]` |

The spec's 65 is the RAQM figure. Both spec tables were measured on a host with
libraqm installed — the same contamination that made 46 of the 49 golden frames
wrong, caught and fixed in commit `9875da82`. Buildroot builds Pillow
`-Craqm=disable` unconditionally, so BASIC is the only engine the phone ever
uses, and `fontref.py` forced BASIC when it captured the reference.

So: **whole-pixel advances, integer accumulation, no kerning, no fractional
carry.** `nd_font.h` is right as it stands. `spec-core-loop.md` §7 and
`spec-ui-framework.md` §0 are hereby marked stale — do not implement from them,
and do not "correct" `fontref.json` to agree with them. WP-03 is unblocked.

**C-2 — approved: `ND_TEXTINPUT_CAP 256`, `ND_TEXTLONG_CAP 1024`.**
One condition: at the cap the widget must **ignore further input**, not truncate.
Silently dropping the tail of a message somebody typed is worse than refusing the
keypress, and the Python's unbounded field never had to choose. Also fix the O(n²)
rewrap while you are there — rewrap from the edited line, not the whole string.

**C-3 — approved: `ND_T9_DIGITS_MAX 32`.** The longest dictionary key is 12, so
behaviour is identical for anything that could ever match. Recorded as a deviation.

**C-4 — keep the 32-entry FIFO exactly as the Python has it.** Eviction order is
observable as decode stalls and there is no reason to change it: 32 icons at
82×82 RGBA is 860 KB against a 53 MB budget. Add one thing that is *not* an
eviction-policy change — a hard per-image ceiling that refuses to cache anything
absurdly large in the first place. That closes the "one big PNG takes a slot at
full size" hole without touching which images survive.

**C-5 — approved, and thank you for finding it.** Write `settings.prop` only when
the content actually changed. This is a real bug: `load_with_defaults` strips the
three `system.os.*` defaults before writing, so the "missing keys" branch is
permanently true and every single `get_setting()` triggers a full atomic rewrite —
temp file, `fsync`, `rename`. `NotifyService`, `BatteryService` and `ModemService`
all call it from hot paths, per ring and per call setup. On 128 MB of UBIFS/NAND
that is genuine flash wear, on hardware the owner cannot easily replace. Invisible
to every test and every golden frame; the only observable difference is the mtime.

**C-6 — keep the headers flat.** `#include "nd_types.h"` with `-Iinclude`. It is
done, it works, and churning every include in the tree buys nothing. The
`nd_child.h` forwarding header onto `nd_proc.h` is a good call — keep it.

---

## Serial logging: recognisable, not byte-identical

Superseding the stricter reading in `SESSION-SCOPE.md`. From the project owner:

> *"serial logging doesn't have to be byte for byte since some stuff could
> genuinely differ, since we have a whole new underlying engine for running apps."*

That is right, and it matters because the C system genuinely has things to say
that the Python did not — `nd-apprun` launching a child, `waitpid` reporting a
signal, held-key repeat, the modem thread.

**What must stay the same:** the colour palette and the tag scheme, so a boot log
still reads the way it does today. That means `[TAG] message` with a bold
256-colour tag, the named palette from `logstyle.py` (MODEM 39, CORE 46, CRASH
196, …), and the *same derived-colour arithmetic* for tags not in the palette:
`141 + (sum % 36)` for app tags, `22 + (sum % 180)` for everything else. Keeping
the arithmetic is what makes a tag invented next year get a sensible stable colour
instead of a clashing one.

**What may differ:** the messages themselves. New subsystems get new tags and new
lines. Wording may change where the C genuinely does something different.

`neodct/tests/golden/log/logref.json` therefore tests the **mechanism** — palette,
derived colours, escape-sequence format, `_split_tag` edge cases — not the message
text. Do not contort a C message to match a Python string.

---

## OPEN

### C-1. The two font metric tables in the specs disagree with `fontref.json`

**Found in:** `spec-core-loop.md` §7 (advance table, "advances are exactly n × size / 8",
`"Menu"` @20 → 65) and `spec-ui-framework.md` §0 (the 4-size × 8-string measured table)
versus `neodct/tests/golden/font/fontref.json` (`"Menu"` @20 → width 68, pen positions
`[0, 20, 36, 52]`, **every advance an integer**).

**Why they differ:** both specs were measured on a host with libraqm installed. The
phone's Pillow is built `-Craqm=disable` and always uses `Layout.BASIC`, which is what
`fontref.py` forced when it captured the reference — the same correction WP-01 applies to
the 49 golden frames.

**What the headers assume:** `fontref.json` wins. `nd_font.h` declares
`nd_font_advance()` as returning **whole pixels**, and there is no fractional
accumulation to get right.

**What we need:** confirmation, plus a decision on whether the two spec tables get
corrected in place or marked stale. Right now an agent who implements `nd_text_size()`
from `spec-ui-framework.md` §0 and then tests it against `fontref.json` will find every
string 2–4 px out and will not know which to believe. This blocks WP-03.

---

### C-2. Text-field capacity

**Found in:** `framework.py:663` (`TextInput`) and `framework.py:800` (`TextInputLong`) —
neither caps its length, and `TextInputLong._wrap_text` rewraps the whole string on every
keypress, so a long message is O(n²) as well as unbounded.

**What the headers assume:** `ND_TEXTINPUT_CAP 256` for a name, number or host name;
`ND_TEXTLONG_CAP 1024` for the SMS composer (the spec implies at least 480). Both are in
`nd_widgets.h` and both are one-line changes.

**What we need:** confirmation, or different numbers. A 160-character SMS is 160 bytes,
so 1024 allows a comfortably long concatenated message.

---

### C-3. The T9 digit accumulator is capped at 32

**Found in:** `t9_engine.py` — `_word_digits` grows without limit, though the longest
dictionary key is 12 and `suggest()` returns nothing past that.

**What the headers assume:** `ND_T9_DIGITS_MAX 32`; the engine stops appending beyond it
rather than truncating or wrapping. Behaviour is identical for anything the dictionary
could match; it differs only for a user holding a key down past 32 presses, where the
Python keeps a digit string that can never match anything and the C keeps 32 of them.

**What we need:** a nod. This is recorded as a deviation rather than left to be
discovered.

---

### C-4. The image cache is entry-counted, and `CODING-STANDARDS.md` §4 wants bytes

**Found in:** `core/main.py:714` `get_image` / `_cache_put` — a 32-entry FIFO with no byte
ceiling. 32 app icons at 82×82 RGBA is 860 KB, and nothing stops one large PNG taking a
slot at full size.

**What the headers assume:** the 32-entry FIFO, unchanged, so eviction order matches the
Python exactly (`ND_IMGCACHE_MAX` in `nd_image.h`).

**What we need:** whether to add a byte ceiling that only ever evicts *earlier* than the
Python would. That is safe for correctness but changes which images survive, which is
observable as a decode stall rather than as a pixel difference.

---

### C-5. `settings.prop` is rewritten on every read (R-24), and it is measured

**Found in:** `SettingsStorage/__init__.py` `load_with_defaults` — the three `system.os.*`
defaults are stripped before writing, so they are never in the stored file, so the
"missing keys" branch is permanently true. Five consecutive `get_setting()` calls produced
five full atomic rewrites (temp file, `fsync`, `rename`).

`NotifyService`, `BatteryService` and `ModemService` all call `get_setting()` from hot
paths — per ring, per call setup. On UBIFS/NAND that is real flash wear.

**What the headers assume:** reproduce it exactly, with the write isolated behind
`nd_settings_flush_if_needed()` in `nd_settings.h` so switching to "write only when the
content actually changed" is a one-line change.

**What we need:** permission to make that one-line change. It is invisible to every test
and every golden frame; the only observable difference is the mtime of `settings.prop`.

---

### C-6. Public headers are flat, not under `include/nd/`

**Found in:** `PORT-PLAN.md` §2.4 spells them `include/nd/nd_types.h`; the header work
package was briefed as `neodct/src/include/nd_*.h`.

**What was done:** flat, `#include "nd_types.h"` with `-Iinclude`. Both spellings cannot
coexist without duplicate files, and the brief was the more recent instruction.

`nd_child.h` (the name `PORT-PLAN.md` §2.4 uses for the fork/exec helper) exists as a
one-line forwarding header onto `nd_proc.h`, so neither spelling of that one is wrong.

**What we need:** nothing, unless the owner prefers the `nd/` subdirectory — in which case
say so now, while it is one `git mv` and one `-I` flag rather than a rewrite of every
`#include` in the tree.


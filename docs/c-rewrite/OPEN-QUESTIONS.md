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


---

### R-1. `nd_image_alpha_bbox`'s rule disagrees with what Pillow's `getbbox()` actually does

**Found in:** `nd_image.h` — *"A pixel counts as empty only when ALL FOUR channels are
zero — that is Pillow's rule, and it is why the battery sprite's `?` label sits where it
does."* Measured against Pillow 12.3.0:

```python
im = Image.new('RGBA', (8, 8), (0, 0, 0, 0)); im.putpixel((6, 2), (255, 255, 255, 0))
im.getbbox()                   # None      <- the default, alpha_only=True
im.getbbox(alpha_only=False)   # (6,2,7,3) <- the rule the header states
```

Pillow has defaulted `getbbox()` to `alpha_only=True` on modes with an alpha channel since
9.2. The all-four-channels rule is reachable only by passing the keyword, and no Python in
the overlay does.

**Why it matters, with numbers.** On the ten real status sprites the two rules do not
differ slightly, they differ completely, because the transparent area of those PNGs is
transparent *white*:

| sprite | `getbbox()` (alpha only) | all-four-channels |
| --- | --- | --- |
| `battery/bat-0.png` | (7, 128, 29, 172) | (0, 0, 36, 180) |
| `battery/bat-4.png` | (5, 6, 29, 172) | (0, 0, 36, 180) |
| the other three battery frames | a real ink box | (0, 0, 36, 180) |
| all five `cellsignal/sig-*.png` | identical under both | identical under both |

So the header's own justification does not hold: under the rule it states, every battery
frame's bbox is the whole 36×180 canvas and cannot be positioning anything. WP-04's
"ten measured status-sprite bboxes" acceptance criterion presumably recorded the
`alpha_only` numbers, since those are what Pillow returns unprompted.

**What was done:** the header wins, because it is frozen and other modules compile against
it — `nd_image_alpha_bbox()` implements the all-four-channels rule as written, and says so
in a comment. Nothing in the shipped Python calls `getbbox()` on an RGBA image
(`engine.py:748` calls it on an L-mode `ImageChops.multiply` result, where the two rules
coincide and `nd_image_masks_overlap()` handles it), so no golden frame moves either way
today.

**What we need:** a decision before WP-04 measures the sprites. Switching to Pillow's
default is one line in `nd_image.c` — `empty = p[3] == 0u` for RGBA — plus one line of the
header comment.

---

### R-2. `nd_image_crop` and `nd_image_crop_zeropad` needed a distinction the header implies but does not spell out

**Found in:** `nd_image.h` describes `nd_image_crop()` as "PIL Image.crop(box)" and
`nd_image_crop_zeropad()` as "PIL's crop() semantics for a box that runs off the edge".
PIL has only one crop, and it always zero-pads, so as written the two are the same
function.

**What was done:** `nd_image_crop()` clips the box to the source and returns the pixels
that exist (NULL if the box misses entirely); `nd_image_crop_zeropad()` always returns the
requested size with the overhang zeroed, which is PIL's behaviour. Every crop in the
shipped code is fully in bounds, so the two agree everywhere it currently matters, and
`crop_zeropad` is the one to port a `PIL.crop()` call onto.

**What we need:** nothing, unless the intent was for both to zero-pad, in which case say so
and `nd_image_crop()` becomes a one-line forward.

---

### R-3. Pillow resizes RGBA through PREMULTIPLIED alpha, and no spec mentions it

**Found in:** `PIL/Image.py`, `Image.resize()`:

```python
if self.mode in ["LA", "RGBA"] and resample != Resampling.NEAREST:
    im = self.convert({"LA": "La", "RGBA": "RGBa"}[self.mode])
    im = im.resize(size, resample, box)
    return im.convert(self.mode)
```

So every LANCZOS resize of an RGBA image — every app icon through `get_image(max_size=…)`,
every `DetailPage` hero picture — is `RGBA → RGBa → resample → RGBA`, and **both**
conversions are lossy in their own right (`MULDIV255` on the way in, an integer
`255*v/alpha` with a clip on the way out). Resampling the stored channels directly is
visibly different, not marginally so: it leaves a halo of the fully transparent pixels'
colour along every icon edge.

`spec-ui-framework.md` and `spec-koki.md` both describe the resize as plain LANCZOS.
`nd_image.h` describes `nd_image_resize_lanczos()` as "a port of Pillow's resample.c",
which is true of the resampler but omits the wrapper.

**What was done:** reproduced exactly, including the `w == src->w && h == src->h`
short-circuit that Pillow uses to skip the round trip entirely. Verified: the 25 shipped
`icon.png` files through the full `get_image` path, and all seven wallpapers plus
`CRASH.jpg` through the full `_load_wallpaper` path, are byte-identical to Pillow.

**What we need:** nothing. Recorded because it is invisible in the specs and an optimiser
who "removes the pointless conversion" will break every icon.

---

## Platform services (WP-06/07/08: props, settings, storage, db, json)

Recorded while porting `SettingsStorage`, `Storage`, `init_databases()` and writing
`nd_json`. Nothing here blocks; each is a place where the C could not be literally 1:1
and the choice should be seen rather than discovered.

### P-1. `nd_setting_update_truthy()`'s frozen signature does not match the Python

**Found in:** `apps/Update/main.py:85` —
`str(get_setting(...)).strip().upper() in ("ON","1","TRUE","YES","ENABLED")`, a
**membership test over five literals**. `nd_settings.h` declares
`bool nd_setting_update_truthy(const char *value, const char *literal_upper)`, which
compares against **one** literal.

**What was done:** implemented exactly as declared, because the header is frozen. The
Update agent therefore has to call it once per literal, or the header gains one more
declaration. Flagging rather than widening the contract unilaterally.

### P-2. Bounds that the Python does not have

The Python has no limits anywhere in this subsystem; the C needs them because two of
these files arrive on an SD card. All are far above anything the project ships, and all
are one `#define`:

| Limit | Value | Reached by |
| --- | --- | --- |
| `ND_PROPS_MAX_ENTRIES` (`nd_props.c`) | 256 | a `settings.prop` with 256+ keys; the merged map has ~25 |
| `ND_PROPS_MAX_BYTES` (header) | 256 KB | a prop file at or over the cap reads as EMPTY, not truncated |
| `ND_SD_MAX_LISTING` (`nd_storage.c`) | 4096 | `readdir` on the card's `update/` folder |
| `ND_JSON_MAX_MEMBERS` (`nd_json.c`) | 4096 | members in one JSON object |
| `ND_JSON_MAX_VALUES` (`nd_json.c`) | 65536 | values in one document; without it a 1 MB `[1,1,1,...]` becomes ~20 MB of parse-time vectors |

A prop file at the byte cap yields an empty map, which for `settings.prop` means the
next `save_settings()` would rewrite it with defaults. That cannot happen with a file
this project produces, but it is the one place a cap is not merely conservative.

### P-3. `str.splitlines()` is only partly reproduced

**Found in:** `SettingsStorage._parse_settings`, `Storage._read_state`.

Python splits on `\n`, `\r`, `\r\n`, and also on `\v`, `\f`, `\x1c`–`\x1e`, `\x85`,
`U+2028` and `U+2029`. The C splits on the first three only, per the spec's
recommendation. `strip()` does handle `\v`, `\f` and `\x1c`–`\x1f`, so the difference is
confined to a prop file that uses one of those as a **line terminator**. No file in the
project does.

### P-4. JSON: four deviations from Python's `json`

1. **`NaN`, `Infinity` and `-Infinity` are rejected.** Python's `json.loads` accepts all
   three by default. Accepting them would let a manifest field be a quiet NaN, which
   every comparison in the update checker would then answer "false" to.
2. **An integer outside `int64_t` is rejected** (`ND_ERR_PARSE`) rather than silently
   becoming a double. Python has arbitrary-precision ints; the alternative was a
   saturated value that lies.
3. **A lone surrogate decodes to U+FFFD.** `json.loads('"\\ud800"')` succeeds in Python
   and produces a `str` that cannot be encoded to UTF-8; there is no "later" here.
4. **The emitter matches `json.dumps` defaults** (`ensure_ascii=True`, so non-ASCII is
   `\uXXXX` and astral characters are surrogate pairs), but `indent=0` means *compact
   with no spaces*, per `nd_json.h`. Python's `indent=None` default is `(', ', ': ')`
   **with** spaces. The header's spelling was followed.

### P-5. A failed atomic write leaves `<path>.tmp` behind

Reproduced from `SettingsStorage.save_settings` / `RemoteShell._write_props`, neither of
which unlinks the temp file when the write fails. Invisible to every test; the next
successful write truncates it. Noting it because deleting it looks like an obvious
tidy-up and would be a (harmless) deviation.

### P-6. The `[Settings] Failed to read` line fires on fewer paths than in Python

`load_settings()` prints that line for **any** exception, including a `UnicodeDecodeError`
on a corrupt `settings.prop`. The C logs it when the file exists but is not readable, and
stays silent on a decode failure, because the dialect functions return an empty map
without a reason code. Same behaviour, one fewer log line. Covered by the relaxed serial
logging decision above.

### P-7. Test-hook paths live in the ND_ROOT namespace, not the host's

`nd_settings_set_paths()` and `nd_storage_set_paths()` take paths that still go through
`nd_path_resolve()`. A host test sets them to `"/User/settings.prop"` and lets
`NEODCT_ROOT` do the redirection, which is what the ported pytests do. Two pytest cases
relied on a *real* unwritable path (`/proc/definitely/not/writable/...`,
`/proc/nope/sdcard`); under `ND_ROOT` those are ordinary creatable directories, so the C
ports put a regular **file** where the directory has to be. That is unwritable for every
user including root, which is the pytest's intent rather than its spelling.

### P-8. Acceptance gate check 3 cannot see `sqlite3.h` (and `jpeglib.h`) under musl

`verify-c-build.sh` passes `pkg-config --cflags freetype2 libpng sqlite3` to `musl-gcc`.
On this host `sqlite3.pc` emits **no** `-I` (the header is in `/usr/include`, which
`musl-gcc` excludes), and `libjpeg` is not in the list at all. So `nd_db.c`, `nd_jpeg.c`
and `nd_fb.c` fail check 3 for a toolchain reason, not a portability one — every other
source in the tree passes, and `nd_db.c` compiles cleanly under `musl-gcc` once the
include path is supplied by hand. The fix is one flag in the gate script
(`-idirafter /usr/include`, or a musl sqlite3/libjpeg in the image). Not changed here
because the gate is outside this work package.

---

### F-1. The 16bpp and "other depth" log lines have no exact Python original

**Found in:** `System/core/main.py:328-341` — the path string is chosen from Pillow's
capabilities, not from the framebuffer's. The Python prints `"BGR;16 16bpp (C, fast)"`
only when Pillow still has the `BGR;16` packer, and otherwise
`"PYTHON RGB565 PACK 16bpp (SLOW -- ~350ms/frame on RV1103!)"` plus the displayd warning
— and it prints that same slow string for *any* depth that is not 32, including 24.

**What was done:** the three strings in `nd_fb.h`'s enum comment win. C has no slow RGB565
variant, so 16bpp always logs `BGR;16 16bpp (C, fast)` and **the SLOW line and its
displayd WARNING never appear**. The raw path had no string at all in either source, so it
logs `RGB888 (C, raw)`. Both configurations are unreachable on QEMU and on hardware (both
give 240x175 @ 32bpp) and no golden frame covers them.

**What we need:** a nod, or the preferred wording. `spec-core-loop.md`'s risk table says
"keep the log lines identical so existing serial-log scraping still works", which cannot
be literally true once the slow path stops existing.

---

### F-2. A capture directory is ND_ROOT-resolved, so `nd-shoot --out` will be re-rooted

**Found in:** `nd_image_save_png()` resolves its path through `nd_path_resolve()`, as the
project rule requires. A capture directory that did *not* resolve would therefore have its
`manifest.json` written in one place and its PNGs in another.

**What was done:** `nd_capture_open()` resolves too, so the whole output directory is
consistent. With `NEODCT_ROOT` unset this is a plain copy and `--out /tmp/frames` means
`/tmp/frames`. With it set — which is how the host harness stages an overlay — the frames
land at `$NEODCT_ROOT/tmp/frames`.

**What we need:** confirmation, because WP-30 has to decide what `nd-shoot --out` promises
its operator. The alternative is for `nd-shoot` to clear the root around the write, which
is a two-line change in the tool rather than in the library.

---

### F-3. A stride shorter than a row of pixels is refused at open

**Found in:** `main.py:302-307` — the Python computes `size = line_length * yres` and
mmaps it. A driver reporting `line_length < xres * bytes_per_pixel` then fails inside
`mm.write()` partway through the first frame.

**What was done:** `nd_fb_open()` returns `ND_ERR_HARDWARE` with a log line naming the
geometry instead. Nothing shipped can produce it; the point is that it is named rather
than discovered as tearing.

---

### F-4. Check 3 of the acceptance gate cannot see the kernel UAPI headers

**Found in:** `neodct/tools/verify-c-build.sh:78` — the musl compile passes only
`-I$SRC/include` and the freetype/libpng/sqlite3 pkg-config flags. This host has no
`linux/` under musl's include root, so `lib/nd_fb.c` fails on `<linux/fb.h>`, exactly as
`lib/nd_jpeg.c` fails on `<jpeglib.h>` and `lib/nd_db.c` on `<sqlite3.h>`. All three are
missing headers on the host, not glibc-only code.

Verified: `nd_fb.c` compiles clean under `musl-gcc -std=c11 -Wall -Wextra` once
`-idirafter /usr/include -idirafter /usr/include/x86_64-linux-gnu` is added, which is
where the distribution keeps `linux/fb.h` and `asm/types.h`. A real Buildroot musl
toolchain ships both. Reading the real struct is what `nd_fb.h` instructs, and hand-rolling
`fb_var_screeninfo` to dodge the include would be worse.

**What we need:** the include path added to check 3 (or the headers installed), so the
check measures portability rather than host packaging.

---

### F-5. Two headers were added outside the frozen set

`include/nd_vclock.h` — the deterministic clock and the pinned PRNG. Nothing in the frozen
set declared a "what time is it" entry point, and the golden-frame oracle cannot exist
without one: `goldenframe.py` replaces nine attributes of Python's `time` module, and the
C equivalent has to be a function every caller already goes through. Everything that reads
the clock should call `nd_time_now()` / `nd_time_monotonic()` / `nd_time_localtime()`
rather than libc directly, or capture mode will not be deterministic.

`include/nd_capture.h` — the two framebuffer backends that are not `/dev/fb0`
(`nd_fb_open_mem()` for tests, the capture sink for `nd-shoot`) plus the manifest writer.
No declaration in `nd_fb.h` was changed.

---

## Input, keymap, keypad and T9 (WP-09/WP-10)

Recorded while porting `_discover_keypad_path`, `_load_matrix_keymap`,
`MATRIX_NAME_TO_CODE`, `pcf8575_keypad.py`, `t9_engine.py`, `t9_dict.py` and
`t9_uinput.py`. Nothing here blocks.

### I-1. Auto-repeat is new behaviour, and it defaults to the four arrows only

**Found in:** nothing in the Python — the matrix scanner reports press edges and
the evdev path drops `EV_KEY` value 2, so no key has ever repeated.

Question 2 above asks for held state *and* key repeat "available to every app, not
just Koki". Repeat is therefore synthesised in `nd_input`, from its own held state,
so an app behaves the same on the i2c keypad (which has no kernel to repeat for it)
and under QEMU.

**Numbers chosen, and why** (`ND_REPEAT_DELAY_S` / `ND_REPEAT_INTERVAL_S` in
`nd_keypad.h`):

| | value | reasoning |
| --- | --- | --- |
| initial delay | **400 ms** | a deliberate single press is 150–250 ms of contact; below ~350 ms ordinary typing produces a second character. Also comfortably shorter than the T9 multi-tap window (1.0 s), so the two never race. |
| interval | **120 ms** | the UI polls input on a 100 ms tick (`read_keypress`'s default, which every blocking widget inherits). Anything faster cannot be observed — the events pile up and the list jumps several rows at once. 120 ms gives a steady ~8 rows/second a person can stop on. |

**Only `103/105/106/108` repeat by default.** That is exactly what was asked for —
list scrolling and NetSurf navigation — and it is the only safe set: a repeat on a
digit would walk the T9 multi-tap cycle behind the user's back, and a repeat on
Enter would open whatever the list landed on. `nd_input_set_repeat()` and
`nd_input_set_repeat_codes()` let an app widen or disable it.

### I-2. `read_keypress()`'s double wait is not reproduced

**Found in:** `core/main.py:1191` — with a matrix backend *and* an evdev fd, the
matrix is polled for the full timeout and evdev is then polled for the full timeout
again, so one call can block for `2 × timeout`.

`nd_input.h`'s contract says "one PRESS, or `ND_KEY_NONE` if none arrived within
`timeout_s`", so `nd_input_read_event()` polls both inside one budget. This cannot
change behaviour on the phone: the target has no `/dev/input/event0` when the matrix
is in use, so only one backend is ever live. It is visible on a dev host with both.

### I-3. The gpiozero matrix backend was dropped, as PORT-PLAN.md recommends

**Found in:** `core/main.py:197` `MatrixKeypadInput`. Reachable only with a keymap
naming a driver other than `pcf8575-i2c` **and** `/dev/gpiochip*` present, neither of
which the target has. Porting it means reimplementing gpiozero over libgpiod for a
path that cannot run. `nd_input` logs a refusal line for that keymap and falls
through to evdev. This removes the project's only gpiozero dependency.

### I-4. `nd_evdev_device_name()` uses EVIOCGNAME first, sysfs second

`nd_input.h` specifies `EVIOCGNAME`; the Python reads
`/sys/class/input/<eventN>/device/name`. Both are implemented, ioctl first, and the
literal word `"unknown"` is substituted by the caller when both fail — which is what
the Python printed. For a real device the two agree.

### I-5. Keymap positions outside 16×16 are dropped rather than stored

`_load_matrix_keymap` would happily record `(99, 3)` in a dict no scan can ever
match. `nd_keymap.matrix_to_code` is a flat 16×16 (frozen in `nd_input.h`), so the
entry is skipped at load. The only observable difference is a keymap whose *entire*
key set is out of range: Python accepts it and matches nothing, the C refuses it with
"no recognized keys". No file the wizard writes can do this.

### I-6. `nd_keymap_save()` does not write `generated_at_unix`

`_build_payload` writes it; `nd_keymap` has no field for it and `nd_input.h` is
frozen. The loader ignores the field, so a round trip is lossless for everything that
is read back. The first-boot wizard (WP-27) should add it when it owns the writer.

### I-7. `nd_t9_dict` reads at most 255 bytes per line

`CODING-STANDARDS.md` §1.5 does not allow a read sized by the file. The builder caps
words at 12, so this is twenty times the slack needed; a longer line would come back
truncated where Python would return the whole thing. Also: a word that does not fit
`ND_T9_WORD_MAX` (16) is skipped from `suggest()` rather than truncated into the
caller's slot.

### I-8. `nd_t9_dict_open()` does NOT go through `nd_path_resolve()`

It takes a real filesystem path, because the tests point it at fixtures and the two
cases that check the shipped 2.88 MiB file reach outside `ND_ROOT` entirely.
`nd_t9_dict_shared()` is where the `ND_PATH_T9_DICT` constant is resolved, so
production code still honours the hook. Same split as `nd_json_parse_file()`, which
resolves internally — pass it the *virtual* path or the root gets prefixed twice.

### I-9. `nd_uinput_kbd` has no struct tag, so the bridge has to cast

`nd_input.h` declares it as an anonymous struct typedef
(`typedef struct { ... } nd_uinput_kbd;`), while `nd_t9.h` forward-declares
`struct nd_uinput_kbd` for `nd_t9_bridge_start()`. Those are two different types to
the compiler, so `nd_t9_bridge.c` keeps the real type internally and casts at its two
entry points. Both headers are frozen; giving the struct a tag would make both
spellings the same type and the cast would disappear.

### I-10. One new header was added: `nd_keypad.h`

`nd_input.h` is frozen and deliberately says nothing about *how* keys are produced.
`nd_keypad.h` is the layer underneath it — `nd_pcf8575`, `nd_matrix`, the keymapped
input backend, `nd_keymap_save()`, the repeat tuning knobs, and the two
`nd_t9_bridge_*_for_test` helpers. It adds only; it changes no existing declaration.


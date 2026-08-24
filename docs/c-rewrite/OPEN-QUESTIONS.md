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


---

## Widgets A: SoftKeyBar, HeaderWidget, VerticalList, PagedList, LevelSelector (WP Widgets-A)

Recorded while porting `framework.py`'s `SoftKeyBar` (447), `HeaderWidget` (502),
`VerticalList` (539), `PagedList` (1175) and `LevelSelector` (1452). All five golden
frames this package can reach — `widget-softkeybar`, `widget-verticallist`,
`widget-verticallist-scrolled`, `widget-pagedlist`, `widget-levelselector` — are
byte-identical to the reference (0 differing pixels each). Nothing here blocks.

### W-1. `nd_ui_metrics.c` was added, and nd_ui.c carries a weak copy of the same five

`nd_ui.h` declares `nd_ui_width/height/softkey_height/content_bottom/header_divider_y`
but no `.c` defined them, and every widget's first line of layout needs them.
`spec-ui-framework.md`'s module table names `nd_ui_metrics.c` for exactly these five,
so that is where they went — **strong**. The core-loop package independently needed
them earlier and left **weak** copies in `nd_ui.c`; the two agree to the digit and
weak-vs-strong resolves cleanly, so neither file has to change. Recorded so that a
later "deduplicate this" removes the weak pair rather than an arbitrary one.

`nd_ui_present()`, `nd_ui_text_size()`, `nd_ui_wait_for_key()` and
`nd_ui_read_keypress()` are **not** in that file: they belong to `nd_ui.c`, which
defines them strongly, because in the core the two key calls must tick the battery,
the modem and the ring before returning.

### W-2. A zero geometry field is C's spelling of Python's missing attribute

`_ui_width` is `int(getattr(ui, "W", DEFAULT_UI_W))`, so a context that has not
assigned `W` still lays out at 240. The C struct always *has* `w`, so the equivalent
of "the attribute is missing" is "the field is still zero", and that is what the five
helpers fall back on. It matters twice: the Python unit tests' `FakeUI`, and a widget
drawn during construction step 13 — the alpha security notice is a blocking modal
inside the constructor, before geometry is assigned.

### W-3. `nd_softkey.has_text` means "not None", not "not empty"

`SoftKeyBar.current_text` can be `None`, `""` or a label, and the C struct's
`char current_text[64]` can only represent the last two. `has_text` is therefore the
`is not None` half: `update("")` leaves `has_text` **true** with an empty
`current_text`, `update(NULL)` leaves it false. Making the flag mean "non-empty"
would duplicate what `current_text[0]` already answers and throw the distinction
away. Nothing in the tree reads the field yet; flagged before something does.

`current_text` is capped at 64 bytes (the header's array). The longest label the
project ships is "Options"; a longer one is truncated in the *record*, never in what
is drawn.

### W-4. `nd_header`'s `sub_index` uses a negative value for Python's `None`

`text_for(sub_index=None)` prints the root id alone. C has no `None`, and `0` is a
legitimate sub-index (`"3-0"`), so `nd_header_text_for/width/draw` treat
`sub_index < 0` as "no sub-index". `PagedList`'s empty state is the only caller that
needs it; every other call site passes `selected_index + 1`, which is at least 1.

### W-5. `ND_LEVELSEL_MAX` clamps a `count` the Python leaves unbounded

`LevelSelector(ui, current, count)` builds `range(1, count + 1)` with no ceiling. The
frozen struct has `char labels[9][16]`, so `count` is clamped to 9 rather than
refused; `current` is still clamped against the **caller's** count first, so the
selected row is what the Python would have picked before the clamp bites. Every
shipped caller passes the default 9. Also: `-Wformat-truncation` at `-O1` cannot see
that `i + 1` is 1..9, so the `snprintf` argument is written through `nd_clamp32()` —
same value, visible range.

### W-6. `nd_pagedlist_wrap()` stops after 64 words

`_wrap_to_lines` splits the whole item name and then uses at most two lines' worth.
CODING-STANDARDS §1.5 does not allow an array sized by input, so the token list caps
at 64 — twenty times what two lines of 24 px type can hold, and past the cap the
behaviour is the Python's own "words remain" branch, which is already the
`"..."` path. A 65-word contact name is therefore rendered identically.

Its `lines[-1] = …` on a possibly-empty list (risk 14 in `spec-ui-framework.md`) is
guarded with a log line rather than a crash. The loop always appends before advancing
`i`, so the guard cannot fire today; it is there so that a future edit that makes it
reachable says so instead of writing out of bounds.

### W-7. PagedList's input flush is a 0.01 s poll, not a bare drain

`nd_input_drain()` is the obvious C spelling and it is **wrong** here: `nd_input.h`
is explicit that AppSelector and PagedList poll with 0.01 s and MessageDialog with
0.0, and the ten milliseconds are what catch a record already on its way from the
screen being replaced. `nd_pagedlist_show()` therefore loops on
`nd_input_read_event(in, 0.01, …)` until it comes back empty, bounded at 256
iterations so a wedged fd cannot spin.

### W-8. Hold-to-repeat needed no widget code, and that is the point

Decision 2 asks for held-key state in every app and names `VerticalList` and
`PagedList` scrolling. `nd_input` already synthesises repeat presses for
103/105/106/108 from its own held state (400 ms then 120 ms, `nd_keypad.h`), so a
held arrow reaches `nd_vlist_show()`/`nd_pagedlist_show()` as an ordinary stream of
presses and scrolls the list exactly as tapping does. The widgets' contribution is
negative: they must **not** filter, coalesce or re-derive repeat, because a widget
that did would behave differently on the i2c keypad from under QEMU — which is
precisely what putting it in `nd_input` avoids.

No static frame moves: repeat produces the frames a person tapping produces, sooner.
`test_widgets_lists.c` drives one press through a real `nd_input` over a pipe and
asserts the selection walks six rows with nothing further written.

### W-9. `VerticalList.show()` redraws on Up/Down even when nothing moved

Ported as-is, including at both ends of the list. It is what repaints over a caller's
transient overlay, and holding Down at the bottom is visibly a steady screen rather
than a dead one. `nd_vlist_handle_key()` does no drawing — the header says so — so
the unconditional redraw lives in `nd_vlist_show()`, keyed off the key code rather
than off whether the index changed.

### W-10. Two Python behaviours reproduced that look like bugs

- `PagedList.show()` returns index **0** for ENTER on an empty list, even though
  `draw()` short-circuits the empty case and never renders a row. Latent in the
  Python and kept.
- `PagedList` centres item text inside `max_w = 223`, not inside the 240 px screen,
  so every item sits ~8 px left of true centre. The golden frame depends on it.

---

## Core: nd_ui, the image cache, the home layout, the home screen (WP core-ui)

Recorded while porting `System/core/main.py`'s `NeoDCT_UI` (516), `get_image` (714),
`render_element` (765), `render_home` (845), `render_home_dialing` (895), `update` (953),
`read_keypress` (1191) and `handle_input` (1240), plus `load_layout` / `load_wallpaper`
and `_scan_apps_from_dir`.

**All six golden frames this package can reach are byte-identical to the reference:**
`home`, `home-panel`, `home-dialing`, `home-nowallpaper`, `home-simulation` and
`home-sms-banner` — 0 differing pixels each, confirmed both by SHA-256 inside
`test_ui.c` and by `goldenframe.py --compare`. Nothing below blocks.

### U-1. `nd_ui.h`'s summary of the "/home" fixup is looser than the Python

The header says *"a path starting `/home` that contains `NeoDCT`"*. `get_image`
(main.py:725) actually tests for **`"System" in path`**, and then takes
`path.split("NeoDCT")[-1]` — which returns the **whole path** when `NeoDCT` does not
occur, so such a path is *prefixed* with `/NeoDCT` rather than rewritten. Both
behaviours are the Python's and both are ported; `test_ui.c` pins all three cases
(rewritten, left alone for want of `System`, left alone for not starting `/home`).
Flagging only because the header and the code disagree about the guard.

### U-2. `ImageFont.load_default()` has no C equivalent, so a font failure draws nothing

`spec-core-loop.md` §6 step 12 already asks this question and it is still open. What
was done: all four faces are left `NULL`, `[UI] Font load failed, using default.` is
logged, and every text draw becomes a no-op. That is visibly wrong rather than subtly
wrong, which is the safer of the two failure modes, but it is not what the Python does
— Pillow's built-in bitmap face would still put readable text on screen. Unreachable
in any shipped configuration (`font.ttf` is in the read-only squashfs) and no golden
frame covers it. Bundling a fallback bitmap face is a real option if the owner wants
one.

The same gap makes `render_home`'s `"No Layout Found"` fall back to `font_s` (14 px)
where the Python passes no `font=` at all and gets the default face. Also unreachable
with a shipped `ui_home.json`.

### U-3. Construction step 13 runs after 14–18, not before them

`nd_ui.h` is emphatic that the alpha security notice is drawn from inside the
constructor *before* `engineering_mode`, `home_layout`, `wallpaper` and `apps` exist,
and that a widget drawn at that moment sees a half-built context. In C the four are
plain struct fields that are zeroed at entry, so "not yet assigned" and "assigned"
are not distinguishable to a widget the way a missing Python attribute is. Rather
than fake it, `nd_ui_init()` runs the common construction (steps 5–12 and 14–18) and
then shows the notice. `nd_msgdialog` reads none of the four — it draws a warning
triangle, wrapped text and a button on a black background — so the dialog is
identical either way, and `widget-messagedialog.png` is the frame that proves it.
Say so if the ordering should be forced anyway.

### U-4. The shared contact picker has no declaration anywhere

`handle_input` opens `System.apps.PhoneBook.shared.list_ui.show_contact_selector` on
Up/Down from the home screen (main.py:1268). The two Dialer screens are now declared
in `nd_widgets.h` (`nd_dialer_show_calling`, `nd_dialer_show_incoming`), but nothing
in `include/` names the picker. `nd_ui.c` declares and weakly references

```c
bool nd_contacts_show_selector(nd_ui *ui, const char *title, const char *btn_text,
                               nd_contact *out);
```

so the key does nothing until PhoneBook's package defines it, and needs a one-line
rename here if that package picks a different name. `nd_db.h` already has the
`nd_contact` row type and says the core dials by name from the home screen, so the
data half of the contract exists.

### U-5. `nd_home_layout.background` is eager where the Python is lazy

`render_home` builds `self._home_bg` on the first frame that needs it —
`get_image(bg).convert("RGB").resize((240,175), LANCZOS)` — and caches it on the
instance. `nd_layout.h` makes `background` an `nd_image *` owned by the layout, so
`nd_layout_load()` does the work up front instead. Two consequences, both dormant
because the shipped layout has `"background": null`:

- a background file that appears *after* boot is picked up by the Python on a later
  frame and never by the C;
- the Python distinguishes "no background declared" (paint black) from "declared but
  it failed to load" (**paint nothing**, leaving the previous frame). The C has one
  NULL for both and paints black. The second case needs a field the frozen struct
  does not have.

### U-6. Every shipped manifest still says `"exec": "main.py"`

`nd_ui.h` writes the default as *`exec -> "main.py"` (now "app.so")*. The scan ports
the Python default literally (`"main.py"`), because that is what
`data.get("exec", "main.py")` says and because the value is read from the file
anyway — all 24 shipped `manifest.json` files spell `main.py`, so changing the
*default* would change nothing.

`nd_app_entry.exec` is therefore populated and **not used to launch anything**.
`nd_proc.h`'s `entry` argument is an entry-point NAME (`nd_app.h`:
`ND_APP_ENTRY_OPEN_MESSAGE`, `ND_APP_ENTRY_OPEN_INBOX`, or NULL for `app_run`), not
a filename, and the code always lives in `ND_APP_SO_NAME` beside the manifest. So
`nd_ui_render_menu()` passes `entry = NULL` and the Read softkey passes one of the
two constants. Somebody should still decide whether the manifests get rewritten to
`"exec": "app.so"` or the field is retired; either way nothing reads it today.

Related and harmless: `"id"` is a **string** in every shipped manifest (`"id": "1"`).
Python's `int()` takes either, so the C parses an int or a decimal string and, like
the Python's `try`/`except`, **drops the whole app** when neither works.

### U-7. `nd_ui_sim.h` was added outside the frozen header set

Third addition, after `nd_vclock.h` and `nd_capture.h` (F-5). It declares the five
status readouts `render_element` needs — battery level, whether a fuel gauge exists,
signal bars, carrier name, banner state — and `nd_ui_sim_status()`, which is
`uistub.StubUI.simulate_status()`. It changes no declaration in `nd_ui.h`.

It has to exist because those four values are attribute lookups on live service
objects in Python and an app process has none of the three services (`nd_ui.h` says
so). The fallbacks are not invented: they are exactly what the Python's own services
report with no hardware — level 3, `hardware False`, `signal_level() is None`,
`operator_display() is None` — which is the state `home-simulation.png` was captured
in, and that frame matching to the pixel is the evidence.

The overrides live in a file-static, because there is one `nd_ui` per process and the
frozen struct has nowhere to put them. `nd-shoot` will need this hook: three of the
six home frames are captured with `simulate_status(battery=4, signal=4,
carrier="Tello")` and cannot be reproduced without it.

### U-8. The image cache's per-image ceiling returns NULL rather than caching uncapped

Decision C-4 asked for "a hard per-image ceiling that refuses to cache anything
absurdly large in the first place". `nd_image.h` says the returned image is **owned by
the cache**, so "do not cache it" and "return it" cannot both be true — handing back a
surface with no owner would leak or double-free. An image over the ceiling is
therefore freed and reported as a miss (`NULL`), with a red log line naming it, which
is the same thing the caller already handles for a failed decode.

The ceiling is **8 MiB of decoded pixels** (2048×1024 RGBA). The biggest thing that
reaches this cache today is an 82×82 icon at 27 KB, so nothing shipped is within two
orders of magnitude of it. Eviction order is untouched: still a 32-entry FIFO on
insertion, still no reordering on a hit, still `pop(next(iter(dict)))`.

### U-9. `[OS] Launching App ID: <n>` prints the list index, not the manifest id

`render_menu` (main.py:941) prints `choice`, which is `AppSelector.show()`'s
zero-based index into `self.apps`, not `apps[choice]["id"]`. Ported as it is —
"port the bug too" — and noted because the message reads as though it were the id and
somebody will eventually try to grep for one.

### U-10. `read_keypress` no longer polls the modem, only drains its queue

Decision 1 gives the modem its own thread, and `nd_modem.h` says outright that
`nd_modem_poll()` is called from that thread and "the UI never calls it". So
`_modem_tick`'s port keeps the event drain and the busy-requeue loop and drops the
`self.modem.poll()` that used to precede them. `_battery_tick` and `_ring_tick` are
unchanged and still run on every `read_keypress`, including the 0.6 s `+CLIP` grace
period before an incoming call is reported.

`nd_ui_read_keypress()` returns `ND_KEY_INCOMING_CALL` where the Python raised
`IncomingCall`. In an **app** process all three service handles are NULL, so all three
ticks are no-ops and the call is the plain read `nd_ui.h` promises.

### U-11. Two cross-cutting files landed here that are not strictly nd_ui

`lib/nd_imgcache.c` implements the `nd_imgcache_*` API declared in `nd_image.h` — it
is `NeoDCT_UI.get_image`'s cache and decision C-4's subject, so it came with this
package rather than with the image codecs.

`lib/nd_softkey.c` was started here as a **weak** placeholder so the six home frames
could be verified before the widget layer landed, and the Widgets-A author has since
taken it over with strong definitions. Nothing needed deleting; `nd_ui.c` only ever
called the two public functions. Recorded as the worked example of how the weak-symbol
hand-off is meant to go — see also W-1.

---

## Widgets B: MessageDialog, TextScroller, InfoScreen, ProgressScreen, DetailPage

`lib/nd_msgdialog.c`, `lib/nd_scroller.c`, `lib/nd_infoscreen.c`, `lib/nd_progress.c`,
`lib/nd_detailpage.c`, `test/unit/test_widgets_dialogs.c`.

All three golden frames this package can reach — `widget-messagedialog`,
`widget-textscroller`, `widget-infoscreen` — are **exact**, 0 differing pixels each,
checked both by SHA-256 in the unit test and by `goldenframe.py --compare`.
Nothing below changes a pixel unless it says so.

### D-1. Two fixed line ceilings replace two unbounded Python lists

`MessageDialog._wrap_text` and `TextScroller._wrap_text` both build unbounded lists.
CODING-STANDARDS.md section 1.5 will not have an array sized by input, so:

* **MessageDialog** wraps into 24 lines on the stack (6,144 bytes). The dialog can only
  ever *draw* `(145 - y - 8) / line_h` lines, which is at most 7 in the paragraph look
  and 5 in the alert look, so 24 is four times the reachable maximum. `max_lines` is
  additionally clamped to the buffer, so overflow ends in the same clipped-plus-`" …"`
  line the Python produces rather than losing text silently.
* **TextScroller** wraps into `ND_SCROLLER_MAX_LINES` (128, the frozen header's own
  constant) in **file-scope BSS**, 32 KB, not on the stack. Section 4's rule is
  "allocate once, reuse"; this is the once. Exactly one scroller is ever on screen —
  both `draw()` and `show()` are modal — so the shared buffer is not a reentrancy
  hazard. A second scroller drawn from inside the first one's key loop would be, and
  nothing in the tree does that.

A source longer than 128 wrapped lines loses the tail. The Python would page through it.
No shipped caller is close: the longest is the Snake help at 8 lines.

### D-2. The 20 px "is it more than two lines?" wrap runs into a three-line buffer

`framework.py:1132` wraps the whole message at 20 px purely to test `len(...) <= 2`.
The C wraps into a 3-line `nd_lines` and reads `n > 2 || truncated`. The two tests
cannot disagree — both answer "more than two" — and the short path avoids wrapping a
long paragraph twice. Only the *count* is used from that pass; when it is two or fewer
the same lines are reused, exactly as the Python reuses `alert_lines`.

### D-3. DetailPage's hero row is re-laid-out inside `draw()`

`_hero_block` produces a closure over a *list* of `(text, font, height)` rows.
`nd_detail_block` holds **one** string and the struct is frozen, so `ND_BLOCK_HERO`
carries only the group's height and `paint_hero()` recomputes the rows from
`(image, title, subtitle, badge)` when the block is painted.

The recomputation is pure — same fitted font, same `_ellipsize`, same `_wrap_lines` —
so it cannot drift from the layout pass that sized the block. It costs one wrap per
frame on the one widget that already re-measures everything. The scratch it wraps into
is 6.5 KB of file-scope BSS (`HERO_MAX_SUB` = 24 subtitle lines, about four viewports'
worth in a ~140 px column), so `draw()` still allocates nothing.

**Question for the owner:** if a field for the hero rows may be added to
`nd_detailpage`, the recompute goes away. It was not added because the header is
frozen and this is not worth unfreezing it for.

### D-4. `DetailPage.hero_box` has no field in the frozen struct

`framework.py:1786` sets `self.hero_box = (0, 0, width, hero_h)` in case A and `None`
otherwise, and `spec-ui-framework.md`'s API table lists `.hero_box` among the
attributes callers read. `nd_detailpage` has no such member. Nothing in the shipped
tree reads it — only `tests/test_update_ui.py` might — and the same information is
`blocks[0].kind == ND_BLOCK_HERO` with `blocks[0].height`. Left out rather than added
to a frozen header. Say the word and it comes back.

### D-5. `DetailPage` owns the further-shrunk hero picture, in a private allocation

`nd_detailpage.image` is documented as `const nd_image *` *borrowed*, and the cache's
`max_size=64` copy genuinely is. But `_fitted_hero()` may thumbnail it again, down to
`MIN_IMAGE` in 8 px steps, and that result is a copy the page owns and must free.

Rather than add a field, `nd_detailpage_init()` makes one allocation whose private
header holds the owned image and whose flexible array member is what `p->blocks`
points at; `nd_detailpage_free()` recovers the header with `offsetof`. The block array
is claimed at `ND_DETAIL_MAX_BLOCKS` (the two-pass body wrap does not know its own
answer until it has run) and then `realloc`'d down to what the page used — ~71 KB
claimed, ~3 KB kept for a typical page.

`p->image` is set to NULL by `nd_detailpage_free()`, because it may point at the copy
that has just gone.

### D-6. A page needing more than `ND_DETAIL_MAX_BLOCKS` blocks loses its tail

256 blocks is the frozen header's ceiling and the Python has none. At 18 px a line
that is 4,608 px of body — 33 viewports. Overflow drops the trailing blocks silently
rather than growing, per section 1.5.

### D-7. `DetailPage._text_width()` is dead code and was not ported

`framework.py:1714` defines it, and it reads `self.scrollable`, which reads
`self.content_height`, which does not exist until after `_layout()` returns. Calling it
from inside `_layout` would raise `AttributeError`. Nothing calls it: `_hero_block`
computes its column directly and the body loop recomputes its own width. Not ported.
Noted because the spec's section 16 mentions it and a reader may go looking.

### D-8. `nd_scroller_paginate()` returns the page count

nd_widgets.h declares `size_t nd_scroller_paginate(const nd_scroller *s, size_t *line_h_out)`
and says two existing tests assert on the pagination directly, without saying what the
`size_t` is. It is the number of pages; `*line_h_out` is the 25 px line height. The
per-page contents are not exposed — `test_widgets_dialogs.c` asserts on them through
the pixels instead, which is a stronger check and needs no new API. If the two Python
tests being referred to want the `(line, height)` list, that is a second function.

### D-9. Two `NULL` arguments have no Python spelling and were given the default

`nd_scroller_init(..., more_text, back_text)` and `nd_infoscreen_show(...,
softkey_text)` correspond to Python keyword arguments with defaults `"More"`, `"Back"`
and `"Back"`. Passing NULL in C means "the default"; passing `""` still means an empty
strip, which is a thing `SoftKeyBar.update("")` supports. `nd_msgdialog`'s
`button_text` is different — it is set through `nd_msgdialog_set_button()` and a NULL
there really does clear the strip, matching `SoftKeyBar.update(None)`.

### D-10. An un-cancellable MessageDialog's `show()` never returns

`nd_msgdialog_set_keys(&d, NULL, 0, NULL, 0)` gives a dialog no key can dismiss, and
`nd_msgdialog_show()` then loops for ever. That is exactly what the Python does, and it
is what the low-battery shutdown notice wants — it is waiting for the power to go, not
for a key. Callers that only want to paint use `nd_msgdialog_render()`.

### D-11. `nd_ui_present()` is a no-op with no framebuffer, and the tests rely on it

Every widget here ends with `ui.fb.update(ui.canvas)`. `nd_ui_present()` returns
`ND_ERR_INVAL` when `ui->fb` is NULL, which is what lets the golden-frame tests render
into a bare canvas with no panel behind them. Not a deviation, just the thing that
makes the oracle usable from a unit test.

---

## Widgets C: TextInput, TextInputLong, PredictiveText, the T9 indicator

Landed as `lib/nd_textinput.c`, `lib/nd_textlong.c`, `lib/nd_predictive.c`
(+ `lib/nd_predictive_priv.h`) and `lib/nd_t9ind.c`, with
`test/unit/test_widgets_text.c` (2,163 checks). `widget-textinput` and
`widget-textinputlong` both render byte-identical to the reference.

Nothing below is blocking. Every item is a decision that had to be made because
Python has no equivalent question, and each is written down so it can be
overruled cheaply.

### T-1. Two fields were ADDED to `nd_textlong` in the frozen `nd_widgets.h`

`size_t wrap_clean_off` and `size_t wrap_clean_lines`. They are the watermark
decision C-2's "rewrap from the edited line, not the whole string" needs, and a
struct field cannot live in a new header the way `nd_ui_sim.h` and `nd_vclock.h`
did. No declaration changed and no other struct was touched.

**Both fields are an optimisation only.** Zeroing them costs a full rewrap and
changes nothing on screen, which is exactly what
`test_incremental_rewrap_matches_full()` and
`test_incremental_rewrap_while_typing()` assert: 264 characters typed one key at
a time, every keypress rendered and its SHA-256 compared against a cold widget
holding the same string, then backspaced all the way out again.

### T-2. `pending_len` and `cursor` are BYTE counts; the Python counts characters

`nd_widgets.h` already says the cursor is a byte offset. `pending_len` follows it,
because `nd_underline_tail()` takes a byte count and everything that can become a
provisional word is ASCII — the dictionary is ASCII and the no-match fallback is
the digit string. Where a byte and a character genuinely differ the C is careful
anyway: backspace deletes one UTF-8 codepoint, the character count in
TextInputLong's header counts codepoints, and a multi-tap REPLACE corrects the
cursor by the byte delta (the Python does not move it at all, because for it the
two characters are always one each).

### T-3. What "ignore further input at the cap" means for a predictive word

C-2 says the widget must ignore the key rather than truncate. For an APPEND or a
QWERTY character that is one length check. For a predictive digit it is not, because
`t9.press()` has already appended the digit to the engine by the time the widget
learns the candidate will not fit. So: the field is left untouched,
ND_WIDGET_RESULT_NONE is returned, **and the digit is popped back off the engine**,
so the engine and the visible field never disagree about what has been typed.

One case has no good answer and was given a defensible one: Clear on a pending word
pops a digit, and the shorter digit string can suggest a LONGER word (`466` → "inn",
`46` → something longer). With the field exactly full that re-prediction cannot fit.
The C then abandons the pending word — `_predict_reset()` — rather than leave the
field and the engine describing different text.

### T-4. `nd_textinput_init()` and `nd_textlong_init()` REFUSE a cap above the ceiling

`cap > ND_TEXTINPUT_CAP` / `cap > ND_TEXTLONG_CAP` returns `ND_ERR_INVAL`. Both draw
paths copy the text (plus the blink marker) onto the stack, so the constant is a hard
ceiling rather than a suggestion, and silently using only part of a caller's larger
buffer would be worse than saying no. If a caller ever legitimately needs more, raise
the constant — do not pass a bigger `cap`.

### T-5. The mode indicator is invisible in every golden frame, and that is correct

`framework._t9_active(ui)` is `getattr(ui, "matrix_input", None) is not None`: T9
multi-tap runs on the real i2c keypad only, because a development QEMU keyboard has a
full QWERTY and takes the DEV_KEYMAP path. `uistub.py` boots with a dead input path,
so `matrix_input` is None and `t9_indicator_size()` returns None. Both text frames
were captured that way. `nd_t9ind_size()` therefore returns 0 and writes neither its
label nor its pencil size out, and the test drives the indicator by setting
`ui.has_matrix_keypad` by hand.

Measured, for whoever needs the numbers: at `font_n` the indicator is 45 px wide in
"abc", 48 in "ABC", 43 in "123", and 15 + 4 + 45 = 64 in predictive, where 15 is
`max(8, int(height("abc") * 0.85))`.

### T-6. The pencil needs round-half-even, and two of its sizes prove it

`half = round(size / 4.0)` and `point = round(size * 0.55)` are Python `round()`,
which is banker's rounding. C's `round()` is half-away-from-zero and disagrees at
`size` 10 (2.5 → 2 vs 3, a barrel one pixel wider all the way down) and at `size` 30
(16.5 → 16 vs 17). `nd_widgets.h` already warned about this; the test pins both sizes
against bitmaps generated from the Python's own `_draw_pencil`, plus the exact set
pixel count at 8, 10, 12, 15, 17 and 30.

### T-7. Two Python behaviours reproduced that look like bugs

- **The text jumps as you type.** `TextInput.draw` centres the line on the INK height
  of the string it is about to draw, so an empty field showing only the cursor is
  3 px tall at 20 px and "Ag" is 21, and the line visibly moves. Reproduced.
- **A multi-tap REPLACE with the cursor at 0 does nothing but still reports "typed".**
  `TextInputLong.handle_key` guards the edit with `if self.cursor > 0` and returns
  `"typed"` either way, so the caller redraws an unchanged screen. Reproduced.
- Neither text widget fits its title, so a long one runs off the right edge — and in
  TextInputLong it runs *under* the character count, which is visible in
  `widget-textinputlong.png` itself.

### T-8. `nd_textinput_show()` keeps the blocking wait, so the cursor still does not blink

`nd_widgets.h` calls this out and it is worth restating: the Python checks its 0.5 s
timer only after `wait_for_key()` has returned, and that blocks, so an idle field
never blinks. The marker toggles on the first keypress arriving more than half a
second after the last toggle. A `poll()` timeout would blink for real and would then
disagree with the captured frames. The C uses `nd_time_now()` for the timer so capture
mode drives it deterministically.

### T-9. `nd_ui_wait_for_key()` cannot return "no key", so `key < 0` is the `None` branch

`show()`'s `if key is None: continue`. In C the only negative returns are
`ND_KEY_NONE` (which `wait_for_key` never yields) and `ND_KEY_INCOMING_CALL`, which an
app process never sees — it gets SIGTERM instead. Treating both as "keep waiting"
matches what `nd_vlist_show()` already does with them.

---

## The capture tool: nd-shoot (WP nd-shoot)

`neodct/src/tools/nd_shoot.c` plus `neodct/src/test/unit/test_shoot.c`. It renders
**25 of the 49** reference screens and refuses to invent the other 24. Nothing here is
blocking; the numbered items are decisions a reviewer should be able to see and
overrule.

### S-1. Twenty-four frames are declared missing rather than drawn

`spec-build-test.md` §3.6 defines 49 shots. Twenty-four of them launch an app, and
`neodct/src/apps/` is empty — no `app.so` exists, the Dialer and `CrashHandler` are not
ported, and Snake and Memory are reached through the Games app. The task's rule was
"never emit a frame you cannot justify", so `nd-shoot` writes no PNG for those names
and `goldenframe.py --compare` reports each as `missing -- not rendered by candidate`.
That is the truth and it is also the next agent's work list.

Three of the twenty-four are byte-identical to widget frames that ARE rendered
(`app-clock` ≡ `widget-messagedialog`, `app-messages` ≡ `widget-pagedlist`,
`app-phonebook` ≡ `widget-verticallist`; §3.6 lists the digests). Copying the pixels
across would have made the count read 28 and would have been a lie — nothing launched
an app. They are skipped with the rest.

### S-2. The skip list is a separate file, not a manifest key

`goldenframe.write_manifest()`'s schema is fixed and `compare()` parses it. Adding a
`"skipped"` key would be ignored today and could collide with a future revision, and
`nd_capture_write_manifest()` belongs to `lib/nd_capture.c`, which is not this module's
to change. So the reasons go in `<out>/nd-shoot-skipped.json`, alongside the manifest:

```json
{ "skipped": [ { "name": "app-clock", "reason": "app not implemented: ..." } ] }
```

`test_shoot.c` asserts rendered ∪ skipped == the 49 reference names, exactly once each,
and that no skipped name left a PNG behind.

### S-3. `home-sms-banner` is reproduced with two throwaway frame commits

`shoot_telephony()` draws `call-active` and `call-incoming` from the SAME `StubUI`
before it, so the banner is that block's **third** frame — and the envelope's
`int(time.time() * 2) % 2` blink phase is decided by the two ticks those screens spent.
Their pixels cannot be reproduced yet; their ticks can. `nd-shoot` commits two
throwaway frames and asserts `nd_vclock_frame() == 2` before drawing the banner. This
is `test_ui.c`'s group D, and the frame comes out byte-identical.

If the Dialer lands and its two screens are drawn for real, the throwaways must be
deleted, not left in — two extra commits would move the blink phase.

### S-4. `--out` is deliberately outside `ND_ROOT`

F-2 predicted this: `nd_capture_open()` and `nd_capture_save()` resolve through
`nd_path_resolve()`, so with the staged root in force `--out DIR` would land inside the
staging directory and be deleted on exit. `nd-shoot` clears the root for the duration
of every capture call and restores it afterwards. `nd_capture` is the one API whose
paths belong to the developer's filesystem rather than the phone's.

### S-5. `NEODCT_ROOT` is honoured only when it looks like a phone root

The Makefile points `NEODCT_ROOT` at one shared empty scratch directory for every unit
test. Taking it at face value would have `nd-shoot` render a home screen with no fonts,
no wallpaper and no apps — a plausible-looking picture that is wrong. So the variable is
used as-is only when `$NEODCT_ROOT/NeoDCT/System` exists; otherwise `nd-shoot` logs that
it is staging its own root and does so. `--overlay` / `$NEODCT_OVERLAY` name the overlay
to symlink `System` at; with neither set it is found relative to the binary.

Like `test_ui.c`, only `/NeoDCT/User` is real — `/NeoDCT/System` is a symlink onto
`neodct/overlay/NeoDCT/System`, so nothing under `neodct/overlay/` can be written to and
a full run costs 0.25 s rather than `uistub.py`'s four 16 MB copytrees.

### S-6. `test_shoot.c` spawns the binary instead of linking a helper

`nd-shoot` has no library surface: the thing that can be wrong is the output directory.
The test forks and `execve`s the real binary (`CODING-STANDARDS.md` §1.1 — `execve` is
the first statement in the child, `_exit` the only other call) and then judges what came
out, which covers the argument parsing, the root staging, the exit status and the output
layout as well as the pixels. It finds the tool at `../bin/nd-shoot` relative to
`/proc/self/exe`, so an ASan test run drives the ASan tool rather than a stale default
one.

### S-7. The two 240×240 frames stay centred, not bottom-aligned

`home-panel` and `menu-panel` come from `nd_capture_device_frame()`, which centres the
band at y=32 the way `uistub.CapturingFramebuffer.device_frame()` does. The hardware
daemon bottom-aligns at y=65. §3.7 is explicit that the two panel names are
documentation aids reproducing the *stub*, and that "fixing" them would break the oracle
and change nothing on the phone. Not changed.

### S-8. The InfoScreen key script uses the core's press/release channel

`widget-infoscreen` is the only blocking screen in the renderable set;
`shoot_docs.py` dismisses it with `ui.keys.push(BACK)`. `nd_input_channel_send()`
already writes native `struct input_event` pairs, so a `KeyScript` is just a channel
with nobody on the far end. Auto-repeat is switched off on it
(`nd_input_set_repeat(in, 0, 0)`): repeat is new behaviour (I-1) and the reference
frames were captured without it, so a script must never synthesise a key the Python did
not deliver.

---

## AppSelector, the app registry and the icons (WP app-menu)

`lib/nd_appsel.c`, the scan behind `nd_ui_scan_apps()`, `test/unit/test_appsel.c`,
`test/unit/test_appreg.c`.

**All nine golden frames this package can reach are byte-identical to the reference:**
`menu-phone-book`, `menu-panel`, `menu-messages`, `menu-games`, `menu-settings`,
`menu-calculator`, `menu-koki-mobile`, `menu-browser` and `menu-music` — **0 differing
pixels each**, confirmed three ways: SHA-256 inside `test_appsel.c`, `goldenframe.py
--compare` against frames rendered by `nd-shoot`, and a per-pixel count over the raw
RGB. All twenty-four shipped `icon.png` files decode and thumbnail to the size the
widget asks for. Nothing below blocks.

### A-1. Nine frames pin nine indices; the notch is checked at all twenty-four

The notch is `track_top + i * (99/23)` with both rectangle corners truncated. The step's
fractional part is `(7i mod 23)/23`, so `round()` and `trunc()` disagree at **eleven** of
the twenty-four indices — 2, 3, 5, 6, 9, 12, 13, 15, 16, 19 and 22 — and the golden
frames visit only three of them (3 Settings, 5 Calculator, 9 Browser). Eight indices
where the wrong rounding rule is invisible to the oracle: 2, 6, 12, 13, 15, 16, 19, 22.

`test_appreg.c` therefore renders every index of the 24-app carousel and of the 13-app
one engineering mode off produces, measures the notch's real bounding box out of the
pixels in columns 228–231, and compares against the same arithmetic done in `long double`
so that it is not the expression under test recompiled.

Two consequences of the Python's arithmetic are reproduced rather than clamped: at index
0 the notch starts at row **33**, three rows *above* the track's first row, and at the
last index it ends at row **138**, three rows *below* the track's last. Both are
visible on a real phone and neither is clipped.

### A-2. An app name wider than the panel centres to a negative x

Python's `//` floors and C's `/` truncates, and they differ for a negative numerator —
which `(screen_w - w) // 2` produces the moment a manifest carries a name wider than
240 px. Nothing shipped reaches it (the widest, "Remote Shell" at 24 px, is 138 px), but
a manifest is user-supplied data and a side-loaded app can. `nd_appsel.c` uses a
`floordiv2()` helper rather than `/` so the centring cannot shift by a pixel depending on
the sign. `test_appsel.c` pins the branch: the title is clipped at both screen edges, not
wrapped, not ellipsised and not dropped.

### A-3. Six ways a manifest is rejected, and none of them is a 999

`_scan_apps_from_dir` (main.py:652) wraps the whole per-app body in `try: … except:
pass`, so an id `int()` cannot parse **drops the entire app** — it does not fall back to
the 999 default, which only applies when the `id` key is absent. The C reproduces all
six rejections and `test_appreg.c` drives each with a synthetic manifest: a non-numeric
id, a decimal-point id (Python's `int()` refuses `"7.5"` in a string), malformed JSON, a
JSON array root, a scalar root, and a directory with no `manifest.json` at all. The
three defaults that *do* fire — `name` → the folder name, `icon` → `icon.png`, `id` →
999 — are driven too, along with `int("  12  ")`'s implicit strip and a negative id.

One place the C is stricter than the Python and no shipped manifest reaches: a **float**
id. `int(7.5)` is 7 in Python; `nd_json.h` is explicit that an integer is not a float, so
`nd_json_int()` refuses it and the app is dropped. Recorded rather than papered over,
because "it worked in Python" is how somebody will find it.

### A-4. `nd_ui_scan_apps()` caps where Python's list does not

`ND_APP_MAX` is 64 and the caller's capacity is a hard bound (CODING-STANDARDS §1.5);
Python appends without one. Twenty-four apps ship, so the cap is not within a factor of
two of anything real. The `max` argument is honoured exactly — a scan asked for three
entries returns three — and the four rejected-argument cases answer 0 without opening
anything.

### A-5. The scan CREATES a directory that is missing, and that is load-bearing

`if not os.path.exists(app_dir): os.makedirs(app_dir)` runs before the listing, so a
phone with no `/NeoDCT/System/engineering/apps` gets one on the first menu open. It is
easy to read as defensive clutter and delete. It is not: `test_appreg.c` asserts the
directory appears, under the staged root rather than at `/`.

### A-6. The icon cap is 82, and `ND_APP_SELECTOR_ICON_MAX` is not it

`ND_APP_SELECTOR_ICON_MAX` is **175** — the whole panel height — and it is the *outer*
`min()`. The number that actually reaches `get_image(max_size=…)` is
`max(24, content_bottom - icon_y - 8)` = `145 - 55 - 8` = **82**, and `icon_y` is
`30 + max(24, int(115 * 0.22))` = 55 because `0.22 * 115` is `25.299999999999997` in
IEEE754 and `int()` takes 25. Anyone reading the constant as "the icon size" will size
the cache wrong; `test_appreg.c` asserts both 55 and 82 so a wrong `icon_y` is caught
here rather than as an unexplained one-pixel shift in a frame.

The thumbnail's dimensions are also what centres the icon — `ix = (240 - img.width) //
2` — so they are checked against Pillow's rule per icon, not just bounded. Twenty-three
of the twenty-four are square and land on 82×82; **Koki's is 120×115 and lands on
82×79**, which `menu-koki-mobile` matching to the pixel confirms.

### A-7. The `exec` field still says `main.py`, and still launches nothing

Unchanged from U-6, restated here because this is the module that reads it: the scan
ports `data.get("exec", "main.py")` literally, all twenty-four shipped manifests spell
`main.py`, and nothing launches from the field — `nd_proc.h`'s `entry` is an entry-point
*name* and the code always lives in `ND_APP_SO_NAME` beside the manifest. The default
mechanism is kept so that rewriting the manifests to `"exec": "app.so"` is a change to
data and not to code; `test_appreg.c` drives a synthetic manifest that already says
`app.so` and checks it arrives verbatim. **Still owed: a decision on whether the
manifests get rewritten or the field is retired.**

### A-8. The empty carousel is a real state and has no golden frame

A failed scan gives `AppSelector` zero items, where the Python would divide by zero on
Down and index past the end on Enter. Both sources guard it before anything else: only
Enter and Clear respond and both mean "back". The `No Apps` frame draws no scrollbar at
all — `test_appsel.c` asserts column 232 is black — and `nd_appsel_draw()` additionally
resets an out-of-range `selected_index` to 0 where the Python would raise `IndexError`.
That last one is a C-only guard with no Python spelling, added because the field is
public in `nd_widgets.h` and a caller can set it.

---

## ClockService and BatteryService (`lib/nd_clock.c`, `lib/nd_battery.c`)

Recorded while porting `System/core/ClockService/__init__.py` (250 lines) and
`System/core/BatteryService/__init__.py` (302 lines). Nothing here blocks; two items
want a ruling.

### CB-1. `set_clock` no longer shells out, and that changes one return value

The Python's own comment justifies `subprocess.call(["date", "-s", ...])` — *"this runs
as a plain script on a busybox system and shelling out is what everything else here
does"*. The C cannot follow it. `nd_clock_start()` creates a thread, and CODING-STANDARDS
1.1 bans `fork()` from a threaded process unless `execve()` is the child's first
statement; even done correctly it is two processes and a busybox dependency to do what
`clock_settime()` does in one syscall. So: `clock_settime(CLOCK_REALTIME)` plus
`ioctl(RTC_SET_TIME)` on `/dev/rtc0`, falling back to `/dev/rtc`, in UTC — which is what
busybox `hwclock -w` writes anyway, there being no `/etc/adjtime` in the overlay.

**One observable difference, deliberate.** The Python returns `True` whenever `date`
*ran*, even if `date` then failed; it only returns `False` when the binary is missing
(`OSError`). The C returns `false` when `clock_settime` fails, e.g. `EPERM`. Nothing in
the tree reads the return value — `apply_floor()` discards it and `sync()` discards it —
so this is currently unobservable, but it is a difference and it is written down rather
than hidden.

### CB-2. `NEODCT_ROOT` now also gates writing the real clock

The Python test suite monkeypatches `set_clock`, with the comment *"a test that sets the
machine's clock is a test that ruins someone's day"*. C has no monkeypatching, and
`nd_clock.h` is in the frozen header set with no test hook in it, so the guard lives in
`nd_clock.c`: **when `nd_path_root()` is non-empty, `nd_clock_set()` logs the change and
then returns without calling `clock_settime()`.**

The reasoning beyond the tests: under `NEODCT_ROOT` every file this module reads —
`version.prop`, `/NeoDCT/User/.clock`, `/proc/net/route` — is a fixture, so acting on a
fixture's contents by moving the developer's real clock would be wrong whether or not a
test asked for it. In production `NEODCT_ROOT` is unset and the guard costs one
comparison against `'\0'`. The log line is emitted either way, because the log line is
the load-bearing half.

**What we need:** a nod that this is the right shape, or a test hook added to
`nd_clock.h` instead.

### CB-3. `/proc/net/route` and `/proc/net/ipv6_route` go through `nd_path_resolve()`

Neither is in `nd_paths.h`. They are resolved anyway, on the same rule as everything
else that is *opened*, which is what makes `_has_route()` testable at all — the host has
a default route, so an unresolved read would return `true` on every machine and the
"no route" branch would never be exercised. Production is unaffected.

### CB-4. The five-sample mean, and why 3.20 V shuts the phone down at all

`sum(deque) / len(deque)` in IEEE-754 double, oldest-first from a zero accumulator. This
is not stylistic: five samples of 3.20 V sum to **exactly 16.0** and divide to **exactly
3.2**, which *is* `<= ND_SHUTDOWN_V`. A running total kept across polls, a pairwise sum
or a Kahan compensation can each land a ULP away, and then the phone runs the pack flat
instead of shutting down. `test_battery.c`'s VEC_A pins it: samples 17 and 18 return
`"shutdown"`, and they only do so because the arithmetic is bit-for-bit the Python's.

### CB-5. `poll()` returns `"shutdown"` BEFORE it latches a warning

Ported as-is and worth flagging because it reads like a fall-through bug. Once
`_shutdown_count` reaches 3 the function returns immediately, so that poll never sets
`_pending_warning` — visible in VEC_A, where samples 17 and 18 report a shutdown and no
warning at all. Since `_shutdown_low_battery()` takes the screen over anyway, nothing is
lost; but a "tidied" version that latches first would change what a later
`take_pending_warning()` hands back if the shutdown is aborted (which it is, on a dev
board, when `poweroff` returns non-zero).

### CB-6. A bad `system.hw.battery_i2c_bus` also discards a good address

`_config_from_settings()` wraps both `int()` calls in one `try`, so
`system.hw.battery_i2c_bus=eleven` sends **both** bus and address back to the defaults
(3 / 0x36) even when the address parsed fine. Reproduced, and asserted in
`test_battery.c`.

### CB-7. Two branches are unreachable on the host and are therefore untested

* **The whole hardware path** — `read16`/`write16`, `debug_snapshot()`, `quickstart()`,
  the `_read_error_streak` logging (first failure only, then a "recovered after N"
  line). There is no i2c bus on the machine the tests run on, and an `I2C_SLAVE` ioctl
  against a regular file fails at the ioctl, so the register code cannot be reached even
  with a fake device node. This belongs in `tests/hw/`.
* **`_level = 3` before the first read.** The constructor calls `poll(force=True)`, and
  the simulation read never fails, so the seed value is always overwritten before anyone
  can observe it. It is still correct to keep — on hardware a failed first read leaves
  it in place, which is the case it exists for.
* **"no route after 5 minutes".** Reaching it costs 60 × 5 s of real sleeping. The
  branch is three lines and shares the `gmt_string` helper with the branch beside it,
  which is tested.

---

## ModemService: the AT engine and the state machine (WP modem)

`lib/nd_modem.c` + `nd_modem_at.c` + `nd_modem_audio.c` + `nd_modem_sim.c` +
`lib/nd_modem_priv.h`, and `test/unit/test_modem.c` — the first tests this
subsystem has ever had (433 checks). The Python is
`System/core/ModemService/__init__.py`, 1084 lines, zero coverage.

Nothing below changes a pixel: no golden frame reaches the modem, and the
27 currently-exact frames are still exact.

### M-1. The frozen `nd_modem_event` can spell four of the ten events

`nd_modem.h` freezes `ND_MODEM_EV_{RING,SMS,HANGUP,CONNECTED}`. The Python
queues ten distinct tuples: `incoming`, `connected`, `ended`, `missed`,
`sms_received`, `sms_sim`, `sms_sent`, `sms_stored_check`, `modem_lost`,
`modem_found` (lines 253, 369, 384–414, 442, 585–610, 851, 971).

The internal ring keeps all ten, with both of `deque(maxlen=8)`'s overflow
directions. `nd_modem_take_pending_event()` is the only place the mapping
happens:

| internal | public |
| --- | --- |
| `incoming` | `ND_MODEM_EV_RING`, `number` set when `+CLIP` gave one |
| `connected` | `ND_MODEM_EV_CONNECTED` |
| `ended`, `missed` | `ND_MODEM_EV_HANGUP` |
| `sms_received` | `ND_MODEM_EV_SMS`, `index` = the SIM slot |
| `sms_sim` | `ND_MODEM_EV_SMS`, `index` = **-1**, `number` = the sender |
| `sms_stored_check` | `ND_MODEM_EV_SMS`, `index` = **-2** |
| `sms_sent`, `modem_lost`, `modem_found` | **dropped** — popped and skipped |

`nd_modem_fetch_sms(m, -1, …)` returns the stashed simulated message, so the
`/tmp/neodct_sim_sms` hook works end to end through `nd_ui.c`'s existing
`handle_modem_event()` with no change to anyone else's file.
`nd_modem_fetch_sms(m, -2, …)` returns `ND_SMS_ERROR`; the sweep is
`nd_modem_read_stored_sms()`, and **the core still has to call it** on that
index — `main.py:1020` does, `nd_ui.c` does not yet.

**What we need:** either three more enum values (`…EV_SMS_SWEEP`,
`…EV_SMS_SIM`, `…EV_MODEM_LOST`) added to the tail of the frozen enum, or
confirmation that dropping `sms_sent` / `modem_lost` / `modem_found` on the
floor is acceptable. Nothing consumes them today; `modem_lost` is the one a
future status bar would want.

### M-2. The rx buffer is capped at 8 KB, and the Python's is not (R-7)

`_read_pending` (line 290) appends every 512-byte chunk to `_rxbuf` and only
ever removes COMPLETE lines. A port emitting binary with no `\n` — the
Qualcomm DIAG port is exactly that, and an `iface == None` candidate can reach
it — grows it without bound. `ND_MODEM_RXBUF_MAX` is 8192; on overflow the
buffer is dropped whole, logged once per adoption, and resynchronises at the
next newline. `test_the_rx_buffer_is_bounded` pushes 16 KB of junk through and
then a real `+CSQ:` line. This is a latent bug in the Python too.

### M-3. `struct nd_lines` is named by a frozen header and defined by none

`nd_modem.h` forward-declares it for `nd_modem_send_at()`; no public header
completes it. It is completed in `lib/nd_modem_priv.h`, so **no caller outside
`lib/` can allocate one** and the engineering Modem app cannot use `send_at()`
as declared. It needs to move into `nd_modem.h` (or `send_at` needs a
different out-parameter) before app 9005 is written.

### M-4. The modem reads `CLOCK_MONOTONIC`, not `nd_time_monotonic()`

The project convention is `nd_vclock.h`. The modem does not follow it. The
virtual clock only advances when a frame is committed, so a thread that
genuinely sleeps on a serial port would spin for ever inside `_transact`'s
deadline loop, and reading the pinned clock from two threads would be a race
for nothing. The modem draws no pixels, so no golden frame can tell. Recorded
rather than left as a silent inconsistency.

### M-5. Four bounds the Python does not have

CODING-STANDARDS §1.5 forbids anything sized by input on the stack, so:

| Thing | Cap | Python |
| --- | --- | --- |
| collected lines per transaction | 64 lines / 4096 bytes | unbounded list |
| one decoded line | 512 bytes | unbounded |
| SMS records from one `+CMGL` sweep | the caller's `max` | unbounded list |
| candidate AT ports in one probe | 32 | unbounded |

A SIM7600 holds about 30 SMS slots and enumerates five ports, so none of these
is within a factor of two of anything real. `nd_lines.truncated` records that
a reply was clipped; nothing reads it yet.

### M-6. A blocked write retries instead of dropping the modem

`_transact` does one `os.write()`. On a port asserting flow control that
raises `BlockingIOError`, which the Python catches as `OSError` and turns into
`_drop_hardware("port write failed: …")` — the modem vanishes because it was
busy for a millisecond. The C waits `TRANSACT_SLEEP_S` and retries; every
other errno still drops the port. This is a deliberate divergence and the only
one in the AT engine.

### M-7. The double `close()` in `_probe_ports` is guarded, visibly identically

Line 224 closes the local `fd` even when `_drop_hardware` already closed it
and set `self.fd = None`; Python swallows the second `OSError`. In C the
number can have been recycled by another thread, so `probe_ports()` closes it
only when `m->fd` is still that descriptor. The externally visible behaviour —
candidate skipped, probe continues — is unchanged.

### M-8. The lock file's parent is created, and a missing lock does not stop the modem

`os.open(LOCK_FILE, O_RDWR|O_CREAT)` at line 145 is unguarded: in the Python a
failure propagates out of the constructor and takes the UI with it. Two C-only
changes:

* `mkdir -p` of the lock file's directory first, because `/tmp` always exists
  in production but `<ND_ROOT>/tmp` does not, and the tests need the hook.
* A lock file that still will not open logs an error, leaves `lock_fd` at -1
  and makes `acquire()` succeed unconditionally. The modem then works and is
  simply not serialised against `S45modem`, which beats not booting.

### M-9. Every path goes through `nd_path_resolve()`, including `/sys` and `/proc`

`/tmp/neodct-modem.lock`, the four `/tmp/neodct_sim_*` hooks,
`/sys/class/tty`, `/proc/asound` and the AT device node itself. In production
`ND_ROOT` is empty and all of it is a plain copy. It is what lets
`test_modem.c` drive port discovery against a staged `bInterfaceNumber` tree
and reach a pty through `/dev/modem`. The *executables* (`aplay`, `arecord`)
are not resolved — `nd_proc.h` is explicit that an executable is not phone
data — and neither is `/dev/null`.

### M-10. `int()` accepts underscores in Python and not here

`int("1_0")` is 10 since PEP 515. `nd_modem__parse_int` rejects it. No modem
emits it; recorded because it is a real difference in a function whose whole
job is to be `int()`.

### M-11. `nd_sms_rec` carries two fields the Python record does not

The Python's record is `{index, sender, body}`. The frozen struct adds
`timestamp` and `unread`; the modem writes `0` and `true`. The `+CMGR:` header
does carry a timestamp (`"24/08/23,10:11:12+04"`) and it is currently thrown
away, exactly as the Python throws it away. Parsing it would be new
behaviour, so it is not done.

### M-12. `nd_modem_send_at()` reports "no hardware" as an error

The Python's `send_at` → `_command` returns `(None, [])` both when there is no
modem and when the port is locked. The C returns `ND_ERR_HARDWARE` for the
first and `ND_ERR_TIMEOUT` for the second, because a frozen `nd_err` return is
no use if it only ever has one failure value. `final_out` is empty in both
cases, which is what the caller actually renders.

### M-13. `nd_modem_close()` joins the thread, so it waits for the request in flight

Worst case that is the 30-second SMS ack. The alternative — cancelling a
thread that holds the flock and an open CMGS — is worse. In practice the
thread wakes every 100 ms and shutdown is immediate. The UI must not call
`nd_modem_close()` from a signal handler.

### M-14. `aplay` and `arecord` are looked up on `PATH` in-process

`subprocess.Popen(["aplay", …])` searches `PATH`; `nd_proc_spawn()` takes a
path and `execve()`s it, so the search happens before the fork (which is also
the only place it is allowed to happen — a `PATH` walk between `fork` and
`exec` would allocate). A miss is reported the way Python's
`FileNotFoundError` was: `Speaker pipe unavailable: aplay: No such file or
directory`.

### M-15. Five Python behaviours reproduced that look like bugs

All of them are pinned by a test.

1. **`dial()` logs the number after filtering but before the empty check**
   (line 771), so `dial("hello")` prints `[MODEM] Requesting Dial: ` and then
   returns False. `test_a_junk_number_logs_an_empty_dial_and_fails`.
2. **`MISSED_CALL:` has no `state != "IDLE"` guard** where `VOICE CALL: END`
   does (lines 405 vs 401), so a missed-call URC arriving with no call up
   still stops the audio and queues an event.
3. **A quote-less `+COPS: 0` sets the operator to `None`** (line 578) rather
   than leaving the last carrier on the home screen.
4. **Port discovery sorts with `strcmp`**, so `ttyUSB10` is tried before
   `ttyUSB2` and `card10` before `card2`. `test_candidate_port_ordering` and
   `test_capture_scan_is_byte_sorted` assert exactly that order.
5. **`bInterfaceNumber` is parsed as hexadecimal**, so a port on interface 16
   reads `"10"` and is neither preferred nor dropped.
   `test_hex_interface_16_is_not_interface_10`.

Two more that are correct but surprising and are also pinned:
`line.split('"')[1]` needs only ONE quote to succeed (`+CLIP: "5551234` parses),
and a mid-command URC is handled AND still appended to the collected lines,
which is the only reason `AT+CEREG?`'s own reply reaches `_parse_reg`.

### M-16. Two loops leave early when the port has already gone

`_transact`'s deadline loop and `send_sms`'s 30-second ack loop both keep
spinning in the Python after `_read_pending` has called `_drop_hardware`,
because neither checks. They return the same answer at the end of the timeout
that the C returns immediately. One user-visible consequence: a modem that
disappears mid-CMGS gives Messages `"Send failed: modem lost"` where the
Python would have said `"Send failed: timeout waiting for network"` thirty
seconds later. The C wording matches the two other drop paths in the same
function, and is what actually happened.

---

## nd-core, nd-apprun, the launcher and the stub app (WP core-loop)

Recorded while porting `launcher.py:main()`, `main.py:run()` and `launch_app()`, and
while writing `lib/nd_proc.c`, `lib/nd_crash.c`, `lib/nd_app.c`, `lib/nd_fb_adopt.c`,
`apprun/nd_apprun.c` and `apps/Stub/main.c`. Nothing here blocks. Each is a place where
the C could not be literally 1:1, or where a decision was made that ought to be seen
rather than discovered.

### X-1. The 24 shipped `manifest.json` files still say `"exec": "main.py"` — DELIBERATELY

The work package asked for them to be rewritten to `"exec": "app.so"`. They were not,
and this is the one instruction in the brief that was not carried out, so the reasoning
is spelled out in full.

`neodct/overlay/` **is the Python reference and it is the oracle.** `goldenframe.py`
drives the real Python UI to produce `neodct/tests/golden/`, and `launch_app()`
(`main.py:907`) builds its import path as `os.path.join(app["path"], app["exec"])`.
Setting `exec` to `app.so` makes every Python app launch fail, which retires the only
mechanism the project has for re-cutting a reference frame — and two frames are already
scheduled for a re-cut (`game-snake`, `game-memory`, answer 4). The brief's own "WHAT NOT
TO DO" says `neodct/overlay/` is not to be modified.

**It also changes nothing in C.** `nd_app.h` fixes the code at `ND_APP_SO_NAME` beside
the manifest; `nd-apprun` composes `<app dir>/app.so` and never reads `exec`;
`nd_ui_render_menu()` passes `entry = NULL`. `nd_app_entry.exec` is parsed and stored and
nothing dereferences it. Already recorded as U-6 and A-7 by two earlier packages, both of
which reached the same conclusion independently.

**The change, whenever the owner wants Python retired**, is one line:

```sh
sed -i 's/"exec": *"main\.py"/"exec": "app.so"/' \
    neodct/overlay/NeoDCT/System/apps/*/manifest.json \
    neodct/overlay/NeoDCT/System/engineering/apps/*/manifest.json
```

It needs no C change and no test change. It should be done in the commit that drops
Python from the defconfigs, not before.

### X-2. Twenty-four apps get the stub, not twenty-five

`SESSION-SCOPE.md` says "all 25 apps" in prose; its own table lists 13 stock + 11
engineering = 24, and the overlay ships exactly 24 app directories with 24
`manifest.json` and 24 `icon.png`. (The 25th `manifest.json` in the tree is
`System/apps/Koki/assets/manifest.json`, which is an asset pack, not an app.) The
Makefile's `STUB_STOCK_APPS` and `STUB_ENG_APPS` name all 24; `make install` puts the one
built `apps/Stub/app.so` into each. Same finding as A-5.

### X-3. `nd-shoot` runs an app IN-PROCESS with `dlopen`, not through `fork`/`execve`

A capture harness has to share an address space with the canvas it is capturing. A child
process draws into its own memory and `nd_capture` would record nothing, so `run_app` in
`tools/nd_shoot.c` resolves `<app dir>/app.so`, `dlopen`s it and calls `app_run(ui)`.

This is the faithful port of the thing it replaces: `uistub.run_app()` imports the app
with `importlib` **into the harness process** and calls `module.run(ui)`, for exactly the
same reason. It is NOT the launcher, and the difference matters, so the out-of-process
path is proved separately and harder: `test/unit/test_proc.c` forks `nd-apprun` for real,
against an app that dereferences NULL, and checks that `waitpid` reports `SIGSEGV`, that
the child's own `si_code`/`si_addr` arrived down the crash pipe, that `crash.log` gained
a report, that the crash screen was drawn, and that the core then goes on to launch
another app.

### X-4. Only `app-clock` of the thirteen stock-app frames is rendered

`spec-build-test.md` section 3.6 records `app-clock` as byte-identical to
`widget-messagedialog`, because the Clock app *is* a "This application has not been
implemented yet." dialog. So the one shipped `app.so` draws that frame for real, and it
comes out byte-exact. The other twelve draw their own screens and the stub cannot stand
in for any of them; they remain in `nd-shoot`'s `SKIPPED[]` with their reasons.

`app-messages` ≡ `widget-pagedlist` and `app-phonebook` ≡ `widget-verticallist` are the
other two duplicate digests in section 3.6, and both are still skipped: those apps draw
a real list, and the fact that the pixels happen to match a widget-gallery frame is not
permission to claim the app rendered them.

### X-5. The app child inherits `NEODCT_ROOT`, which `nd_app.h` does not list

`nd_app.h` names three inherited things: `NEODCT_KEYPAD_FD`, `NEODCT_CRASH_FD`,
`NEODCT_FB_FD`. A fourth is required and `nd_proc_launch_app()` sets it: the child is a
fresh process that reads `NEODCT_ROOT` for itself, and the core's root may have been set
with `nd_path_set_root()` rather than through the environment (which is exactly what
every host test and `nd-shoot` do). Without it the child looks for `app.so` at the
unprefixed `/NeoDCT/System/apps/...`, finds nothing, and exits 1 — which is what it did
until the second run of `test_proc`. In production `ND_ROOT` is empty and the variable is
not set at all.

Any inherited copy of all four is stripped from the child's environment first, so a stale
descriptor number from a previous launch can never be picked up.

### X-6. The three descriptors keep their own numbers; the fd map only clears `FD_CLOEXEC`

`nd_proc_spec.fds` looks like a plan to `dup2` onto fixed slots. It is not used that way:
`nd_app.h` says "the numbers themselves are not fixed" and the child is *told* what they
are, so each descriptor is listed as a map onto itself. That branch exists because
`dup2(fd, fd)` is a documented no-op and does **not** clear the close-on-exec flag, so a
descriptor opened with `pipe2(O_CLOEXEC)` would vanish at the exec. The `fcntl(F_SETFD, 0)`
in that branch is the whole reason it is there.

### X-7. `nd_fb_adopt_fd()` and `nd_app_fb_from_env()` are additions to `nd_app.h`

`nd_fb.h` can open exactly one thing: `/dev/fb0`. The point of `NEODCT_FB_FD` is that an
app process needs no permission on that device (`SECURITY.md`), so the descriptor has to
be turned back into an `nd_fb` without a second `open()`. The implementation is a new
file, `lib/nd_fb_adopt.c`, so `nd_fb.c` — another work package's — was not touched; the
two declarations are additive and live in `nd_app.h` because they exist solely for the
process boundary that header describes.

**The adopted mapping is NOT re-zeroed.** `nd_fb.h` explains that the device path zeroes
once at open so later partial-band writes leave the letterbox rows black; that already
happened, in the core. Zeroing again would blank the panel between the app starting and
its first frame — a visible flash the Python never had.

### X-8. The crash report is a fixed binary record, not a backtrace

`nd_crash.h` says the child writes "the signal, si_code, faulting address and a
backtrace". There is no backtrace. `backtrace()` lives in `execinfo.h`, which is a glibc
extension; `MUSL.md` makes musl the target libc and musl does not ship it, and there is
no async-signal-safe substitute worth the risk inside a handler that is already running
on a corrupted stack.

So the handler writes one `write(2)` of a POD struct — magic, `si_signo`, `si_code`,
`si_addr` and the entry-point name — which is smaller than `PIPE_BUF` and therefore
atomic, and the CORE formats the human line from it. That keeps every `snprintf` on the
side of the boundary that is allowed to call one. This is the concrete shape of the
"honest limitation" `nd_crash.h` opens with.

### X-9. The fatal handler must unblock the signal before re-raising

Worth writing down because the symptom is silent and wrong rather than loud. `sigaction`
blocks the delivered signal for the duration of its own handler, so `raise(signo)` after
`SA_RESETHAND` merely marks it pending: the handler returns, `_exit(128 + signo)` runs,
and the core sees `WIFEXITED` with status 139 instead of `WIFSIGNALED` with `SIGSEGV`.
Both are classified as a crash, so the screen looked right and the *reason* was wrong.
`sigprocmask(SIG_UNBLOCK, ...)` immediately before the raise fixes it and is
async-signal-safe.

### X-10. `SIGTERM` and `SIGKILL` are not crashes

They are how the core reclaims the screen for an incoming call (`nd_app.h`'s teardown
contract, and `nd_proc_terminate()`'s escalation). `nd_proc_launch_app()` logs
"was stopped by the core, not by a fault" and shows no crash screen. This is the direct
descendant of the Python's `except (KeyboardInterrupt, IncomingCall): raise` — a ringing
phone was never an app crash there either.

### X-11. Four additive declarations in `nd_crash.h`

`nd_crash_draw_engineering()`, `nd_crash_summary()`, `nd_crash_signal_name()` and
`nd_crash_set_entry()`. The first is not optional: `spec-build-test.md` section 3.6's
`crash-screen` recipe calls `CrashHandler._draw_engineering_crash_screen()` **directly**,
without the wait, so the C has to expose the same split or the frame cannot be
reproduced. Nothing above them in the header changed.

### X-12. The core cannot resume from a fault in its own code

The Python loop wraps its whole body in `except BaseException:`, logs, sleeps 0.1 s and
carries on. Almost everything that ever caught was an app crashing, because an app was
`exec_module`'d straight into the core process — and that failure is the one this design
removes rather than papers over. What is genuinely gone is resuming the core itself: C
has no consistent state to resume into after `SIGSEGV`. `nd-core` installs a handler that
writes one async-signal-safe line to stderr and re-raises, so the death is visible on the
serial console instead of silent.

`nd_ui_render_menu()` and `nd_proc_launch_app()` already contain their own equivalents of
the Python's inner `try/except` blocks, so the loop body has nothing left to guard.

### X-13. While an app child runs, the core pumps keys but does not tick the services

In Python, `_battery_tick`, `_modem_tick` and `_ring_tick` lived inside
`read_keypress()`, and an app calling `ui.read_keypress()` was what kept them running.
An app process cannot call the core's `read_keypress`, so `nd_proc_launch_app()`'s pump
loop forwards press and release records onto the channel and does nothing else.

That is the correct destination — OPEN-QUESTIONS answer 1 moves the modem to its own
thread in the core precisely so a call interrupts an app that never polls anything — but
**the thread is not written yet**, so today an incoming call does not interrupt a running
app. The core's half of the sequence is already here and tested: `nd_proc_terminate()`
sends `SIGTERM`, waits out the grace period and escalates to `SIGKILL`. The services work
package has to call it from the modem thread.

### X-14. `nd_proc_launch_app()` ignores `SIGPIPE` for the duration of the pump

The child can exit at any instant and the next forwarded key then hits a pipe with no
reader. `nd-core` ignores `SIGPIPE` globally, but a library must not depend on its caller
having done so — the first version of `test_proc.c` was killed by signal 13 partway
through the suite. The disposition is saved and restored around the loop.

### X-15. The reaper keeps what it reaps

`nd_proc.h` says the reaper "skips pids that are being waited on explicitly". A signal
handler cannot know which pid somebody is about to wait on without a lock, and taking a
lock in a handler is the other classic way to deadlock a threaded process. So the handler
reaps everything into a 16-slot ring of `(pid, status)` and `nd_proc_wait()` checks the
ring before it calls `waitpid()`. Same observable behaviour, no lock, and it closes the
race where the reaper collects the app child a microsecond before the launcher asks about
it and `waitpid` answers `ECHILD` with no status. Pinned by
`test_reaper_keeps_the_status`.

A full ring drops the oldest entry: a status nobody has asked for in sixteen deaths is a
status nobody is going to ask for.

### X-16. `nd_path_join()` RESOLVES — do not resolve its output

`nd_path_join(out, sz, dir, child)` returns an **ND_ROOT-resolved** path, not a joined
virtual one. Calling `nd_path_resolve()` on its result prefixes the root twice, which
under an empty root is invisible and under a test root produces
`/tmp/x/tmp/x/NeoDCT/...`. It cost two debugging cycles here (once in `nd-apprun`, once
in `test_proc.c`'s staging) and it is not stated in `nd_paths.h`. Build a virtual path
with `nd_snprintf("%s/%s", ...)` and resolve once, or use `nd_path_join` and resolve
never.

### X-17. `nd-shoot` stages `System` as a directory of symlinks, with `apps/` expanded

It used to be one symlink onto `neodct/overlay/NeoDCT/System`. An app directory now has
to gain an `app.so`, and nothing may be written into the overlay, so `System` is a real
directory whose entries are all symlinks except `apps/`, which is expanded into real
per-app directories of symlinks plus one symlink to the built `apps/Stub/app.so`.

Every file the renderer opens is therefore the same file it opened before. Verified, not
assumed: the 25 frames that were byte-exact before the change are byte-exact after it,
and `test_shoot.c` reports 27 of 27.

`engineering/apps` is deliberately left as a plain symlink — nothing launches an
engineering app, and the menu only needs each one's `manifest.json` and `icon.png`.

### X-18. The stub app does not reproduce Clock's `while True:`

`System/apps/Clock/main.py` wraps `warningmsg.show()` in a loop and returns on key 46,
28 or 50. `MessageDialog`'s own key set already covers those, so the loop would spin only
on a key the dialog itself ignored — which the C dialog does not return. Same pixels,
same exit conditions, one less loop. The Clock app also clears rows 0..145 and flushes
once before the dialog; that frame is not `frames[-1]` and no reference image contains
it.

### X-19. Three new `nd-core` flags

`--headless` (no `/dev/fb0`), `--no-splash`, and `--idle-measure` (boot fully, print one
readiness line, then hold still). The acceptance gate's check 8 invokes
`nd-core --headless --idle-measure` and waits for a line containing "idle", so the last
two are required by the gate rather than optional.

### X-20. Two boot steps are still somebody else's

* `ClockService` and `RemoteShell` are `spec-core-services.md`'s. `nd_main.c` references
  `nd_clock_start` and `nd_rs_start_if_enabled` **weakly**, the pattern `nd_ui.c`
  established for the modem, the battery and the notify service: when the module lands
  the reference resolves and the branch starts being taken with no edit here. Both calls
  sit inside a "boot continues" guard in the Python, so "not linked" is a case the
  Python already had a message for. `lib/nd_clock.c` landed from the services package
  mid-session and now resolves; `nd_rs_start_if_enabled` still logs
  `[RSHELL] remote shell unavailable: not linked in this build`.
  The server list `ND_NTP_SERVERS` needs the same treatment, since it is data the same
  module owns.
* `run()`'s step 1, `System.hw.i2c_keypad_setup.maybe_run_first_time_setup(fb)`, is not
  called. It belongs to `System/hw`, it may `execv` the whole UI, and there is nothing to
  call yet. The line it goes on is marked in `core_run()`.

### X-21. The boot splash falls back to the UI face when DejaVu is missing

`launcher.py` loads `/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf` at 20 and
`DejaVuSans.ttf` at 14 and falls back to `ImageFont.load_default()`, a bundled bitmap
face C has no equivalent of. The C falls back to `font.ttf` at 20 and logs that it did.
Same gap as U-2, and the splash is not a golden frame. Both DejaVu files are present on
the host this was measured on, so the fallback is dormant here.

### X-22. Idle RSS

`nd-core --headless --idle-measure`, measured from `/proc/<pid>/smaps_rollup` the same
way the Python baseline was:

| build | RSS | PSS | what it had loaded |
| --- | --- | --- | --- |
| Python, `uistub.StubUI` + `ui.update()` | 23,900 kB | 20,644 kB | fonts, wallpaper, 24 apps, sqlite — plus the capture harness |
| `nd-core --headless --idle-measure`, staged root with a wallpaper | **6,516 kB** | **4,814 kB** | fonts, wallpaper, 24 apps, sqlite, the modem and clock threads |
| the same, staged root, no wallpaper configured | 5,772 kB | 4,082 kB | one 240x175 RGB surface fewer |
| the same, empty root — what the gate measures | 5,288 kB | 3,634 kB | no fonts, no wallpaper, no apps |

Both sides are `/proc/<pid>/smaps_rollup`, read the same way, on the same host, in the
same session. `verify-c-build.sh` check 8 reports 5,592 kB and PASSes its 9 MB target.

**3.7x smaller than the Python by RSS, 4.3x by PSS.** The second row is the honest one:
it is the phone as it actually boots. Two caveats a reader is owed. The Python row is
measured through `uistub`, which is the only way to run that UI without a framebuffer, so
it carries the harness's own weight — it is slightly unfair in the C's favour. And the
gate runs `nd-core` with no `NEODCT_ROOT`, so its number is the last row: the flattering
one. Say which root a figure had.

### X-23. Not yet done in this package, and knowingly

* Nothing calls `nd_proc_launch_app()` with `ND_APP_ENTRY_OPEN_MESSAGE` outside the
  tests. `nd_ui.c`'s notification path already passes it; Messages has no `app.so` of its
  own yet, so it reaches the stub, which exports no `app_open_message` and is correctly
  reported as "App has no app_open_message(ui)".
* `nd_proc_spec.owner` is recorded and not yet acted on. The four owner classes only
  start to differ once the modem's audio bridge and the tone player exist.
* `dlclose()` is not called on the way out of `nd-apprun`. An app's static destructors
  would then run after its own `app_shutdown()`, and on the `SIGTERM` path the phone is
  already ringing. The process is one `_exit()` from returning every page anyway.

### X-24. A shutdown signal sets a flag AND arms a two-second alarm

Found while running the acceptance gate: `nd-core` ignored `SIGTERM` completely and had
to be `SIGKILL`ed. The flag `on_quit()` sets is only read by the two loops in
`core_run()`, and the core was sitting in construction step 13's blocking modal, which
looks at nothing.

In Python a `SIGINT` raises `KeyboardInterrupt`, which unwinds from wherever the process
is -- including out of a modal's key wait -- and `run()` re-raises it, so the UI process
always dies. C has no unwinding. So the handler now does both: it asks for a graceful
exit, and it arms `alarm(2)`, whose handler writes one async-signal-safe line and
`_exit(0)`s. A clean shutdown finishes long before the alarm; a wedged one still goes,
and an init system never has to `SIGKILL` the phone.

### X-25. `--idle-measure` acknowledges the first-boot notice before it measures

Same root cause, different consequence. On a phone that has never been booted,
`nd_ui_init()` step 13 draws a modal and waits for a key; with no keypad attached
nothing ever dismisses it, which is faithful and is also the end of any unattended
measurement. The gate would have measured a half-built core and then hung on `wait`.

`--idle-measure` is explicitly a measurement mode with no user, so it writes
`/NeoDCT/User/.ack_security_warning` first -- into whatever `ND_ROOT` is in force, so a
staged root stays inside itself -- and logs that it did. Every byte the notice would
account for is freed the moment it is dismissed, so the number is the same either way;
the state being measured is a phone that has been booted before, which is the state
worth measuring. No other mode touches that file.


---

## NotifyService and the ringer (`lib/nd_notify.c`)

Recorded while porting `System/core/NotifyService/__init__.py` (232 lines). The banner
state machine is a literal transcription and has nothing to declare. The ringer does:
it is the one place in this package where the C deliberately does not do what the
Python does, and the reason is R-9.

### N-1. The ringtone STREAMS. This is R-9, and it is a deviation on purpose

`miniaudio.decode_file(path, SIGNED16, nchannels=2, sample_rate=44100)` materialises the
whole tone before the first note. Measured against the sixteen shipped tones with
`dr_mp3` (frame counts read out of the files, not estimated):

| Tone | frames @ 48 kHz | duration | decoded to 44.1 kHz stereo int16 |
| --- | --- | --- | --- |
| `Low.mp3` (the default) | 163,840 | 3.41 s | 602 kB |
| `Nokia Tune.mp3` (44.1 kHz) | 208,896 | 4.74 s | 836 kB |
| `Ring Ring.mp3` | 339,968 | 7.08 s | 1.25 MB |
| `Valkyrie.mp3` | 1,511,424 | 31.5 s | 5.55 MB |
| `Brave Scotland.mp3` | 1,728,512 | 36.0 s | 6.35 MB |
| `Tchaikovsky.mp3` | 1,740,800 | 36.3 s | **6.40 MB** |

Those numbers agree with the spec's table to within a rounding of the duration. A user
who picks Tchaikovsky adds 6.4 MB to the **core** process the moment the phone rings and
holds it until they answer — on a build whose whole idle RSS is 5.5 MB.

So `lib/nd_notify.c` decodes as it plays. `nd_tone_src` (`lib/nd_notify_priv.h`) pulls
4096 source frames at a time out of `dr_mp3`/`dr_wav`, converts to 44100 Hz stereo
int16, and seeks back to frame 0 at EOF. Peak cost is the decoder object plus two
buffers: **~100 kB for any tone**, against 6.4 MB for the worst one. The 64 kB figure in
the brief is the output ring — `ND_RING_CHUNK_FRAMES` is 16384 stereo frames, exactly
65536 bytes, which is also one `send()` and 372 ms of audio.

**The loop stays sample-exact**, which is the property `_loop_generator` was written to
have: the frame after the file's last frame is frame 0, with no gap, no fade and no
silence. `test_notify.c` checks that two ways — directly, four times round a 97-frame
fixture read in awkward pieces, and end to end, 16384 frames captured out of the player
and compared frame by frame against a 997-frame source.

The spec's suggested mitigation of *"refuse to ring from a file over N MB and fall back
to Low.mp3"* is **not implemented and is not needed**: streaming makes the tone's length
irrelevant to memory. A cap would only introduce a size at which a user's own ringtone
mysteriously stops working.

### N-2. The samples go to `aplay`, not to a linked ALSA

The Python opens a `miniaudio.PlaybackDevice`. The C writes the same S16_LE / 2 channels
/ 44100 Hz stream to `aplay -q -t raw -f S16_LE -c 2 -r 44100 --buffer-time=500000`,
which is the 500 ms `RING_BUF_MS` expressed the way aplay spells it. Reasons, in order
of weight:

* **`aplay` is already a hard dependency.** Both defconfigs set
  `BR2_PACKAGE_ALSA_UTILS_APLAY=y`, `play_tone()` has always spawned it, and
  `nd_modem_audio.c` spawns it for call audio. Nothing new ships.
* **The Python's stated reason for avoiding mpv is honoured.** The docstring says
  miniaudio exists so a ringtone does not cost a *"~24MB mpv process"*. `aplay` streaming
  raw PCM is a few hundred kB. Avoiding *mpv* was the point; linking an audio library
  was the means.
* **`libneodct.so` then links no audio library at all.** The host that renders the golden
  frames has no `libasound` headers and no `pkg-config alsa`, so a linked backend could
  not be built there, let alone tested. `make` does not gain a dependency.

**A socketpair, not a pipe, and `send(MSG_NOSIGNAL)`, not `write()`.** When the ringer
stops, the player dies while the feeder thread is mid-write. On a pipe that is SIGPIPE,
and the only way to suppress SIGPIPE is process-wide — which a library has no business
doing to `nd-core`, whose reaper and shutdown handler live on the main thread. On a
socket it is a plain `EPIPE` on one thread and nothing else notices.

**mpv remains the fallback, reached by the same condition.** `dr_mp3`/`dr_wav` cannot
read `.wma`, `.flac` or `.ogg` — and neither can miniaudio, so a `.wma` tone already
falls through to mpv today and still does.

### N-3. Two vendored single-header decoders, and one suspended warning set

`lib/vendor/dr_mp3.h` (5426 lines) and `lib/vendor/dr_wav.h` (9193 lines), from
`mackron/dr_libs`, unmodified, public domain or MIT-0. They are `#include`d into
`nd_notify.c` between `#pragma GCC diagnostic push/pop` with `-Wconversion` and friends
turned off **for those two includes only**; everything this project writes still compiles
under the full set. Editing a vendored DSP header to silence warnings about arithmetic
that is correct is how a port acquires a bug that cannot be reported upstream.

Cost, measured by relinking `libneodct.so` with and without `nd_notify.c`:
**331,960 -> 455,448 bytes stripped** (+123.5 kB), 1,355,144 -> 1,902,592 unstripped.
That +123 kB is the whole MP3 and WAV decoding capability, and it does not move idle
RSS -- the gate still reports 5,596 kB, against a 9,216 kB ceiling -- because none of
those pages are faulted in until the phone rings.

### N-4. One Python branch has no C equivalent

`ringtone_path()` wraps the settings read in `try/except` and prints
`[NOTIFY] Ringtone setting unreadable ({exc}).` if `SettingsStorage` raises.
`nd_settings_get_copy()` cannot raise; it returns the default. The branch is therefore
unreachable in C and is not reproduced. Every other line of that function is, including
the `{configured!r}` repr quoting in `Ringtone missing: '...'` and the deliberate silence
of the last-resort directory sweep.

Related, and reproduced rather than corrected: the extension-retry list is
`.mp3 .wav .wma .flac .ogg` but the last-resort sweep only accepts `.mp3 .wav .wma`. A
tones directory holding nothing but `.ogg` files therefore ends in `None` and
`[NOTIFY] No ringtone available; ringing silently.`, even though the retry list would
have accepted an `.ogg`. That asymmetry is the Python's.

### N-5. One log line says what the C did instead of what the Python said

`[NOTIFY] miniaudio ring failed ({exc}); trying mpv.` became
`[NOTIFY] Streaming ring failed (not MP3 or WAV); trying mpv.` — same tag, same colour,
same position in the chain, same next step, and it names the thing that actually failed.
This is the licence in SESSION-SCOPE.md's logging rule: the mechanism must match, the
message may differ where the C genuinely does something different. Every other
`[NOTIFY]` string in the module is byte-for-byte the Python's.

### N-6. `stop_ring()` can still print twice, on purpose

The Python checks both handles and prints from each, so a state where the miniaudio
device *and* the mpv process were somehow both live prints `[NOTIFY] Ringer stopped.`
twice. The C keeps that shape (`n->ring` and `n->mpv_pid` are separate). It should never
happen; if it ever does, seeing it twice on the serial console is the point.

### N-7. The resampler is linear, and it is exact at 44100

Thirteen of the sixteen shipped tones are 48 kHz mono; `Drip Groove.mp3` and
`Nokia Tune.mp3` are 44.1 kHz mono; `sms.wav` is 22.05 kHz mono. miniaudio's default
converter is a linear resampler, so this is one too — with an integer accumulator
(`frac += rate; while (frac >= 44100) frac -= 44100`) rather than a float one, because a
float cursor over a tone looped for a minute drifts and an integer one cannot.

At 44100 Hz in, the fraction is zero on every output frame and the path degenerates to a
bit-exact copy with mono duplicated across both channels. That is checked, not assumed
(`test_a_44100_mono_source_comes_out_bit_for_bit_in_both_channels`).

A file with more than two channels keeps the first two rather than downmixing. No
shipped tone has more than one; if a user ever puts a 5.1 WAV in `/NeoDCT/System/tones`
they get the front pair.

### N-8. Not covered by the host tests, and knowingly

* **ALSA itself.** Whether the phone's card accepts S16_LE stereo at 44100 through
  `aplay -t raw`, and whether `--buffer-time=500000` is honoured, is a `tests/hw`
  question. The host has no sound card, no `aplay` and no `mpv`; the tests substitute
  shell scripts on `PATH`, which is indistinguishable from the real thing as far as this
  module can tell, and proves everything up to the ALSA boundary.
* **`make TSAN=1`.** The feeder's stop flag is a `volatile sig_atomic_t`, matching the
  pattern `nd_proc.c` already uses. TSan would call the flag a race even though the
  thread's real wake-up is the `shutdown()`-induced `EPIPE`. The gate does not run TSan;
  noting it so nobody is surprised.

---

## CubeBench (`apps/CubeBench`, engineering app 9998)

### CB-1. `eng-cubebench` came out BYTE-EXACT, not merely inside its tolerance

The frame tolerance policy above puts `eng-cubebench` in the `tolerance` class, budgeting
"single-digit pixels along the wireframe edges" for a one-ULP `sin`/`cos` disagreement
between libcs. **On this host the delta is zero.** `test_cubebench.c` drives the built
`app.so` through `nd_capture` with a 60-frame budget and the virtual clock, and the
SHA-256 of frame 60 equals the manifest's
`4dc887d233374e143e9f8cfffdfeef857ba7ed97b8ac8500d055b15e1c04c6b9`.

That is expected rather than lucky: CPython's `math.sin`/`math.cos` are the platform
libm's, so the Python capture and the C port ran the same glibc code on the same
`double`s. The tolerance still has to stay, because it exists for **musl on the device**,
which is a different libm and has not been measured yet.

The test therefore reports the real number every run rather than only asserting the cap:
a byte-exact frame prints `BYTE-EXACT (0 differing pixels)`, and anything else prints the
count, the percentage and the bounding box before checking it against
`ND_CUBEBENCH_PIXEL_CAP` (9). A port regression that happened to stay under 9 pixels
cannot hide.

**Question for the owner:** once a musl build has run on hardware, should `eng-cubebench`
be promoted from `tolerance` to `exact` if it is still zero there? It would close the one
hole in the "any pixel change fails the build" guarantee.

### CB-2. Sixty frames, and where that number comes from

Nothing in `main.py` limits the run: `while True:` with `read_keypress(0)`. The reference
frame is bounded by the *harness*, not the app — `uistub.StubUI`'s default
`idle_budget=60` makes the 61st idle poll raise `ScriptExhausted`, so 60 frames reach the
framebuffer and `frames[-1]` is the sixtieth. `shoot_docs.py`'s `frame_budget=240` never
bites.

The C reproduces the same *end state* through the frame budget instead of the key budget:
`nd_fb_update()` returns `ND_ERR_BUSY` on the 61st commit and `app_run()` returns. The
61st frame is composed onto the canvas and then discarded, exactly as the Python composes
nothing and unwinds — either way the ring holds frame 60 and the virtual clock has ticked
60 times. Verified in the test.

### CB-3. `fps_inst` is computed and never displayed

`main.py` assigns `fps_inst = 1.0 / dt` on every frame and only ever draws `fps_display`.
Ported as-is (`nd_cubebench_fps.inst`) rather than deleted: it costs one divide, it is
what a reader of the Python expects to find, and it is the obvious hook for anyone who
later wants an instantaneous readout.

### CB-4. The FPS window restarts at `now`, so only the first window holds six frames

`fps_window_start = now` rather than `+= 0.5`. With the virtual clock's 0.1 s tick the
first window closes on frame 6 (6 frames / 0.5 s = **12.0**) and every window after it
holds five (5 / 0.5 = **10.0**). So the reference frame reads `FPS 10.0`, and frames 1-5
read `FPS 0.0`. This looks like an off-by-one and is not: a long frame shortens the next
window instead of accumulating debt, which is the behaviour you want from a benchmark.

### CB-5. `min(w, content_bottom)` feeds BOTH `size` and `fov`, with different factors

`size = min * 0.22` and `fov = min * 1.1` are separate constants over the same base, and
`view_dist = size * 5.5` chains off the first. At 240x145 that is 31.9, 159.5 and 175.45.
They were tuned by eye; 0.22 and 1.1 are not a hidden single factor and must not be
folded together. Pinned in `test_cubebench.c`.

### CB-6. The perspective divide clamps at 0.1 rather than culling

`if denom < 0.1: denom = 0.1`. A vertex that rotates behind the camera does not get
dropped — it gets `scale = fov * 10` and lands far off-screen, where the rasteriser clips
it. With `view_dist = 5.5 * size` the cube never actually reaches that state, so the
branch is dead on this panel. Ported anyway, and tested, because it is the only thing
standing between a future geometry change and a division by zero.

### CB-7. The glyph cache was already in `lib/nd_font.c`; measured, it is worth 2.4x here

`PERFORMANCE.md` predicted that a naive C port would inherit Pillow's 75%-of-the-frame
text cost and asked for a glyph cache. `lib/nd_font.c` already had one — all 95 printable
ASCII characters rasterised into one arena at `nd_font_load()`, under 80 KB for all four
sizes — so nothing needed writing. It was measured rather than assumed, by temporarily
bypassing the cache and re-running:

| | cached | re-rasterised per call |
| --- | --- | --- |
| `draw.text("FPS 60.0")` | 0.0022 ms | 0.0166 ms |
| whole CubeBench frame | 0.0538 ms | 0.1307 ms |

So the cache is worth **7.6x on the text call** and **2.4x on the frame**. The prediction
was right about the mechanism.

### CB-8. `nd_draw_rect_fill` is now the most expensive thing in the frame

With text cached, clearing the 240x146 content rectangle costs **0.0305 ms**, which is
57% of the 0.0538 ms frame — and it is *slower* than Pillow's 0.0189 ms for the same
rectangle, because Pillow fills a solid run per row and `nd_draw_rect_fill()` writes pixel
by pixel. Not touched here: `lib/nd_draw.c` belongs to another work package and a
row-at-a-time fill is a change to the most pixel-critical function in the project. Raising
it because it is the next lever, and because "the C is slower than Pillow at this one
thing" is exactly the sort of fact that should not stay in a scratch buffer.

---

## Browser (`apps/Browser`, app 11)

Ported for real, from `System/apps/Browser/main.py` (261 lines). NetSurf itself is
untouched. Everything below is a decision taken while porting the launcher; the first
three are the ones worth an answer.

### BR-1. `_CpuSampler` indexes the whitespace split, and its own comment says not to

`main.py` reads `/proc/<pid>/stat`, and the comment above the read says:

> utime and stime are fields 14 and 15, after the comm field which may itself contain
> spaces -- index from the closing ")".

The code then does `parts = handle.read().split()` and `int(parts[13]) + int(parts[14])`,
which is exactly the thing the comment warns against. It works because the only process
this sampler is ever pointed at is `netsurf-fb`, whose `comm` has no space in it. The
comment describes a fix that was never applied.

**Ported as-is**, per CODING-STANDARDS.md section 9.4 and spec-apps-core.md's explicit
"Bug preserved on purpose". `read_busy_ticks()` splits on whitespace and takes fields 13
and 14. Pinned in `test_browser.c` with a `(netsurf-fb)` stat fixture. If the sampler is
ever pointed at something else, it must be changed to scan forward from the last `)` —
and that is a behaviour change, so it needs saying out loud first.

### BR-2. The key bridge has no thread and no i2c, because an app process has neither

This is the one structural difference between the Python launcher and the C one, and it
is forced by a decision that was already settled.

In Python, `Browser.run()` ran **inside the core**, so `_start_key_bridge(ui)` could hand
`ui.matrix_input` — the live i2c scanner — to `T9BrowserBridge`, which then owned a thread
that scanned the expander and typed into uinput. Neither half of that is available now:

* apps are separate processes and **no app touches the i2c bus** (the settled
  cross-process decision, restated at the top of `nd_keypad.h`). The core reads the keypad
  and forwards presses *and* releases down the inherited channel;
* this process `fork()`s, and CODING-STANDARDS.md 1.1 is about exactly what a thread does
  to a `fork()`. Adding one here to read a pipe would be the worst possible place for it.

So the launcher builds the bridge object with **`nd_t9_bridge_new_for_test()`** — the
constructor `nd_keypad.h` documents as "the same object with no input source and no
thread" — and drives `nd_t9_bridge_handle_code()` from its own `poll()` loop, on the same
descriptor set as the stderr pump. Every keycode decision (2/4/6/8 as the d-pad, 5 as
follow-link, `#` cycling through cursor mode) is still `lib/nd_t9_bridge.c`'s and is
unchanged.

**The question is only about the NAME.** `_for_test` in shipping code reads as a mistake.
`lib/nd_t9_bridge.c` is another work package's file so it was not touched; the fix, when
someone owns it, is a two-line alias — `nd_t9_bridge_new_manual()` /
`nd_t9_bridge_free_manual()` — with the existing spellings kept for the tests.

### BR-3. `ui->has_matrix_keypad` is always false inside an app, so the bridge gate moved

`_start_key_bridge` returns `None` when `ui.matrix_input is None`, i.e. on QEMU and dev
boards where a real keyboard already reaches netsurf. Bridging one there would double
every press, because netsurf would see both the real device and the virtual one.

The C cannot ask the same question the same way. `nd_ui_init_app()` sets
`ui->has_matrix_keypad = nd_input_has_matrix(ui->input)`, and `ui->input` in an app is a
**pipe**, which has no matrix by construction — so the flag is false in every app process
on every device. (That also means the T9 mode indicator is never drawn inside an app,
which is a separate bug in `lib/nd_ui.c` / `lib/nd_input.c` and is raised here because
this is where it was found. Not fixed: those are not this work package's files.)

`nd_browser_needs_key_bridge()` therefore trusts `ui->has_matrix_keypad` when it is true
and otherwise asks the same file the core asks: `nd_keymap_load(ND_PATH_KEYMAP, ...)`
succeeding means keypad-only hardware. That is the same evidence, read from the same
place, one process further out. It is correct today and it becomes redundant the moment
the core propagates the flag — at which point the second half can be deleted.

### BR-4. The pipe is now drained even when `/dev/console` will not open

`main.py`:

```python
proc = Popen([...], stderr=subprocess.PIPE)
if stderr is not subprocess.DEVNULL:
    _pump_browser_log(proc, stderr)
proc.wait()
```

`stderr` is `DEVNULL` when `open("/dev/console")` failed. In that case nothing ever reads
`proc.stderr`, and the pipe is the one the comment four lines above warns about:

> the pipe has to be drained while netsurf is alive or it fills and blocks it.

So on a phone with no console, the Python's browser wedges after roughly 64 KB of output
and there is no diagnostic anywhere, because the diagnostic is what filled the pipe.

**Deliberate deviation, and the task asked for it.** `nd_browser_pump()` treats
`console_fd < 0` as "read and discard": the pipe is drained to EOF regardless, and nothing
is written. Observable behaviour with a working console is identical. Covered by
`t_run_without_a_console`, which pushes 3000 lines through a case root that has no
`/dev/console` at all.

### BR-5. `errors="replace"` is reproduced; NUL is the one byte that cannot be

Python decodes each stderr line `utf-8` with `errors="replace"` and re-encodes it.
`sanitise_utf8()` implements the same WHATWG "maximal subpart" algorithm CPython's decoder
uses, so an ill-formed sequence becomes ONE U+FFFD covering its longest well-formed
prefix, not one per byte.

The single divergence is NUL. Python decodes `b"\x00"` to `U+0000` and writes it straight
back out; a C string cannot carry it, so it becomes U+FFFD here. netsurf does not emit NUL
on stderr, so this is a difference in a case that does not arise — recorded rather than
hidden.

### BR-6. `subprocess.run(["dmesg"])` is a PATH lookup; `execve` is not

`_dump_dmesg_tail` relies on the shell-style PATH search `subprocess` does for a bare
name. `execve` takes a path, and CODING-STANDARDS.md 1.1 rules out doing the search
between the fork and the exec. `ND_BROWSER_DMESG` is therefore `/bin/dmesg`, which is
where both busybox and util-linux install it, and where the target image has it. `argv[0]`
is still the bare `"dmesg"` the Python passes. If an image ever puts it somewhere else the
tail is simply not dumped, which is the same outcome the Python's bare `except` produces.

### BR-7. Two fixed buffers replace two unbounded Python reads

`readline()` on netsurf's stderr and `splitlines()` on dmesg's output are both unbounded.
On a 53 MB phone the launcher allocates neither per line, so both are capped at
`ND_BROWSER_LINE_MAX` (1024 bytes, matching `ND_LOG_LINE_MAX`):

* a stderr line longer than the cap is emitted as its own tagged line and the remainder
  becomes the next one — no bytes are lost, the split moves;
* a dmesg line longer than the cap is truncated, because it is being echoed verbatim and
  a half-line with a continuation would read worse than a short one.

netsurf's longest real line is a certificate complaint carrying a URL and is well inside
1024. Raised because it is a cap that did not exist before, not because it is expected to
bite.

### BR-8. `HOME` is passed unresolved, unlike every path the launcher opens

`env.setdefault("HOME", "/NeoDCT/User")` hands a string to another program rather than
opening it, so it is passed literally and does NOT go through `nd_path_resolve()`. Every
path this module *opens* — the browser binary, `/dev/console`, `/dev/null`, `/bin/dmesg`,
`/proc/<pid>/stat`, `keymap.json` — does go through it, which is what lets the whole
launcher be tested against a fake netsurf in a scratch root. Noting the asymmetry because
it is deliberate and looks like an oversight.

### BR-9. `_log_console` opens `O_APPEND` where Python opened `"wb"`

Python's `open(CONSOLE, "wb", buffering=0)` is `O_WRONLY|O_CREAT|O_TRUNC`. On
`/dev/console`, a character device, `O_CREAT` and `O_TRUNC` are both no-ops, so the two
are the same call on the phone. `O_APPEND` (and no `O_CREAT`) is used instead because it
is the only spelling that also behaves sensibly when the "console" is the ordinary file a
host test points `ND_ROOT` at. A missing console is still a silent return, which is what
the Python's bare `except` does.


---

## The image: musl, the boot script, the package and the post-build hooks (WP image/boot)

### IMG-1. `post-build-prune-tests.sh` was reading the wrong argument, so no luckfox
image has ever had its own inittab

Buildroot calls a post-build script as

    script TARGET_DIR $BR2_ROOTFS_POST_SCRIPT_ARGS $BR2_ROOTFS_POST_BUILD_SCRIPT_ARGS

and **both** defconfigs set `BR2_ROOTFS_POST_SCRIPT_ARGS="$(BR2_DEFCONFIG)"` because the
qemu board's post-image script needs the defconfig path. So `$2` is an absolute path on
the build machine and the platform id (`luckfox-armv7` / `qemu-aarch64`) is **last**.

`post-build-prune-tests.sh` read `$2`. `"${PLATFORM%%-*}" = "luckfox"` therefore never
matched, and `etc/inittab.luckfox` was deleted at the end of the script without ever
being copied over `etc/inittab` — every hardware image so far has booted the generic
inittab instead of the luckfox one. `post-build-system-metadata.sh` had the identical bug
and its header comment records it being fixed there (it wrote a build-machine path into
`system.os.platform`); the sibling script was not fixed at the same time.

Now takes the last argument, exactly as the metadata script does.

**Flagging rather than assuming:** the two inittabs differ, and the phone has been
shipping the generic one for long enough that the current behaviour may be what anybody
has actually tested on hardware. If the luckfox console setup turns out to be wrong on a
real board after this, this is the change that did it.

### IMG-2. `PYTHONPYCACHEPREFIX` is kept in `run_neodct.sh` although nothing uses it yet

Line 30 still exports it after the launch line became `/NeoDCT/System/bin/nd-core`. It is
dead as far as the core is concerned. It is kept because the image still carries python3
and Pillow (SESSION-SCOPE.md: both ship until the last app is real) and the rootfs is a
read-only squashfs, so anything that does reach for python during the transition still
needs a writable cache prefix. Deleting it belongs with removing python from the
defconfigs, not before.

### IMG-3. `verify-c-build.sh` does not musl-check `displayd/`

Step 3 of the acceptance gate globs `lib core apprun apps tools` and stops there, so
`displayd/neodctDisplay.c` — the one file whose libc dependency was the entire musl
blocker — is the one file the musl check skips. Verified by hand instead: it compiles
clean under `musl-gcc -std=c11 -Wall -Wextra -Werror` once the kernel UAPI headers are
reachable (`-idirafter /usr/include -idirafter /usr/include/$(gcc -print-multiarch)`,
which is what the gate already does for the other directories). Not changed here because
`neodct/tools/` is another agent's file; one line added to that `find` closes it.

### IMG-4. The neodct package rsynced the developer's host build tree into the cross build

`NEODCT_SITE_METHOD = local` makes buildroot `rsync -au` the whole of `neodct/src` into
`$(@D)`, timestamps preserved and no `--delete`. `neodct/src/build/` is the developer's
**x86-64** build tree, and it lands in exactly the directories the cross build writes to,
with mtimes newer than the sources beside it — so make would declare everything up to
date and hand x86-64 objects to the ARM linker. Anybody who ran `make` in `neodct/src`
before building an image would have hit it. Fixed with
`NEODCT_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --exclude /build`.

Also: `NEODCT_LICENSE_FILES = ../../LICENSE` could never resolve — the path is taken
relative to `$(@D)`, which is inside `output/build/`, and the licence is at the repository
root above `NEODCT_SITE`. A `POST_RSYNC` hook copies it in now, so `make legal-info` works.

### IMG-5. Overlay ELF files never go through buildroot's strip

`target-finalize` strips at Makefile:760, copies `BR2_ROOTFS_OVERLAY` at :791 and runs the
post-build scripts at :802 — in that order. So a binary carried in the overlay is never
stripped by buildroot and never will be. That gap held the 24 KB `neodct_displayd` blob
until it started being built from source. The prune script now closes it with a narrow
pass over `/NeoDCT` that honours `BR2_STRIP_none`, refuses to run without a cross strip
out of the buildroot host tree, checks ELF magic per file and cannot fail the build.
Package-installed binaries are already stripped by then, so it is a no-op on those.

---

## nd-shoot: wiring CubeBench and Browser in, and measuring (WP nd-shoot-apps)

### SA-1. `eng-cubebench` renders with **0 differing pixels of 42,000**

`nd-shoot` now launches `Cube Bench` for real, through the same `dlopen` +
`app_run()` path that already drew `app-clock`, and its frame is byte-identical to
`golden/eng-cubebench.png` — SHA-256
`4dc887d233374e143e9f8cfffdfeef857ba7ed97b8ac8500d055b15e1c04c6b9` on both sides.
The whole set is **28 rendered, 28 byte-exact, 21 skipped**; the 27 frames that
were exact before are unchanged.

The `tolerance` class stays as it is. CB-1 already explains why the zero is
expected rather than lucky — CPython's `math.sin` is the platform libm's, so the
Python capture and the C port ran the same glibc code on the same `double`s — and
the budget exists for **musl on the device**, which has not been measured. Nothing
here changes that argument; it only confirms it on this host a second time, from
inside the capture tool rather than from `test_cubebench.c`.

`test_shoot.c` now handles the tolerance class rather than requiring the digest:
a mismatch on `eng-cubebench` alone is measured against the stored PNG and checked
against the nine-pixel cap, with the count and the bounding box printed either
way. No other name gets that path, and a byte-exact run says so out loud
(`eng-cubebench is BYTE-EXACT (0 differing pixels)`) so the budget cannot quietly
start being used.

### SA-2. The engineering apps are in `System/engineering/apps`, not `System/apps`

This is why `eng-cubebench` could not have been rendered by removing it from the
skip table alone. `nd-shoot` stages `/NeoDCT/System` as a real directory whose
entries are symlinks onto the read-only overlay, expanding only `apps/` so each
app can gain an `app.so`. `engineering/` was a plain symlink into the overlay, and
nothing can be written under there — so `engineering/apps/CubeBench/app.so` had
nowhere to exist. `engineering/` is now expanded the same way, with `tools/` left
as a symlink.

Two things follow that a reader should not have to rediscover:

* **The manifest name has a space and the directory name does not.**
  `engineering/apps/CubeBench/manifest.json` says `"name": "Cube Bench"`.
  `uistub.run_app()` matches on the manifest name (`if app["name"] == name`) and
  so does `nd-shoot`'s `app_index_of()`, so the recipe's string is used verbatim.
* **`rescan_apps()` scans both directories into one list**, sorted by id, so an
  engineering app is an ordinary entry of `ui->apps` once
  `system.ui.engineering_mode` is ON — which `write_settings()` already set.

### SA-3. Which `app.so` a staged app directory gets is now resolved per app

It was always `apps/Stub/app.so`. It is now `build/<variant>/apps/<Name>/app.so`
if this build produced one, and the stub otherwise, matched on the overlay's
directory name. CubeBench and Browser line up today and an app a later work
package adds lines up without touching `tools/nd_shoot.c`.

Staging a real `.so` is not the same as rendering with it: only the frames that
call `run_app_inproc()` `dlopen` anything. Everything else reads the manifest and
the icon, which are the overlay's own files either way.

### SA-4. CubeBench must be launched with NO held key, unlike app-clock

`run_app_inproc()` takes the key to hold as a parameter now. `app-clock` needs a
held `ND_KEY_ENTER` because `MessageDialog.show()` blocks and C has no
`ScriptExhausted` to raise out of a read. CubeBench must be given `ND_KEY_NONE`:
`main.py`'s `EXIT_KEYS = {14, 28, 46, 50}` contains 28, so a held ENTER would end
the run at frame 1 with a blank screen that still looked plausible. It gets an
empty key channel instead — `uistub`'s empty `KeyScript` — so `read_keypress(0)`
polls and returns `ND_KEY_NONE` without depending on whether the host has a
`/dev/input/event0`.

The budget is **60**, not `shoot_docs.py`'s `frame_budget=240`. CB-2 has the
reasoning: `uistub.StubUI`'s `idle_budget=60` is what actually ends the Python's
run, and the C reaches the same end state through the frame budget.

### SA-5. The skip table's reasons were a single blanket string, and it had gone stale

Every entry said `"app not implemented: neodct/src/apps/ is empty"`. That was true
when it was written and had not been true since `apps/CubeBench` and `apps/Browser`
landed — which is how `eng-cubebench` stayed skipped while the app that draws it
was sitting in the build directory. Each frame now carries the reason for that
frame, and "nobody has written this app" is worded differently from "the app
exists and something else is in the way".

`RENDERED[]` had drifted the same way, in the other direction: it was missing
`app-clock` and `crash-screen`, so `--list` under-reported by two. Both tables are
now checked rather than trusted:

* `nd-shoot` fails its own run if the number of frames it saved is not
  `ND_ARRAY_LEN(RENDERED)`.
* `test_shoot.c` runs `--list`, and requires every name it calls rendered to be in
  the manifest that run wrote, every name it calls skipped to be in the skip file,
  and the counts to match both ways.

### SA-6. There is no reference frame that launches the Browser

`apps/Browser` is a real port and is staged with its real `app.so`, but none of
the 49 names in spec-build-test.md section 3.6 runs it: `menu-browser` is the
app-selector tile, rendered from the manifest and the icon, and it was already
byte-exact. So there was nothing to un-skip on the Browser's account. Recorded
because "the Browser is missing from nd-shoot" is the obvious wrong conclusion
from the frame list, and because a future `app-browser` recipe would need a
reference frame captured from the Python first.

### SA-7. `0.2419 ms` is not the Python's CubeBench frame time

`PERFORMANCE.md`'s pre-port table decomposes five things and sums to 0.2419 ms.
CubeBench's frame draws **four** strings — `"3D Cube"` (7 chars), `"FPS %.1f"`
(8), `"BACK/OK to exit"` (15) and the softkey label — and calls `get_text_size()`
twice, which in Pillow is `font.getbbox()` and rasterises the string again to
measure it. The five-part sum leaves out about 6/7 of the frame.

Measured properly on this host, both sides back to back in one window, driving the
real `main.py` through `uistub`: the Python frame is **1.6333 ms (612 fps)** and
the C frame is **0.1025 ms (9,752 fps)** — **15.9×**, or 16× allowing for the
run-to-run spread on a machine several agents are compiling in. The full
side-by-side table is in `PERFORMANCE.md`.

The prediction was right about the *mechanism*: text was 75% of the frame as
decomposed then, is 79.5% of it as decomposed properly now, and is 8.0% in C.

**No question for the owner here** — the pre-port number is not wrong, it is a
decomposition being read as a total. `PERFORMANCE.md` now says so at the top of
the file so the two halves are read together.

### SA-8. The glyph cache is worth 11.9× on a text call and 1.82× on the frame

Measured by doing what a per-call renderer does — FreeType directly, with
`nd_font.c`'s own `FT_Set_Pixel_Sizes` / `FT_LOAD_DEFAULT` / `FT_RENDER_MODE_NORMAL`
— and adding the compositing both paths pay. `"FPS 60.0"` costs 0.0020 ms cached
and 0.0242 ms uncached (11.2–12.7× across runs). Per glyph that is 0.0028 ms of
FreeType work avoided; CubeBench draws 30 glyphs a frame, so a naive port would
spend 0.083 ms a frame re-rasterising characters it rasterised on the previous
frame: **0.1850 ms instead of 0.1019 ms, 1.82× on the whole frame**.

Against *Pillow's* text the figure is 138×, because Pillow pays the per-call
FreeType cost AND the interpreter's per-call overhead. 11.9× is the cache on its
own. The gap between "11.9× on the call" and "1.82× on the frame" is the useful
part of the result: once text is nearly free, the frame is whatever is left, and
here that is one fill loop.

`test_cubebench_perf.c` also asserts the cache is *free* rather than a trade — all
95 printable ASCII glyphs at all four sizes, byte-compared against a freshly
rasterised FreeType bitmap. If they ever differed, every golden frame would be a
coin toss.

### SA-9. CB-8 revisited: `nd_draw_rect_fill` is 5× SLOWER than Pillow, not 1.6×

CB-8 recorded the content-rectangle clear at 0.0305 ms against Pillow's 0.0189 ms,
calling it 1.6× slower. Measured back to back today it is **0.0718 ms against
Pillow's 0.0145 ms**, and it is **70% of the whole C frame**. The absolute numbers
moved because this tree now has several agents compiling in it and CB-8's were
taken on a quiet machine; **the 5× ratio is the robust figure**, because both
sides of it were measured in the same window under the same load.

The cause is in `lib/nd_draw.c`'s `hline()`: Pillow stores an RGB image as 4 bytes
per pixel internally and fills a run with 32-bit stores, while `hline()` issues a
3-byte `memcpy` per pixel, 35,040 times.

```c
if (k->bpp == 1) {
    memset(p, k->bytes[0], (size_t)(x1 - x0 + 1));
    return;
}
for (x = x0; x <= x1; x++, p += k->bpp)
    memcpy(p, k->bytes, k->bpp);
```

**Not touched**, for CB-8's reasons, which have not changed: `lib/nd_draw.c` is
another work package's, this is the most pixel-critical function in the project,
and every frame is byte-exact as it stands.

**Question for the owner:** there is a one-line version of the fix that cannot
change a pixel — when a colour's `bpp` bytes are all equal (black and white, which
is all CubeBench draws), take the `memset` path that the 8-bit case already takes.
That is a strictly narrower condition than the existing branch and is provably
byte-identical. Is that worth doing inside the port, or does anything touching
`nd_draw.c` wait for the 1:1 guarantee to be signed off? The larger fix — filling a
row at a time for the general case — should certainly wait.

---

## PhoneBook and the shared contact picker (WP apps-phonebook)

`System/apps/PhoneBook/main.py` (214 lines) and `System/apps/PhoneBook/shared/list_ui.py`
(72 lines), ported to `apps/PhoneBook/{main.c,pb_db.c}` and `lib/nd_contacts.c`.
Golden frames `app-phonebook` and `contacts-picker` are both **byte-exact, 0 differing
pixels**, and nothing else in the set moved.

### PB-1. One header was added: `nd_contacts.h`

**Found in:** `lib/nd_ui.c:135`, and U-4 above.

U-4 recorded that nothing in `include/` named the shared picker, and that `nd_ui.c`
declares it locally and weakly:

```c
bool nd_contacts_show_selector(nd_ui *ui, const char *title, const char *btn_text,
                               nd_contact *out);
```

`include/nd_contacts.h` is that declaration, in the frozen-header namespace, plus the
full-argument form the apps need:

```c
bool nd_contacts_pick(nd_ui *ui, const char *title, const char *btn_text,
                      const char *search_query, const char *header_root,
                      nd_contact *out, size_t *out_index);
```

**`nd_ui.c` was not touched.** The name it already picked is the name that shipped, so
the weak reference resolves and the home screen's Up/Down key now opens the picker. The
`nd_contact` row type and `nd_contacts_query()` were already in `nd_db.h`; nothing was
duplicated.

**Placement: `lib/nd_contacts.c`, in `libneodct.so`, not in `apps/PhoneBook/app.so`.**
`list_ui.py` has three importers in two processes — `core/main.py:1268`,
`apps/PhoneBook/main.py` and `apps/Messages/main.py`. `nd-core` forks and execve's
`nd-apprun` and never `dlopen`s an app itself, so a definition inside an app's `.so` is
one the core process could never call. `spec-apps-core.md` C2 reached the same
conclusion. Messages and the Dialer link it the same way.

One consequence worth knowing about: Up/Down on the home screen was a no-op for as long
as the symbol was missing, and now opens the picker for real. What happens after a row is
picked is still partly weak — `nd_ui.c:1344` calls `nd_modem_dial()` and then
`nd_dialer_show_calling()`, and the Dialer screens are not ported, so the call is placed
with no call screen behind it. That is the Dialer work package's, not this one's.

### PB-2. `get_all_contacts()` is unbounded; the C reads at most 256 rows

**Found in:** `shared/list_ui.py:20-30`.

The Python `fetchall()`s the whole table and puts every name on a `VerticalList`.
`CODING-STANDARDS.md` §1.5 forbids an array sized by input, so `ND_CONTACTS_PICK_MAX`
is 256: two heap allocations totalling ~55 KB, made when the picker opens and freed
before it returns. A SIM holds 250 contacts, so the cap is above anything the phone can
reach by importing, but a database with more rows shows only the first 256 by name
order. Same class of bound as P-2.

### PB-3. `time.sleep()` is skipped while the virtual clock is running

**Found in:** `shared/list_ui.py:52` (1.5 s), `PhoneBook/main.py:40` (1.0 s) and
`main.py:180` (2.0 s).

`goldenframe._Frozen` patches nine attributes of `time` and `sleep` is **not** one of
them, so under capture the Python really does sleep. In C, `dwell()` returns immediately
when `nd_vclock_enabled()`. The reasoning: under capture, time is a frame counter by the
project's own decision (`nd_vclock.h`), a real sleep advances neither the clock nor a
pixel, and the only thing it changes is how long the oracle and the unit tests take. On
the phone the sleeps are real and the durations are the Python's.

No golden frame passes through a sleep — the picker's empty state is unreachable with
the seeded contact, and `_draw_center_message` has no reference frame.

### PB-4. A missing `phonebook.db` is an empty phone book, not a crash

**Found in:** `shared/list_ui.py:16-30` — no `try`/`except` anywhere in the file.

`sqlite3.connect()` on a missing path **creates an empty file**, and the `SELECT` against
the table that is not in it then raises `sqlite3.OperationalError` straight out of the
app and into the crash screen. `nd_contacts_query()` (`lib/nd_db.c`, another work
package) returns zero rows instead, which lands in the picker's ordinary empty state and
draws "No Contacts". `spec-apps-core.md` §6b asked for exactly this and asked for the
deviation to be noted; this is the note.

### PB-5. "Call" does not call — the bug README.md line 144 advertises

**Found in:** `PhoneBook/main.py:170-181`, `run_contact_options()` item 0.

```python
if sel == 0: # Call
    ui.draw.rectangle((0, 0, screen_w, content_bottom), fill="black")
    y = max(12, int(content_bottom * 0.30))
    ui.draw.text((10, y),      "Calling...",  font=ui.font_xl, fill="white")
    ui.draw.text((10, y + 35), contact[1],    font=ui.font_n,  fill="white")
    ui.draw.text((10, y + 60), contact[2],    font=ui.font_s,  fill="white")
    ui.fb.update(ui.canvas)
    time.sleep(2)
```

That is the whole branch. **The modem is never touched**: no `dial()`, no
`Dialer/call_screen`, no hang-up key, no call-log entry. The screen says "Calling..." for
two seconds and returns to the contact's menu. `README.md` line 144 has listed it since
0.3.0 — *"Phonebook (SQLite-backed; calling action is buggy)"*.

**Ported exactly, and pinned by a test.** `nd_phonebook_calling_screen()` draws those
three strings at those three coordinates and does nothing else;
`test_phonebook.c::test_calling_screen_does_not_dial` composes the same frame by hand and
compares digests, so both "it stopped drawing the number" and "it started dialling" fail
the test.

**Question for the owner:** the fix is four lines — `nd_modem_dial(ui->modem, ...)` then
`nd_dialer_show_calling(ui, number, name)`, which is what `nd_ui.c:1342` already does for
the home screen's Up/Down picker. It is deliberately **not** applied. Two things stop it
being a free win: an app process has `ui->modem == NULL` by `nd_app.h`'s rules, so
dialling from inside PhoneBook needs a route to the core that does not exist yet; and
`y = max(12, int(content_bottom * 0.30))` with its hard 35/60 line spacing is a
hand-drawn screen that the real `call_screen` would replace outright, which changes
pixels. Should this wait for the Dialer work package, or is the intended behaviour "hand
the number back to the core and let it dial"?

### PB-6. `delete_contact_action()` has no exception handling at all

**Found in:** `PhoneBook/main.py:147-157`.

`add_entry_action` and `edit_contact_action` each wrap their statement in
`try/except Exception as e: print(f"[PB] ... Error: {e}")`, and — because the
`_draw_center_message` call is *inside* the `try` — a failure means no "Saved!" and no
"Updated!". Both are reproduced.

`delete_contact_action` has neither, so a sqlite failure there unwinds out of the app and
reaches the crash screen. The C logs `[PB] Delete Error: <sqlite message>` and still
draws "Erased". Crashing the phone book over a transient sqlite error is worse than the
confirmation being slightly wrong, and `CODING-STANDARDS.md` §3 forbids failing silently
— but it *is* a divergence, and it is the only one in `apps/PhoneBook/main.c`.

### PB-7. Three quirks in `main.py` that are ported, not fixed

1. **Two menu entries have no branch.** `run()`'s `if/elif` chain covers 0, 1, 2, 3 and
   5. "Send entry" (index 4) and "1-touch dialing" (index 6) are drawn, selectable, and
   choosing either just redraws the menu. `speed_dial` is written as 0 on insert and
   never read.
2. **Search and Edit disagree about the empty string.** `run()` tests the search box with
   `if query:`, so confirming an empty field is a cancel; `edit_contact_action` tests
   `if new_name is None:`, so confirming an empty field **saves an empty name**. Both
   spellings are kept (`query[0] != '\0'` vs `!= NULL`).
3. **Erase has no confirmation.** One Enter on the picker and the row is gone. The
   Python's own comment says *"In M3 we can add a 'Are you sure?' dialog here"*.

### PB-8. `_ensure_serial_redirect()` compares the device, not `sys.stdout.name`

**Found in:** `PhoneBook/main.py:8-21`.

The Python skips the redirect when `getattr(sys.stdout, "name", None)` already equals
`SERIAL_CONSOLE_DEVICE` (`$NEODCT_SERIAL_DEVICE`, default `/dev/ttyAMA0`). C has no name
on a descriptor, so `ensure_serial_redirect()` asks the same question of the file itself:
`fstat(1)` and `stat(device)`, and skip when both are the same character device. Every
failure is swallowed, as the bare `except Exception: pass` swallows it.

The two answers differ only where stdout is a pipe or a file that is not the serial
device, in which case both redirect. Note this runs at *import* time in Python and as the
first statement of `app_run()` in C, because that is the earliest moment the translation
unit has control.

### PB-9. `nd_shoot.c`: how each of the two frames is stopped

**Found in:** `neodct/tools/shoot_docs.py:103` and `:181`.

Both recipes end the screen with something C cannot raise. `app-phonebook` is
`run_app(ui, "Phone book", keys=[])`, where the first `read_keypress()` raises
`ScriptExhausted` and the frame already on the canvas is the one saved; `contacts-picker`
is `ui.keys.push(BACK)` followed by a `try`/`except BaseException: pass`.

In C both are stopped with the Back key: `STOCK_CASES` gives PhoneBook `ND_KEY_CLEAR` as
its held key, and the picker gets a one-key script. That looks like the mistake
`run_app_inproc()`'s comment warns about — handing an app a key it reads as "quit" — and
here it is the right answer, because the reference frame **is** the first screen the app
draws. `shoot_stock_apps()` was also restructured to build a fresh `nd_ui` and a fresh
virtual clock per case, which is what `with StubUI(...)` does per case in the recipe and
what it did not do before.

---

## Messages (`apps/Messages/`, work package MSG)

Landed as `apps/Messages/main.c`, `apps/Messages/msg_db.c` and
`apps/Messages/messages.h`, with `test/unit/test_messages.c` (150 checks) and the two
golden frames `app-messages` and `app-messages-inbox` now rendered by `nd-shoot`. Both
match the reference byte for byte: **0 differing pixels each**, confirmed by the SHA-256
over raw RGB inside `test_messages.c` and again by `goldenframe.py --compare`.

### MSG-1. Sending an SMS from an app process cannot reach the modem

**Found in:** `Messages/main.py:246-266`, `_send_message_flow()`.

```python
modem = getattr(ui, "modem", None)
if modem is None:
    MessageDialog(ui, "ModemService is not running.").show()
    return False
...
ok, detail = modem.send_sms(number, text)
```

The Python branch exists and is never taken, because Messages runs **inside the core
process**, where `ui.modem` is the live `ModemService`. With process-per-app it is always
taken: `nd_app.h` is explicit that the modem, battery and notify handles "live in the
core and are NULL in an app's context", and `nd_ui_init_app()` leaves `ui->modem` NULL.

**Ported exactly.** `nd_msg_send_flow()` tests `ui->modem == NULL`, shows that dialog,
and otherwise calls `nd_modem_send_sms(ui->modem, number, body, detail, sizeof detail)` —
the corpus-tested CMGS `>` prompt + Ctrl-Z + `+CMGS` ack path, not a second serial
implementation. So the code is 1:1 and the *outcome* is not: on the phone today, Write
Message -> Options -> Send gets as far as the number field and then says
"ModemService is not running."

This is the same shape as PB-5 ("Call" does not call) and has the same fix: a route from
an app to the core's services. **Question for the owner:** should that route be a small
request/response protocol over the existing app pipe (the core already owns the modem
thread and would do the CMGS transaction on the app's behalf), or should Messages move
back into the core the way `nd_contacts.c` did? A send takes up to 30 s
(`ND_SMS_SEND_TIMEOUT_S`), so whichever it is has to be asynchronous or the core stops
polling the modem while an app is texting.

Until it is answered, `test_send_flow_without_a_modem` pins the current behaviour so the
day it changes, it changes deliberately.

### MSG-2. The list screens read at most 128 rows

**Found in:** `Messages/main.py:88-105`, `_fetch_inbox_messages()` / `_fetch_outbox_messages()`.

Both return every row in the table and `_show_inbox` puts every one of them on a
`VerticalList`. `CODING-STANDARDS.md` section 1.5 will not have an array sized by the
database, so `ND_MSG_LIST_MAX` is 128 and a list screen shows the newest 128.
`sizeof(nd_msg_rec)` is 1,112 bytes, so the three arrays together are about 139 KB,
`calloc`ed when the screen opens and freed before it returns — one live allocation per
list screen and never one per frame.

A SIM holds about 30 SMS slots and `nd_modem_read_stored_sms()` sweeps them into the
inbox one at a time, so 128 is well above what the hardware can put there in one go. It
is *not* above what years of texts would accumulate, and the Python has no purge. Same
question as PB-2: is silently showing the newest 128 acceptable, or should the inbox
paginate?

### MSG-3. None of the seven statements has any exception handling

**Found in:** `Messages/main.py:88-171`.

Every query is a bare `sqlite3.connect(...)` / `execute` / `close` with no `try`. A
locked database or a full filesystem therefore raises out of the app and reaches the
crash screen. The C logs nothing and carries on, which is the same deliberate divergence
`PB-6` records for `delete_contact_action()`: crashing the messaging app over a transient
sqlite error is worse than the "Erased!" dialog being slightly wrong. `nd_msg_save_outbox()`
is the one that returns an `nd_err`, and its only caller ignores it — as the Python does,
whose `_save_outbox_message()` returns nothing and whose caller shows "Saved!"
unconditionally.

### MSG-4. `if result == "deleted": continue` is a no-op, and the spec says otherwise

**Found in:** `Messages/main.py:352-372` (`_show_inbox`) and `:374-394` (`_show_outbox`).

```python
        result = _show_message_detail(...)
        if result == "deleted":
            continue
```

That `continue` is the **last statement in the `while True` body**, so the loop repeats
whether the detail screen returned `"deleted"` or `None`. Backing out of a message
returns to the inbox list, and only Back *on the list* leaves Messages.

`spec-apps-core.md` section 7 reads it the other way round — *"Note the loop only
continues on `"deleted"`; a normal Back out of the detail screen ends `_show_inbox`
entirely"* — and that is wrong. The Python is the oracle, so the C loops.
`test_inbox_marks_read_on_open` needs three Back presses to leave, which is what pins it.

**Action for the owner:** the spec paragraph should be corrected, or the behaviour
changed on purpose. They currently disagree.

### MSG-5. How `nd-shoot` stops each of the two frames

**Found in:** `neodct/tools/shoot_docs.py:104-105`.

```python
("Messages", [],      "app-messages",       -1, 240),
("Messages", [ENTER], "app-messages-inbox", -1, 240),
```

Both recipes end the screen with `ScriptExhausted`, which C cannot raise out of a
blocking read.

`app-messages` is straightforward and is done the way `app-phonebook` is done: a **held
Back**. `nd_pagedlist_show()` drains the channel before its first draw (the 0.01 s poll
`nd_input.h` insists on keeping distinct from MessageDialog's 0.0), so a queued key would
be eaten; the held press survives the drain and its first repeat arrives after the frame
is committed. `run()` returns, and the frame already on the canvas is the one saved.

`app-messages-inbox` could not be done that way. Enter picks "Inbox", the empty inbox
paints `_show_empty_state`, and **that screen exits on key 14 alone** — so the only key
that gets out of it also sends the app back to the root menu, which redraws and becomes
the last frame. The alternatives were both worse: freezing the recording with
`nd_capture_set_budget()` means hardcoding a frame count, and saving `nd_capture_recent(cap, 1)`
means hardcoding how many frames a PagedList redraw commits.

So that frame is captured through **`app_open_inbox()`** — the entry point the core
really calls when the notification banner is opened with several unread messages
(`nd_app.h`, `spec-apps-core.md` section C3). It is `_show_inbox(ui, 2, 1)`: the same
function with the same arguments that the root menu's `sel == 0` branch calls, so the
pixels are the same screen by construction, the app is genuinely launched and genuinely
draws it, and the one entry point that had no coverage anywhere (X-23 noted exactly that)
now has some. `test_messages.c` renders the frame both ways — through `app_open_inbox()`
and through `nd_msg_show_empty_state()` directly — and both digests match the reference.

**Question for the owner:** is that substitution acceptable, or should `nd-shoot` grow a
general "stop the app at frame N" mechanism so every remaining app can be shot exactly
along the recipe's path?

### MSG-6. `_show_stub_screen()` is dead code and is not ported

**Found in:** `Messages/main.py:32-51`.

No caller anywhere in the tree. `spec-apps-core.md` already says so; recorded here so
that "the C is missing a function the Python has" is a known answer rather than a bug
report.

### MSG-7. `_wrap_text()` is the fifth word-wrapper, and it is not any of the other four

**Found in:** `Messages/main.py:53-86`.

`nd_text.h` already documents four that disagree, and `nd_widgets.h` adds PagedList's as
a fifth. This is a sixth. What makes it its own function:

* `text.split()` with no argument, so runs of whitespace collapse and **newlines are
  lost** — a multi-line SMS renders as one paragraph.
* an empty or whitespace-only string returns **one empty line**, not zero.
* a word too wide for the column is trimmed until `word + "..."` fits and gets that
  `"..."` **unconditionally**, including when it is the last word. `nd_pagedlist_wrap()`
  deliberately does not, and `nd_widgets.h` says that difference is visible on the Call
  Log.
* after such a word, `current` is reset to `""`, so the next word starts a fresh line
  rather than joining the trimmed one.

It is `nd_msg_wrap_text()` in `apps/Messages/main.c`, not in `nd_text.c`: it belongs to
this app, and moving it next to the other five would invite exactly the merge that
`nd_text.h`'s header warns against.

One deviation, and it is a correctness one: Python's `word[:-1]` drops one **character**,
so the C drops one whole UTF-8 code point rather than one byte. Dropping a byte would
leave a truncated sequence that the font cannot measure or draw. No golden frame contains
non-ASCII, so nothing observable changes.

### MSG-8. Two small things that are reproduced rather than fixed

1. **The outbox is re-created without the WAL pragma.** `init_databases()` in the core
   creates `sms_outbox.db` with `PRAGMA journal_mode=WAL`; `_save_outbox_message()`
   re-issues a bare `CREATE TABLE IF NOT EXISTS outbox`. On a phone the core has booted
   this is invisible, because `journal_mode` lives in the file header. On one whose
   outbox was deleted, this app brings it back in rollback-journal mode. Same asymmetry
   `nd_db.h` records for the call log, and reproduced for the same reason.
   The CREATE text in `msg_db.c` is the app's own indentation, deliberately **not**
   `ND_SCHEMA_OUTBOX`: sqlite stores the statement verbatim in `sqlite_master`, so the
   two are different bytes in a `.dump` depending on which one ran first.

2. **No softkey is set before the detail page's Options list.** `_show_message_detail()`
   opens a `VerticalList` without touching the bar, and `VerticalList.draw()` clears only
   rows 0..145 — so the "Options" the detail screen painted is still there above the
   options menu. Reproduced; it is what the phone shows.

3. **The composer capitalises the first character.** `TextInputLong.handle_key()` does
   `if len(self.text) == 0: char = char.upper()` (framework.py:762 and 973-975), so a
   draft saved to the outbox starts with a capital. Not a Messages behaviour at all, but
   it surprised this port's first test and is worth having written down.

---

### MSG-W. `test_messages.c:752` asserts the wrong wrap result — the C is right

**Status: verified against the Python. Fix the TEST, not `nd_msg_wrap_text`.**

The assertion is:

```c
g_api.wrap_text(&lines, &fx.ui, "aaa bbb ccc ddd", 60, fx.font_n);
CHECK_STR(nd_lines_at(&lines, 0), "aaa bbb");   /* <- wrong */
```

Measured on the shipped `font.ttf` at `font_n` (size 20), BASIC layout:

| string | advance |
| --- | --- |
| `"aaa"` | 48 px |
| `"aaa bbb"` | **104 px** |

The column is 60 px, so `"aaa bbb"` cannot fit and the greedy fill is right to
break after `"aaa"`. Confirmed by running the real Python:

```
Messages._wrap_text("aaa bbb ccc ddd", 60, font_n)  ->  ['aaa', 'bbb', 'ccc', 'ddd']
```

`nd_msg_wrap_text` returns `"aaa"` for line 0, which matches. The expectation
was written without measuring.

**Do not make the C produce `"aaa bbb"`.** That would diverge Messages'
wrapping from the Python for every narrow column on the phone, to satisfy an
assertion that was never true.

#### While confirming this: the line above it is right for a non-obvious reason

```c
g_api.wrap_text(&lines, &fx.ui, "hello\nthere", 220, fx.font_n);
CHECK_INT(lines.n, 1);
CHECK_STR(nd_lines_at(&lines, 0), "hello there");
```

That looks wrong — the framework's `_wrap_lines` splits on `\n` and would give
two lines — but Messages does **not** use `_wrap_lines`. Its own `_wrap_text`
starts with `(text or "").split()`, which splits on *all* whitespace including
newlines and then rejoins the words with single spaces. So one line
`"hello there"` is genuinely correct here:

```
Messages._wrap_text("hello\nthere", 220, font_n)  ->  ['hello there']
```

This is exactly the trap `nd_text.h` warns about at the top of the file: there
are **six** text-fitting routines in the Python and they deliberately disagree.
Checking `nd_msg_wrap_text` against `_wrap_lines` gives the wrong answer in both
directions. Check every text routine against *its own* Python original.

**Answer:** _(no owner decision needed — this is a test fix)_

---

## Calculator, Clock, Power and Tones (work package apps-small)

Recorded while porting the four smallest stock apps:
`System/apps/Calculator/main.py` (159 lines), `System/apps/Clock/main.py` (18),
`System/apps/Power/main.py` (103) and `System/apps/Tones/main.py` (229) into
`apps/Calculator`, `apps/Clock`, `apps/Power` and `apps/Tones`.

Three golden frames moved from the skip list into the rendered set, and a
fourth changed hands. All four are **byte-exact, 0 differing pixels**:
`app-calculator`, `app-calculator-options`, `app-tones`, and `app-clock`
— which apps/Stub used to draw and apps/Clock now draws.

### CA-1. Division by zero is the one branch this port ADDS

**Found in:** `System/apps/Calculator/main.py:67`, `except ZeroDivisionError:`.

Python raises on `x / 0.0`; C returns an infinity. Left alone, `8 / 0 =` would
have printed `inf` on a phone that prints `Error`. `nd_calc_fold()` therefore
tests the divisor explicitly before dividing. It is the same behaviour reached
a different way, and it is the only place in `apps/Calculator/main.c` where the
C says something the Python does not have written down.

Nothing else in that file needed adding. `+`, `-` and `*` overflow to an
infinity in both languages (Python float multiplication does not raise
`OverflowError`), so `1e308 * 10` shows `Error` in both — through
`format_number`'s `inf` test, not through an exception.

### CA-2. `int(value)` is arbitrary precision and `(long long)` is not

**Found in:** `System/apps/Calculator/main.py:24`,
`if value == int(value) and abs(value) < 10 ** MAX_DIGITS:`

Python evaluates `int(value)` **first**, for any magnitude, and only then
checks the `10**12` ceiling. `int(1e300)` is exact in Python; `(long long)1e300`
is undefined behaviour in C. So `nd_calc_format_number()` uses `trunc()` for the
equality test — exact for every finite double — and casts only inside the
branch that `fabs(value) < 1e12` has already made safe.

The formatting itself needed nothing: CPython's float formatting and
glibc/musl `printf` are both correctly rounded, so `"%.8f"` and `"%.6e"` of the
same double produce the same digits. Thirty-three values are pinned in
`test_calculator.c`, every expectation produced by **running** the Python's own
`format_number`, not by reasoning about it.

### CA-3. Three unreachable branches in the Python are reproduced anyway

`type_digit()` special-cases an entry of `"-0"`, `type_point()` special-cases
`"-"`, and the twelve-character limit is measured after `lstrip("-")`. **Nothing
in this app can put a `-` at the front of the entry**: there is no sign key and
no unary minus in the Options menu. `type_digit()`'s `ch != "."` can never be
false either, because only `type_point()` writes a point and it does not go
through `type_digit()`.

All four are ported. CODING-STANDARDS.md section 9.4 — a later port that adds a
sign key will want them, and deleting reachable-looking code is how a port
loses a rule nobody wrote down.

### CA-4. The Calculator's two frames are picked by the FRAME BUDGET, not by a key

**This is CB-2's substitution again**, and it is the second place in the tree
that needs it.

`Calculator.loop()` redraws on **every** key it recognises, Clear included, and
there is no key it treats as "leave without redrawing": Clear deletes a
character and draws, and any key it ignores never returns at all. The Python
does not need one — `uistub.KeyScript` runs out and `wait_for_key()` raises
`ScriptExhausted` straight out of the loop, and `frames[-1]` is whatever was
already committed.

C cannot raise out of a read. So `nd-shoot` queues the recipe's keys followed by
enough Clears to empty the entry and return, and sets an `nd_capture` budget at
the frame the reference holds:

| frame | keys | budget |
| --- | --- | --- |
| `app-calculator` | `1 2 3` then 4× Clear | **4** — `draw()` plus one per digit |
| `app-calculator-options` | `7 Enter` then 3× Clear | **3** — `draw()`, the `7`, the list |

The Clears after the budget still run and still redraw; `nd_capture` refuses
them with `ND_ERR_BUSY`, records nothing and **does not tick the virtual
clock**, so neither the ring nor the clock can tell they happened. Both frames
are byte-exact, and `test_calculator.c` pins the frame count, the tick count and
the digest the way `test_cubebench.c` does.

`app-tones` needs none of this: `PagedList` drains the channel before it draws,
so its Back arrives as a held-key repeat afterwards, exactly as `app-clock`'s
does.

### CL-1. X-18 is closed: the Clock app's `while True:` is now reproduced

X-18 recorded that `apps/Stub` did not reproduce Clock's loop or its
`(46, 28, 50)` exit set, because `MessageDialog`'s own key set already covered
the same exits. `apps/Clock` is a real port now and both are back, along with
the `ui.fb.update(ui.canvas)` on line 10 that commits a frame **before** the
dialog is drawn.

That extra frame is captured and discarded — `shoot_docs.py` saves `frames[-1]`
— but it is committed, and `test_clock_app.c` checks the count is 2 so that a
tidy-up removing it is a failing test rather than a silent change to what the
virtual clock has ticked. `app-clock` is still byte-identical to
`widget-messagedialog`.

**Two keys leave this screen, not one.** `warningmsg.show()` returns the key
that dismissed it and the Python throws it away, so the loop's own
`wait_for_key()` needs a second press; a Clear never leaves at all, because
`MessageDialog` cancels on it and `(46, 28, 50)` does not contain 14. That is
the phone's behaviour today and it is ported as-is — which also means the only
bounded way to observe the loop from a test is `nd_app_should_exit()`, and that
is how `test_clock_app.c` does it.

### PW-1. `subprocess.Popen(["poweroff"])` searches $PATH; `nd_proc_spawn()` does not

**Found in:** `System/apps/Power/main.py:45`.

`Popen` uses `execvp` semantics. `nd_proc_spawn()` takes a path, deliberately
(nd_proc.h: executables are not ND_ROOT-resolved). So `nd_power_which()` does
the lookup first — the name itself when it contains a slash, `$PATH` otherwise,
`confstr(_CS_PATH)`'s value when `$PATH` is unset — and its failure is the
`OSError` the Python's `except OSError: continue` catches.

The candidate ORDER is untouched: `poweroff`, `/sbin/poweroff`,
`busybox poweroff`. On an image with both of the first two they are different
programs.

### PW-2. A missing `sync` is logged, not propagated

**Found in:** `System/apps/Power/main.py:62`, `subprocess.call(["sync"])`.

`subprocess.call` raises `FileNotFoundError` when the binary is absent, and
nothing in `_go_down` catches it — so on an image without `/bin/sync`, asking
the phone to switch off shows a **stack trace** instead. The C logs
`[Power] cannot run sync: ...` and carries on to the halt.

**This is a deliberate deviation.** No golden frame covers it, and the
alternative is a crash screen on the one path whose entire job is to bring the
system down cleanly. Flagging it rather than reproducing it.

### PW-3. `str(OSError)` has no C spelling

**Found in:** `System/apps/Power/main.py:82`,
`_tell(ui, "Cannot ask for recovery: %s" % exc)`.

Python prints `[Errno 30] Read-only file system: '/NeoDCT/User/.ndsys'`. The C
prints `strerror(errno)` after the same colon — same message, same place, same
consequence (no reboot), different words. Reproducing the errno-number-and-repr
formatting would be inventing a Python detail in C for no reader's benefit.

### PW-4. `nd_power_go_down()` is deliberately not covered by a test

It runs `sync` and then the first `poweroff` that exists. On a developer's
machine and on a CI runner that is a real poweroff, and a test suite that can
switch off the machine it runs on is not a test suite. `test_power.c` drives the
three pieces underneath it — the `$PATH` lookup, the candidate walk over
commands that do not exist, and the recovery flag — and never the composition.
The hole is named in `power.h`, in the test's header comment and here.

For the same reason `app_run()` is only ever driven with Back on the first
screen.

### TN-1. Three caps the Python does not have

`_scan_tones()` builds an unbounded Python list from an SD card's contents.
CODING-STANDARDS.md section 1.5 will not have an array sized by the contents of
a card, so `apps/Tones/tones.h` fixes three numbers:

| | | |
| --- | --- | --- |
| `ND_TONES_MAX` | 256 tones | heap, `(256 + 1) * 352 = 90,464 bytes`, freed before the screen returns |
| `ND_TONES_NAME_MAX` | 96 bytes of display name | about twenty characters fit across 240 px at 18 px, so a longer name was already running off the edge |
| `ND_TONES_WALK_MAX` | 64 directories pending | the stock tones directory is flat; a card is not necessarily |

A card with more than 256 `.mp3` files shows the first 256 in walk order and
logs `[Tones] More than 256 tones; the rest are not listed.` rather than
silently truncating.

### TN-2. `SUPPORTED_EXTS = (".mp3")` is a STRING, not a one-element tuple

**Found in:** `System/apps/Tones/main.py:33`. The missing comma makes it
`str`, not `tuple`. `str.endswith` accepts both, so it behaves identically
**today** — and would stop behaving identically the moment a second extension
was added without the missing comma being noticed, because `(".mp3", ".wav")`
is a tuple and `(".mp3" ".wav")` is the string `".mp3.wav"`.

The C is a plain single-suffix test, which is what runs today. Reproducing the
latent bug is not possible in C (there is no type confusion to reproduce);
naming it is.

### TN-3. The tone list holds LOGICAL paths, where uistub hands the Python staged ones

`os.walk` is one of `uistub.PathRemap`'s patched calls, so under the capture
harness `os.walk("/NeoDCT/System/tones")` yields **staged** roots and
`set_setting("system.audio.ringtone", ...)` records a path under `/tmp`. On the
device there is no remap and the same code records `/NeoDCT/System/tones/...`.

The C keeps the paths logical and resolves only at `opendir`/`stat`/`mpv` time.
That matches the device exactly, matches the project convention (`settings.prop`
holds `/NeoDCT/...` paths and `nd_notify.c` resolves them when it reads them),
and avoids a double prefix when `nd_notify_ringtone_path()` reads the setting
back under a test root. Only the stub's behaviour differs, and no golden frame
covers the tone list.

### TN-4. The ringtone preview still spawns mpv — nd_notify.h has nowhere else to send it

**Found in:** `System/apps/Tones/main.py:35`, `MPV_CMD`.

The brief for this work package asked for the preview to go through
`nd_notify`'s audio path rather than shelling out. It cannot, yet.
`nd_notify.h` exposes exactly two ways to make a noise:

* `nd_notify_play_tone(n, path)` — spawns `aplay`, so **WAV only**. All sixteen
  shipped tones are `.mp3`; routing the preview through it would make every
  one of them silent.
* `nd_notify_start_ring(n)` — streams through `nd_tone_src` (N-1, N-2) and is
  exactly the right machinery, but it resolves its own path from
  `system.audio.ringtone` and cannot be pointed at an arbitrary file.

So the preview keeps mpv, spawned through `nd_proc_spawn()` — fork then execve,
no shell, the descriptor plan built before the fork, stdout and stderr on
`/dev/null` where `subprocess.DEVNULL` puts them. That is a one-to-one port of
what the phone does today, and `app_shutdown()` kills the player, which is the
SIGTERM teardown contract `nd_app.h` makes the symbol mandatory for.

> **The change worth making, when nd_notify.h can be reopened:** a
> `nd_notify_preview_start(nd_notify *n, const char *path)` /
> `nd_notify_preview_stop()` pair reusing `nd_tone_src` and `aplay`, playing
> once rather than looping. It would delete this app's `which_exec()`, delete
> the mpv dependency from the preview path, and cost the ~100 kB N-1 measured
> instead of mpv's ~24 MB. `nd_notify.h` belongs to another work package and
> `lib/nd_notify.c` is being changed by nobody this session, so this is filed
> rather than done.

### TN-5. `_flush_input()` reads `ui->input`, not `ui->keypad_fd`

The Python selects on `ui.keypad_fd` with a 0.01 s timeout. The C reads the
same records through `ui->input`, which is where an app process's key channel
lives and is what every widget's own flush already uses (`nd_pagedlist.c`). The
0.01 s is kept: `nd_input.h` is explicit that PagedList's 0.01 and
MessageDialog's 0.0 are different numbers on purpose.

### AS-1. `test/unit/smallapp_test.h`, and why it is a header

The four tests need the same fixture — a minimal `nd_ui` with real fonts, an
`nd_capture` framebuffer, a key channel and the golden manifest. Four copies
would have been six hundred duplicated lines. It is a header and not a `.c`
because the Makefile globs `test/unit` into one binary per source file, so a
shared `.c` there would become a test with no `main()`. `draw_ref.inc` and
`platform_test.h` are there for the same reason.

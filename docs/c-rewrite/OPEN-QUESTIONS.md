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

## OPEN

_(none — add new entries below in the format at the top of this file)_

# Koki: the C port, and the one decision everything else hangs off

`game.py` is 2,817 lines containing **500 `yield` statements** across **304
registered handlers** (243 written; six loops install the rest — see
"Counts"). Every one of those handlers is a Python generator that stops
mid-statement and resumes on the next frame. C has no generators. Choosing
how to get that back is the whole port; everything else is transcription.

This file records the choice, the reasoning, what it costs, and where the
port actually got to.

---

## The decision

**Protothreads: a `switch`/`case __LINE__` trampoline over an explicit
per-script frame struct, with sub-scripts as nested frames in a fixed array.**

`docs/c-rewrite/spec-koki.md` §4.4 recommends this and it is the right
recommendation. The three macros are in `koki_sched.h`:

```c
#define KOKI_BEGIN(F)  switch ((F)->pc) { case 0:
#define KOKI_YIELD(F)  do { (F)->pc = __LINE__; return KOKI_YIELDED; \
                            case __LINE__:; } while (0)
#define KOKI_END(F)    } (F)->pc = -1; return KOKI_DONE
```

A script is an ordinary C function that reads like the Python it came from:

```c
static koki_step s_logo(koki_frame *F)          /* @eng.on("flag", LOGO) */
{
    KOKI_BEGIN(F);
    COSTUME(LOGO, "costume1");
    koki_show(LOGO);
    koki_layer_front(LOGO);
    LOGO->ghost = 100.0;
    for (F->i = 0; F->i < 20; F->i++) {
        LOGO->ghost -= 5.0;
        WAIT(F, 0.01);
    }
    ...
    KOKI_END(F);
}
```

`F` is `koki_frame *`: `F[0]` is this function's own suspension state,
`F[1]` is its child's, `F[2]` its grandchild's. `WAIT(F, t)`,
`GLIDE(F, ...)`, `GLIDE_TO`, `WAIT_UNTIL` and `PLAY_UNTIL` (all in
`koki_game.h`) drive the engine's own generators as child protothreads at
`F+1`; `KOKI_CALL(F, expr)` does the same for a game sub-script, which may
itself wait at `F+2`. Measured maximum depth in `game.py` is 3;
`KOKI_STACK_DEPTH` is 5 and nothing indexes past it.

There is deliberately no "repeat n times" macro: `for _ in range(n)` becomes
a plain `for (F->i = 0; F->i < n; F->i++)`, sixty-six times, because the
counter living in the FRAME is exactly what a reader must not miss and a
macro would hide it.

### Why not the other two

**Explicit hand-written state machines (option 1).** This *is* option 1 —
with the compiler writing the 500 state constants instead of a human. That
is the entire difference and it is decisive: `case __LINE__` cannot be
mis-numbered, cannot go stale when a statement is inserted in the middle of a
script, and cannot silently collide with another script's numbering. Hand
enumeration of 500 suspension points across 304 handlers has no mechanism
that makes a mistake loud, and a mis-numbered state in a boss fight surfaces
as "the dragon sometimes skips its second attack", weeks later.

**Coroutines via `ucontext` (option 2). Ruled out on a fact, not a
preference: musl does not implement it.** Measured on this checkout:

```
$ musl-gcc -o ucx ucx.c
undefined reference to `getcontext'
undefined reference to `makecontext'
undefined reference to `swapcontext'
```

`docs/c-rewrite/MUSL.md` has the project moving to musl and
`SESSION-SCOPE.md`'s done-bar includes "clean under `musl-gcc` too", so an
approach that does not link there is not an approach.

Even on glibc the memory arithmetic argues against it. The measured peak is
**32 concurrent scripts** (spec §9; this port measured 32 too, and 44 under
an artificial sweep), each needing its own stack. At a miserly 16 KB that is
512 KB of dirty pages; at anything comfortable it is megabytes — against a
device where the whole OS idles at 5.4 MB and CODING-STANDARDS §1.5 already
says the stack is the thing you do not put sized-by-input data on. The
protothread equivalent is 124 KB of frame structs, and it is 124 KB whether
one script is live or all 304 are.

A hand-rolled `switch` trampoline is what is left of option 2 once the stacks
are removed, and that is protothreads again.

**A bytecode interpreter (option 3).** The romantic answer, and the task
description is right that it is what Scratch actually is. It is still the
wrong answer *here*, for one reason: **`game.py` is no longer a `.sb3`.** It
is hand-written Python that has already left the block language behind —

```python
PLAYER.x = max(-235, min(235, PLAYER.x))
PLAYER.sy = 15 if (PLAYER.sy < 1 and key("z")) else 0
if V["taken damage"] >= 14 and not costumeIs("Oof2"): ...
```

Those are arbitrary expressions over an arbitrary variable table, plus
closures (`_touch_plat`), plus predicates passed to `wait_until`. A bytecode
VM faithful to *this* input has to be a general expression language with a
compiler, and that compiler has no oracle — the golden frame tests the
*game*, not the toolchain that emitted it. It also puts a dispatch loop
between every game statement and the CPU on the one app whose entire
justification is that C is faster than Python. The port would become
"design a language, write its compiler, then rewrite the game in it", with
three places for a bug to hide instead of one.

If the input were still the `.sb3`, option 3 would win. It is not.

### What it costs

The costs are real and a reviewer should hold the port to them:

1. **No C stack local may survive a `KOKI_YIELD`.** Every loop counter,
   saved coordinate and temporary that lives across a suspension goes in the
   frame struct (`F->i`, `F->j`, `F->s0`, `F->s1`, `F->x0` …). A local that
   forgets this is not a compile error; it silently resets on resume. This
   is the single failure mode of the technique and it is why the frame
   struct's fields are named rather than a `void *user` blob.
2. **No `switch` inside a script body** unless written as its own
   protothread — Duff's device does not nest. All 304 handlers are
   `if`/`while`/`for` only, so nothing was bent to satisfy this.
3. **`-Wimplicit-fallthrough` (which `-Wextra` turns on) fires on
   `KOKI_CALL`,** where the `case` label follows a plain assignment rather
   than a `return`. The spec's §4.4 sketch of `KOKI_CALL` does not build
   under this project's flags for exactly that reason. Fixed with a
   `KOKI_FALLTHROUGH` attribute macro before the label; see `koki_sched.h`.
4. **Arguments to a called sub-script are re-evaluated on every resume,**
   because the call macro re-enters the callee each frame. Python evaluates
   them once, at `yield from`. This is not theoretical — `game.py` writes
   `yield from _riby_cannon_volley(R(1, 3))`, and a literal transcription
   would redraw the count from the Mersenne Twister on every frame of the
   volley and desynchronise the whole RNG stream from the original. Three
   sites are affected, all in `koki_game_final.c`, and all three draw into
   the frame first:

   ```c
   F->s0 = (double)RND(1, 3);                       /* drawn ONCE */
   KOKI_CALL(F, riby_cannon_volley(&F[1], (int32_t)F->s0));
   ```

   `for _ in range(R(2, 6))` has the same shape and the same fix. Every
   other call site passes a constant or a stable sprite pointer.

### What it buys

- **Zero per-script stack.** One `koki_frame[5]` per handler slot, and the
  slot table is a fixed array: `sizeof(koki_slot)` is 416 bytes (360 of them
  the frame stack), so 304 handlers cost **124 KB, fixed, for the whole
  game**, live scripts or not. No pool, no allocation failure path, no "out
  of script slots" runtime error the spec's 64-slot pool would have needed.
  The whole `koki_engine` is 175 KB, and the 45 sprites another 7 KB.
- The C reads like the Python beside it, which is what makes 2,817 lines of
  transcription checkable by a human.

---

## The other structural decision: one slot per handler, generation-counted

Scratch's broadcast semantics are the second thing that does not survive a
naive translation. `broadcast(m)` **restarts** every handler listening for
`m`, and `active` is a Python `dict` whose *insertion order is the execution
order* — assigning to a present key keeps its position, a deleted key that
comes back is appended at the end. Execution order is therefore dynamic and
observable.

The port gives every registered handler exactly one slot
(`koki_slot`), and threads the live ones onto an intrusive doubly-linked
list which *is* `active`. Position is kept or appended by the same rules.
Because a restart reuses the slot, Python's "the old Script object is a
different object that is marked dead" is reproduced with a **generation
counter**: the per-frame snapshot records `(slot, generation)`, and a slot
whose generation has moved on is skipped exactly as Python skips the dead
object it is still holding. Without that, a broadcast that restarts a script
mid-pass would step the *new* instance in the same frame, and the spec's
one-frame broadcast latency (§4.3) would collapse.

### The case one slot per handler nearly got wrong

A script can broadcast a message **it is itself a handler for**, and
`s_riby_jump_attacks` does exactly that as its last statement — game.py's
comment says why: without the self-restart the final fight soft-locks with
Riby inert. In Python the broadcast puts a brand-new Script at the key, and
the old generator's `StopIteration`, arriving a moment later, marks the OLD
object dead and leaves the new one alone.

With one slot there is only one object, so a literal reading marks the slot
dead and sweeps away the restart. `koki_step_frame()` therefore compares the
slot's generation before and after the step: if it moved, the script
restarted itself, `StopIteration` belongs to an instance that no longer
exists and is not applied, and the frame stack is re-zeroed so the new
instance starts from the top.

This was found by reading the code, not by a test — and then a test was
written for it. Without the fix, 1,628 of 2,200 frames of a Riby fight
diverge from the Python, starting at frame 438. `test_koki.c`'s `jumpatk`
scenario and its `probe_self` unit probe are that regression test.

---

## Counts, measured on this checkout

Driven by instrumenting `register_all()` with a stub engine rather than by
grepping, because grep gets this wrong:

| | value |
| --- | --- |
| `yield` statements in `game.py` | **500** |
| `@eng.on` decorator sites | **243** |
| **handlers actually registered** | **304** |
| distinct events | **107** |
| distinct handler function names | 250 |

The 243 → 304 gap is six registration loops that install the same handler
body against several messages or several sprites: the nine stop-music
messages on the Stage, the four doors' hide/flash/still handlers, the three
death messages on `Cannon ball`, and the five grade screens on `Sprite2`.
`spec-koki.md`'s 304 and 107 are exactly right; a `grep -c "@eng.on"` is not.

Where a Python factory closes over a sprite (`_mk_hide(_d)`), the C is one
handler function that reads `koki_script_sprite()` — the slot already carries
the sprite pointer that `stop_other_scripts` compares against, so no closure
state is needed and no code is duplicated four times. The five grade screens
close over a costume and a music track instead, and the grade IS the message,
so `koki_script_event()` plus a five-row table does the same job there.

Registration order — which decides initial run order and
`stop_other_scripts` grouping — is the textual order of
`koki_register_all()`, exactly as in Python.

## Files

| File | What |
| --- | --- |
| `koki.h` | the engine's whole surface: sprite, engine, script, caches |
| `koki_sched.h` | the protothread macros and `koki_frame` |
| `koki_manifest.h` / `.c` | `assets/manifest.json` → targets, costumes, sounds |
| `koki_cache.c` | `LRUImages`, byte-budgeted, three instances |
| `koki_sprite.c` | sprite state, motion, costumes, sound helpers |
| `koki_render.c` | `_costume_variant`, `render`, `backdrop` |
| `koki_collide.c` | `screen_rect`, `_alpha_mask`, `_paste_origin`, `touching` |
| `koki_sched.c` | handlers, `active`, `broadcast`, the per-frame pass |
| `koki_input.c` | `Input` over the inherited keypad channel |
| `koki_audio.c` | `SoundManager` |
| `koki_rng.c` | CPython-compatible MT19937 |
| `koki_engine.c` | construction, `now()`, the main loop, the pause menu |
| `koki_game.h` | the game's shared declarations |
| `koki_game_boot.c` | Stage, logo, intro, start button, panel, White, Platform, Player, CharacterAnim |
| `koki_game_lobby.c` | lives, health bar, game over, the four doors |
| `koki_game_lv1.c` | Enemy 1 boss |
| `koki_game_lv2.c` | plane vs dragon |
| `koki_game_lv3.c` | Popi chase |
| `koki_game_final.c` | Riby |
| `koki_game_ending.c` | trophy walk and the score screen |
| `app.c` | `app_run()` / `app_shutdown()` |

---

## Sanctioned deviations from the Python

Each one is here because a header or a scope document required it, not
because it was convenient.

1. **The matrix-keypad branch of `Input.poll()` is not ported.**
   `nd_input.h` settles this: the core synthesises press *and* release
   records onto the pipe the app inherits, and states in as many words that
   "Koki's matrix-scanner branch disappears from app code entirely". What
   remains is the evdev drain, which is what the pipe carries. The single-key
   backend's latching-release bug (spec §8, step 3) therefore cannot occur
   and is not reproduced.
2. **The in-process miniaudio mixer is not ported; sound runs through the
   external-player fallback or is disabled.** A `dr_mp3`-class decoder and an
   ALSA writer thread are a new third-party dependency in `lib/`, which this
   task's scope forbids without asking. `/dev/snd` does not exist on the host
   or in QEMU today, so the shipped code path here is the `_disable()` one —
   which is also the path the golden frame was captured through. See
   "What is not done".
3. **`malloc_trim(0)` is `#ifdef __GLIBC__`,** as CODING-STANDARDS and the
   Python comment both anticipate; musl has no equivalent and the process
   exit that replaces it is free.
4. **`main.py`'s `sys.modules` purge has no equivalent and needs none** —
   an app is its own process now (`nd_app.h`), so exit returns everything.
   `koki_engine_teardown()` still runs, because the audio device has to be
   released and the log has to stay honest.

Everything else, including the bugs, is reproduced: `Enemy4Stats` missing
from `set_layer_order` and therefore drawing in front of `White`; the
flip branch multiplying by an unrounded `img.width * scale`; the lobby
backdrop's 16 black rows; half-to-even rounding at every paste origin.

---

## Sanctioned deviations, continued

5. **A C script cannot raise.** engine.py wraps each script step in
   `try/except`, prints a traceback and marks the script dead; C has no
   equivalent and the port has none. It was never reached: over 18,200
   compared frames the Python printed no "script crashed" line, so the
   handler covers a case that does not occur in the shipped game. A C script
   that faulted would take the app process down and get the crash screen,
   which is the architecture's answer (`nd_app.h`) rather than an omission.
6. **`Input.poll()` parses whole records.** The Python reads 24 bytes at a
   time and mis-parses two queued 16-byte records as one 24-byte one. The C
   reads a buffer and walks it in record-sized units, which cannot do that.
   The difference is unobservable on the phone (both ends of the pipe are the
   same word size) and is a latent bug rather than a behaviour.

---

## Status: what runs and what does not

**The whole thing is ported.** All 1,153 lines of `engine.py` and all 2,817
lines of `game.py`: 304 registered handlers over 107 messages, 45 sprites,
every level, both bosses that come back, the game-over screen and all five
grade endings.

### How that is known, rather than believed

Not by playing it and squinting. The Python is still in the tree, PIL 12.3.0
is the version every pixel rule was measured against, and
`neodct/tools/goldenframe.py` makes a run reproducible -- so the C and the
Python can be run side by side on the same input and their frames compared
byte for byte. That is what was done:

| run | frames | input | result |
| --- | --- | --- | --- |
| boot to the title card | 400 | none (the `app-koki` recipe) | **0 differing pixels**, and frame 400 equals `golden/app-koki.png` |
| a scripted play session | 6,000 | 763 press/release events on a fixed schedule, fed through a real pipe on both sides | **0 mismatched frames** |
| `tools/smoke.py`'s six scenarios | 6,400 | its own broadcasts and key holds, RNG seeded 42 | **0 mismatched frames** |
| a coverage sweep | 5,400 | 46 broadcasts spaced through one run | **0 mismatched frames** |
| a long Riby jump-attack fight | 2,200 | one broadcast, then left alone | **0 mismatched frames** (1,628 mismatches before the self-restart fix) |

**20,400 frames compared, zero differences.** Across those runs **304 of 304
handlers ran at least once**, so no script body is unexecuted code.

The comparison is on the SHA-256 of the raw RGB canvas -- `goldenframe.frame_digest`
on one side, `nd_capture_digest` on the other -- which is the same hash the
stored reference set uses.

### What is measured

| | value |
| --- | --- |
| peak concurrent scripts | **32** (44 in the artificial sweep) -- spec-koki.md measured 32 |
| image cache peak | 0.5-2.6 MB depending on the level, inside the 3 MB budget |
| fx cache | sits AT its 1 MB budget in every scene, exactly as the spec predicted |
| mask cache peak | 155 KB, inside the fixed 256 KB |
| script slot table (frame stacks included) | 304 x 416 B = **124 KB**, fixed |
| peak RSS, C | **8.4 MB** for a 5,400-frame session |
| peak RSS, Python, same session | **30.5 MB** |
| RSS after `koki_engine_teardown()` | 4.2 MB, i.e. the caches really do go back |

### Reproducing the comparison

`test/unit/test_koki.c` IS the harness: it is `tools/smoke.py`'s six
scenarios plus one of this port's own, rewritten against the C engine, with
the Python's per-frame digests embedded as the reference. `make test` runs it. To re-cut those digests after
a deliberate change, run the Python side of the same scenarios (a fresh
`engine.Engine` per scenario, `random.seed(42)`, `headless_frames = N`,
`input.poll` replaced by the scripted holds, and
`goldenframe.frame_digest(ui.canvas)` after each `render()`) and paste the
result into `CHECKPOINTS`. Do not re-cut them to make a failure go away.

### `nd-shoot` can stop skipping `app-koki`

`tools/nd_shoot.c:135` lists `app-koki` as "out of scope this session". It no
longer is, and this session could not edit that file — but the frame was
reproduced through the same machinery it uses (a staged root,
`nd_vclock_enable()`, `nd_capture` with a 400-frame budget, `app_run` by
`dlopen`) and comes out **byte-identical to the stored reference**. It was
also checked with `nd_app_set_dir()` NEVER CALLED, which is how nd-shoot
runs an app — `app.c` falls back to `/NeoDCT/System/apps/Koki` and the frame
is the same. Lifting the skip is three edits: delete the `SKIPPED[]` entry,
add `"app-koki"` to `RENDERED[]` (main() checks the two against the frames
actually saved), and add one `run_app_inproc(cap, ui, "Koki Mobile", 400,
"app-koki", NULL, 0u, ND_KEY_NONE, "app_run")` to `shoot_stock_apps()`.

### What is NOT done

- **The in-process audio mixer.** Deviation 2 above: it needs an MP3 decoder
  and an ALSA writer thread in `lib/`, which is a new third-party dependency
  and outside this task's scope. The external-player fallback and both
  disable paths ARE ported, and on the host and in QEMU (no `/dev/snd`) the
  disable path is the one that runs -- which is also how the reference frame
  was captured. Sound reaches no pixel and no timing.
- **The i2c matrix keypad branch.** Deviation 1: `nd_input.h` removes it from
  app code by design.
- **Playing it on real hardware.** Nothing here has run on a phone or in
  QEMU; there was no framebuffer to run against. Every claim above is from
  the host, against the Python.

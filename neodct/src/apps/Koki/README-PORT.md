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
| `koki_mixer.c` | `_MiniaudioMixer`: the voices, the streaming decode and the fold |
| `koki_audio.c` | `SoundManager`: the backend ladder, and the mixer's one `aplay` |
| `koki_audio_priv.h` | the sink's three entry points, for `test_koki_audio.c` |
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
2. ~~**The in-process miniaudio mixer is not ported; sound runs through the
   external-player fallback or is disabled.**~~ **PAID OFF.** It was true
   when this was written — a `dr_mp3`-class decoder was then a new
   third-party dependency — but `lib/vendor/dr_mp3.h` and `dr_wav.h` have
   since been vendored for the ringtone and `lib/nd_notify.c` compiles them,
   so the decoder was already in the image. The mixer is `koki_mixer.c` and
   its sink is in `koki_audio.c`; see "The in-process mixer" at the end of
   this file. The external-player ladder and both disable paths are still
   there, still reached exactly as `engine.py` reaches them.
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

- ~~**The in-process audio mixer.**~~ Done; see "The in-process mixer" at
  the end of this file. What is still not done there is **hearing it**: no
  machine this port has run on has a sound card or an `aplay`, so the sink
  is exercised against a stand-in player and the mix against a reference
  mixer. On the host and in QEMU (no `/dev/snd`) the disable path is still
  the one that runs, which is also how the reference frame was captured, so
  sound continues to reach no pixel and no timing.
- **The i2c matrix keypad branch.** Deviation 1: `nd_input.h` removes it from
  app code by design.
- **Playing it on real hardware.** Nothing here has run on a phone or in
  QEMU; there was no framebuffer to run against. Every claim above is from
  the host, against the Python.

---

# The in-process mixer (0.4.x): music and effects at the same time

Deviation 2 above is now paid off. `koki_audio.c` no longer starts a media
player per sound; it decodes and **mixes in this process** and feeds **one**
`aplay`. This section is the design, written before the code, and it is the
part to read before changing any of the four numbers in it.

## Why there can only be one player

`neodct/overlay/etc/init.d/S17audio`, generated on the real device, is the
whole constraint:

> ALSA's stock "default" mixes through dmix, and dmix needs an ALSA timer
> this kernel does not provide. Opening it fails outright. So the slave here
> is plain **hw** -- one program at a time gets the card.

The game corroborates it twice from the other side, in `engine.py`'s own
comments: `mpg123` "stuttered and reset whenever aplay grabbed the card",
and "two concurrent mpvs OOM'd a 72 MB VM". So overlapping sound cannot be
two processes. It has to be one stream, summed before it leaves us --
which is exactly what `engine.py`'s preferred backend, `_MiniaudioMixer`,
already does. This is a port of THAT, not an invention.

## What replaces what

| was | is |
| --- | --- |
| `mpv` per music track, ~24 MB private RSS, respawned on every change | one voice inside this process, streamed |
| `aplay` per sound effect, fork+exec+ALSA-open per effect | one voice inside this process, no fork |
| up to four processes fighting for a non-mixing `hw` device | one `aplay`, started once, for the app's whole life |

The decoders are `lib/vendor/dr_mp3.h` and `dr_wav.h`, **already vendored and
already compiled** -- `lib/nd_notify.c` is the one translation unit that
defines their implementations and `libneodct` exports the symbols.
`apps/MusicPlayer/audio.c` reuses them exactly this way today. This adds no
third-party code, which is what made the earlier deviation necessary.

## Decision 1 -- latency, which is the owner's actual complaint

The owner says the current audio is "very laggy". Replacing spawn latency
with buffer latency would only move the lag, so the buffer is sized first
and everything else fits around it.

Koki runs at 30 FPS: **33.3 ms per frame**. An effect must land within a
frame or two of the frame that triggered it.

The only thing that delays an effect is audio that has ALREADY been handed
onward, because a sound started now can only be mixed into samples not yet
written. So the design minimises exactly that, and it is the sum of three
queues:

| queue | named constant | frames | ms |
| --- | --- | --- | --- |
| the chunk currently being mixed | `KOKI_MIX_CHUNK_FRAMES` = 128 | 128 | **5.8** |
| our end of the socket to `aplay` | `KOKI_MIX_SOCK_BYTES` = 2048 requested | 512 | **23.2** *(measured)* |
| `aplay`'s ALSA ring | `KOKI_MIX_ALSA_MS` = 30 | 662 | **30.0** |
| | | **total** | **59.0 ms = 1.8 frames** |

At `KOKI_MIX_RATE` = 22050 Hz mono s16, one millisecond is 44.1 bytes, and
`KOKI_MIX_CHUNK_FRAMES` = 128 frames is 256 bytes = 5.805 ms. That chunk is
both the mix granularity and the write size, so a sound started at any
instant is mixed into the next chunk: the quantisation cost is 5.8 ms, not a
buffer.

**The socket number is measured, not assumed.** A default `AF_UNIX`
`SOCK_STREAM` send buffer on Linux is 212,992 bytes, which at 44,100 B/s is
**969 ms** of audio in flight -- nearly a second of lag, and precisely the
trap a naive "just stream it to aplay" falls into. `SO_SNDBUF` is therefore
requested at `KOKI_MIX_SOCK_BYTES`. The kernel clamps `sk_sndbuf` up to its
`SOCK_MIN_SNDBUF` floor (4608 bytes here) and charges each `skb`'s
*truesize*, not its payload, so what a 256-byte write actually gets is:

```
SO_SNDBUF requested 2048 -> sk_sndbuf 4608 -> 1024 bytes of payload = 23.2 ms
```

measured on this kernel with a socketpair and non-blocking writes. The code
reads the value back with `getsockopt` and logs the real figure rather than
the requested one, because that floor is a kernel constant and may differ on
the phone. It estimates payload as `sk_sndbuf / 4`, which on this kernel
gives 1152 bytes against the 1024 a direct measurement gives -- deliberately
the conservative direction, so the reported latency is never flattering.

**Measured end to end** by `test_koki_audio.c`, which starts the real sink
against a stand-in player on `PATH`:

```
socket holds 1152 bytes (26.1 ms), ALSA ring 30 ms, total 62 ms
  = 1.9 frames at 30 FPS
```

and the bytes that reach the player are compared to a reference mixer's
output: **byte-identical**.

`aplay` is given `--buffer-time` = `KOKI_MIX_ALSA_MS` x 1000 and
`--period-time` = one chunk, so its ring is a few periods deep.

**The tunable.** `NEODCT_KOKI_ABUF_MS` -- the same environment variable
`engine.py` already has -- moves `KOKI_MIX_ALSA_MS`, clamped to
`[KOKI_MIX_ALSA_MS_MIN, KOKI_MIX_ALSA_MS_MAX]` = [10, 500]. Note the
Python's default for it is **150 ms**; 30 ms is deliberately five times
tighter, and raising it is the first thing to try if the phone crackles.

**Underrun: what happens, and how we know.** If the feeder is late, `aplay`
runs the ALSA ring dry and ALSA repeats or drops -- audibly, a click. There
is no recovery to attempt: the samples were needed and are gone, and the
stream resumes by itself on the next write, which is what an underrun
already is.

We detect it *from inside the feeder*, without parsing `aplay`'s stderr
(which goes to `/dev/null`, and which would need a second reader thread).
The feeder knows two numbers exactly: `written_ms`, the audio it has handed
over, and `elapsed_ms` since the first byte went out. The card cannot have
consumed more than we wrote, so as long as the pipeline is fed,
`elapsed_ms < written_ms`. When `elapsed_ms > written_ms + KOKI_MIX_ALSA_MS`
the sink has been waiting on us -- the guard term is one ALSA buffer,
because playback really starts a period or so after our first write and
without it every startup would look like an underrun. Each crossing bumps a
counter and logs once:

```
[Koki] audio underrun: the mixer fell N ms behind -- raise NEODCT_KOKI_ABUF_MS
```

`koki_sound_check()`, which the main loop already calls every 30 frames,
reports the counter, so an underrun reaches the serial console the same way
a dead music player does today. `koki_mixer_underruns()` exposes it to the
tests.

## Decision 2 -- how many effects overlap, and what happens to the fourth

**Three**, `KOKI_SND_MAX_SFX`, which is `_MiniaudioMixer.MAX_SFX = 3`, plus
one looping music voice: four voices total.

Beyond three, **the NEW effect is dropped. Nothing is stolen from the
oldest.** That is `engine.py`:

```python
self.voices = [v for v in self.voices if not v.done]
if len(self.voices) < self.MAX_SFX:
    self.voices.append(self._Voice(self, path, False))
```

-- prune finished voices, then append *only if there is room*. There is no
queue and no eviction. It is also what Scratch does, and what the outgoing
subprocess path in this file already did. Cutting an effect off mid-way to
start a newer one would be a change to the game's sound, so it is not made.

Note `NEODCT_KOKI_MAX_SFX` and the "MemTotal < 72 MB drops MAX_SFX to 1"
rule do **not** apply here, and that is also fidelity: in the Python both
live in the subprocess fallback's constructor, after the mixer branch has
already returned. They existed because mpv's cost is per PROCESS. There are
no processes here.

## Decision 3 -- the mix rate, and disagreement between a WAV and an MP3

Output is fixed: **22050 Hz, mono, signed 16-bit**, `KOKI_MIX_RATE`. That is
`_MiniaudioMixer.RATE`, and it is not a guess about the assets -- it is what
they are. Measured over all 57 files in `assets/snd`:

| | count | format |
| --- | --- | --- |
| WAV | 38 | PCM, 1 channel, 22050 Hz, 16-bit -- every one |
| MP3 | 19 | MPEG-2 Layer III, mono, 22050 Hz -- every one |

So nothing "wins" a disagreement, because each voice is converted to the mix
format *before* it reaches the sum. A voice whose file rate differs is
resampled by the same integer linear interpolator `lib/nd_notify.c` already
uses for ringtones: a `frac` accumulator counted in units of 1/22050 of an
output frame and advanced by the source rate, so it is exact integer
arithmetic that cannot drift over a track looped for three minutes -- a
float accumulator does. At 22050 Hz in, `frac` is zero on every output frame
and the loop degenerates to a copy: **every shipped asset is bit-exact, and
no resampling runs at all.** A file with more than one channel is averaged
down to mono, which is what miniaudio's converter does for `nchannels=1`;
no shipped asset needs it either.

## Decision 4 -- clipping

**Saturating 16-bit add, applied PAIRWISE, in voice order.** Per sample:

```
s = a + b   in int32;  s > 32767 -> 32767;  s < -32768 -> -32768
```

This is `audioop.add(a, b, 2)`, and `_MiniaudioMixer._mix`'s pure-Python
fallback spells out the same clamp for the stdlib >= 3.13 case.

Pairwise is load-bearing and is **not** the same as summing everything in a
wider accumulator and clamping once. With three voices at 30000, 30000 and
-30000, pairwise gives `sat(sat(30000+30000) + -30000)` = `sat(32767-30000)`
= **2767**; a single int32 sum gives **30000**. The Python folds voice by
voice (`mixed = data if mixed is None else self._mix(mixed, data)`), so the
port folds voice by voice, and the order it folds in is the Python's too:
**the sfx voices in the order they started, then music last**. With no live
voice the chunk is silence.

There is no per-voice gain and none is added: `engine.py` has none, and
introducing one would change every sound in the game.

## Where the code lives, and why none of it is in `lib/`

| file | what |
| --- | --- |
| `koki_mixer.c` | the mixer proper: voices, streaming decode, resample, the saturating fold, `koki_mixer_pull()`. No thread, no process, no I/O to a player. |
| `koki_audio.c` | `SoundManager` unchanged in shape: the backend ladder, plus the `aplay` sink and the feeder thread that turns `koki_mixer_pull()` into bytes. |
| `koki_audio_priv.h` | three sink entry points, for the test only. `lib/`'s own `nd_*_priv.h` pattern: the sink has no business in `koki.h`, which is what the game is written against, but it is also the half most able to go wrong on the phone and it cannot be reached through `koki_sound_open()` on a machine with no `/dev/snd`. |

A `lib/nd_mixer.c` was permitted and is **not** taken. Nothing else in the
system mixes: `nd_notify.c` plays one ringtone, `apps/MusicPlayer` plays one
track, `nd_modem_audio.c` bridges a call -- and the phone's own rule is that
those never overlap anyway ("a ringtone and a call are not supposed to
overlap", S17audio). A shared mixer would be a new public surface in a
library every process maps, with exactly one caller, to hold semantics that
are `engine.py`'s and not the system's: 22050 mono, three voices, drop the
fourth, fold pairwise. When a second caller appears, `koki_mixer.c` is
thirty minutes from being that library. Today it would be speculative
generality paid for in every process's mapping.

Splitting the mixer away from the sink is what makes it testable on a
machine with no sound card: `koki_mixer_pull()` is a pure function of the
voices and returns samples to the caller's buffer, so a unit test can start
music and three effects from the real shipped assets and check the output
sample by sample against the individual voices. That is how "they genuinely
overlap" is demonstrated here rather than asserted.

## Memory

Streaming, never whole-file: the longest track is 183.07 s, which is 8.1 MB
decoded (risk R-9) and 4 KB at a time here.

| | bytes |
| --- | --- |
| `drmp3` decoder state, per MP3 voice | 32,376 *(measured, `sizeof`)* |
| `drwav` decoder state, per WAV voice | 408 *(measured)* |
| source staging, per voice: `KOKI_MIX_STAGE_FRAMES` 1024 x 2 ch x 2 B | 4,096 |
| the mixer's own chunk buffers (accumulator + one voice scratch) | 512 |
| **peak, 4 live voices, worst case all MP3** | **~146 KB** |
| **measured**, 3 WAV effects over 1 MP3 track, `/proc/self/statm` | **68 KB** |

The measurement is lower than the table because only music is an MP3 in
practice -- 38 of the 57 assets are WAV and `drwav`'s state is 408 bytes
against `drmp3`'s 32 KB. `test_koki_audio.c` prints the figure on every run
and fails if it passes half a megabyte, which is the ceiling that catches
"somebody decoded the whole 183-second track".

Voice state is heap-allocated when a sound starts and freed when it ends, so
a silent game holds none of it, and the cap is the four slots. That sits
inside `spec-koki.md`'s own ~200 KB audio line, against the ~24 MB it
replaces.

## Fork safety

CODING-STANDARDS 1.1: a `fork()` from a threaded process is only safe
because the child does nothing but `execve`. The ordering here keeps even
that off the table -- **`aplay` is spawned BEFORE the feeder thread is
created, exactly once, and the mixer never forks again**. If the mixer
starts, the process has one extra thread and no further children. If it
fails to start, it tears the thread down before returning, and only then may
the subprocess ladder fork. The two paths are never both live.

A consequence, stated plainly: if `aplay` dies mid-game the mixer stops and
logs, and the game continues silent. It does not fall back to spawning
players, because that would fork with the feeder thread running. This is the
same outcome the current code has when the music player dies.

## What `app_shutdown()` does

Unchanged in shape and still the contract in `nd_app.h`: `app_shutdown()`
-> `koki_engine_teardown()` -> `koki_sound_shutdown()`, which stops the
feeder, terminates `aplay` and closes the socket, in the order
`lib/nd_notify.c` establishes and for its reasons -- kill the player first
so a blocked `send()` returns, shut our end down as a second way out, then
**join, and only then close** the descriptor, because closing a descriptor
another thread is sitting in `send()` on is how a number gets recycled
underneath it. It is idempotent, so the normal path calling teardown first
costs nothing.

## What is NOT verified

**There is no sound card on the machine this was written on**, and no
`aplay` binary either. What is verified here is the arithmetic and the
plumbing: the saturating fold, the fold order, the resampler, the voice
policy, the latency and underrun sums, that four voices genuinely appear in
one stream, that the bytes leaving the socket are exactly those four voices
mixed, and the resident memory. Whether it *sounds* right -- whether 30 ms
of ALSA ring is enough on the phone's USB card, whether a real `aplay`
accepts these arguments, whether the effects feel on-time to the person
holding it -- has not been and cannot be checked here.

The first thing to do on the phone is run with `NEODCT_KOKI_SOUND_DEBUG=1`,
so `aplay`'s own complaints reach the console, and watch for the underrun
line. If it appears, raise `NEODCT_KOKI_ABUF_MS` before changing anything in
the code. `NEODCT_KOKI_AUDIO=subprocess` puts the old external players back
without a rebuild, which is the escape hatch if the mixer turns out to be
wrong on hardware.

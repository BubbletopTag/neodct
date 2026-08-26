# Koki game (engine + game) — C port specification

Source of truth:

| File | LOC | Role |
| --- | --- | --- |
| `neodct/overlay/NeoDCT/System/apps/Koki/main.py` | 43 | app entry point (`run(ui)`) |
| `neodct/overlay/NeoDCT/System/apps/Koki/engine.py` | 1153 | Scratch-3-like runtime |
| `neodct/overlay/NeoDCT/System/apps/Koki/game.py` | 2817 | the game itself, 304 scripts |
| `neodct/overlay/NeoDCT/System/apps/Koki/tools/build_assets.py` | 266 | host-side asset baker (never runs on device) |
| `neodct/overlay/NeoDCT/System/apps/Koki/tools/harness.py` | 134 | host-side headless runner |
| `neodct/overlay/NeoDCT/System/apps/Koki/tools/smoke.py` | 149 | host-side per-level smoke runner |
| `neodct/overlay/NeoDCT/System/apps/Koki/manifest.json` | 6 | app manifest (`id: "10"`, name `Koki Mobile`) |
| `neodct/overlay/NeoDCT/System/apps/Koki/assets/manifest.json` | — | 52 KB baked asset table, 47 targets |
| `neodct/overlay/NeoDCT/System/apps/Koki/assets/img/*.png` | — | 235 pre-scaled PNGs, 1.4 MB on disk, **8.80 MB fully decoded as RGBA** |
| `neodct/overlay/NeoDCT/System/apps/Koki/assets/snd/*` | — | 57 files, 13.2 MB (40 WAV sfx + 17 MP3 music) |
| `neodct/overlay/NeoDCT/System/apps/Koki/icon.png` | — | 120×115 RGBA launcher icon |
| `docs/KOKI_PORT_NOTES.md` | — | prior memory work and sanctioned deviations |

Background reading that constrains this port: `docs/c-rewrite/ARCHITECTURE.md`
(apps are separate processes, `nd-apprun` + `app.so`), `docs/c-rewrite/CODING-STANDARDS.md`,
and `neodct/tools/goldenframe.py` (the pixel oracle — Koki is one of its captured frames,
`app-koki`).

Every number below was read out of the Python or **measured** on this checkout.
Measured values are marked *(measured)*.

---

## What this does (plain English)

Koki is a small platform game — a boss-rush, four levels and a final boss — that a friend
of the project originally made in **Scratch**, the block-based kids' programming language.
Somebody decompiled that Scratch project and rewrote it, block for block, as two Python
files. It is 22% of all the code in NeoDCT, and it is the only thing on the phone that
draws a moving picture thirty times a second.

There are two halves.

**`engine.py` is a tiny copy of Scratch itself.** Scratch has a very particular way of
working, and the game only behaves correctly if you copy that way of working exactly:

- The world is a **stage** 480 × 360 units wide, with (0,0) in the middle, x growing
  right and y growing **up**. Our screen is 240 × 175, so everything is drawn at half
  size and a sliver is cropped off the top and bottom.
- Everything on screen is a **sprite**: a named thing with a position, a direction, a
  size percentage, a visible/hidden flag, two "effects" (ghost = transparency,
  brightness = lighten/darken), and a list of **costumes** (pictures). "Set costume to
  costume7" is how all animation is done.
- Sprites run **scripts**. A script is a list of instructions that runs a bit at a time —
  "walk forward, wait half a second, change costume, wait again". Scratch runs one step of
  every script each frame, then draws the screen, then does it again. In Python this is
  written with *generators*: a function that can pause in the middle (`yield`) and be
  resumed next frame. In C there are no generators, and reproducing this is the single
  hardest part of the port.
- Scripts talk to each other by **broadcasting a message**. `broadcast("startlv1")` wakes
  up every script that is listening for `"startlv1"`. There are 107 different messages and
  304 listening scripts. Crucially, in Scratch, broadcasting a message that is *already
  running* **restarts it from the top** — the engine copies that, and the game depends on
  it constantly.
- Two sprites "touch" when their actual visible pixels overlap — not their rectangles.
  Scratch costumes usually have a lot of empty transparent space around the drawing, so
  rectangle collision made the boss kill you before he reached you. The engine therefore
  keeps, for each picture, a black-and-white stencil of which pixels are solid, and
  multiplies two stencils together to see whether anything survives.

**`game.py` is the game.** It has no cleverness in it at all: it is 304 hand-transcribed
scripts, one per Scratch script, full of exact numbers — "glide to x=173, y=-48 over 0.3
seconds", "wait 0.05", "if the health bar reaches the costume called Oof2, broadcast
'enemy1 oof'". Porting it means copying every one of those numbers.

**What the player sees.** The Dynaris logo fades in and animates, then a bobbing Koki
title with music, then a flashing START. Enter shows a controls panel, Enter again drops
you in the lobby: a side-view room with four doors. You walk with 4/6, jump with 5, and
stand on a door and press 0 to enter it. Door 1 is a boss fight against a bomb-throwing
blob; door 2 is a flying section on a biplane against a dragon; door 3 is a running chase
away from a popcorn machine; door 4 is the final boss, an evil version of Koki called
Riby. Beat him and you walk off with a trophy and get graded A–F on how well you did,
each grade with its own picture and its own music track. Run out of lives and you get a
GAME OVER screen.

**Why this is the hardest thing to port.** Not the game logic — that is long but
mechanical. It is that C has nothing that behaves like a Python generator, and 304 scripts
depend on being able to stop mid-sentence and carry on next frame. Section
["The scheduler"](#4-the-script-scheduler) sets out exactly how to build that in C
without rewriting the game's structure.

---

## Files and where they go in C

| Python | C destination |
| --- | --- |
| `apps/Koki/main.py` | `apps/Koki/src/app.c` — `app_run()`, the `nd-apprun` entry point |
| `apps/Koki/engine.py` (runtime) | `apps/Koki/src/koki_engine.c/.h`, `koki_sprite.c`, `koki_render.c`, `koki_collide.c`, `koki_sched.c` |
| `apps/Koki/engine.py` (`LRUImages`) | `apps/Koki/src/koki_cache.c/.h` |
| `apps/Koki/engine.py` (`Input`) | `apps/Koki/src/koki_input.c/.h` |
| `apps/Koki/engine.py` (`SoundManager`, `_MiniaudioMixer`) | `apps/Koki/src/koki_audio.c/.h` |
| `apps/Koki/engine.py` (`random.Random`) | `libneodct`: `nd_mt19937.c/.h` (CPython-compatible) |
| PIL `Image.open` on the 235 PNGs | `libneodct`: `nd_png.c` (shared with the rest of the OS) |
| PIL `paste`/`point`/`resize`/`crop`/`ImageChops.multiply` | `libneodct`: `nd_image.c`, `nd_draw.c` (shared rasterizer) |
| `assets/manifest.json` parsing | `apps/Koki/src/koki_manifest.c/.h` + `libneodct`'s JSON reader |
| `apps/Koki/game.py` | seven files: `koki_game_boot.c`, `koki_game_lobby.c`, `koki_game_lv1.c`, `koki_game_lv2.c`, `koki_game_lv3.c`, `koki_game_final.c`, `koki_game_ending.c`, plus shared `koki_game_common.c/.h` |
| `tools/build_assets.py` | **stays Python.** Host-only, needs `rsvg-convert`/`ffmpeg`/ImageMagick, never runs on the device. Do not port. |
| `tools/harness.py`, `tools/smoke.py` | `apps/Koki/test/koki_harness.c` — a host build of the engine with a stub `ui`, scripted keys and PNG dumps. Needed as the port oracle. |
| `assets/**` | **unchanged, byte for byte.** The baked PNGs and the manifest are the contract between the builder and the runtime; re-baking would change pixels. |

The assets stay where they are: `/NeoDCT/System/apps/Koki/assets/`. The app directory
keeps `manifest.json` (`{"name":"Koki Mobile","id":"10","icon":"icon.png","exec":"main.py"}`)
— `exec` becomes `app.so` under the new app contract, everything else is unchanged.

---

## Behaviour that must be reproduced exactly

### 0. Constants

```c
#define KOKI_STAGE_SCALE   0.5      /* 480x360 stage -> 240x180, cropped to 175 */
#define KOKI_SCREEN_W      240
#define KOKI_SCREEN_H      175
#define KOKI_CENTER_X      120.0    /* SCREEN_W / 2.0 */
#define KOKI_CENTER_Y      87.5     /* SCREEN_H / 2.0 */
#define KOKI_FPS           30
#define KOKI_FRAME_DT      (1.0/30.0)
#define KOKI_ALPHA_SOLID   40       /* alpha > 40 counts as solid for collision */
```

`ALPHA_THRESH_LUT[v] = (v > 40) ? 255 : 0`, `IDENTITY_LUT[v] = v`.

Playability knobs read once in `register_all()`:

```
ATK     = float(getenv("NEODCT_KOKI_ATTACK_SLOW") or "1.35")
          if parse fails -> 1.35;  if not (ATK > 0) -> 1.35   (rejects 0, negatives, NaN)
IFRAMES = 0.9   (seconds of post-hit invincibility; not configurable)
```

`ATK > 1` slows attacks down. `ATK` multiplies glide durations and **divides** per-frame
step sizes. Every use is listed in the game-logic section.

### 1. Coordinate system and geometry

Screen position of a sprite's costume top-left corner (the "paste origin"):

```
px_f = CENTER_X + sprite.x * 0.5 - cx
py_f = CENTER_Y - sprite.y * 0.5 - cy
```

where `cx`,`cy` are the costume's rotation centre **already scaled at bake time**
(they come out of `assets/manifest.json` as floats). The final integer paste position is

```
px = (int) round_half_to_even(px_f)
py = (int) round_half_to_even(py_f)
```

**This is Python's `round()`, which rounds halves to even, not away from zero.** It is
observable: `CharacterAnim/costume2` has `cy = 19.5`, so `py_f = 68 - y*0.5`; at `y = -75`
that is `105.5 → 106`, at `y = -73` it is `104.5 → 104`. Using `lrint()` with the default
rounding mode (`FE_TONEAREST`) gives the same answer; using `floor(v+0.5)` does not.
Do **not** use `(int)(v+0.5)`.

Sprites are never rotated. `rotation_style` is only ever `"all around"` (rendered
unrotated) or `"left-right"` (rendered mirrored when `direction < 0`). `direction` is
otherwise ignored by the renderer. `point_towards()` computes a real angle but the only
sprite that uses it (`EvilCannon`) is `left-right`, so only the sign matters.

### 2. Asset manifest schema (`assets/manifest.json`)

```jsonc
{
  "stage_scale": 0.5,
  "targets": {
    "<target name>": {
      "size":            <int>,     // the % the costumes were BAKED at ("baked_size")
      "default_size":    <int>,     // the sprite's runtime starting size %
      "x": <float>, "y": <float>,   // editor-left pose from the sb3
      "direction": <float>,
      "rotation_style": "all around" | "left-right",
      "current_costume": <int>,     // 0-based index
      "costumes": [
        { "name": "<costume name>",
          "img":  "img/<md5>@<scale*10000>[c].png",
          "cx": <float>, "cy": <float>,        // scaled rotation centre
          "bbox": [x0,y0,x1,y1] | null }       // visible-pixel box, null = whole image
      ],
      "sounds": { "<sound name>": { "file": "snd/<md5>.wav|.mp3", "dur": <float seconds> } }
    }
  }
}
```

47 targets: `Stage` plus 46 sprites. **45 sprites are instantiated by the game; the target
named `Stuff` is never referenced and must not be created** (creating it would add a
sprite to the draw list). `Stage` is not a sprite — it holds the six backdrops and eleven
music/sfx tracks.

Costume-name lookup is **case-insensitive** and resolves to the **first** costume with
that lower-cased name (`setdefault`, so duplicates keep the earliest index).
`next_costume()` steps through the manifest order, which is the sb3 order and is **not**
alphabetical — e.g. `Enemy4Stats` is
`costume1, costume9, costume10, costume11, costume12, costume13, costume14, costume8,
costume2, costume3, costume4, costume5, costume6, costume7, Oof2` (15 entries, so 14
damage steps).

PNG facts *(measured)*: 234 files are 8-bit RGBA non-interlaced (colour type 6), one
(`backdrop2`) is 8-bit RGB (colour type 2). All carry `bKGD` and most carry `cHRM`/`tEXt`,
which Pillow ignores; the C decoder must ignore them too (no gamma correction, no
background compositing). No `tRNS`, no palettes, no interlacing, no 16-bit.

### 3. Sprite model

```c
typedef struct {
    const char *name;
    const char *img_path;      /* relative to assets/ */
    float cx, cy;
    int   has_bbox;
    int   bx0, by0, bx1, by1;  /* valid iff has_bbox */
} koki_costume;

typedef struct {
    const char   *name;
    koki_costume *costumes;  int n_costumes;
    /* sounds table: name -> {file, dur} */
    int    baked_size;        /* manifest "size" */
    double x, y;              /* stage units, float */
    double direction;         /* degrees, Scratch convention: 90 = right */
    int    rotation_style;    /* ALL_AROUND | LEFT_RIGHT */
    int    visible;           /* ALWAYS starts 0 */
    double size;              /* percent; starts at manifest "default_size" */
    double ghost;             /* 0..100 (+) */
    double brightness;        /* -100..100 */
    int    costume_i;         /* manifest "current_costume" % n_costumes */
    double sy;                /* Player only: vertical velocity. Init 0. */
} koki_sprite;
```

Initial state: `x`, `y`, `direction`, `rotation_style`, `costume_i` from the manifest;
`size = default_size`; `ghost = 0`; `brightness = 0`; **`visible = 0` always**, regardless
of the manifest (the original's "when flag clicked: hide").

Motion helpers:

- `goto(x,y)`, `goto_sprite(other)` — plain assignment of doubles.
- `point(d)` — `direction = d`.
- `point_towards(o)` — `direction = degrees(atan2(o.x - x, o.y - y))` (Scratch's
  swapped-argument atan2).
- `move_steps(s)` — `x += s*sin(radians(direction)); y += s*cos(radians(direction))`.
- `clear_fx()` — `ghost = 0; brightness = 0`.

### 4. The script scheduler

This is the part with no C equivalent, and it must be reproduced *exactly*, including its
frame-latency quirks.

#### 4.1 Registration

`eng.on(event, sprite)` appends `(sprite, fn, key)` to `handlers[event]`, where `key` is a
monotonically increasing integer (`_hkey`, starting at 1) assigned in **registration
order**. `sprite` may be `NULL` (Stage-level handlers). Registration order is the textual
order of `register_all()` and is load-bearing (it determines the initial run order and
`stop_other_scripts` grouping).

Measured totals: **107 distinct events, 304 registered handlers.** The complete
registration table is reproduced in the game-logic section; the C port must register in
exactly that order.

#### 4.2 `broadcast(event)`

```
for (sprite, fn, key) in handlers[event]:      # registration order
    old = active[key]
    if old:  old.dead = true                   # Scratch: a broadcast RESTARTS a script
    active[key] = new Script(key, sprite, fn)   # dict-insert semantics: see below
```

`active` is an **insertion-ordered map keyed by `key`**. Assigning to a key that is
already present keeps its existing position; assigning to a key that is absent appends it
at the end. This is Python `dict` ordering and it decides script execution order, so it
must be reproduced with an ordered structure (a doubly linked list of slots plus an index
by key), not a plain array scan.

#### 4.3 The per-frame pass

```
snapshot = list(active.values())            # taken before stepping anything
for sc in snapshot:
    if sc.dead: continue
    current = sc
    step sc one "yield"                     # StopIteration -> sc.dead = true
                                            # any error      -> log + sc.dead = true
current = NULL
for key in [k for k,sc in active if sc.dead]:
    if active[key].dead: del active[key]    # a restart may have replaced it already
render()
```

Consequences that the game relies on and the C port must keep:

1. **Every broadcast has one frame of latency.** A script created during frame *N* is not
   in frame *N*'s snapshot, so its first statement runs during frame *N+1*, before frame
   *N+1*'s render. This includes the "instant" handlers (the ones written as
   `... ; return; yield`): setting a backdrop or hiding a sprite in response to a
   broadcast lands one frame later.
2. Restarting a script that has already been stepped this frame does not step the new
   instance this frame either.
3. A dead script is removed from `active`; if it is broadcast again later it is
   re-inserted **at the end**, changing the run order. Order is therefore dynamic.
4. `stop_other_scripts(sprite)` marks `dead` every script whose `sprite` pointer equals
   the given sprite **except the currently running one**. It does not remove them from
   `active` (that happens in the sweep). Note it compares the sprite, so it kills *all*
   handlers on that sprite, including ones started by different messages.
5. `stop_all_scripts()` marks everything dead (used by the smoke harness, not by the game).
6. `start_flag()` is `broadcast("flag")`, issued once at the top of `run()`.

#### 4.4 Implementing generators in C

Recommended approach: **protothreads with an explicit per-script context struct.**
Do not use `ucontext`/`makecontext` (32 concurrent scripts *(measured peak)* × a stack each
is 256–512 KB of dirty pages on an 8 MB budget), and do not hand-write 304 state machines.

```c
/* koki_sched.h */
typedef enum { KOKI_YIELDED = 0, KOKI_DONE = 1 } koki_step;

#define KOKI_BEGIN(ctx)      switch ((ctx)->pc) { case 0:
#define KOKI_YIELD(ctx)      do { (ctx)->pc = __LINE__; return KOKI_YIELDED; \
                                  case __LINE__:; } while (0)
#define KOKI_END(ctx)        } (ctx)->pc = -1; return KOKI_DONE;
```

Rules that follow from this and must be in the reviewer's checklist:

- **No C stack locals survive a `KOKI_YIELD`.** Every loop counter, saved coordinate and
  temporary that lives across a yield goes in the script's context struct.
- A `switch` inside a script body must not contain a `KOKI_YIELD` unless it is written as
  a nested protothread (Duff's device does not nest). All 304 scripts are `if`/`while`/
  `for` only, so this is satisfiable.
- `yield from <sub-generator>` is a **child protothread**: the parent stores the child's
  context inline and calls it until it returns `KOKI_DONE`.

  ```c
  #define KOKI_CALL(ctx, child_fn, child_ctx) \
      do { (child_ctx)->pc = 0; \
           (ctx)->pc = __LINE__; case __LINE__: \
           if (child_fn(child_ctx) == KOKI_YIELDED) return KOKI_YIELDED; \
      } while (0)
  ```

  Nesting depth needed *(measured from the Python)* is **3**: e.g.
  `_riby_damaged → _riby_dash_sweeps → _riby_laugh → wait`. Size the context structs for
  depth 4 to be safe.
- The engine's own generators — `wait`, `wait_until`, `glide`, `glide_to_sprite`,
  `play_until_done` — become child protothreads with their own tiny contexts.

Per-script context allocation: 304 possible scripts, 32 live *(measured)*. Allocate the
context inline in the `Script` object from a fixed pool of, say, 64 slots of 128 bytes;
a script whose context does not fit is a build-time error, not a runtime one.

### 5. Time

```
Engine.now() = (headless ? _vtime : monotonic_clock_seconds())
```

- `_vtime` is only non-NULL in the headless harness. `run()` sets `_vtime = 0.0` **before**
  `start_flag()`, and adds `FRAME_DT` at the **end** of every frame. So within a frame all
  scripts see one timestamp.
- On device, `now()` is `time.monotonic()` read at the instant the script step runs, so two
  scripts in the same frame can see slightly different times. Reproduce with
  `clock_gettime(CLOCK_MONOTONIC)` per call.

`wait(secs)`:

```
end = now() + secs
loop: yield; if now() >= end: return
```

**It always yields at least once**, so `W(0.01)` is one frame and `W(0)` is one frame.
At 30 fps `W(0.05)` is two frames (0.0333 < 0.05 ≤ 0.0667).

`wait_until(pred)`: `while not pred(): yield` — evaluates the predicate first, so it can
complete with zero yields (and therefore run to the caller's next statement in the same
frame).

`glide(secs, tx, ty)`:

```
x0,y0 = x,y ; tx,ty = float(tx),float(ty) ; t0 = now()
loop:
    yield
    k = (secs > 0) ? (now() - t0) / secs : 1.0
    if k >= 1.0: x,y = tx,ty ; return
    x = x0 + (tx-x0)*k ;  y = y0 + (ty-y0)*k
```

Note `t0` is captured on the **first step**, not at creation, and the first step moves
nothing. `glide_to_sprite(secs, other)` samples `other.x, other.y` **once, at the first
step** (Scratch semantics), then behaves as `glide`.

### 6. Rendering

```
render():
    if backdrop_img: canvas.paste(backdrop_img, (0,0))     # 240x175 RGB, opaque copy
    else:            fill canvas with black
    for s in layers:                                       # back -> front
        if !s.visible or s.ghost >= 95: continue
        (img, cx, cy) = costume_variant(s)
        px = round_half_even(CENTER_X + s.x*0.5 - cx)
        py = round_half_even(CENTER_Y - s.y*0.5 - cy)
        if px > 240 or py > 175 or px + img.w < 0 or py + img.h < 0: continue
        canvas.paste(img, (px,py), mask=img)               # alpha blend, clipped
    ui.fb.update(canvas)
```

*(measured)* worst case is **12 sprites drawn in one frame** (final-boss fight); the
average during play is 5–8.

#### 6.1 `costume_variant` — the effect/flip/scale cache

```
c   = s.costumes[s.costume_i]
img = load_image(c.img_path)                     /* RGBA, from the LRU img cache */

flip    = (s.rotation_style == LEFT_RIGHT && s.direction < 0)
size_q  = (int) round_half_even(s.size / s.baked_size * 20)         /* 5% steps */
ghost_q = clamp(0, 90, ((int) floor(max(0.0, s.ghost) / 10)) * 10)  /* 10% steps */
bri_q   = clamp(-100, 100, ((int) floor(s.brightness / 25)) * 25)   /* 25% steps */

if (!flip && size_q == 20 && ghost_q == 0 && bri_q == 0)
    return (img, c.cx, c.cy);                    /* fast path, no copy */

key   = (c.img_path, size_q, flip, ghost_q, bri_q)
scale = size_q / 20.0
cx = c.cx; cy = c.cy
if (scale != 1.0) { cx *= scale; cy *= scale; }
if (flip)         { cx = img.width * scale - cx; }     /* NOTE: unrounded float width */
if (cached = fx_cache.get(key)) return (cached, cx, cy);

out = img
if (scale != 1.0)
    out = resize_nearest(out, max(1,(int)(out.w*scale)), max(1,(int)(out.h*scale)))
if (flip)
    out = flip_left_right(out)
if (bri_q) { add = (int)(2.55 * bri_q);
             lut[v] = clamp(0,255, v + add);  out = point(out, lut,lut,lut, identity); }
if (ghost_q) { f = (100 - ghost_q)/100.0;
               lut[v] = (int)(v * f);         out = point(out, identity×3, lut); }
fx_cache.put(key, out)
return (out, cx, cy)
```

Details that are easy to get wrong:

- `ghost_q` and `bri_q` use **floor** division, not truncation. `brightness = -10` gives
  `floor(-10/25) = -1 → -25`, not `0`. `brightness = -50 → -50`. `ghost = 94.9 → 90`.
- `size_q` uses Python `round()` (half-to-even). `size = 2, baked = 100` gives
  `0.02*20 = 0.4 → 0`, so `scale = 0.0` and the image is resized to **1×1** with
  `cx = cy = 0`. This actually happens: `intro` starts at `size = 2`.
- The flip branch multiplies by the **unrounded** `img.width * scale`, while the resized
  image is `max(1, (int)(img.width * scale))` pixels wide. Keep the discrepancy.
- `(int)` on a float in Python truncates toward zero. `max(1, int(w*scale))`.
- The brightness LUT add is `(int)(2.55 * bri_q)` — truncated, so `bri_q = 50 → 127`,
  `bri_q = -50 → -127` (C's `(int)` on a negative float also truncates toward zero, same
  as Python's `int()`), `bri_q = 100 → 255`, `bri_q = 25 → 63`.
- The ghost LUT is `(int)(v * f)` — truncated. `ghost_q = 50 → f = 0.5 → v/2` truncated.
- Brightness is applied to R,G,B only (alpha untouched); ghost is applied to A only. When
  both are set, brightness first, then ghost, as two separate LUT passes over the whole
  image.
- The variant cache is keyed on the **path**, not the sprite, so two sprites sharing a
  costume file share cache entries.

**Permitted optimisation:** the fx cache is a pure speed cache; the same pixels come out
if the LUTs and the nearest-neighbour lookup are folded into the blend loop. Folding is
recommended (it removes a 0.5–1 MB cache) *provided* the arithmetic order is preserved:
nearest-sample, then brightness LUT, then ghost LUT, then blend. `cx`/`cy` must still be
computed by the formula above.

#### 6.2 Alpha blend — the exact formula *(measured against Pillow 12.3.0)*

`canvas.paste(src_rgba, (px,py), mask=src_rgba)` composites RGB destination with RGBA
source using the source's own alpha, with **one rounding at the end**:

```
out = (dst * (255 - a) + src * a + 127) / 255      /* integer division */
```

This was verified exhaustively over sampled `(dst, src, alpha)` triples. It is **not** the
two-`MULDIV255` form found in older Pillow (`MULDIV255(dst,255-a) + MULDIV255(src,a)`),
which differs for e.g. `a=1, dst=1, src=128` (gives 2 where Pillow gives 1). Use the
single-rounding form.

Source RGB is used as-is (not premultiplied). The paste is clipped to the canvas on all
four sides; `px`/`py` may be negative.

#### 6.3 Nearest-neighbour resize *(measured)*

```
src_x = min(src_w - 1, (int)((dst_x + 0.5) * src_w / dst_w))
src_y = min(src_h - 1, (int)((dst_y + 0.5) * src_h / dst_h))
```

Verified for up- and down-scales. Not `floor(dst_x * src_w / dst_w)`.

#### 6.4 Layer order

`eng.sprite(name)` creates the sprite on first use and appends it to `layers`.
`set_layer_order(names)` then does a **stable** sort by the index of each sprite's name in
`names`, with **999** for names not listed. The order passed by `game.py` (back → front):

```
Door4, Platform, Door1, Door2, PlaneChar, Door3, cutscene1, Cannon ball, Cannon ball2,
Abyss, intro, StartButton, Enemy1Stats, KokiPlaneStats, KokiStats, Sprite1, Shockwave2,
Shockwave5, Shockwave4, Shockwave3, Shockwave, Cannon, QuickPress, Enemy2, Cannon2,
Gas tank, Lives, Enemy 3, Player, Cannon3, Enemy3Stats, CharacterAnim, GameOver,
Enemy 1, Riby, A to Dodge, Enemy2Stats, EvilCannon, Cannon ball4, Cannon ball3,
Reward, Sprite2, Dynaris Logo, White
```

That is 44 names for 45 sprites: **`Enemy4Stats` is missing from the list**, gets index
999, and therefore renders **in front of everything**, including `White`. Reproduce this;
it is visible on the final-boss screen.

`front()` removes the sprite and appends it; `back()` removes it and inserts at index 0.

#### 6.5 Backdrops

```
backdrop(name):                       /* case-insensitive match against Stage costumes */
    img = decode(costume.img) as RGB          /* NOT RGBA, and NOT in the img cache */
    left = round_half_even(costume.cx - 120.0)
    top  = round_half_even(costume.cy - 87.5)
    left = clamp(0, max(0, img.w - 240), left)
    top  = clamp(0, max(0, img.h - 175), top)
    backdrop_img = img.crop(left, top, left+240, top+175)   /* out-of-bounds -> BLACK */
    backdrop_name = costume.name
```

The crop can run past the image, and PIL fills the overhang with zeros (black). The six
backdrops resolve to *(measured)*:

| backdrop | file size | cx, cy | crop box | black padding |
| --- | --- | --- | --- | --- |
| `backdrop1` (title) | 255×191 | 127.250, 95.500 | (7, 8, 247, 183) | none |
| `backdrop2` (intro / ending) | 244×183 | 122.000, 91.500 | (2, 4, 242, 179) | none |
| `backdrop3` (level 1) | 286×193 | 139.281, 100.638 | (19, 13, 259, 188) | none |
| `backdrop4` (level 2) | 271×201 | 133.355, 96.605 | (13, 9, 253, 184) | none |
| `backdrop5` (lobby / final) | 245×159 | 123.000, 90.500 | (3, 0, 243, 175) | **bottom 16 rows black** |
| `backdrop6` (level 3) | 295×202 | 134.395, 105.327 | (14, 18, 254, 193) | none |

The lobby's 16 black rows at the bottom are real on-screen output, not a bug to fix.

An unknown backdrop name prints `[Koki] unknown backdrop <name>` and leaves the current
backdrop alone.

### 7. Collision

`touching(other, inset=0.0)`:

```
if (!self.visible || !other.visible) return false;
a = self.screen_rect(inset);  b = other.screen_rect(inset);
ox0 = max(a.l, b.l); oy0 = max(a.t, b.t);
ox1 = min(a.r, b.r); oy1 = min(a.b, b.b);
if (ox0 >= ox1 || oy0 >= oy1) return false;
if (self.size != self.baked_size || other.size != other.baked_size)
    return true;                                  /* runtime-scaled: rect result only */
ma = self.alpha_mask();  mb = other.alpha_mask();
(ax,ay) = self.paste_origin();  (bx,by) = other.paste_origin();
ca = ma.crop((int)(ox0-ax), (int)(oy0-ay), (int)(ox1-ax)+1, (int)(oy1-ay)+1);
cb = mb.crop((int)(ox0-bx), (int)(oy0-by), (int)(ox1-bx)+1, (int)(oy1-by)+1);
if (ca.size != cb.size) { w = min(ca.w,cb.w); h = min(ca.h,cb.h);
                          ca = ca.crop(0,0,w,h); cb = cb.crop(0,0,w,h); }
return multiply(ca, cb).getbbox() != NULL;        /* i.e. any pixel non-zero */
```

`screen_rect(inset)` — the box of the **visible pixels**, in screen coordinates:

```
c  = costumes[costume_i];  img = load_image(c.img_path);  iw = img.width
(bx0,by0,bx1,by1) = c.bbox ? c.bbox : (0,0,iw,img.height)
cx = c.cx
if (rotation_style == LEFT_RIGHT && direction < 0) {
    cx  = iw - cx;
    tmp = bx0; bx0 = iw - bx1; bx1 = iw - tmp;
}
scale = size / baked_size
left = CENTER_X + x*0.5 + (bx0 - cx)*scale
top  = CENTER_Y - y*0.5 + (by0 - cy)*scale
w    = (bx1 - bx0)*scale
h    = (by1 - by0)*scale
if (inset) { dx = w*inset/2; dy = h*inset/2;
             return (left+dx, top+dy, left+w-dx, top+h-dy); }
return (left, top, left+w, top+h)
```

`paste_origin()` — top-left of the **whole** costume image, flip-aware, **assuming
scale 1** (only reached when both sprites are at baked size):

```
cx = c.cx;  if (flip) cx = img.width - cx;
return (CENTER_X + x*0.5 - cx,  CENTER_Y - y*0.5 - c.cy)
```

`alpha_mask()` — the costume's alpha channel, flipped if needed, thresholded with
`ALPHA_THRESH_LUT`, cached by `(img_path, flip)` in the 256 KB mask cache:

```
a = alpha_channel(load_image(path))          /* 8-bit */
if (flip) a = flip_left_right(a)
a = point(a, ALPHA_THRESH_LUT)               /* v > 40 ? 255 : 0 */
```

Notes:

- `(int)` truncates toward zero; here the operands are always ≥ 0 because
  `screen_rect(0).left ≥ paste_origin().x` (the bbox offset is non-negative), so it is
  equivalent to floor. Keep truncation anyway.
- The `+1` on the far edges deliberately over-crops; PIL pads the overhang with 0, which
  keeps the multiply harmless. Reproduce the pad-with-zero crop.
- `multiply(a,b)` on 8-bit is `(a*b)/255`; with 0/255 inputs the result is 0 or 255, so
  "any non-zero pixel" is the whole test. There is no need for a real multiply — an
  early-out `if (a[i] && b[i]) return true;` is bit-identical and much faster.
- Mask peak *(measured)*: 54 KB during level 2/3, well inside the 256 KB budget.
- `inset` is used in exactly two places: `_cball_friendly_fire` uses `inset=0.3`
  (rects shrink 15% on each side); everything else uses 0.

### 8. Input

```c
static const struct { uint16_t code; koki_key key; } KEYMAP[] = {
  {105,LEFT},{106,RIGHT},{103,UP},{108,DOWN},{44,Z},{45,X},{28,ENTER},{14,BACK},
  /* phone keypad aliases, matching MATRIX_NAME_TO_CODE in core/main.py */
  {5,LEFT},{7,RIGHT},{3,UP},{9,DOWN},   /* num4 num6 num2 num8 */
  {6,Z},{11,X},                          /* num5 = jump, num0 = action */
  {42,Z},{43,X},                         /* star = jump, hash = action */
};
```

Eight logical keys: `left right up down z x enter back`.

`Input` keeps `held` (a set) and `pressed` (edges seen this frame). `poll()` runs once per
frame at the top of the loop:

1. Clear `pressed`.
2. If `ui.keypad_fd` is open, drain it non-blockingly: `select()` with timeout 0, then
   `read(fd, 24)`. Accept **24-byte** records (`struct input_event` with 64-bit
   `timeval`, unpacked as `llHHI`) and **16-byte** records (32-bit `timeval`, `IIHHI`);
   any other length is skipped. Only `type == EV_KEY (1)` is considered. `value == 1`
   adds to `held` (and to `pressed` if it was not already held), `value == 0` discards
   from `held`, `value == 2` (autorepeat) is ignored.
   Any exception on `select`/`read` breaks out of the drain loop for that frame.
3. If `ui.matrix_input` exists, call `matrix.read_key(0)` (one scan pass, which updates
   the backend's own held state), then:
   - `holder = matrix.scanner if it has one else matrix`; `state = holder._held`;
     `to_code = matrix.matrix_to_code`.
   - **Rollover backend** (`state` is a dict — this is `I2CMatrixScanner._held`, a
     `{(row,col): missed_scan_count}` map): build `cur` = the logical names of every
     `(row,col)` in the dict mapped through `matrix_to_code` then `KEYMAP`; then
     `pressed |= cur - prev_matrix_held`, `held -= prev_matrix_held - cur`, `held |= cur`,
     `prev_matrix_held = cur`. This gives true multi-key rollover on hardware.
   - **Single-key backend** (anything else): if `read_key` returned a code that maps to a
     name, discard the previously-remembered matrix name from `held` if it differs, then
     add the new name to `pressed` and `held` and remember it. Additionally, if a name is
     remembered and `state is None`, discard it. *Note: `MatrixKeypadInput._held` in
     `core/main.py` is a `set()`, never `None`, so on the gpiozero backend this release
     path never fires and a key is only released when a different key is pressed.* That
     is the current shipped behaviour; reproduce it, and record it in OPEN-QUESTIONS if
     it is to be fixed.
4. At construction, print one of
   `[Koki] matrix keypad: rollover scanner (simultaneous keys supported)` or
   `[Koki] matrix keypad detected: single-key input only (no simultaneous move+jump).`

`key(name)` → membership in `held`. `any_key()` → `held` non-empty.
`kdir()` → `(right ? 1 : 0) - (left ? 1 : 0)`.

### 9. Caches

```c
typedef struct { /* insertion-ordered map, LRU by access */ } koki_lru;
/* cost(img) = width * height * channels   (RGBA=4, RGB=3, L=1) */
/* get(): on hit, move the entry to the most-recent end */
/* put(): if key present, just move to end and RETURN (do not replace);
          else insert, add cost, then
          while (bytes > budget && count > 1) evict the least-recent  */
```

The `count > 1` guard means a single image larger than the budget is kept anyway.

Three caches:

| cache | key | value | budget |
| --- | --- | --- | --- |
| `_img_cache` | `img_path` | decoded RGBA | `NEODCT_KOKI_IMG_CACHE_KB` × 1024, default below |
| `_fx_cache` | `(path, size_q, flip, ghost_q, bri_q)` | processed RGBA | `NEODCT_KOKI_FX_CACHE_KB` × 1024 |
| `_mask_cache` | `(path, flip)` | thresholded 8-bit alpha | fixed **256 KB** |

Budget tiering, from the first line of `/proc/meminfo` (`MemTotal: <kB>`):

```
default            img=3072 KB  fx=1024 KB
MemTotal <  40*1024 kB (40 MB)   -> img=1024 KB  fx=384 KB
MemTotal <  72*1024 kB (72 MB)   -> img=1536 KB  fx=512 KB
```

and when the tier is not the default, print
`[Koki] small RAM (<N>MB): cache budgets <img>/<fx>KB`. Any failure reading
`/proc/meminfo` silently leaves the defaults. Environment overrides are applied after the
tiering and are unconditional.

The backdrop (240×175 RGB = 126,000 B) is **not** in any cache — it is a single owned
buffer replaced on each `backdrop()` call. The canvas is another 126,000 B, owned by the
core's `ui`.

Measured peaks over a full tour of the game at 64 MB (default budgets):

| scene | live scripts | sprites drawn | img cache | fx cache | mask cache | ms/frame (host) |
| --- | --- | --- | --- | --- | --- | --- |
| level 1 | 20 | 8 | 365 KB | 1019 KB | 27 KB | 0.21 |
| level 2 | 32 | 5 | 1306 KB | 1013 KB | 54 KB | 0.17 |
| level 3 | 32 | 7 | 577 KB | 1018 KB | 54 KB | 0.23 |
| final boss | 31 | 12 | 466 KB | 1023 KB | 33 KB | 0.24 |
| ending | 13 | 3 | 595 KB | 1013 KB | 0 KB | 0.21 |

The fx cache sits **at its budget** in every scene, i.e. it thrashes; folding effects into
the blend (§6.1) removes that whole megabyte.

### 10. Sound

Three backends, chosen once at construction of `SoundManager(assets_dir)`:

1. `NEODCT_KOKI_NOSOUND` set → disabled, log
   `[Koki] SOUND DISABLED: NEODCT_KOKI_NOSOUND set -- game continues silent`.
2. `/dev/snd` is not a directory → disabled with reason
   `/dev/snd missing (no ALSA device; kernel audio not implemented yet?)`.
3. Unless `NEODCT_KOKI_AUDIO == "subprocess"`, try the in-process mixer. On success print
   `[Koki] audio: in-process miniaudio mixer`. On failure, if
   `NEODCT_KOKI_AUDIO == "miniaudio"` disable; otherwise print
   `[Koki] miniaudio unavailable (...); falling back to external players`.
4. External-player fallback (see below).

`_disable(reason)` and `_log_once(msg)` de-duplicate through one `reasons_logged` set.

#### 10.1 The in-process mixer (the shipped path)

Everything in `assets/snd` is **22050 Hz, mono, signed 16-bit** after decode
(`_MiniaudioMixer.RATE = 22050`, `MAX_SFX = 3`).

- One playback device: `SIGNED16`, 1 channel, 22050 Hz, buffer
  `NEODCT_KOKI_ABUF_MS` ms (default **150**).
- A `music` voice (looping, at most one) plus up to `MAX_SFX = 3` one-shot voices.
- The device pull callback asks for `required` frames; the mixer produces `required*2`
  bytes. For each live voice (`voices` first, then `music`): read that many bytes, pad
  with zeros if short, and saturating-add into the accumulator. Voices that raise are
  marked done. Done sfx voices are pruned; a done music voice clears `music`.
  With no voices, emit silence.
- Saturating add is per-sample `int16` clamped to `[-32768, 32767]`.
- A looping voice restarts its decoder at EOF; a one-shot marks itself done.
- `play_music(path)` **replaces** the current music voice outright.
- `play_sfx(path)` prunes done voices, then appends only if `< MAX_SFX` are live —
  a fourth simultaneous sfx is dropped, not queued.
- `stop_music()` clears the music voice; `stop_all()` clears music and all sfx;
  `close()` stops and closes the device.
- All mutations take one lock shared with the audio thread.

In C this is a decoder (dr_wav-style for the 40 WAVs, dr_mp3-style for the 17 MP3s) plus
an ALSA `snd_pcm` in a thread. The WAVs are already 22050/mono/s16, so WAV playback is a
memcpy; only the MP3s need a decoder. **Music tracks are up to 1.47 MB compressed and up
to 183 s long — they must be streamed, never fully decoded** (183 s × 22050 × 2 =
8.1 MB).

#### 10.2 External-player fallback

Kept for hardware bring-up. `avail = {aplay, mpg123, mpv} ∩ PATH`.
`players["wav"] = NEODCT_KOKI_WAV_PLAYER or first of (aplay, mpv) available`;
`players["mp3"] = NEODCT_KOKI_MP3_PLAYER or first of (mpv, mpg123) available`.
If the wav player ends up as `mpv` and `MemTotal < 72*1024 kB`, `MAX_SFX` drops to **1**.
`NEODCT_KOKI_MAX_SFX` overrides `MAX_SFX` (ignored if unparseable).
If `mpv` is available, probe `mpv <MPV_EXTRA> --version` with a 10 s timeout; on success
use the flags, otherwise print
`[Koki] this mpv rejects the memory-trim flags; running it plain`.

```
MPV_EXTRA = --no-config --load-scripts=no --audio-display=no --cache=no
            --demuxer-max-bytes=1MiB --demuxer-max-back-bytes=256KiB
aplay :  aplay -q <path>                       (no loop support)
mpg123:  mpg123 -q [--loop -1] <path>
mpv   :  mpv --no-video --really-quiet --no-terminal <MPV_EXTRA> [--loop=inf] <path>
```

A looped WAV with `aplay` selected falls back to `mpv` (log
`looped wav needs mpv (aplay can't loop)`) or is skipped (`no looping wav player; music
skipped`). Exactly one asset needs this: `Koki D score` (14.46 s, under the builder's 15 s
music threshold, so it is a WAV but is used as looping music).
If no player at all: `[Koki] audio players: sfx=... music=...` is replaced by
`no audio player found (aplay/mpg123/mpv)` and sound is disabled.
`check()` (called every 30 frames) notices a dead music process and logs
`music player died (rc=...) -- bad option or OOM kill? check dmesg / run with
NEODCT_KOKI_SOUND_DEBUG=1` once.

#### 10.3 Sprite-level sound API

- `Sprite.play(name)` → `sound.sfx(sounds[name].file)`; unknown name prints
  `[Koki] <sprite>: unknown sound '<name>'` and returns.
- `Sprite.play_until_done(name)` → start the sfx, then `wait(sounds[name].dur)`.
  Used once: `SCORE.play_until_done("output (33)")`, `dur = 4.05`.
- `Sprite.music(name)` → `sound.music(file)`. Used once: `GOVER.music("KokiPrototypelOBBY")`
  and five times in the grade table.
- `Engine.stage_music(name)` / `stage_sfx(name)` look the name up in
  `manifest.targets["Stage"].sounds`.
- `Engine.stop_music()` → `sound.stop_music()`.

Asset inventory *(measured)*: 57 unique files, 13.2 MB. Largest music
`B Movie_ 16-bit Sega Genesis Remix` 1.47 MB / 183.07 s (declared on the Stage but never
played by any script). Smallest `pop` 1110 B / 0.02 s.

### 11. Main loop, pause menu, teardown

```
run():
    if headless: _vtime = 0.0
    start_flag()
    frames = 0; busy_acc = 0; t_report = monotonic()
    while (!quit) {
        t0 = monotonic()
        input.poll()
        if ("back" in input.pressed) { if (pause_menu()) break; input.poll(); }
        <script pass, then dead sweep>            /* §4.3 */
        render()
        busy = monotonic() - t0;  busy_acc += busy;  frames++
        if (frames % 30 == 0) sound.check()
        if (perf && monotonic() - t_report >= 5.0) {
            print "[Koki] avg frame %.1fms (%.1f fps)"
                  busy_acc/frames*1000, frames/(monotonic()-t_report)
            frames = 0; busy_acc = 0; t_report = monotonic()
        }
        if (headless) { _vtime += FRAME_DT; if (--headless_frames <= 0) break; }
        else          { rest = FRAME_DT - busy; if (rest > 0) sleep(rest); }
    }
    finally: teardown()
```

`perf` = `NEODCT_KOKI_PERF` set.

`pause_menu()` — drawn straight onto the shared canvas with the core's own drawing calls,
then `fb.update`:

```
draw.rectangle((30,55,210,120), fill=black, outline=white)   /* 1 px border, inclusive */
draw.text((45,62),  "Quit Koki?",       font=ui.font_n /* 20 px */, fill=white)
draw.text((45,90),  "Enter=Yes  C=No",  font=ui.font_s /* 14 px */, fill=white)
fb.update(canvas)
while (input.key("back")) { input.poll(); sleep(0.02); }      /* swallow the held key */
loop { input.poll();
       if ("enter" in pressed) return true;                   /* quit the app */
       if ("back"  in pressed) return false;                  /* resume */
       sleep(0.02); }
```

The menu does not re-render the game; the frame underneath stays frozen behind the box.
On resume, the next `render()` paints over it.

`teardown()` (always runs, even on the exception path): stop all scripts, clear `active`,
clear `handlers`, empty all three caches (map **and** byte counter), clear `sprites` and
`layers`, drop `backdrop_img`, `sound.shutdown()`, then `malloc_trim(0)` via
`ctypes.CDLL(None)` — in C, call `malloc_trim(0)` directly, guarded by `#ifdef __GLIBC__`.

`main.py` additionally purges the `engine` and `game` modules from `sys.modules` before
and after the run and calls `gc.collect()`. Under the new architecture the app is its own
process, so process exit replaces all of that — but `teardown()` must still run so the
audio device is released and the log stays honest.

### 12. Randomness

`Engine.random = random.Random()` — a **private** Mersenne Twister seeded from OS
entropy, *not* the global `random` module. `randint(a,b)` normalises to
`lo, hi = min, max`, casts both with `int()` and returns
`random.Random.randint(lo, hi)` = `lo + _randbelow(hi - lo + 1)`.

For golden-frame verification the C port must reproduce CPython's algorithm bit for bit:

- **MT19937**, `init_by_array` seeding (CPython's `random_seed` for a non-negative int
  splits `abs(n)` into 32-bit little-endian words and calls `init_by_array`).
- `getrandbits(k)` for `k ≤ 32` is `genrand_uint32() >> (32 - k)`; `getrandbits(0)` is 0.
- `_randbelow(n)`: `k = bit_length(n); do r = getrandbits(k); while (r >= n); return r`.
  `n == 0` returns 0.

Test vectors *(measured, CPython 3.13)*:

| seed | first 4 × `getrandbits(32)` | `randint(1,3)` ×8 | `randint(5,30)` ×6 | `randint(-240,240)` ×5 |
| --- | --- | --- | --- | --- |
| 42 | 2746317213, 478163327, 107420369, 3184935163 | 3,1,1,3,2,1,1,1 | 25,8,5,28,13,12 | 87,−183,−228,139,−100 |
| 1234 | 4150886329, 3342196574, 1892932127, 501869158 | 2,1,1,1,3,1,3,3 | 29,19,8,5,7,30 | 158,−15,−181,−237,−194 |
| 20240101 | 868755655, 1962355856, 4178678115, 1712989311 | 1,2,2,2,3,3,3,2 | 11,19,17,17,24,22 | −137,−7,−36,−43,67 |

Note that `goldenframe.py` seeds only the **global** `random`, so Koki's private RNG stays
unseeded there. The `app-koki` reference frame is at frame 400 of the boot sequence, before
any `randint()` call, so it is deterministic anyway. The harness seeds explicitly
(`eng.random.seed(...)`), and the C harness must expose the same hook.

---

### 13. The game — all 304 scripts

This section is the whole of `game.py`. It is written as pseudocode with the exact
constants; a C agent with only this document must be able to reproduce every script.

Notation:

- `W(t)` = `yield from eng.wait(t)` — always at least one frame (§5).
- `Y` = a bare `yield` — one frame.
- `glide(spr, t, x, y)` = `yield from spr.glide(t, x, y)` (§5).
- `glideTo(spr, t, other)` = `glide_to_sprite`, target sampled once on the first step.
- `bc "m"` = `eng.broadcast("m")`.
- `stopOther(spr)` = `eng.stop_other_scripts(spr)`.
- `key(k)` = the key is currently held.
- `R(a,b)` = `eng.randint(a,b)` (inclusive).
- `V[...]` = the global variable table.
- `costume "x"` = `set_costume("x")` (case-insensitive name lookup).
- `costumeIs("x")` = current costume name equals `x`, case-insensitively.
- **Instant** marks a handler with no yield before its `return` — it still costs one
  frame of latency (§4.3) but completes in that single step.

Handlers are listed in **registration order**, which is the order they appear here. That
order is part of the contract (§4.1).

#### 13.1 Globals and sprite creation

```
V = { "lives": 3, "doors": 1, "taken damage": 0, "knockouts": 0,
      "has healed": 0, "cannondefeats": 1, "RibyDanger": 0,
      "evilcanonballdirection": -90, "healwavedirection": 2, "damageway4": 2 }
```

Two more keys are created lazily by the damage gates: `"_hurt_t"` and `"_plane_hurt_t"`
(defaulting to `-99` when absent).

Sprites are created in this order (this is also the pre-`set_layer_order` list order):

```
Player, CharacterAnim, Platform, Dynaris Logo, intro, StartButton, Sprite1, White,
Door1, Door2, Door3, Door4, Enemy 1, KokiStats, GameOver, Shockwave, Shockwave2,
Enemy1Stats, Cannon, Cannon ball, QuickPress, KokiPlaneStats, cutscene1, PlaneChar,
Enemy2, Enemy2Stats, Cannon2, Gas tank, A to Dodge, Sprite2, Abyss, Enemy 3, Lives,
Enemy3Stats, Cannon3, Cannon ball2, Enemy4Stats, EvilCannon, Cannon ball3, Shockwave3,
Shockwave4, Shockwave5, Cannon ball4, Reward, Riby
```

Short names used below: `PLAYER ANIM PLAT LOGO INTRO STARTBTN PANEL WHITE DOOR1..4 EN1
KSTAT GOVER SW1 SW2 E1STAT CANNON CBALL QUICK KPSTAT CUT1 PLANE EN2 E2STAT CANNON2 GAS
DODGE SCORE ABYSS EN3 LIVES E3STAT CANNON3 CBALL2 E4STAT EVILC CBALL3 SW3 SW4 SW5 CBALL4
REWARD RIBY`.

Then `set_layer_order([...])` (§6.4).

#### 13.2 Shared sub-scripts

```
flash(spr, times = 2):                       # "_flash"
    repeat times:
        spr.ghost = 50; spr.brightness = 50; W(0.05)
        spr.ghost = 0;  spr.brightness = 0;  W(0.05)

white_fade_out():                            # "_white_fade_out"
    WHITE.show(); WHITE.front(); WHITE.ghost = 0
    repeat 20: WHITE.ghost += 5; Y
    WHITE.hide()

door_flash(door):                            # infinite
    forever: door.costume "costume1"; W(0.3); door.costume "costume2"; W(0.3)

door_interact(door, on_enter, unlock_check = NULL):
    door.hide(); W(0.5)
    if unlock_check and not unlock_check(): return
    door.show()
    forever:
        if door.touching(ANIM) and (key(x) or key(up)): on_enter(); return
        Y

en1_idle_5():
    repeat 5: EN1.costume "costume1"; W(0.3); EN1.costume "costume2"; W(0.3)

en1_shockwave_volley():
    repeat 3:
        EN1.costume "costume3"; W(0.05)
        EN1.costume "costume4"; W(0.05)
        EN1.costume "costume5"; W(0.05)
        bc "shockwave"
        repeat 2:
            EN1.costume "costume10"; W(0.05)
            EN1.play "explbomb"
            EN1.costume "costume6"; W(0.05)
            EN1.costume "costume5"; W(0.2)
        EN1.costume "costume3"; W(0.05)
        EN1.costume "costume2"; W(0.05)
        EN1.costume "costume1"; W(0.1)

riby_run_anim():
    forever: for c in (costume6, costume8, costume4, costume9): RIBY.costume c; W(0.01)

riby_laugh(times):
    repeat times: RIBY.costume "costume16"; W(0.05); RIBY.costume "costume15"; W(0.05)

danger(v):                                   # "_danger"
    V["RibyDanger"] = v ; bc "playerfinalenable"
```

#### 13.3 Stage (sprite = NULL)

| # | event | body |
| --- | --- | --- |
| 1 | `flag` | **Instant.** `backdrop("backdrop1")` |
| 2 | `start intro` | **Instant.** `stage_music("Koki prototype theme")` |
| 3 | `go to lobby` | **Instant.** `backdrop("backdrop5")`; `stage_music("Koki New Level Lobby")` |
| 4 | `startlv1` | **Instant.** `backdrop("backdrop3")`; `stage_music("Koki Level 1")` |
| 5 | `startlv2` | **Instant.** `backdrop("backdrop4")`; `stage_music("Koki Level 2")` |
| 6 | `startlv3` | **Instant.** `backdrop("backdrop6")`; `stage_music("Popi vs Koki")` |
| 7 | `startfinallevel` | **Instant.** `backdrop("backdrop5")`; `stage_music("Riby boss fight prototype music 1")` |
| 8 | `ending cutscene` | **Instant.** `backdrop("backdrop2")`; `stage_music("lobbykoki")` |
| 9 | `boxing bell` | **Instant.** `stage_sfx("boxing bell")` |

Then **nine identical handlers**, one per message, each **Instant** `stop_music()`, for
`oofie`, `enemy1 oof`, `planeoofie`, `enemy2 oof`, `game over`, `falloofie`,
`enemy 4 oof`, `the end`, `stopmusic` — registered in exactly that order.

| # | event | body |
| --- | --- | --- |
| 19 | `final cutscene` | `W(1.0)`; `stop_music()`  *(deviation: the original slides the music pitch down 400% over ~80 frames; mpv/miniaudio cannot pitch-bend, so it just stops)* |
| 20 | `take damage` | **Instant.** `t = now(); if t - V["_hurt_t" or -99] >= 0.9: V["_hurt_t"] = t; bc "koki hurt"` |
| 21 | `takeplanedamage` | **Instant.** same shape with `V["_plane_hurt_t"]` → `bc "plane hurt"` |

Handlers 20 and 21 are the sanctioned i-frame gate: a double shockwave no longer costs
two HP. They are registered where they appear in `game.py`, i.e. **after** the
`CharacterAnim` block for #20 and inside the plane block for #21 — see the ordering table
in §13.15.

#### 13.4 Boot: Dynaris Logo → intro → StartButton → controls panel

```
flag / LOGO  [_logo]:
    LOGO.costume "costume1"; LOGO.show(); LOGO.front(); LOGO.ghost = 100
    repeat 20: LOGO.ghost -= 5; W(0.01)        # 95,90,...,0 ; >=95 is not drawn
    LOGO.play "Collect Sound Effect"
    W(0.1)
    repeat 12: LOGO.next_costume(); Y          # 13 costumes, so 1->13
    W(0.5)
    repeat 20: LOGO.ghost += 5; W(0.01)        # 5,...,100
    W(1)
    LOGO.ghost = 100; LOGO.hide()
    bc "start intro"

start intro / INTRO  [_intro]:
    INTRO.goto(0,0); INTRO.costume "Koki Icon"; eng.backdrop("backdrop2"); INTRO.show()
    INTRO.size = 2                              # Scratch clamps "set size -100%"
    repeat 5: INTRO.size += 5;  Y               # 7,12,17,22,27
    repeat 4: INTRO.size += 20; Y               # 47,67,87,107
    repeat 3: INTRO.size += 1;  Y               # 108,109,110
    W(0.1)
    repeat 12: INTRO.size -= 1;  Y              # 109..98
    INTRO.size = 100
    W(0.5)
    bc "start button enable"
    forever: glide(INTRO, 0.5, 0, 5); glide(INTRO, 0.5, 0, 0)

start game / INTRO  [_intro_out]:
    stopOther(INTRO); glide(INTRO, 0.3, 0, -298); INTRO.hide()

start button enable / STARTBTN  [_btn_anim]:
    STARTBTN.show()
    forever: costume "costume1"; W(0.4); costume "costume2"; W(0.4)

start button enable / STARTBTN  [_btn_input]:
    forever:
        if key(enter):
            stopOther(STARTBTN)
            STARTBTN.play "startbutton"
            STARTBTN.size = 70
            repeat 3: STARTBTN.next_costume(); STARTBTN.size += 5; Y   # 75,80,85
            STARTBTN.size = 70
            repeat 5: costume "costume1"; W(0.05); costume "costume2"; W(0.05)
            costume "costume1"; W(0.05)
            bc "start game"; STARTBTN.hide(); return
        Y

start game / PANEL (Sprite1)  [_panel]:
    W(0.5)
    PANEL.show(); PANEL.size = 100; PANEL.costume "costume1"; PANEL.goto(0,-394)
    glide(PANEL, 0.3, 0, 0)
    W(1)
    PANEL.costume "costume2"
    forever:
        if key(enter):
            stopOther(PANEL); PANEL.goto(0,0); PANEL.play "startbutton"
            repeat 5: costume "costume2"; W(0.05); costume "costume1"; W(0.05)
            costume "costume2"; W(0.05)
            PANEL.goto(0,0); glide(PANEL, 0.3, 0, -291)
            bc "go to lobby"; PANEL.hide(); return
        Y
```

`Sprite1`'s two costumes are the only ones whose art is *rewritten at bake time*: the
builder paints over the panel's text area with the NeoDCT keypad controls
(§ build_assets, `rewrite_controls_panel`). Nothing at runtime knows about that.

#### 13.5 White (full-screen fades)

```
PlayerEnable / WHITE  [_white_pe]:  white_fade_out()
start intro  / WHITE  [_white_si]:  white_fade_out()

whitechange / WHITE  [_white_change]:
    WHITE.show(); WHITE.front(); WHITE.ghost = 100
    repeat 10: WHITE.ghost -= 10; Y            # 90,80,...,0
    WHITE.ghost = 0; W(0.05)
    repeat 20: WHITE.ghost += 5;  Y            # 5,...,100
    WHITE.hide()

the end / WHITE  [_white_end]:
    WHITE.costume "costume2"; WHITE.show(); WHITE.front(); WHITE.ghost = 100
    repeat 10: WHITE.ghost -= 10; Y
    W(1)
    bc "ending score"
    WHITE.ghost = 0; W(0.05)
    repeat 20: WHITE.ghost += 5; Y
    WHITE.hide(); WHITE.costume "costume1"
```

#### 13.6 Platform

```
go to lobby / PLAT: PLAT.back(); show(); costume "costume2"; goto(0,-141)
                    glide(PLAT, 0.5, 0, -50); bc "PlayerEnable"
level1      / PLAT: show(); costume "costume3"; goto(0,-141); glide(PLAT, 0.2, 0, -50)
startlv2    / PLAT: Instant. hide()
level3      / PLAT: show(); costume "costume5"; goto(0,-141); glide(PLAT, 0.2, 0, -50)
ending cutscene / PLAT: Instant. show(); costume "costume1"; goto(0,-50)
```

#### 13.7 Player — the invisible physics box

`Player`'s collision costume `char2` is a **1 × 4 pixel** image (`cx = 0.25, cy = 3.304`,
no bbox) *(measured)*. `PLAYER.ghost = 99` keeps it out of the render pass (`ghost >= 95`)
while still counting as `visible` for collision.

```
touch_plat() := PLAYER.touching(PLAT, inset = 0.0)

PlayerEnable / PLAYER  [_player_physics]:
    show(); costume "char2"; ghost = 99; goto(-200, 30); sy = 0
    forever:
        dx = kdir() * 5
        x += dx
        x = clamp(-235, 235, x)                 # Scratch stage fencing
        if touch_plat(): x -= dx
        sy -= 1
        y += sy
        if touch_plat():
            y -= sy
            sy = (sy < 1 and key(z)) ? 15 : 0
        Y

RUNenable / PLAYER  [_player_run]:              # level 3: fixed x, stronger jump
    show(); costume "char2"; ghost = 99; goto(90, -54); sy = 0
    forever:
        sy -= 1; y += sy
        if touch_plat(): y -= sy; sy = (sy < 1 and key(z)) ? 17 : 0
        Y
```

Then **seven identical handlers** (`turn anim`, `oofie`, `falloofie`, `disableplayer`,
`final cutscene`, `ending cutscene`, `stopmusic`), each **Instant**:
`stopOther(PLAYER); PLAYER.hide()`.

```
rightdash / PLAYER: play "slide"; point(90)
                    for s in (30,30,30,15,15,15,5,5,5): move_steps(s); Y
leftdash  / PLAYER: play "slide"; point(-90)   ; same step list
```

`move_steps` at direction ±90 is `x += ±s` exactly (`sin(±90°) = ±1`, `cos(±90°) ≈ 6.1e-17`
— the y drift is 1.8e-15 stage units and is not observable, but compute it the same way).

#### 13.8 CharacterAnim — the visible Koki

```
PlayerEnable / ANIM  [_anim_follow]:  forever: ANIM.goto_sprite(PLAYER); Y

PlayerEnable / ANIM  [_anim_reset]:   Instant.
    brightness = 0; ghost = 0; costume "costume10"; rotation_style = "left-right"
    show(); costume "costume2"

PlayerEnable / ANIM  [_anim_walk]:
    forever:
        moving = key(left) or key(right)
        if moving and not (key(z) or not ANIM.touching(PLAT)):
            forever:
                if (not (key(left) or key(right))) or key(z)
                   or (key(left) and key(right)): break
                for c in (costume6, costume8, costume4, costume9):
                    ANIM.costume c; W(0.01)
            ANIM.costume "costume2"; W(0.05)
        Y

PlayerEnable / ANIM  [_anim_jump]:
    forever:
        if key(z) and not ANIM.touching(PLAT):
            while not ANIM.touching(PLAT): ANIM.costume "costume10"; W(0.01)
            ANIM.costume "costume11"; W(0.05)
            ANIM.costume "costume2";  W(0.01)
        Y

PlayerEnable / ANIM  [_anim_idle]:
    forever:
        W(10)
        if not input.any_key() and ANIM.touching(PLAT):
            ANIM.costume "costume3"; W(0.05); ANIM.costume "costume2"; W(0.01)

PlayerEnable / ANIM  [_anim_face]:
    forever: if key(right): point(90);  if key(left): point(-90);  Y
```

*(The animation gates test `ANIM.touching(PLAT)`, not the physics dot. That is a
deliberate 2026-07-10 fix — the earlier port tested `PLAYER` and the walk cycle never
played. The original Scratch tests CharacterAnim, so this is the faithful behaviour.)*

```
idleanim  / ANIM: Instant. costume "costume2"        # registered, never broadcast
turn anim / ANIM: stopOther(ANIM); costume "costume4"; W(0.1)
                  costume "door"; W(0.6); costume "costume2"

oofie / ANIM  [_anim_oofie]:
    stopOther(ANIM); play "hit"; costume "OOF"; brightness = 0; ghost = 0
    W(1); play "Lose sound"
    repeat 7: y += 5; Y
    glide(ANIM, 0.7, ANIM.x, -204)
    ANIM.hide(); W(2); bc "whitechange"; W(0.05)
    if V["lives"] <= 0: bc "game over"; return
    bc "go to lobby"

koki hurt / ANIM  [_anim_dmg_sound]:  play "hit2"; repeat 10: costume "OOF"; Y
koki hurt / ANIM  [_anim_dmg_flash]:
    repeat 10: brightness=50; ghost=50; W(0.05); brightness=0; ghost=0; W(0.05)

dodge / ANIM: play "slide"; costume "costume11"
              repeat 10: brightness=50; ghost=50; W(0.05); brightness=0; ghost=0; W(0.05)

disableplayer / ANIM: Instant. stopOther(ANIM); hide()

falloofie / ANIM:
    stopOther(ANIM); hide(); play "Fall"; W(1); play "Lose sound"; W(2.7)
    bc "whitechange"; W(0.05)
    if V["lives"] <= 0: bc "game over"; return
    bc "go to lobby"

RUNenable / ANIM  [_anim_run_follow]:  ghost = 0; forever: goto_sprite(PLAYER); Y
RUNenable / ANIM  [_anim_run_start]:   Instant. show(); bc "RUNNN"; point(90)

RUNNN / ANIM  [_anim_runnn_follow]: forever: goto_sprite(PLAYER); Y
RUNNN / ANIM  [_anim_runnn_anim]:
    ghost = 0; brightness = 0
    forever: for c in (costume6,costume8,costume4,costume9): costume c; W(0.01)
RUNNN / ANIM  [_anim_runnn_jump]:
    forever:
        if key(z):
            ghost = 0; brightness = 0
            stopOther(ANIM); bc "jumpRUN"
            costume "costume10"; W(1.1)
            costume "costume11"; W(0.05)
            stopOther(ANIM); bc "RUNNN"
        Y

jumpRUN / ANIM: forever: goto_sprite(PLAYER); Y

final cutscene / ANIM  [_anim_finalcut]:
    y = -75; costume "door"; W(0.05)
    bc "temporary hit"; play "hit2"; costume "OOF"
    glide(ANIM, 0.1, 135, -75)
    costume "costume2"; W(1)

cutscenehit / ANIM:
    y = -75; bc "temporaryhit2"; play "hit2"; play "slide"
    costume "costume11"; goto(135,-75); glide(ANIM, 0.2, -200, -75)
    costume "OOF"; W(0.05); costume "costume2"; W(1)

enemy4 damage / ANIM: play "Fall2"; costume "door"; W(0.1); costume "costume2"

ending cutscene / ANIM  [_anim_ending_walk]:
    show(); goto(-243,-75); glide(ANIM, 20, 250, -75)
    stopOther(ANIM); bc "the end"; hide()
ending cutscene / ANIM  [_anim_ending_anim]:
    forever: for c in (costume6,costume8,costume4,costume9): costume c; W(0.1)

stopmusic / ANIM  [_anim_lv3_dance]:
    stopOther(ANIM); show(); goto(90,-75)
    repeat 3: costume "costume11"; W(0.3); costume "costume3"; W(0.3)
```

#### 13.9 Lives icon, Koki health bar, GAME OVER

```
go to lobby / LIVES:
    if V["lives"] >= 3: costume "costume1"
    elif V["lives"] == 2: costume "costume2"
    elif V["lives"] == 1: costume "costume3"      # lives <= 0 leaves the costume as-is
    W(1); LIVES.show()
doorinteracted  / LIVES: Instant. hide()
ending cutscene / LIVES: Instant. hide()

startlv1 / KSTAT: Instant. costume "costume1"
PlayerEnable / KSTAT: bc "restoreallhealth"; show(); goto(-308,-144)
                      glide(KSTAT, 0.3, -150, -144)
koki hurt / KSTAT  [_kstat_dmg_bounce]:
    V["taken damage"] = 1                          # ASSIGNMENT, not +=
    glide(KSTAT, 0.05, -150, -139); glide(KSTAT, 0.1, -150, -144)
koki hurt / KSTAT  [_kstat_dmg]:
    KSTAT.next_costume()
    if costumeIs("Oof"): bc "oofie"; V["knockouts"] += 1; W(0.05); hide()
restoreallhealth / KSTAT: Instant. costume "costume1"
start game / KSTAT:       Instant. V["lives"] = 3
oofie / KSTAT:            Instant. V["lives"] -= 1
falloofie / KSTAT:        Instant. V["lives"] -= 1; hide()
startlv2 / KSTAT: stopOther(KSTAT); goto(-150,-144); glide(KSTAT,0.3,-308,-144); hide()
startlv3 / KSTAT: bc "restoreallhealth"; show(); goto(-308,-144)
                  glide(KSTAT, 0.3, -150, -144)
temporary hit / KSTAT:  costume "costume2"; glide(0.05,-150,-139); glide(0.1,-150,-144)
temporaryhit2 / KSTAT:  costume "costume3"; glide(0.05,-150,-139); glide(0.1,-150,-144)
partialrestore / KSTAT: Instant.
    if costumeIs("costume2"): costume "costume1"
    elif costumeIs("costume3"): costume "costume2"
    elif costumeIs("costume4"): costume "costume3"
ending cutscene / KSTAT: Instant. stopOther(KSTAT); hide()
```

`KokiStats` costumes are `costume1, costume2, costume3, costume4, Oof` — four hits then a
knockout.

```
game over / GOVER  [_gameover_music]: Instant. GOVER.music("KokiPrototypelOBBY")
game over / GOVER  [_gameover]:
    front(); show(); costume "costume1"; W(0.5)
    forever:
        costume "costume2"
        if key(enter):
            play "startbutton"; eng.stop_music()
            repeat 5: costume "costume1"; W(0.05); costume "costume2"; W(0.05)
            costume "costume2"; W(0.05)
            bc "whitechange"; bc "go to lobby"; bc "lockAlldoors"
            V["lives"] = 3; hide(); stopOther(GOVER); return
        Y
```

#### 13.10 Doors and the lobby

```
go to lobby / DOOR1 [_door1_watch]: door_interact(DOOR1,
        on_enter = { bc "turn anim"; bc "level1"; bc "doorinteracted" })
go to lobby / DOOR1 [_door1_flash]: door_flash(DOOR1)
level1      / DOOR1: bc "whitechange"; W(0.05); bc "startlv1"; bc "PlayerEnable"

go to lobby / DOOR2 [_door2_watch]: door_interact(DOOR2,
        on_enter = { bc "turn anim"; bc "level2"; bc "doorinteracted" },
        unlock_check = V["doors"] >= 2)
go to lobby / DOOR2 [_door2_flash]: door_flash(DOOR2)
level2      / DOOR2: bc "whitechange"; W(0.05); bc "startlv2"; bc "disableplayer";
                     bc "planecutscene"
unlock door2 / DOOR2: Instant. V["doors"] = 2
lockAlldoors / DOOR2: Instant. V["doors"] = 1

go to lobby / DOOR3 [_door3_watch]: door_interact(DOOR3,
        on_enter = { bc "turn anim"; bc "level3"; bc "doorinteracted" },
        unlock_check = V["doors"] >= 3)
go to lobby / DOOR3 [_door3_flash]: door_flash(DOOR3)
level3      / DOOR3: bc "whitechange"; W(0.05); bc "startlv3"; bc "RUNenable"

go to lobby / DOOR4 [_door4_watch]: door_interact(DOOR4,
        on_enter = { bc "turn anim"; bc "level 4" },       # no "doorinteracted"
        unlock_check = V["doors"] >= 4)
go to lobby / DOOR4 [_door4_flash]: door_flash(DOOR4)
level 4     / DOOR4: stopOther(DOOR4); bc "final cutscene"; DOOR4.show();
                     door_flash(DOOR4)
door4openagain / DOOR4 [_door4_again_flash]: door_flash(DOOR4)
door4openagain / DOOR4 [_door4_again_watch]:
    forever:
        if DOOR4.touching(ANIM) and (key(x) or key(up)):
            bc "turn anim"; bc "whitechange"; W(0.5); bc "ending cutscene"; return
        Y
```

Then the bulk-registered hide/flash handlers, in this exact registration order:

```
for d in (DOOR1,DOOR2,DOOR3,DOOR4):  on("doorinteracted", d) -> Instant hide()
                                     on("level2",         d) -> Instant hide()
for d in (DOOR1,DOOR2,DOOR3):        on("level3",         d) -> Instant hide()
on("level3", DOOR4)                                          -> Instant hide()
for d in (DOOR1,DOOR2,DOOR3,DOOR4):  on("ending cutscene", d)-> Instant stopOther(d); hide()
for d in (DOOR1,DOOR2,DOOR3):
        on("final cutscene", d)  -> stopOther(d); d.show(); door_flash(d)
        on("startfinallevel", d) -> Instant stopOther(d); d.costume "costume1"
on("startfinallevel", DOOR4)     -> Instant stopOther(DOOR4); costume "costume1"
```

*(Note the source builds the four `doorinteracted`/`level2` pairs interleaved per door:
`DOOR1.doorinteracted, DOOR1.level2, DOOR2.doorinteracted, DOOR2.level2, ...`.)*

#### 13.11 Level 1 — Enemy 1

```
startlv1 / EN1 [_en1_start]:
    goto(173,-48); show(); clear_fx()
    en1_idle_5(); en1_shockwave_volley()
    bc "enemy1 idle"; W(0.2); bc "spawncanon"

startlv1 / EN1 [_en1_bell]:  W(2); bc "boxing bell"; bc "hitbox"

hitbox / EN1: forever: if EN1.touching(ANIM): bc "take damage"; return
                       Y
take damage / EN1: W(1); bc "hitbox"

enemy1 idle / EN1: forever: costume "costume1"; W(0.3); costume "costume2"; W(0.3)

enemy1damage / EN1:
    stopOther(EN1); play "output (24)"; costume "OOF"; W(0.5)
    costume "costume1"; W(0.5); bc "enemy1 pounce"

enemy1 pounce / EN1:
    costume "costume7"; play "output (24)2"; bc "quickhitbox"; bc "quick tap"
    glide(EN1, 0.3*ATK, 0, 94)
    glideTo(EN1, 0.3*ATK, ANIM)
    play "explbomb"; costume "costume8"; EN1.y = -48; W(0.05)
    costume "costume9"; W(0.2)
    stopOther(EN1); costume "costume1"; glide(EN1, 0.3, 173, -48)
    bc "hitbox"; bc "phaseagain"

phaseagain / EN1: en1_idle_5(); en1_shockwave_volley()
                  bc "enemy1 idle"; W(0.2); bc "spawncanon"

quickhitbox / EN1:
    forever:
        if EN1.touching(ANIM):
            if key(x): bc "dodge"  else: bc "take damage"
            return
        Y

oofie / EN1:       Instant. stopOther(EN1)
go to lobby / EN1: Instant. stopOther(EN1); hide()

enemy1 oof / EN1:
    stopOther(EN1); play "explbomb2"; costume "OOF"
    repeat 10: y += 5; Y
    costume "OOF2"; glide(EN1, 0.6, EN1.x, -48)
    ghost = 0; repeat 20: ghost += 5; Y
    hide(); W(1); bc "whitechange"; W(0.05)
    bc "unlock door2"; bc "go to lobby"

en1final / EN1:                                   # final-boss possession entry
    show(); clear_fx(); goto(265,109); glide(EN1, 0.1, 84, -48)
    play "explbomb"; costume "costume8"; W(0.05); costume "costume9"; W(0.2)
    stopOther(EN1); costume "costume1"; en1_idle_5()

possessen1 / EN1:
    stopOther(EN1); play "output (24)"; costume "OOF"; W(0.5); costume "costume1"
    brightness = 0
    repeat 6: brightness -= 25; W(0.05)           # -25..-150 (quantised, clamped -100)
    repeat 4: brightness += 25; Y                 # -150..-50
    brightness = -50                              # stays dark while possessed
    en1_idle_5()
    forever:
        costume "costume7"; play "output (24)2"; bc "quick tap"; bc "quickhitbox"
        glide(EN1, 0.3*ATK, 0, 94); glideTo(EN1, 0.3*ATK, ANIM)
        play "explbomb"; costume "costume8"; EN1.y = -48; W(0.05)
        costume "costume9"; W(0.2)
        costume "costume1"; glide(EN1, 0.3, 173, -48)
        bc "activehitonen1"
        en1_idle_5(); en1_shockwave_volley()
        bc "activehitonen1"
        en1_idle_5()

activehitonen1 / EN1:
    forever:
        if EN1.touching(ANIM) and key(x):
            stopOther(EN1); play "explbomb2"; costume "OOF"
            repeat 10: y += 5; Y
            costume "OOF2"; glide(EN1, 0.6, EN1.x, -48)
            bc "RibyOUT"
            ghost = 0; brightness = 0
            repeat 20: ghost += 5; Y
            hide(); W(1); return
        Y
```

*(`brightness = -150` quantises to `bri_q = -100` for rendering; the raw value is kept
on the sprite. `brightness` is the port's stand-in for the original's purple colour
shift.)*

Shockwaves (level 1) — built by a factory, two instances:

```
mk_shockwave(spr, delay):
    move():
        if delay: W(delay)
        spr.show(); spr.goto_sprite(EN1)
        glide(spr, 1*ATK, -241, -45)
        stopOther(spr); spr.hide()
    hit():
        if delay: W(delay)
        forever: if spr.touching(ANIM): bc "take damage"; return
                 Y

SW1 = mk_shockwave(Shockwave,  delay = 0)
SW2 = mk_shockwave(Shockwave2, delay = 0.2)
register: on("shockwave", SW1)(move), on("shockwave", SW1)(hit),
          on("shockwave", SW2)(move), on("shockwave", SW2)(hit)
```

Enemy 1 health bar (`Enemy1Stats`: `costume1..costume7, Oof, Oof2` — two steps per hit, so
**4 hits**):

```
startlv1 / E1STAT: bc "restoreallhealth"; show(); costume "costume1"; goto(308,-144)
                   glide(E1STAT, 0.3, 150, -144)
enemy1damage / E1STAT [_bounce]: glide(0.05,150,-139); glide(0.1,150,-144)
enemy1damage / E1STAT [_dmg]:    repeat 2: next_costume(); W(0.05)
                                 if costumeIs("Oof2"): bc "enemy1 oof"
go to lobby / E1STAT: goto(150,-144); glide(0.3,308,-144); hide()
game over   / E1STAT: Instant. hide()
```

Cannon and cannonball:

```
spawncanon / CANNON [_flash]: CANNON.show(); flash(CANNON)
spawncanon / CANNON [_arm]:
    play "recording1"; goto(-79,-69); glide(CANNON, 0.3, -79, -74)
    forever: if key(x) and CANNON.touching(ANIM): bc "canonball"; return
             Y
canonball / CANNON: play "explosion meme"; flash(CANNON); W(0.5); hide()
go to lobby / CANNON: Instant. stopOther(CANNON); hide()

canonball  / CBALL [_fly]:  goto(-41,-62); glideTo(CBALL, 0.7, EN1)
                            bc "enemy1damage"; hide()
canonball  / CBALL [_show]: show(); back(); flash(CBALL)
canonball  / CBALL [_ff]:   forever: if CBALL.touching(ANIM, inset=0.3):
                                        bc "take damage"; return
                                     Y
canonball2 / CBALL [_show]: show(); back(); flash(CBALL)
canonball2 / CBALL [_fly]:  goto_sprite(CANNON2); glideTo(CBALL, 0.7, EN2)
                            bc "enemy2 damage"; hide()
cannonball3/ CBALL [_show]: show(); back(); flash(CBALL)
cannonball3/ CBALL [_fly]:  goto_sprite(CANNON3); glideTo(CBALL, 0.4, EN3)
                            bc "enemy 3 damage"; hide()
go to lobby / CBALL: Instant. hide()
for m in (oofie, planeoofie, falloofie):
    on(m, CBALL) -> Instant. stopOther(CBALL); hide()
```

*(the death handlers exist so an in-flight ball's damage broadcast cannot land 0.7 s after
the player dies, restarting the boss over the corpse or granting door progression.)*

QuickPress prompt:

```
quick tap / QUICK [_input]:
    costume "costume1"
    forever:
        if key(x):
            stopOther(QUICK)
            bc (ANIM.x < EN1.x ? "leftdash" : "rightdash")     # deviation: dash AWAY
            QUICK.hide(); return
        Y
quick tap / QUICK [_show]:
    costume "costume1"
    repeat 4: show(); W(0.05); hide(); W(0.05)
    stopOther(QUICK)
Chase for the door / QUICK: costume "costume2"
    repeat 6: show(); W(0.05); hide(); W(0.05)
startfinallevel / QUICK:    costume "costume3"
    repeat 5: show(); W(0.2);  hide(); W(0.2)
```

#### 13.12 Level 2 — plane vs dragon

```
planecutscene / CUT1:
    W(0.5); show(); back(); costume "falling"; goto(-187,180)
    glide(CUT1, 0.5, -187, 0)
    bc "startplane"; bc "enableplane"; hide()

planecutscene / PLANE [_in]:   show(); goto(-332,0); glide(PLANE, 1, -195, 0)
planecutscene / PLANE [_prop]: repeat 10: costume "costume1"; W(0.01)
                                          costume "costume3"; W(0.01)
                               bc "planeANIM"
planeANIM / PLANE: forever: costume "costume2"; W(0.01); costume "costume5"; W(0.01)

enableplane / PLANE [_up]:     forever: while key(up):   y += 7; Y
                                        Y
enableplane / PLANE [_down]:   forever: while key(down): y -= 7; Y
                                        Y
enableplane / PLANE [_bounds]: forever: if y < -70: y += 7
                                        if y > 180: y -= 7
                                        Y
enableplane / PLANE [_boost]:                          # sanctioned chordless deviation
    last_dir = "up"; last_t = -99.0
    forever:
        if key(up):   last_dir = "up";   last_t = now()
        if key(down): last_dir = "down"; last_t = now()
        if key(z):
            boost = (now() - last_t <= 2.0) ? last_dir : (PLANE.y < 55 ? "up" : "down")
            play "slide"
            dy = (boost == "up") ? 20 : -20
            repeat 6: y += dy; Y
            bc "rechargeeffect"; W(1)
        Y

rechargeeffect / PLANE: flash(PLANE, 5)

plane hurt / PLANE [_hit]:
    stopOther(PLANE); bc "enableplane"; play "hit sound"
    for c in (costume4, costume6, costume9, costume10): costume c; W(0.05)
    play "beep"
    repeat 8: costume "costume7"; W(0.01); costume "costume8"; W(0.01)
    ghost = 0; bc "planeANIM"
plane hurt / PLANE [_flash]: flash(PLANE, 10)

planeoofie / PLANE:
    stopOther(PLANE); play "hit sound"; play "hit"; bc "planeoofanim"
    W(1); play "Lose sound"; glide(PLANE, 1, 37, -246)
    play "explbomb2"; hide(); W(2); bc "whitechange"; W(0.05)
    if V["lives"] <= 0: bc "game over"; return
    bc "go to lobby"
planeoofanim / PLANE: forever: costume "Oofie"; W(0.01); costume "Oofie2"; W(0.01)
go to lobby / PLANE:  Instant. stopOther(PLANE); hide()
```

Plane health bar (`KokiPlaneStats`: `costume1..costume4, Oof` — 4 hits):

```
startlv2  / KPSTAT: Instant. costume "costume1"
startplane/ KPSTAT: show(); goto(-308,-144); glide(0.3,-150,-144); bc "restoreplane"
plane hurt/ KPSTAT [_bounce]: V["taken damage"] += 1          # += here, = in KSTAT
                              glide(0.05,-150,-139); glide(0.1,-150,-144)
restoreplane / KPSTAT: Instant. costume "costume1"
plane hurt/ KPSTAT [_dmg]: next_costume(); W(0.05)
                           if costumeIs("Oof"): bc "planeoofie"; V["knockouts"] += 1
                                                W(0.05); hide()
planeoofie/ KPSTAT: Instant. V["lives"] -= 1
go to lobby/KPSTAT: goto(-150,-144); glide(0.3,-308,-144); hide()
```

Enemy2 (the dragon):

```
startlv2 / EN2 [_entry]:
    show(); costume "costume1"; goto(170,227); glide(EN2, 1, 170, 0); W(0.2)
    costume "costume2"; W(0.05); costume "costume3"; W(0.05)
    play "grrr"
    repeat 5: costume "costume4"; W(0.05); costume "costume5"; W(0.05)
    costume "costume3"; W(0.05); costume "costume2"; W(0.05)
    costume "costume1"; W(0.05)
    bc "en2 cycle 1"

hitbox2 / EN2: forever: if EN2.touching(PLANE): bc "takeplanedamage"; return
                        Y

en2_attack(track_secs, track_reps, pace):
    bc "hitbox2"
    costume "costume6"; W(pace)
    repeat 5: next_costume(); W(pace)
    bc "enemy bright"
    repeat track_reps: glide(EN2, track_secs, 170, PLANE.y)    # PLANE.y read per rep
    play "output (27)"; costume "costume12"; W(0.05)
    repeat 10: next_costume(); W(0.05)
    for c in (costume11,costume10,costume9,costume8,costume7): costume c; W(0.05)
    costume "costume6"; W(0.2)

en2 cycle 1 / EN2: forever: repeat 4: en2_attack(0.1, 5, 0.05)
                            bc "spawncanon222"; W(1)
en2 cycle 2 / EN2: forever: repeat 4: en2_attack(0.05, 1, 0.03)
                            bc "spawncanon222"; W(0.5)

enemy bright / EN2: flash(EN2, 6)
planeoofie  / EN2: Instant. stopOther(EN2)

enemy2 damage / EN2 [_damaged]:
    stopOther(EN2); bc "enemy bright"; play "output (24)"; play "output (31)"
    costume "Damage"; W(0.05)
    repeat 9: next_costume(); W(0.05)
    play "grrr"
    repeat 5: costume "costume5"; W(0.05); costume "costume4"; W(0.05)
    costume "costume3"; W(0.05); costume "costume2"; W(0.05); costume "costume1"; W(0.05)
    bc "ping enemy2 stats"
enemy2 damage / EN2 [_flash]: flash(EN2, 10)

enemy2 oof / EN2:
    stopOther(EN2); clear_fx(); costume "OOF"
    play "output (29)"; play "output (28)"
    repeat 15: goto(R(-240,240), R(-180,180)); Y
    play "output (30)"; costume "OOF2"; W(0.05); costume "OOF3"; W(0.05)
    hide(); V["doors"] = 3; W(3); bc "go to lobby"
go to lobby / EN2: Instant. stopOther(EN2); hide()
```

`Enemy2`'s costume list is `costume1..costume22, OOF, OOF2, OOF3, Damage, Damage2..Damage6`
(31 entries) — `next_costume()` from `Damage` walks `Damage2..Damage6` and then **wraps to
`costume1`**; the `repeat 9` in `_en2_damaged` therefore ends on `costume4`.

Enemy2 health bar (`costume1..costume7, Oof2` — 7 hits; cycle 2 at `costume_number >= 5`):

```
startlv2 / E2STAT: bc "restoreplane"; show(); front(); costume "costume1"
                   goto(308,-144); glide(0.3,150,-144)
enemy2 damage / E2STAT [_bounce]: glide(0.05,150,-139); glide(0.1,150,-144)
enemy2 damage / E2STAT [_dmg]:    next_costume(); W(0.05)
                                  if costumeIs("Oof2"): bc "enemy2 oof"
ping enemy2 stats / E2STAT: Instant.
    bc (costume_number >= 5 ? "en2 cycle 2" : "en2 cycle 1")
go to lobby / E2STAT: goto(150,-144); glide(0.3,308,-144); hide()
game over   / E2STAT: Instant. hide()
```

Cannon2, gas tank, dodge hint:

```
spawncanon222 / CANNON2 [_flash]: show(); flash(CANNON2)
spawncanon222 / CANNON2 [_arm]:   play "recording1"
    forever: if key(x) and CANNON2.touching(PLANE): bc "canonball2"; return
             Y
spawncanon222 / CANNON2 [_drop]:  play "recording1"; show(); goto(-100,186)
    glide(CANNON2, 1.5, -100, -193); stopOther(CANNON2); hide()
canonball2 / CANNON2: play "explosion meme"; flash(CANNON2)
go to lobby / CANNON2: Instant. stopOther(CANNON2); hide()

en2 cycle 2 / GAS: Instant. bc "spawnfuel"
spawnfuel / GAS [_fly]:   show(); goto(242, R(-88,150))
    glide(GAS, 1.5, -243, GAS.y); stopOther(GAS); hide()
spawnfuel / GAS [_touch]: forever:
    if GAS.touching(PLANE): bc "restoreplane"; play "heal"; V["has healed"] = 1
                            stopOther(GAS); hide(); return
    Y
planeoofie  / GAS: Instant. stopOther(GAS); hide()
go to lobby / GAS: Instant. stopOther(GAS); hide()

level2 / DODGE: show(); front()
    repeat 5: costume "costume1"; W(0.1); costume "costume2"; W(0.1)
    hide()
```

*(`glide(GAS, 1.5, -243, GAS.y)` evaluates `GAS.y` at the call site, i.e. the randomised
spawn height, before the glide starts.)*

#### 13.13 Level 3 — Popi chase

```
startlv3 / EN3 [_place]:      Instant. show(); goto(-169,-55)
startlv3 / EN3 [_hitbox_arm]: W(2); bc "hitbox"
hitbox / EN3:      forever: if EN3.touching(ANIM): bc "take damage"; return
                            Y
take damage / EN3: W(1); bc "hitbox"
startlv3 / EN3 [_run_start]:  Instant. bc "enemy3 run"
enemy3 run / EN3:  forever: for c in (costume7,costume6,costume5,costume4):
                                costume c; W(0.01)
startlv3 / EN3 [_begin_atk]:  brightness = 0; V["cannondefeats"] = 1; W(3); bc "en3atk"

en3atk / EN3:
    forever:
        repeat 5:
            play "output (24)2"; brightness = 0
            repeat 10: brightness += 10; W(0.05)      # 10..100
            play "recording2"; bc "shootenemyball"
            brightness -= 50; W(0.05)
            brightness -= 50; W(0.05)
            brightness = 0
            flash(EN3, 5)
        bc "spawncannon333"; W(3)

enemy 3 damage / EN3: stopOther(EN3); bc "enemy3hit"; play "output (24)"; flash(EN3, 10)
enemy3hit / EN3:
    for c in (costume8,costume9,costume10,costume11,costume12,costume4):
        costume c; W(0.05)
    bc "enemy3 run"; W(1); bc "en3atk"

enemy 3 oof / EN3:
    stopOther(EN3); V["cannondefeats"] += 1; play "output (24)"
    for c in (costume8,costume9,costume10,costume11,costume12): costume c; W(0.05)
    goto(-169,-55); glide(EN3, 0.2, -293, -55); hide()
    if V["cannondefeats"] in (2,3):
        bc "Chase for the door"; bc "startshockwaves333"; bc "enemy3 run"
        bc "restoreEN3"
        show(); goto(-293,-55); glide(EN3, 1, -169, -55); W(1); bc "en3atk"
    elif V["cannondefeats"] == 4:
        bc "stopmusic"; W(3); V["doors"] = 4; bc "go to lobby"

oofie / EN3:       Instant. stopOther(EN3)
go to lobby / EN3: Instant. stopOther(EN3); hide()
```

```
shootenemyball / CBALL2 [_fly]:  goto(-122,-41); glide(CBALL2, 0.2*ATK, 250, -41); hide()
shootenemyball / CBALL2 [_show]: show(); back(); flash(CBALL2)
shootenemyball / CBALL2 [_hit]:  forever:
    if CBALL2.touching(ANIM): bc "take damage"; hide(); stopOther(CBALL2); return
    Y
go to lobby / CBALL2: Instant. hide()

spawncannon333 / CANNON3 [_flash]: show(); flash(CANNON3)
spawncannon333 / CANNON3 [_arm]:   play "recording1"
    forever: if key(x) and CANNON3.touching(ANIM): bc "cannonball3"; return
             Y
spawncannon333 / CANNON3 [_slide]: goto(262,-72); glide(CANNON3, 2, -263, -72)
                                   stopOther(CANNON3); hide()
cannonball3 / CANNON3: play "explosion meme"; flash(CANNON3); W(0.5); hide()
hidecannon3 / CANNON3: Instant. stopOther(CANNON3); hide()   # registered, never sent
go to lobby / CANNON3: Instant. stopOther(CANNON3); hide()

startshockwaves333 / SW3: forever: W(R(5,30)); bc "shock3"
shock3 / SW3 [_move]: show(); goto(241,-75)
                      while SW3.x > -241: SW3.x -= 7/ATK; Y
                      stopOther(SW3); hide()
shock3 / SW3 [_hit]:  forever: if SW3.touching(ANIM): bc "take damage"; return
                               Y
enemy 3 oof / SW3: Instant. stopOther(SW3); hide()

level3 / ABYSS: ABYSS.back(); show()
    forever: if ABYSS.touching(PLAYER, inset=0.0): bc "falloofie"; return
             Y
```

Enemy 3 health bar (`costume3, costume4, costume5, costume6, costume7, Oof2` — 5 hits):

```
startlv3 / E3STAT: bc "restorecanon"; W(0.05)      # "restorecanon" has NO handler
                   show(); costume "costume3"; goto(308,-144); glide(0.3,150,-144)
enemy 3 damage / E3STAT [_bounce]: glide(0.05,150,-139); glide(0.1,150,-144)
enemy 3 damage / E3STAT [_dmg]:    next_costume(); W(0.05)
                                   if costumeIs("Oof2"): bc "enemy 3 oof"
restoreEN3 / E3STAT: costume "costume3"; glide(0.05,150,-139); glide(0.1,150,-144)
go to lobby / E3STAT: goto(150,-144); glide(0.3,308,-144); hide()
game over   / E3STAT: Instant. hide()
```

#### 13.14 Final level — Riby

```
final cutscene / RIBY:
    show(); point(90); goto(198,-75)
    costume "door"; W(0.1)
    costume "costume2"; W(1)
    costume "costume3"; W(0.05)
    costume "costume13"; W(0.7)
    costume "costume3"; W(0.05)
    costume "costume14"; W(0.05)
    riby_laugh(10)
    point(-90); costume "costume2"; W(0.2)
    costume "costume3"; W(0.05); costume "costume4"; W(0.05)
    goto(198,-75)
    glide(RIBY, 0.05, 176, -51); bc "cutscenehit"
    glide(RIBY, 0.05, 143, -38)
    glide(RIBY, 0.05, 126, -75)
    costume "costume11"; W(0.05); costume "costume2"; W(1)
    bc "startfinallevel"; bc "PlayerEnable"; bc "playerfinalenable"

startfinallevel / RIBY: show(); goto(126,-75); point(-90); costume "costume2"; W(1)
                        bc "phase1riby"
RibyRun / RIBY: riby_run_anim()

riby_cannon_volley(times):
    repeat times:
        danger(1); RIBY.costume "costume2"; bc "aim canon evil"
        repeat 10: RIBY.point(ANIM.x > RIBY.x ? 90 : -90); W(0.05)
        bc "shootEVILcanonball"
    bc "hideEvilCanon"; danger(0); W(2); bc "jumpatkriby"

phase1riby / RIBY:
    V["RibyDanger"] = 1; bc "RibyRun"; bc "playerfinalenable"
    goto(126,-75); glide(RIBY, 0.5, 0, -75)
    stopOther(RIBY); bc "playerfinalenable"
    costume "costume10"
    glide(RIBY, 0.3, -99, 30); glide(RIBY, 0.1, -146, -75)
    bc "shockwaveriby"; play "groundpound"; costume "costume11"
    glide(RIBY, 0.1, -151, -75)
    danger(0); riby_laugh(20); danger(1)
    point(90); bc "RibyRun"; glide(RIBY, 0.5, 126, -75)
    stopOther(RIBY); bc "playerfinalenable"
    costume "costume10"; goto(126,-75)
    glide(RIBY, 0.1, 126, 30); glide(RIBY, 0.1, 126, -75)
    bc "shockwaveriby"; play "groundpound"; costume "costume11"; W(0.1)
    danger(0); riby_laugh(20); danger(1)
    point(-90); bc "RibyRun"; glide(RIBY, 0.5, 0, -75)
    stopOther(RIBY); W(1)
    riby_cannon_volley(1)

playerfinalenable / RIBY:
    forever:
        if RIBY.touching(ANIM):
            if V["RibyDanger"] == 1: bc "take damage"; return
            if V["RibyDanger"] == 0 and key(x): bc "enemy4 damage"; return
        Y

riby_dash_sweeps():
    repeat R(2,6):
        riby_laugh(7); danger(1); point(-90); play "slide"; costume "costume11"
        glide(RIBY, 0.2, -200, -75)
        riby_laugh(7); danger(1); point(90);  play "slide"; costume "costume11"
        glide(RIBY, 0.2, 200, -75)
    costume "costume13"; W(1); danger(0)
    bc "RibyRun"; point(-90); glide(RIBY, 1, 0, -75)
    stopOther(RIBY); costume "costume2"; danger(0); W(1); bc "jumpatkriby"

enemy4 damage / RIBY:
    V["damageway4"] = R(1,3)                  # E4STAT may overwrite this with 4
    stopOther(RIBY); play "hit2"; costume "OOF"; flash(RIBY, 10)
    ghost = 0; danger(1); point(-90); bc "RibyRun"; glide(RIBY, 0.5, 0, -75)
    stopOther(RIBY); danger(1)
    costume "costume2"; W(0.7); costume "costume13"; W(0.3); costume "costume2"; W(0.5)
    if V["damageway4"] == 1: riby_cannon_volley(R(1,3))
    elif V["damageway4"] == 2: bc "jumpatkriby"
    elif V["damageway4"] == 3:
        bc "RibyRun"; point(90); glide(RIBY, 0.5, 200, -75)
        stopOther(RIBY); danger(1); riby_dash_sweeps()
    elif V["damageway4"] == 4:
        stopOther(RIBY); danger(1); point(90); costume "costume2"; W(0.3)
        bc "RibyRun"; glide(RIBY, 0.6, -56, -75)
        stopOther(RIBY); costume "costume2"; W(0.3)
        bc "en1final"; W(1)
        costume "costume10"
        glide(RIBY, 0.1, -1, -4); glide(RIBY, 0.1, 35, -22); glide(RIBY, 0.1, 83, -72)
        hide(); stopOther(RIBY); bc "possessen1"

jumpatkriby / RIBY:
    repeat R(3,6):
        costume "costume10"; play "jump"; point(RIBY.x < 0 ? 90 : -90)
        glide(RIBY, 0.2, 0, 33)
        costume "costume17"; W(0.1)
        point(ANIM.x > RIBY.x ? 90 : -90); W(0.2)
        danger(1)
        glideTo(RIBY, 0.5*ATK, ANIM)
        play "groundpound"; glide(RIBY, 0.01, RIBY.x, -75)
        danger(0); bc "shockwaveriby"
        costume "costume11"; W(0.05); costume "costume2"; W(2)
        danger(1)
    W(1); bc "jumpatkriby"        # anti-softlock: every other pattern chains onward

RibyOUT / RIBY:
    show(); goto(EN1.x, -75); danger(0); riby_laugh(15); danger(1)
    bc "RibyRun"; point(90); glide(RIBY, 0.5, 200, -75)
    stopOther(RIBY); danger(1); riby_dash_sweeps()

enemy 4 oof / RIBY:
    stopOther(RIBY); play "hit"; costume "OOF"; clear_fx(); W(1)
    repeat 7: y += 5; Y
    glide(RIBY, 0.7, RIBY.x, -204)
    hide(); bc "door4openagain"

go to lobby / RIBY: Instant. stopOther(RIBY); hide()
oofie / RIBY:       Instant. stopOther(RIBY)
```

Evil cannon and its two balls:

```
aim canon evil / EVILC:
    play "recording1"; front(); clear_fx(); show()
    goto_sprite(RIBY); EVILC.y = -74
    repeat 30: point(ANIM.x > EVILC.x ? 90 : -90); W(0.05)
    V["evilcanonballdirection"] = (EVILC.direction > 0) ? 90 : -90
shootEVILcanonball / EVILC: play "explosion meme"; flash(EVILC); clear_fx()
hideEvilCanon / EVILC: Instant. stopOther(EVILC); hide()
go to lobby   / EVILC: Instant. stopOther(EVILC); hide()

mk_evil_ball(spr, step, edge):
    fly():  spr.goto_sprite(EVILC); spr.show()
            if step > 0: while spr.x < edge: spr.x += step; Y
            else:        while spr.x > edge: spr.x += step; Y
            spr.hide(); stopOther(spr)
    show(): spr.show(); spr.front(); flash(spr)
    hit():  forever: if spr.touching(ANIM): bc "take damage"; spr.hide()
                                            stopOther(spr); return
                     Y
    lobby(): Instant. spr.hide()

CBALL3: step = +20/ATK, edge = +240
CBALL4: step = -20/ATK, edge = -240
register per ball, in this order:
    on("shootEVILcanonball", spr)(fly)
    on("shootEVILcanonball", spr)(show)
    on("shootEVILcanonball", spr)(hit)
    on("go to lobby",        spr)(lobby)
(CBALL3 first, then CBALL4)
```

`V["evilcanonballdirection"]` is written but never read — keep the write.

Riby's shockwaves:

```
shockwaveriby / SW3 [_left]:     show(); goto_sprite(RIBY)
                                 while SW3.x > -241: SW3.x -= 10/ATK; Y
                                 stopOther(SW3); hide()
shockwaveriby / SW3 [_left_hit]: forever: if SW3.touching(ANIM): bc "take damage"; return
                                          Y
shockwaveriby / SW4 [_right]:    show(); goto_sprite(RIBY)
                                 while SW4.x < 241: SW4.x += 10/ATK; Y
                                 stopOther(SW4); hide()
shockwaveriby / SW4 [_right_hit]:forever: if SW4.touching(ANIM): bc "take damage"; return
                                          Y
shockwaveriby / SW5 [_heal]:     W(0.7); V["healwavedirection"] = R(1,2)
                                 show(); goto_sprite(RIBY)
                                 if V["healwavedirection"] == 1:
                                     while SW5.x <  241: SW5.x += 10; Y   # NOT /ATK
                                 else:
                                     while SW5.x > -241: SW5.x -= 10; Y   # NOT /ATK
                                 stopOther(SW5); hide()
shockwaveriby / SW5 [_heal_touch]: forever:
    if SW5.touching(ANIM): bc "partialrestore"; SW5.play "pop"; hide()
                           stopOther(SW5); return
    Y
```

Riby health bar (`Enemy4Stats`, 15 costumes, 14 damage steps; **not in the layer order, so
it draws in front of everything** — §6.4):

```
startfinallevel / E4STAT: bc "restorecanon"; W(0.05)
                          show(); costume "costume1"; goto(308,-144)
                          glide(0.3,150,-144)
enemy4 damage / E4STAT [_bounce]: glide(0.05,150,-139); glide(0.1,150,-144)
enemy4 damage / E4STAT [_dmg]:
    next_costume(); W(0.05)
    if costume_name.lower() in ("costume5","costume6","costume7"): V["damageway4"] = 4
    if costumeIs("Oof2"): bc "enemy 4 oof"
go to lobby / E4STAT: goto(150,-144); glide(0.3,308,-144); hide()
game over   / E4STAT: Instant. hide()
ending cutscene / E4STAT: Instant. stopOther(E4STAT); hide()
```

The costume order is `costume1, costume9, costume10, costume11, costume12, costume13,
costume14, costume8, costume2, costume3, costume4, costume5, costume6, costume7, Oof2`,
so `damageway4 = 4` (the possession phase) triggers on damage steps 11, 12 and 13.

#### 13.15 Ending — trophy walk and score screen

```
ending cutscene / REWARD: show(); front(); forever: goto_sprite(ANIM); Y
the end / REWARD:         Instant. stopOther(REWARD); hide()

ending score / SCORE:
    front(); show(); costume "costume1"
    play_until_done("output (33)")          # sfx + wait 4.05 s
    W(0.6)
    if V["knockouts"] == 0:
        if V["taken damage"] == 0: bc "a"
        else:
            if V["lives"] > 2:
                bc (V["has healed"] > 0 ? "c" : "d")
            else: bc "b"
        return
    if V["lives"] > 2:
        bc (V["has healed"] > 0 ? "c" : "d")
        return
    if V["knockouts"] in (1,2): bc "c"; return
    if V["knockouts"] > 6:      bc "f"; return
    bc "b"

grades = { "a": ("costume5",  "Koki A score music"),
           "b": ("costume7",  "lowatrezzo"),
           "c": ("costume9",  "Koki C Ending score music"),
           "d": ("costume10", "Koki D score"),
           "f": ("costume11", "Koki F score") }

for g, (costume, track) in grades.items():        # insertion order a,b,c,d,f
    on(g, SCORE) -> Instant. SCORE.costume costume; SCORE.music(track)
    on(g, SCORE) -> W(1)
                    forever:
                        if key(enter):
                            stopOther(SCORE); eng.sound.stop_all()
                            bc "whitechange"; W(0.05); bc "go to lobby"
                            SCORE.hide(); return
                        Y
```

`Koki D score` is a **14.46 s WAV** used as looping music — the only asset that needs the
looping-WAV path (§10.2).

#### 13.16 Complete event → handler table

The definitive registration table *(measured by instrumenting the real engine)*. Handlers
within each row are in registration order; the C port must produce the same list.

| event | n | handlers (sprite:function) |
| --- | --- | --- |
| `flag` | 2 | Stage:_stage_flag, Dynaris Logo:_logo |
| `start intro` | 3 | Stage:_stage_intro, intro:_intro, White:_white_si |
| `start button enable` | 2 | StartButton:_btn_anim, StartButton:_btn_input |
| `start game` | 3 | intro:_intro_out, Sprite1:_panel, KokiStats:_kstat_newgame |
| `go to lobby` | 30 | Stage:_stage_lobby, Platform:_plat_lobby, Lives:_lives, Door1:_door1_watch, Door1:_door1_flash, Door2:_door2_watch, Door2:_door2_flash, Door3:_door3_watch, Door3:_door3_flash, Door4:_door4_watch, Door4:_door4_flash, Enemy 1:_en1_lobby, Enemy1Stats:_e1stat_out, Cannon:_cannon_lobby, Cannon ball:_cball_lobby, PlaneChar:_plane_lobby, KokiPlaneStats:_kpstat_out, Enemy2:_en2_lobby, Enemy2Stats:_e2stat_out, Cannon2:_cannon2_lobby, Gas tank:_gas_lobby, Enemy 3:_en3_lobby, Cannon ball2:_eball_lobby, Cannon3:_cannon3_lobby, Enemy3Stats:_e3stat_out, Riby:_riby_lobby, EvilCannon:_evilc_lobby, Cannon ball3:_lobby, Cannon ball4:_lobby, Enemy4Stats:_e4stat_out |
| `whitechange` | 1 | White:_white_change |
| `PlayerEnable` | 9 | White:_white_pe, Player:_player_physics, CharacterAnim:_anim_follow, CharacterAnim:_anim_reset, CharacterAnim:_anim_walk, CharacterAnim:_anim_jump, CharacterAnim:_anim_idle, CharacterAnim:_anim_face, KokiStats:_kstat_in |
| `RUNenable` | 3 | Player:_player_run, CharacterAnim:_anim_run_follow, CharacterAnim:_anim_run_start |
| `RUNNN` | 3 | CharacterAnim:_anim_runnn_follow, CharacterAnim:_anim_runnn_anim, CharacterAnim:_anim_runnn_jump |
| `jumpRUN` | 1 | CharacterAnim:_anim_jumprun_follow |
| `turn anim` | 2 | Player:_player_off, CharacterAnim:_anim_door |
| `idleanim` | 1 | CharacterAnim:_anim_idle_set |
| `disableplayer` | 2 | Player:_player_off, CharacterAnim:_anim_disable |
| `leftdash` / `rightdash` | 1 each | Player:_player_ldash / Player:_player_rdash |
| `dodge` | 1 | CharacterAnim:_anim_dodge |
| `take damage` | 3 | Stage:_dmg_gate, Enemy 1:_en1_rearm, Enemy 3:_en3_rearm |
| `koki hurt` | 4 | CharacterAnim:_anim_dmg_sound, CharacterAnim:_anim_dmg_flash, KokiStats:_kstat_dmg_bounce, KokiStats:_kstat_dmg |
| `takeplanedamage` | 1 | Stage:_plane_dmg_gate |
| `plane hurt` | 4 | PlaneChar:_plane_hit, PlaneChar:_plane_hit_flash, KokiPlaneStats:_kpstat_bounce, KokiPlaneStats:_kpstat_dmg |
| `oofie` | 8 | Stage:_stage_stop, Player:_player_off, CharacterAnim:_anim_oofie, KokiStats:_kstat_oofie, Enemy 1:_en1_playerdead, Cannon ball:_cball_playerdead, Enemy 3:_en3_playerdead, Riby:_riby_playerdead |
| `falloofie` | 5 | Stage:_stage_stop, Player:_player_off, CharacterAnim:_anim_fall, KokiStats:_kstat_fall, Cannon ball:_cball_playerdead |
| `planeoofie` | 6 | Stage:_stage_stop, Cannon ball:_cball_playerdead, PlaneChar:_plane_dead, KokiPlaneStats:_kpstat_oof, Enemy2:_en2_playerdead, Gas tank:_gas_off |
| `game over` | 7 | Stage:_stage_stop, GameOver:_gameover_music, GameOver:_gameover, Enemy1Stats:_e1stat_go, Enemy2Stats:_e2stat_go, Enemy3Stats:_e3stat_go, Enemy4Stats:_e4stat_go |
| `restoreallhealth` | 1 | KokiStats:_kstat_restore |
| `partialrestore` | 1 | KokiStats:_kstat_partial |
| `temporary hit` / `temporaryhit2` | 1 each | KokiStats:_kstat_temphit / _kstat_temphit2 |
| `doorinteracted` | 5 | Lives:_lives_hide, Door1:_h, Door2:_h, Door3:_h, Door4:_h |
| `level1` | 2 | Platform:_plat_lv1, Door1:_door1_go |
| `level2` | 6 | Door2:_door2_go, Door1:_h, Door2:_h, Door3:_h, Door4:_h, A to Dodge:_dodge_hint |
| `level3` | 7 | Platform:_plat_lv3, Door3:_door3_go, Door1:_h, Door2:_h, Door3:_h, Door4:_h, Abyss:_abyss |
| `level 4` | 1 | Door4:_door4_go |
| `unlock door2` / `lockAlldoors` | 1 each | Door2:_door2_unlock / Door2:_doors_lock |
| `door4openagain` | 2 | Door4:_door4_again_flash, Door4:_door4_again_watch |
| `startlv1` | 5 | Stage:_stage_lv1, KokiStats:_kstat_lv1, Enemy 1:_en1_start, Enemy 1:_en1_bell, Enemy1Stats:_e1stat_in |
| `boxing bell` | 1 | Stage:_stage_bell |
| `hitbox` | 2 | Enemy 1:_en1_hitbox, Enemy 3:_en3_hitbox |
| `enemy1 idle` | 1 | Enemy 1:_en1_idle |
| `enemy1damage` | 3 | Enemy 1:_en1_damaged, Enemy1Stats:_e1stat_bounce, Enemy1Stats:_e1stat_dmg |
| `enemy1 pounce` / `phaseagain` / `quickhitbox` | 1 each | Enemy 1 |
| `enemy1 oof` | 2 | Stage:_stage_stop, Enemy 1:_en1_dead |
| `en1final` / `possessen1` / `activehitonen1` | 1 each | Enemy 1 |
| `shockwave` | 4 | Shockwave:_move, Shockwave:_hit, Shockwave2:_move, Shockwave2:_hit |
| `spawncanon` | 2 | Cannon:_cannon_flash, Cannon:_cannon_arm |
| `canonball` | 4 | Cannon:_cannon_fired, Cannon ball:_cball_fly, Cannon ball:_cball_show, Cannon ball:_cball_friendly_fire |
| `canonball2` | 3 | Cannon ball:_cball2_show, Cannon ball:_cball2_fly, Cannon2:_cannon2_fired |
| `cannonball3` | 3 | Cannon ball:_cball3_show, Cannon ball:_cball3_fly, Cannon3:_cannon3_fired |
| `quick tap` | 2 | QuickPress:_quick_input, QuickPress:_quick_show |
| `Chase for the door` | 1 | QuickPress:_quick_chase |
| `planecutscene` | 3 | cutscene1:_cut1_fall, PlaneChar:_plane_in, PlaneChar:_plane_prop |
| `planeANIM` / `planeoofanim` / `rechargeeffect` | 1 each | PlaneChar |
| `enableplane` | 4 | PlaneChar:_plane_up, _plane_down, _plane_bounds, _plane_boost |
| `startplane` | 1 | KokiPlaneStats:_kpstat_in |
| `restoreplane` | 1 | KokiPlaneStats:_kpstat_restore |
| `startlv2` | 6 | Stage:_stage_lv2, Platform:_plat_lv2, KokiStats:_kstat_lv2, KokiPlaneStats:_kpstat_reset, Enemy2:_en2_entry, Enemy2Stats:_e2stat_in |
| `hitbox2` / `enemy bright` | 1 each | Enemy2 |
| `en2 cycle 1` | 1 | Enemy2:_en2_cycle1 |
| `en2 cycle 2` | 2 | Enemy2:_en2_cycle2, Gas tank:_gas_trigger |
| `enemy2 damage` | 4 | Enemy2:_en2_damaged, Enemy2:_en2_dmg_flash, Enemy2Stats:_e2stat_bounce, Enemy2Stats:_e2stat_dmg |
| `enemy2 oof` | 2 | Stage:_stage_stop, Enemy2:_en2_dead |
| `ping enemy2 stats` | 1 | Enemy2Stats:_e2stat_ping |
| `spawncanon222` | 3 | Cannon2:_cannon2_flash, _cannon2_arm, _cannon2_drop |
| `spawnfuel` | 2 | Gas tank:_gas_fly, Gas tank:_gas_touch |
| `startlv3` | 7 | Stage:_stage_lv3, KokiStats:_kstat_lv3, Enemy 3:_en3_place, Enemy 3:_en3_hitbox_arm, Enemy 3:_en3_run_start, Enemy 3:_en3_begin_atk, Enemy3Stats:_e3stat_in |
| `enemy3 run` / `en3atk` / `enemy3hit` | 1 each | Enemy 3 |
| `enemy 3 damage` | 3 | Enemy 3:_en3_damaged, Enemy3Stats:_e3stat_bounce, Enemy3Stats:_e3stat_dmg |
| `enemy 3 oof` | 2 | Enemy 3:_en3_phase_down, Shockwave3:_sw3_off |
| `restoreEN3` | 1 | Enemy3Stats:_e3stat_restore |
| `shootenemyball` | 3 | Cannon ball2:_eball_fly, _eball_show, _eball_hit |
| `spawncannon333` | 3 | Cannon3:_cannon3_flash, _cannon3_arm, _cannon3_slide |
| `hidecannon3` | 1 | Cannon3:_cannon3_hide *(never broadcast)* |
| `startshockwaves333` | 1 | Shockwave3:_sw3_random |
| `shock3` | 2 | Shockwave3:_sw3_move, Shockwave3:_sw3_hit |
| `stopmusic` | 3 | Stage:_stage_stop, Player:_player_off, CharacterAnim:_anim_lv3_dance |
| `final cutscene` | 7 | Stage:_stage_finalcut, Player:_player_off, CharacterAnim:_anim_finalcut, Door1:_h, Door2:_h, Door3:_h, Riby:_riby_cutscene |
| `cutscenehit` | 1 | CharacterAnim:_anim_cuthit |
| `startfinallevel` | 8 | Stage:_stage_final, Door1:_h, Door2:_h, Door3:_h, Door4:_h, QuickPress:_quick_final, Riby:_riby_fight_start, Enemy4Stats:_e4stat_in |
| `phase1riby` / `RibyRun` / `RibyOUT` / `jumpatkriby` / `playerfinalenable` | 1 each | Riby |
| `enemy4 damage` | 4 | CharacterAnim:_anim_en4dmg, Riby:_riby_damaged, Enemy4Stats:_e4stat_bounce, Enemy4Stats:_e4stat_dmg |
| `enemy 4 oof` | 2 | Stage:_stage_stop, Riby:_riby_dead |
| `aim canon evil` / `hideEvilCanon` | 1 each | EvilCannon |
| `shootEVILcanonball` | 7 | EvilCannon:_evilc_fire, Cannon ball3:_fly, _show, _hit, Cannon ball4:_fly, _show, _hit |
| `shockwaveriby` | 6 | Shockwave3:_rsw_left, Shockwave3:_rsw_left_hit, Shockwave4:_rsw_right, Shockwave4:_rsw_right_hit, Shockwave5:_rsw_heal, Shockwave5:_rsw_heal_touch |
| `ending cutscene` | 13 | Stage:_stage_ending, Platform:_plat_end, Player:_player_off, CharacterAnim:_anim_ending_walk, CharacterAnim:_anim_ending_anim, Lives:_lives_hide2, KokiStats:_kstat_end, Door1:_h, Door2:_h, Door3:_h, Door4:_h, Enemy4Stats:_e4stat_end, Reward:_reward |
| `the end` | 3 | Stage:_stage_stop, White:_white_end, Reward:_reward_off |
| `ending score` | 1 | Sprite2:_score |
| `a` `b` `c` `d` `f` | 2 each | Sprite2:_show, Sprite2:_input |

`restorecanon` is broadcast twice (by `Enemy3Stats` and `Enemy4Stats`) and has **no
handler** — a dead message inherited from the sb3. Keep the broadcast.

#### 13.17 Deliberate deviations from the Scratch original

These are already in the shipped Python and are part of "1:1" for this port. Do not
"fix" them back.

| Deviation | Where | Reason |
| --- | --- | --- |
| Pixelate effect omitted entirely | everywhere (Scratch sets 10–15) | blocks would be ~1 px at 240×175 |
| "Possessed" Enemy 1 uses brightness −50 instead of a colour shift | `possessen1` | no colour effect in the port |
| Final-cutscene music pitch-slide replaced by "wait 1 s then stop" | Stage `final cutscene` | the player cannot pitch-bend |
| Music loops with a looping voice instead of `forever / play until done` | all music | — |
| Post-hit i-frames (0.9 s) gate `take damage` / `takeplanedamage` | Stage handlers 20, 21 | a double shockwave used to cost 2 HP |
| Quicktime dash flees *away* from Enemy 1 (`leftdash` if `ANIM.x < EN1.x`) | `quick tap` | the original always dashed right, into the boss |
| Plane boost is chordless (last vertical tap within 2 s, else toward open space) | `enableplane` `_boost` | single-key keypads cannot chord |
| Attack speeds scaled by `ATK` (default 1.35 = 35 % slower) | 14 sites, all listed above | playability |
| `jumpatkriby` re-broadcasts itself after `W(1)` | Riby | otherwise the fight soft-locks |
| In-flight cannonballs are killed on player death | `Cannon ball` | stops a posthumous boss restart |
| Animation gates test `CharacterAnim`, not the physics dot | `_anim_walk` / `_anim_jump` | matches the original; the earlier port had it wrong |
| Collision is pixel-mask, not full-canvas rect | `touching()` | canvas margins caused phantom hits |

Complete list of `ATK` uses (for a `NEODCT_KOKI_ATTACK_SLOW=1.0` A/B test):
`_en1_pounce` ×2 glides, `_en1_possessed` ×2 glides, `_mk_shockwave` glide,
`_eball_fly` glide, `_sw3_move` step (`7/ATK`), `_rsw_left`/`_rsw_right` steps
(`10/ATK`), `_riby_jump_attacks` glideTo, evil-ball steps (`±20/ATK`).
`Shockwave5`'s heal wave deliberately does **not** scale.

#### 13.18 Environment variables

| Variable | Default | Effect |
| --- | --- | --- |
| `NEODCT_KOKI_ATTACK_SLOW` | `1.35` | attack-speed multiplier; invalid/≤0/NaN → 1.35 |
| `NEODCT_KOKI_IMG_CACHE_KB` | 3072 / 1536 / 1024 by RAM tier | costume RGBA cache budget |
| `NEODCT_KOKI_FX_CACHE_KB` | 1024 / 512 / 384 by RAM tier | effect-variant cache budget |
| `NEODCT_KOKI_NOSOUND` | unset | disable audio entirely |
| `NEODCT_KOKI_AUDIO` | unset | `subprocess` forces external players; `miniaudio` makes the mixer mandatory |
| `NEODCT_KOKI_ABUF_MS` | `150` | mixer device buffer, ms |
| `NEODCT_KOKI_WAV_PLAYER` / `NEODCT_KOKI_MP3_PLAYER` | auto | force an external player binary |
| `NEODCT_KOKI_MAX_SFX` | 3 (1 if `mpv` sfx on <72 MB) | concurrent sfx cap |
| `NEODCT_KOKI_SOUND_DEBUG` | unset | log spawn command lines, let player stderr through |
| `NEODCT_KOKI_PERF` | unset | print `avg frame Xms (Y fps)` every 5 s |

---

## Public interface (the functions other parts call)

### From the core into the app

Exactly one entry point. Today:

```python
# apps/Koki/main.py
def run(ui): ...        # blocks until the player quits; returns None
```

In C, under `nd-apprun`:

```c
/* apps/Koki/src/app.c — the only exported symbol */
int app_run(nd_app_ctx *ui);   /* 0 = normal exit */
```

The `ui` context Koki actually touches — this is the whole contract, and it is much
smaller than the widget apps need:

| Member | Used for |
| --- | --- |
| `ui->canvas` | the shared 240×175 RGB888 surface everything is composited into |
| `ui->fb.update(canvas)` | present one frame |
| `ui->keypad_fd` | raw evdev fd, read directly (never `read_keypress`) |
| `ui->matrix_input` | optional I²C/gpiozero matrix keypad object (`read_key`, `matrix_to_code`, `scanner->_held`) |
| `ui->draw` | pause-menu rectangle and text only |
| `ui->font_n` (20 px), `ui->font_s` (14 px) | pause-menu text only |

Koki **does not** use `ui.read_keypress`, `ui.softkey`, any widget, any setting, any
database, the modem, or `/NeoDCT/User`. It writes nothing persistent — there is no
high-score file. Its only inputs are its own `assets/` directory and `/proc/meminfo`.

Under the process-per-app architecture the matrix keypad becomes a problem: the scanner
object lives in the core and Koki reaches into its private `_held` state. Two options,
both need a decision (see Open Questions): expose the raw evdev-style stream over the
app's inherited fd (preferred — the core's matrix driver already synthesises key codes),
or add a tiny shared-memory "held key bitmask" the core publishes and apps read.

### Inside the app

```c
/* koki_engine.h */
koki_engine *koki_engine_new(nd_app_ctx *ui, const char *app_dir);
void         koki_engine_free(koki_engine *e);       /* == teardown() */
void         koki_engine_run(koki_engine *e);
koki_sprite *koki_sprite_get(koki_engine *e, const char *name);   /* create-on-first-use */
void         koki_set_layer_order(koki_engine *e, const char *const *names, int n);
void         koki_backdrop(koki_engine *e, const char *name);
void         koki_broadcast(koki_engine *e, const char *event);
int          koki_on(koki_engine *e, const char *event,
                     koki_sprite *sprite, koki_script_fn fn, size_t ctx_size);
void         koki_stop_other_scripts(koki_engine *e, koki_sprite *sprite);
void         koki_stop_all_scripts(koki_engine *e);
double       koki_now(koki_engine *e);
int          koki_key(koki_engine *e, koki_key k);
int          koki_kdir(koki_engine *e);
int32_t      koki_randint(koki_engine *e, int32_t a, int32_t b);

/* koki_sprite.h — the Scratch verbs */
void  koki_set_costume_name(koki_sprite *s, const char *name);
void  koki_set_costume_index(koki_sprite *s, int i);
void  koki_next_costume(koki_sprite *s);
int   koki_costume_is(const koki_sprite *s, const char *name);
int   koki_costume_number(const koki_sprite *s);       /* 1-based */
void  koki_show(koki_sprite *s); void koki_hide(koki_sprite *s);
void  koki_front(koki_sprite *s); void koki_back(koki_sprite *s);
void  koki_clear_fx(koki_sprite *s);
void  koki_goto(koki_sprite *s, double x, double y);
void  koki_goto_sprite(koki_sprite *s, const koki_sprite *o);
void  koki_point(koki_sprite *s, double dir);
void  koki_point_towards(koki_sprite *s, const koki_sprite *o);
void  koki_move_steps(koki_sprite *s, double steps);
int   koki_touching(koki_sprite *a, koki_sprite *b, double inset);
void  koki_play(koki_sprite *s, const char *sound);
void  koki_sprite_music(koki_sprite *s, const char *sound);

/* koki_sched.h — child protothreads used by scripts */
koki_step koki_wait(koki_wait_ctx *c, koki_engine *e, double secs);
koki_step koki_glide(koki_glide_ctx *c, koki_sprite *s, double secs, double tx, double ty);
koki_step koki_glide_to(koki_glide_ctx *c, koki_sprite *s, double secs, koki_sprite *o);
koki_step koki_play_until_done(koki_wait_ctx *c, koki_sprite *s, const char *sound);
```

`libneodct` must supply, for this app: `nd_image_new/free`, `nd_image_load_png`,
`nd_image_paste_alpha` (the §6.2 blend), `nd_image_crop_zeropad`, `nd_image_resize_nearest`
(§6.3), `nd_image_point_lut`, `nd_image_flip_h`, `nd_image_channel_a`,
`nd_draw_rect`, `nd_draw_text`, and `nd_mt19937_*`.

---

## External dependencies and their C replacements

| Dependency | Used for | C replacement |
| --- | --- | --- |
| `PIL.Image.open` | decoding 235 PNGs (8-bit RGBA/RGB, non-interlaced) | `nd_png.c` in `libneodct`: zlib `inflate` + PNG filters. ~450 LOC. zlib is already in the image. **Must ignore `bKGD`, `cHRM`, `tEXt`, `tIME`** exactly as Pillow does. |
| `PIL.Image.convert("RGBA"/"RGB")` | normalise decoded images | trivial channel expansion (RGB→RGBA sets alpha 255) |
| `PIL.Image.paste(img,(x,y),img)` | every sprite blit | `nd_image_paste_alpha`, formula in §6.2, clipped |
| `PIL.Image.paste(img,(0,0))` | backdrop copy | opaque `memcpy` per row |
| `PIL.Image.paste(colour, box)` | black fill when no backdrop | `nd_image_fill` |
| `PIL.Image.crop` | backdrop window, mask overlap | zero-padding crop |
| `PIL.Image.resize(NEAREST)` | runtime-scaled sprites | §6.3 |
| `PIL.Image.transpose(FLIP_LEFT_RIGHT)` | left-right rotation style | row reversal |
| `PIL.Image.point(lut)` | brightness / ghost / alpha threshold | per-band LUT pass; can be folded into the blend |
| `PIL.Image.getchannel("A")` | collision masks | stride-4 gather |
| `PIL.ImageChops.multiply` + `getbbox` | mask overlap test | `if (a[i] && b[i]) return 1;` — bit-identical, no allocation |
| `PIL.ImageDraw.rectangle` / `.text` | pause menu only | `nd_draw_rect`, `nd_draw_text` from the shared rasterizer + FreeType at 20 px / 14 px |
| `miniaudio` (`PlaybackDevice`, `stream_file`, dr_wav/dr_mp3) | the audio backend | ALSA `snd_pcm` in a thread + a WAV reader (trivial: everything is already 22050/mono/s16) + an MP3 decoder. Recommend vendoring **`minimp3`** (~2 kLOC, public domain, fixed-point ARM path) rather than libmpg123. |
| `audioop.add` | saturating s16 mix | 10-line loop; on Cortex-A7 use `__QADD16` via `arm_acle.h` or plain clamps |
| `threading.Lock` | mixer/audio-thread guard | `pthread_mutex_t` |
| `subprocess.Popen` / `shutil.which` | external-player fallback | `posix_spawn` + `PATH` walk. **Must obey the fork/exec rule** in CODING-STANDARDS §1.1. Consider dropping the fallback entirely (see Risks). |
| `select.select` + `os.read` on `keypad_fd` | held-key tracking | `poll()`/`read()` on the fd, 24- and 16-byte `input_event` |
| `struct.unpack("llHHI" / "IIHHI")` | evdev record parse | direct struct cast; 32-bit ARM gives 16-byte records (`IIHHI`), but accept both |
| `random.Random` | 8 `randint` sites | `nd_mt19937.c` (§12) |
| `json.load` | `assets/manifest.json`, 52 KB | `libneodct` JSON reader; parse **once** into interned structs and free the text |
| `collections.OrderedDict` | LRU caches, `active` script map | intrusive doubly linked list + open-addressed index |
| `ctypes.CDLL(None).malloc_trim` | return heap at exit | `malloc_trim(0)` under `#ifdef __GLIBC__` |
| `open("/proc/meminfo")` | cache tiering | `fopen` + parse the first line |
| `time.monotonic`, `time.sleep` | frame pacing | `clock_gettime(CLOCK_MONOTONIC)`, `nanosleep` |
| `gc.collect()` / `sys.modules` purge | per-launch cleanup | not needed: the app is its own process |
| `rsvg-convert`, `ffmpeg`, ImageMagick | **host only**, `build_assets.py` | not ported |

Nothing else. Koki has no network, no sqlite, no settings, no filesystem writes.

---

## Proposed C modules

| File | Contents | est. LOC |
| --- | --- | --- |
| `apps/Koki/src/app.c` | `app_run()`, signal handling, engine lifecycle | 80 |
| `apps/Koki/src/koki_manifest.c/.h` | parse `assets/manifest.json` into target/costume/sound tables; intern strings | 380 |
| `apps/Koki/src/koki_cache.c/.h` | byte-budgeted LRU (§9), three instances, `/proc/meminfo` tiering | 240 |
| `apps/Koki/src/koki_sprite.c/.h` | sprite struct, costume lookup, motion and looks verbs | 320 |
| `apps/Koki/src/koki_render.c` | `costume_variant` (§6.1), layer list, `render()`, backdrop (§6.5) | 420 |
| `apps/Koki/src/koki_collide.c` | `screen_rect`, `paste_origin`, `alpha_mask`, `touching` (§7) | 260 |
| `apps/Koki/src/koki_sched.c/.h` | protothread macros, `Script`, ordered `active` map, `broadcast`, frame pass, `wait`/`glide` child threads | 520 |
| `apps/Koki/src/koki_input.c/.h` | evdev drain, matrix backends, keymap (§8) | 260 |
| `apps/Koki/src/koki_audio.c/.h` | mixer thread, ALSA device, voice list, WAV reader, minimp3 glue, external fallback | 700 |
| `apps/Koki/src/koki_engine.c/.h` | `Engine` struct, main loop, pause menu, perf, teardown (§11) | 380 |
| `apps/Koki/src/koki_game_common.c/.h` | globals table, sprite handles, shared sub-scripts (§13.2), registration driver | 420 |
| `apps/Koki/src/koki_game_boot.c` | §13.3–13.6, 13.9 (logo, intro, panel, White, Platform, lives/health, game over) | 900 |
| `apps/Koki/src/koki_game_player.c` | §13.7–13.8 (Player physics, CharacterAnim, damage gates) | 750 |
| `apps/Koki/src/koki_game_lobby.c` | §13.10 (doors) | 380 |
| `apps/Koki/src/koki_game_lv1.c` | §13.11 (Enemy 1, shockwaves, cannon, cannonball, QuickPress) | 800 |
| `apps/Koki/src/koki_game_lv2.c` | §13.12 (plane, Enemy2, Cannon2, gas tank) | 850 |
| `apps/Koki/src/koki_game_lv3.c` | §13.13 (Popi, projectiles, abyss) | 650 |
| `apps/Koki/src/koki_game_final.c` | §13.14 (Riby, evil cannon, Riby shockwaves) | 1050 |
| `apps/Koki/src/koki_game_ending.c` | §13.15 (Reward, score screen, grades) | 260 |
| `apps/Koki/test/koki_harness.c` | headless stub `ui`, scripted keys, PNG dump, virtual clock — replaces `tools/harness.py` and `tools/smoke.py` | 380 |
| `libneodct/nd_png.c/.h` | PNG decode (shared) | 450 |
| `libneodct/nd_mt19937.c/.h` | CPython-compatible MT19937 + `randint` (shared) | 150 |
| **total** | | **≈ 11 500** |

Of that, ~6 000 lines are the mechanical transcription of `game.py` and ~4 000 are the
runtime; ~600 (PNG + MT19937) belongs in `libneodct` and is shared with other apps.

RAM estimate for the C build, with the fx cache folded into the blend (§6.1):

| Item | Bytes |
| --- | --- |
| canvas (owned by core) | 126 000 |
| backdrop RGB | 126 000 |
| costume RGBA cache | budgeted 1 024–3 072 KB (recommend **768 KB** on device; measured working set peaks at 1 306 KB with the *Python* eviction policy, but the ending scene alone needs only ~600 KB) |
| collision mask cache | ≤ 256 KB, measured peak 54 KB (recommend 64 KB) |
| manifest structs (47 targets, 235 costumes, 285 sound refs, interned strings) | ~60 KB |
| script contexts (64 × 128 B) + engine bookkeeping | ~20 KB |
| audio: 1 music streaming buffer + 3 sfx voices + ALSA period | ~200 KB |
| `libneodct` + libc + zlib text (shared, already mapped) | 0 private |
| **private working set** | **≈ 1.4–1.9 MB** |

Against a Python+Pillow app whose caches alone are budgeted at 4.3 MB on top of the
15–17 MB interpreter, that is the whole point of the exercise.

---

## Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| **Generators → C.** 304 scripts depend on pausing mid-function; a protothread macro silently loses any stack local that crosses a `KOKI_YIELD`, and the failure looks like a subtle gameplay bug, not a crash. | **high** | Mandate the context-struct discipline in §4.4; write the macros first and land them with a unit test; add a `clang-tidy`/review rule "no non-`static` local declared before a `KOKI_YIELD` in a script function". Port one whole level first (§split, wave 2) and diff frames before writing the rest. |
| **Script execution order is dynamic** (insertion-ordered map, dead scripts removed and re-appended). Getting it wrong changes which sprite moves first within a frame, which changes collisions, which changes the game. | **high** | Reproduce the ordered map exactly (§4.2/§4.3). Add a harness mode that logs `(frame, key, sprite, fn)` for the first N frames from both builds and diff the logs before diffing pixels. |
| **The one-frame broadcast latency.** It is easy to "helpfully" run a newly created script in the same frame; every cutscene timing then shifts by a frame. | **high** | Snapshot the active list before stepping, exactly as §4.3. Covered by the trace diff above. |
| **Rounding.** Three different roundings coexist: `round()` half-to-even for paste positions and `size_q`, floor for `ghost_q`/`bri_q`, truncate-toward-zero for `int()`. One wrong choice shifts sprites by a pixel across the whole game. | **high** | §1, §6.1. Unit-test each helper against the table of measured values; the golden frames catch the rest. |
| **Pillow's alpha-blend rounding.** The obvious `MULDIV255` implementation differs from Pillow 12.3.0 for low alphas (§6.2) and would fail the pixel diff on every ghost/flash frame. | **high** | Use `(dst*(255-a) + src*a + 127)/255`. Unit-test against the counter-examples in §6.2. |
| **PNG decode differences.** A decoder that honours `bKGD` or applies gamma produces different pixels from Pillow's. | medium | Ignore all ancillary chunks. Golden-test the decoder by hashing all 235 decoded images against a Python-generated reference list. |
| **MP3 decoding.** 17 tracks up to 183 s; naive "decode to memory" costs 8 MB for the longest. | medium | Stream. Cap the decode buffer at one ALSA period. Vendor `minimp3` with the fixed-point path. |
| **The external-player fallback carries `fork()` into a threaded process.** With the mixer thread running, a bare `fork()`+`exec` sequence that touches `malloc` in the child deadlocks. | medium | Either drop the fallback (the miniaudio path is the shipped one and the buildroot config has no `aplay`/`mpg123`/`mpv` guarantee) or implement it with `posix_spawn` only. Recommend **dropping** it and keeping only "mixer or silence" — record in Open Questions. |
| **Matrix-keypad private state.** `Input` reaches into `matrix.scanner._held` and `matrix.matrix_to_code`, which live in the core process under the new architecture. | medium | Decide the cross-process input contract before porting `koki_input.c`. Preferred: the core writes synthesised evdev records to a pipe the app inherits, so the app only ever has the `keypad_fd` path. |
| **`Enemy4Stats` renders in front of `White`** because it is missing from the layer order. An agent will "fix" it. | medium | Called out in §6.4; add a golden frame of the final-boss screen. |
| **The lobby backdrop's 16 black rows** look like a bug. | low | Called out in §6.5; add a golden frame of the lobby. |
| **RNG fidelity.** Any scene driven by `randint` (Enemy2's death scatter, Riby's pattern choice, level-3 shockwave timing) diverges immediately if MT19937 is not CPython-exact. | medium | §12 with test vectors. Only 8 call sites, so an off-by-one in call *order* is as damaging as a wrong generator — trace the RNG call sequence in both builds. |
| **`time.monotonic` per script step.** Two scripts in the same frame can see different times on device, so glide interpolation is not frame-exact in real running. | low | Reproduce faithfully (read the clock per call). Golden frames use the virtual clock, where it does not matter. |
| **goldenframe's virtual tick is 0.1 s, not 1/30 s.** Koki runs three times faster in game-time under the oracle than on the device. | medium | The C golden shooter must advance its virtual clock by exactly 0.1 s per `fb.update`, or `app-koki` will not match. Documented in `goldenframe.py`; repeat it in the C shooter. |
| **Asset size on a 128 MB NAND.** 13.2 MB of audio and 1.4 MB of images ship in the read-only squashfs. | low | Unchanged by the port; noted so nobody "optimises" the assets and breaks the pixel oracle. |
| **fx cache thrashing.** *(measured)* the effect cache is at its 1 MB budget in every scene, so it evicts constantly and re-runs `resize`+`point` on hot sprites. | low | Folding effects into the blend (§6.1) removes the cache and the thrash at once, with identical output. |
| **`Sprite.sy`** is a dynamic Python attribute set only by the two Player scripts; forgetting it in the struct is a silent zero. | low | It is in the struct in §3. |

---

## Tests that cover this

**There are no unit tests for Koki.** `neodct/tests/` (36 files, 659 test functions) contains
nothing that imports `System.apps.Koki`; the only mention anywhere in the suite is the
string `app-koki` inside `neodct/tests/golden/manifest.json`. That is the single most
important fact in this section: **the game is currently verified by screenshots and by
hand, not by tests.**

What does exist, and how it can act as a port oracle:

1. **`neodct/tools/goldenframe.py` — the designated oracle.** Its own docstring says it is
   "the reference the C port is verified against". It runs `shoot_docs.py` with a virtual
   clock (epoch `1704112496.0`, tick `0.1 s`, advanced once per `fb.update`), `TZ=UTC` and
   `random.seed(20240101)`, then SHA-256s the raw RGB bytes of each frame
   (`b"%d,%d|" % (w,h)` prefix, then `tobytes()`) into `golden/manifest.json`.
   `shoot_docs.py` shoots Koki as
   `("Koki Mobile", [], "app-koki", -1, 400)` — launch the real app, no keys, stop it by
   exhausting a **400-frame budget** on the framebuffer, keep the **last** frame. The
   stored reference is
   `app-koki  sha256 86e9bd2c6587cd3c8622c5b9e9b10574841a1b3a3d47b2d5db7630b1d8360df1`,
   size `[240,175]`. There is also `menu-koki-mobile`
   (`62b1046e2f359d47c188cde2491a585242a8f0d79ddcc72a8459e79256e8433a`), which is the
   launcher tile, not the game.
   **Coverage: exactly one frame, from the boot sequence, with no input.** It proves the
   logo/intro timing, the intro's size ramp, `backdrop2`, the white fade and the paste
   arithmetic — and nothing about the levels.

2. **`tools/harness.py` (134 LOC) — the real oracle for gameplay.** It stubs `ui`
   (240×175 RGB canvas, `FakeFB` that saves selected frames, `keypad_fd = None`,
   `matrix_input = None`, default PIL fonts), sets `NEODCT_KOKI_NOSOUND=1`, seeds
   `eng.random` (default 1234), replaces `eng.input.poll` with a scripted
   frame → held-key map, sets `eng.headless_frames`, and wraps `eng.render` to count
   frames. `--press F:key`, `--hold A-B:key`, `--shot F`, `--shot-every N`, `--trace`
   (prints every broadcast with its frame number).

   This is deterministic end to end: virtual clock at exactly `FRAME_DT`, seeded RNG,
   scripted input. **It is the tool that makes the port provable**, and the C build needs
   an argument-compatible twin (`apps/Koki/test/koki_harness.c`) so the two can be run
   side by side and diffed:

   ```
   python3 tools/harness.py --frames 1200 --seed 1234 --shot-every 10 --out ref/
   ./build/koki-harness   --frames 1200 --seed 1234 --shot-every 10 --out cand/
   python3 neodct/tools/goldenframe.py --compare ref/ cand/
   ```

3. **`tools/smoke.py` (149 LOC) — per-scene entry points.** Six scenarios, each booting a
   fresh engine, killing the boot sequence at frame 1 (`stop_all_scripts()` +
   hide `Dynaris Logo`), seeding with 42, then injecting broadcasts at fixed frames:

   | scenario | frames | injected | held keys | shots |
   | --- | --- | --- | --- | --- |
   | `lv1` | 1000 | f2 `level1`; f500, f800 `enemy1damage` | z at 150-152, 180-182, 210-212 | 10,120,200,420,520,560,700,999 |
   | `lv2` | 1200 | f2 `startlv2`+`planecutscene`; f700, f1000 `enemy2 damage` | up 200-240, 400-430; down 300-340 | 30,80,160,260,420,620,750,1100 |
   | `lv3` | 1000 | f2 `level3`; f500 `enemy 3 damage` | z 200-205, 300-305, 600-605 | 30,150,250,420,520,700,999 |
   | `final` | 1400 | f2 `go to lobby`; f40 `doors = 4`; f60 `final cutscene` | — | 80,140,200,260,330,420,600,800,1000,1399 |
   | `gameover` | 400 | f2 `lives = 0`; f3 `game over` | enter 200-205 | 50,150,260,399 |
   | `ending` | 1400 | f2 `ending cutscene` | — | 50,300,620,700,760,900,1399 |

   Porting these six scenarios to the C harness gives **59 comparison frames covering every
   level, the final boss, game over and the ending** — the coverage the golden set is
   missing. Do this before writing any game logic.

4. **`neodct/tests/test_uistub.py`** covers the stub the golden capture rides on
   (the 240×175 band letterboxed into a 240×240 panel starting at row 32, frame copying,
   the `set_budget` choke point that stops Koki). It does not exercise Koki itself, but a
   C `nd-shoot` must reproduce the same letterboxing.

**What to add as part of the port** (there is no reason to inherit the coverage gap):

- Unit tests for the arithmetic helpers, against the measured tables in this document:
  paste blend (including the `a=1,dst=1,src=128 → 1` counter-example), nearest resize,
  round-half-to-even, `ghost_q`/`bri_q` floor quantisation, LRU eviction with the
  `count > 1` guard, MT19937 test vectors.
- A PNG-decode conformance test: hash all 235 decoded images and compare against a list
  generated once from Pillow.
- A scheduler trace test: run the boot sequence 400 frames in both builds and diff the
  `(frame, key, sprite, handler)` log.
- A collision test built from the real assets: for a grid of positions, compare
  `touching()` between Python and C for `PLAYER/PLAT`, `ANIM/EN1`, `ANIM/SW1`,
  `CBALL/ANIM (inset 0.3)`, `ABYSS/PLAYER`.

---

## How this could be split across agents

The dependency structure is a clean T: one runtime everybody needs, then seven
independent game files. The runtime must be finished and frozen before the game agents
start, or they will all write against a moving target.

**Wave 0 — shared, blocking (1 agent, or borrow from the `libneodct` team).**
`nd_png.c`, `nd_mt19937.c`, and the `libneodct` image primitives Koki needs
(`paste_alpha`, `crop_zeropad`, `resize_nearest`, `point_lut`, `flip_h`, `channel_a`).
Deliverable: the arithmetic unit tests in "Tests" pass. ~600 LOC. **Everything blocks on
this.**

**Wave 1 — the runtime (2 agents, ~1 week, they must talk).**

- *Agent A — scheduler + engine loop.* `koki_sched.c`, `koki_engine.c`, `koki_manifest.c`,
  `koki_cache.c`. Deliverable: the protothread macros with a unit test, the ordered
  `active` map with a trace test, the manifest loaded and the main loop running with an
  empty game.
- *Agent B — sprites, render, collision, input.* `koki_sprite.c`, `koki_render.c`,
  `koki_collide.c`, `koki_input.c`. Deliverable: a hand-written 20-line test script that
  shows a sprite, glides it and flips it, matching the Python harness pixel for pixel.

Agents A and B share `koki_engine.h`; land that header first, jointly, before either
writes an implementation.

**Wave 1b — audio (1 agent, fully parallel with wave 1).** `koki_audio.c`: ALSA device,
mixer thread, WAV reader, `minimp3`. Testable on its own against the 57 real asset files;
no dependency on the engine beyond the `sfx/music/stop` API. This is the one piece that
can start immediately.

**Wave 1c — the harness (1 agent, fully parallel).** `koki_harness.c` plus the six smoke
scenarios and the `goldenframe --compare` wiring. This agent should also generate the
Python-side reference frames so wave 2 has something to diff against on day one.

**Wave 2 — the game (5 agents, fully parallel once wave 1 is frozen).** Each takes one
self-contained slice of `game.py`, writes it against the frozen engine API, and is done
when its smoke scenario matches the Python frames:

| Agent | Files | Spec sections | Oracle | est. LOC |
| --- | --- | --- | --- | --- |
| C | `koki_game_boot.c` + `koki_game_common.c` | 13.1–13.6, 13.9 | `app-koki` golden frame, `gameover` scenario | 1 300 |
| D | `koki_game_player.c` + `koki_game_lobby.c` | 13.7, 13.8, 13.10 | needed by all others — **land first within wave 2** | 1 100 |
| E | `koki_game_lv1.c` | 13.11 | `lv1` scenario | 800 |
| F | `koki_game_lv2.c` | 13.12 | `lv2` scenario | 850 |
| G | `koki_game_lv3.c` + `koki_game_ending.c` | 13.13, 13.15 | `lv3`, `ending` scenarios | 900 |
| H | `koki_game_final.c` | 13.14 | `final` scenario | 1 050 |

Agent D's Player/CharacterAnim work is a hard dependency for E, F, G and H (every level
collides against `ANIM`), so it ships first and the other four start one or two days
behind. Agents C and D can share `koki_game_common.c` if C lands the globals table and the
sprite-handle block on day one.

**Wave 3 — integration (1 agent).** Registration order across the seven files (§13.16 is
the authority), the full-playthrough frame diff, memory measurement on the 53 MB target,
and the `NEODCT_KOKI_ATTACK_SLOW=1.0` A/B check.

**Rules for the parallel wave.** Every agent registers its handlers through one
`koki_game_register_all()` in `koki_game_common.c` that calls the seven per-file
registration functions **in the source order of `game.py`** — registration order is part
of the behaviour (§4.1), so it cannot be left to link order or to each agent's taste.
No agent may add, remove or reorder a handler; if the spec looks wrong, it goes in
OPEN-QUESTIONS, not into the code.

---

## Open questions for the project owner

### koki — should the external-player audio fallback be ported at all?
**Found in:** `neodct/overlay/NeoDCT/System/apps/Koki/engine.py:295-506` (`SoundManager`).
**What the Python does:** prefers the in-process miniaudio mixer, and falls back to
spawning `aplay`/`mpg123`/`mpv` when python-miniaudio is missing.
**Why it is unclear:** in C the mixer is always compiled in, so the fallback can never be
needed; and `KOKI_PORT_NOTES.md` records that `aplay`/`mpg123` are not even in the
buildroot config. Keeping it means carrying `posix_spawn` in a process that has an audio
thread, which is exactly the fork/exec hazard CODING-STANDARDS §1.1 warns about.
**Options:** (a) port it faithfully with `posix_spawn`; (b) drop it and keep only
"mixer or silent", preserving `NEODCT_KOKI_NOSOUND` and the log lines.
**Answer:** _(pending)_

### koki — how does the app process read the matrix keypad?
**Found in:** `engine.py:58-149` (`Input`), which reads `ui.matrix_input.scanner._held` and
`ui.matrix_input.matrix_to_code` directly.
**What the Python does:** Koki reaches into the core's keypad driver object to recover the
set of *held* keys, because `read_keypress()` only reports edges.
**Why it is unclear:** under the process-per-app architecture that object lives in the
core's address space.
**Options:** (a) the core synthesises evdev records (press *and* release) onto a pipe the
app inherits as `keypad_fd`, so the app only needs the evdev path; (b) a small shared-memory
page holding a held-key bitmask; (c) the app opens the I²C bus itself (rejected — two
scanners fighting over the chip).
**Answer:** _(pending)_ — (a) is recommended and would also let the gpiozero-backend
release bug below disappear.

### koki — the gpiozero matrix backend never releases a key
**Found in:** `engine.py:147` (`if self._matrix_code is not None and state is None`) against
`core/main.py:225` (`MatrixKeypadInput._held = set()`).
**What the Python does:** the release check tests `state is None`, but the gpiozero backend
stores a `set()`, which is never `None`. A key is therefore only released when a *different*
key is pressed. (The pcf8575 rollover backend takes the other branch and is fine.)
**Why it is unclear:** `KOKI_PORT_NOTES.md` describes the gpiozero backend as keeping "a
single value, None when released", which no longer matches the code — so this is either a
regression or a stale comment.
**Options:** (a) reproduce the current behaviour exactly; (b) fix it as part of the port.
**Answer:** _(pending)_

### koki — is `NEODCT_KOKI_ATTACK_SLOW = 1.35` the shipped default forever?
**Found in:** `game.py:20-27`.
**What the Python does:** slows fourteen attack timings by 35 % by default; `1.0` restores
the original Scratch speeds.
**Why it is unclear:** a 1:1 port of the *Scratch original* would be 1.0; a 1:1 port of
*NeoDCT today* is 1.35. The golden frames were captured at 1.35.
**Options:** (a) keep 1.35 as the C default (matches the current phone); (b) ship 1.0.
**Answer:** _(pending — the spec assumes (a))_

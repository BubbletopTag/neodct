# Where the time actually goes

Measured before writing any C, the same way the memory was measured: run the real
Python and look, rather than assume.

> **The C has since been built and measured. See [MEASURED: CubeBench in C](#measured-cubebench-in-c-2026-08-24)
> below.** The prediction in this first half was right about the mechanism and
> wrong about one number: the `0.2419 ms` it derives is the sum of *five* things
> in a frame that draws *four* strings, so it understates the Python's real frame
> by about 6×. Read the two halves together, not the first one alone.

---

## CubeBench, decomposed

The owner asked for CubeBench to be ported for real, as a visible demonstration of
the speed difference. So it is worth knowing in advance where its time goes,
because the obvious answer is wrong.

Per frame, on this x86-64 host:

| What | ms/frame | Share |
| --- | --- | --- |
| Python: 36 trig calls + 8 perspective projections | 0.0061 | **3%** |
| Pillow C: clear the content rectangle | 0.0189 | 8% |
| Pillow C: draw 12 wireframe lines | 0.0086 | 4% |
| **Pillow C: draw the "FPS 60.0" label** | **0.1821** | **75%** |
| Pillow C: `tobytes()` for the framebuffer blit | 0.0261 | 11% |

**The cube is not the expensive part.** The rotation maths — the thing that looks
like the workload — is 3% of the frame. Three quarters of it is drawing eight
characters of text.

That is not a Python problem. It is already C, inside Pillow. Pillow re-renders
the string through FreeType on every single call: load glyph, hint, rasterise,
composite, for each character, every frame. Nothing is cached between calls.

### What this means for the port

A naive C translation — same structure, FreeType called the same way — would be
**roughly as slow as the Python**, because it would inherit the same behaviour
that costs 75% of the frame. The 3% of Python-side maths is all that would
disappear.

The real win is a **glyph cache**: rasterise each character once per font size at
startup into an atlas, then drawing text becomes a memcpy-and-blend per character.
Text goes from ~0.18 ms to something near free.

This matters far beyond CubeBench. **117 of the roughly 260 draw calls in the whole
shipped codebase are `draw.text`.** Every menu, every list row, every header, every
softkey label pays this cost today, ten times a second, forever. The home screen
redraws unconditionally at 10 Hz whether anything changed or not.

So the glyph cache is not a CubeBench optimisation. It is the single largest
performance lever in the entire port, and it is invisible to the golden-frame
oracle — cached glyphs are bit-identical to freshly rendered ones, so caching
cannot break pixel equality. It can be added at any point without risking the 1:1
guarantee.

**Sizing it:** 95 printable ASCII characters × 4 sizes, 8-bit coverage. Using the
measured ink boxes in `fontref.json`, the whole atlas is on the order of tens of
kilobytes — trivial against a 53 MB budget, and it replaces per-frame FreeType work
entirely.

### The honest expectation for the demo

CubeBench in C will be meaningfully faster, but the headline number will come from
text caching and from removing the interpreter's per-frame overhead — not from the
cube maths being native. Do not promise a 50× cube. Measure it and report what it
actually is.

The more interesting number is not CubeBench's FPS at all: it is what the **home
screen** costs at idle, since that is what runs all day and what drains the
battery.

---

## MEASURED: CubeBench in C, 2026-08-24

Everything above this line was written before any C existed. Everything below is
the measurement it asked for, taken on the same x86-64 host, by
`neodct/src/test/unit/test_cubebench_perf.c` — which runs on every `make test`,
drives the **shipped `apps/CubeBench/app.so`** through `dlopen` + `app_run()` with
the real monotonic clock, and re-measures the Python side with
`neodct/tools/uistub.py` driving the real `System/engineering/apps/CubeBench/main.py`.

Every figure is the **minimum over repeated runs**, not the mean: several agents
build in this tree at once, so a mean measures whoever else was compiling. The
minimum can only over-estimate the cost, never under-estimate it.

### The headline

Both sides measured back to back in one window, so the same machine load is in
both columns.

| | ms/frame | fps |
| --- | --- | --- |
| Python, whole frame, real app through `uistub` | **1.6333** | **612** |
| C, whole frame, shipped `app.so` | **0.1025** | **9,752** |

**15.9× faster, and it draws the identical frame** — `eng-cubebench` is
byte-exact against the Python's own capture: **0 differing pixels of 42,000**,
SHA-256 `4dc887d2…c04c6b9` on both sides.

Across a dozen runs the C frame measured 0.0994–0.1054 ms and the Python 1.63–1.93
ms, so the ratio is 15–19× depending on how busy the machine is. Take 16×.

### First, a correction to the number above

**The `0.2419 ms` at the top of this file is not the Python's frame time.** It is
the sum of the five things that section decomposed, and CubeBench's frame draws
**four** strings, not one:

| main.py draws | chars |
| --- | --- |
| `"3D Cube"` | 7 |
| `"FPS %.1f"` | 8 |
| `"BACK/OK to exit"` | 15 |
| `SoftKeyBar.update("Exit")` | 4, plus the bar |

plus two `get_text_size()` calls, which in Pillow are `font.getbbox()` and
rasterise the string all over again in order to measure it. Left out, those are
about 6/7 of the real frame. Measured properly — every call `run()` makes, both
languages, same window:

| | Python (ms) | C (ms) | C is |
| --- | --- | --- | --- |
| trig + project (36 trig calls, 8 projections) | 0.0074 | *inside the frame loop* | |
| clear the content rectangle | 0.0145 | **0.0718** | **5.0× SLOWER** |
| 12 wireframe lines | 0.0133 | 0.0020 | 6.7× |
| text `"3D Cube"` | 0.1965 | 0.0019 | 103× |
| text `"FPS 60.0"` | 0.2160 | 0.0020 | 108× |
| text `"BACK/OK to exit"` | 0.4690 | 0.0040 | 117× |
| 2 × `get_text_size` | 0.2340 | 0.0002 | ~1000× |
| `SoftKeyBar.update("Exit")` | 0.2177 | 0.0163 | 13× |
| `read_keypress(0)` | 0.0002 | *inside the frame loop* | |
| framebuffer commit | 0.0347 | 0.0037 | 9× |
| **sum of the parts** | **1.4033** | **0.1019** | **13.8×** |
| **text and text metrics** | **1.1155 (79.5%)** | **0.0081 (8.0%)** | **138×** |

The C parts sum to 0.1019 ms against a whole frame of 0.1025 ms — 99.4% accounted
for. The Python's parts sum to 1.4033 ms against a whole frame of 1.6333 ms; the
missing 0.23 ms is interpreter overhead — attribute lookups, tuple building,
`"FPS %.1f" %`, the `for` machinery — spread across the loop, which is precisely
the part that has no C counterpart at all.

**The prediction in the first half of this file was right about the mechanism.**
Text was 75% of the frame as decomposed then, is 79.5% of it as decomposed
properly now, and is 8.0% in C.

### What the glyph cache bought

`lib/nd_font.c` rasterises all 95 printable ASCII characters into one arena at
`nd_font_load()`, so `nd_draw_text()` is a blend per character and never touches
FreeType. Pillow does the opposite: load, hint, rasterise and composite each
character on every call, caching nothing between calls.

The cache has no runtime off switch, and adding one to the most pixel-critical
module in the project purely to benchmark it would be a bad trade. So the uncached
side is measured by doing what a per-call renderer does — FreeType directly, with
`nd_font.c`'s own three settings (`FT_Set_Pixel_Sizes` in pixels,
`FT_LOAD_DEFAULT` so the font's own hinting runs, `FT_RENDER_MODE_NORMAL` for
8-bit coverage) — and adding the compositing, which both paths pay:

| `"FPS 60.0"`, eight characters | ms |
| --- | --- |
| cached (`nd_draw_text`) | 0.0020 |
| rasterise the 8 glyphs through FreeType | 0.0222 |
| …plus the same blend = uncached | 0.0242 |
| **the cache is worth** | **11.9× on this call** (11.2–12.7× across runs) |

Per glyph that is **0.0028 ms** of FreeType work avoided. CubeBench draws 30
glyphs a frame outside the softkey bar, so a naive C port — same structure,
FreeType called the way Pillow calls it — would spend **0.083 ms** a frame
re-rasterising characters it had already rasterised on the previous frame:

| whole CubeBench frame in C | ms | fps |
| --- | --- | --- |
| with the glyph cache | 0.1019 | 9,814 |
| without it (30 glyphs re-rasterised) | 0.1850 | 5,405 |
| **the cache is worth** | **1.82× on the frame** | |

So: **11.9× on a text call, 1.82× on this frame, 138× against Pillow's text.**

The gap between "11.9× on the call" and "1.82× on the frame" is the useful part of
the result. Once text is nearly free, the frame is whatever is left — and here
what is left is one fill loop, below. Against Pillow the text figure is 138×
because Pillow is paying *both* the per-call FreeType cost and the interpreter's
per-call overhead; against a naive C port it is 11.9×, which is the cache on its
own.

The cache being *free* rather than a trade is asserted, not assumed:
`test_cubebench_perf.c` compares all 95 printable ASCII glyphs at all four sizes
against a freshly rasterised FreeType bitmap and requires every byte to match. If
they ever differed, every golden frame would be a coin toss.

### The next lever is `nd_draw_rect_fill`, and it is slower than Pillow

With text cached, **clearing the 240×146 content rectangle is 70% of the C frame**
— 0.0718 ms of 0.1019 ms — and it is **5.0× slower than Pillow doing the same
fill** (0.0145 ms). This is the one thing in CubeBench the C loses at, and the
reason is visible in `lib/nd_draw.c`'s `hline()`:

```c
if (k->bpp == 1) {
    memset(p, k->bytes[0], (size_t)(x1 - x0 + 1));
    return;
}
for (x = x0; x <= x1; x++, p += k->bpp)
    memcpy(p, k->bytes, k->bpp);      /* 3 bytes, 35,040 times */
```

Pillow stores an RGB image as 4 bytes per pixel internally and fills a run with
32-bit stores; `hline()` issues a 3-byte `memcpy` per pixel. 35,040 pixels at
about 2 ns each is the 0.0718 ms.

**Not fixed here.** `lib/nd_draw.c` belongs to another work package, a
row-at-a-time fill is a change to the most pixel-critical function in the project,
and the frame is byte-exact as it stands. Raised because it is the next lever, and
because "the C is slower than Pillow at this one thing" is exactly the sort of
fact that should not stay in a scratch buffer.

There is a narrow version of the fix that cannot change a pixel: a colour whose
`bpp` bytes are all equal — black and white, which is all CubeBench draws — can
take the `memset` path the 8-bit case already takes. `OPEN-QUESTIONS.md` SA-9
asks the owner whether that belongs inside the port or after it.

`OPEN-QUESTIONS.md` CB-8 recorded this first, with 0.0305 ms for the same fill on
a quiet machine. This tree now has several agents compiling in it and the figure
here is 0.0718 ms. **The 5× ratio against Pillow is the robust number**, because
both sides of it were measured back to back under the same load.

### What this does and does not say

- It says the port is **15.9× faster on this app** (16× taking the run-to-run
  spread into account), on this host, at 240×175.
- It does **not** say the cube maths got faster. Trig and projection were 3% of
  the Python's five-part decomposition and are 0.5% of the full one; they were
  never the workload. As predicted: do not promise a 50× cube.
- It is an x86-64 number. The device is a 32-bit Cortex-A7 where the interpreter
  overhead the C removes is relatively larger and the memory bandwidth the fill
  needs is relatively smaller, so the ratio there will differ. Nothing here has
  been run on hardware.
- CubeBench spins a core flat out on purpose. The number that matters for battery
  is still the home screen at idle, which the section above is right about and
  which nothing in this session measured.

---

## The 10 Hz unconditional redraw

`core/main.py:run()` calls `ui.update()` every loop iteration, then waits up to
0.1 s for a key. So the UI repaints ten times a second whether or not anything
changed — including when the phone is sitting on the home screen in a pocket.

`neodctDisplay.c` already defends the *panel* against this: it diffs each frame
against the previous one and sends nothing over SPI when they match, which is why
idle costs almost no SPI traffic. But the CPU still composites the entire frame,
including all that uncached text, before the diff discovers it was identical.

Two independent improvements, both behaviour-preserving:

1. **Cache glyphs** (above), which makes the wasted redraw much cheaper.
2. **Only redraw when something changed** — a dirty flag set by input, a service
   event, or the clock's minute rolling over. This is a bigger behavioural change
   and needs care around the blinking envelope and cursor, which genuinely do
   animate. Worth doing, but after the port is proven 1:1, not during it.

Item 2 is out of scope for the port. Recorded here so it is not forgotten, because
on a battery-powered device an idle loop that repaints ten times a second is the
kind of thing that quietly costs hours of standby time.

---

## MEASURED: boot and app launch, 2026-08-24

The owner, running both builds side by side in QEMU: *"the dialer comes up
instantly after clicking call, but the other apps still have a slight delay"*
and *"they launch slower ON C"* — and an eleven-second cold boot against a
wanted five.

Both turned out to be real, both turned out to have causes nothing about the
C-versus-Python framing would have predicted, and **neither was findable on a
desktop.** That last point is the most important thing in this section.

### Why the dialer felt different

It is not an app. The dialer is `lib/nd_dialer.c`, drawn by the core in the
core's own process. Nothing is spawned, nothing is linked, nothing is
initialised — the screen just changes.

An app is a separate process, because a crashing app must not take the phone
with it (`nd_apprun.c`). Python never paid for that: `main.py:910` runs an app
with `importlib.util.spec_from_file_location` and `exec_module`, straight into
the launcher's own interpreter. The app inherits the already-loaded fonts, the
wallpaper, the app registry and the open databases as a matter of course.

So the comparison the owner was making — dialer against app — was really
"no process" against "a whole process", and the C was being asked to pay for
isolation the Python never bought.

That is the honest floor. Everything above it was waste.

### The measurements

On the phone (aarch64, musl, squashfs, QEMU), warm:

| | before | after |
| --- | --- | --- |
| `/bin/true` | 10 ms | 10 ms |
| `nd-apprun`, exec + link only | 20 ms | 20 ms |
| **a real app launch** | **200 ms** | **50 ms** |

The 20 ms floor is `execve` plus the dynamic loader resolving eleven shared
objects. It is the price of the process boundary and it is not going away
without a pre-linked zygote.

### What the other 180 ms was

`NEODCT_BENCH=1`, on the target:

| mark | ms | who actually reads it |
| --- | --- | --- |
| `ui_init_app: unread sms` | **60–70** | the flashing envelope on the **home screen** |
| wallpaper PNG decode | ~104 | the **home screen** background |
| engineering-mode setting | ~28 | only the app scan |
| `rescan_apps`, every manifest.json | ~18 | the **app selector** |
| `ui_home.json` | ~4 | the **home screen** |
| four `FT_New_Face` calls | 12 | genuinely needed |
| surfaces (canvas + scratch column) | 5 | genuinely needed |
| `dlopen(app.so)` | 3 | genuinely needed |

Every row above the line is the home screen's state, rebuilt from scratch by
every app process, for a screen that app will never draw. Not one file in
`apps/` reads any of it — `grep` says zero, and `settings_app.h` had already
written it down.

They are lazy now: loaded on first read rather than at construction. The core's
first frame pays exactly what its constructor used to; an app pays nothing. A
future app that genuinely wants the wallpaper still gets it, which a
"skip this in apps" flag could not have given it.

### The part worth remembering

**The host said the wrong thing, twice.**

On this x86-64 desktop, `nd_db_count_unread_sms()` costs **0.005 ms**. On the
phone it costs **60–70 ms** — over half of the whole launch — because counting
unread SMS opens SQLite, and the staged host root has no database file to open
while the phone has three. A desktop profile does not merely understate that
cost; it reports it as free.

The wallpaper was the same story in reverse: 1.7 ms on the host, ~104 ms on the
phone, because the host reads a decompressed PNG out of page cache and the
phone reads it through squashfs on an emulated CPU. Same code, same input, a
sixty-fold difference in what it is worth fixing.

This is why `nd_bench.h` exists and why the marks are committed rather than
patched in and reverted. Twice this was chased by hand-editing `fprintf()`s
into `nd_ui.c`; twice the host answer disagreed with the phone's. With
`NEODCT_BENCH` unset — every boot anyone will ever see — a mark is one cached
`getenv` and a branch, and the clock is not even read.

```
NEODCT_BENCH=1 /NeoDCT/System/bin/nd-apprun /NeoDCT/System/apps/Clock run
```

### The "cold launch" was not a cold cache — a correction

The paragraph that stood here said the first app opened after boot took
120–170 ms because `app.so`, the font and the databases came off squashfs
uncompressed for the first time. **That was wrong**, and the evidence was
already in the numbers above it: the *second* launch measured worse than the
first — 45.9 ms in `4 faces` against 24.6 ms. A cold cache does not get
colder.

What it actually was: the rcS split moved the rest of the boot off the
critical path, which is why the home screen appears in a third of the time —
but the work did not stop. `S11udevall` alone is 2.7 s, and on a single core
it was then competing with the UI the user was already looking at, and with
any app they opened in that window.

Two experiments settled it. Waiting 25 s for the background work to finish
and *then* launching an app that had never been opened: **40 ms**, no penalty
at all. And running the background group under `nice -n 10`, which is now
what `rcS` does:

| launch | before | after |
| --- | --- | --- |
| 1st ever, during boot | 120 ms | **50 ms** |
| 2nd, never opened before | 140 ms | **50 ms** |
| 3rd, 4th | 50 ms | 50 ms |

Boot itself is unchanged at 6.0 s, within noise of the 5.9 s before it.
`nice 10` and not 19 on purpose: this is the rest of the boot, with the
network and the modem behind it, so it should yield to the UI rather than be
starved by it.

A page-cache pre-warm at boot was written first, on the strength of the wrong
diagnosis, and deleted: it did not help, and being another 1 MB of squashfs
decompression in exactly the contended window, it made the real problem
slightly worse. Cache the CPU, not the pages.

**One launch does warm the next, though — and it is shared.** Opening one app
makes every *other* app fast, including ones never opened, because what warms
is `libneodct.so`, freetype, sqlite, the font and the databases. Each
`app.so` is a few tens of KB and does not move the number.

### The boot: 17.4 s to 5.9 s

Measured in the same QEMU, on a container that ran the owner's 11-second boot
in 17.4 s — so read the ratio, not the absolute.

| | before | after |
| --- | --- | --- |
| initramfs done (`[ndsys]`) | 6.69 s | 1.47 s |
| `Starting UI...` | 17.09 s | 5.26 s |
| **UI up** | **17.42 s** | **5.93 s** |

Four things, in order of what they were worth:

**1. `busybox init` ran every init script before starting the UI.**
`::sysinit:/etc/init.d/rcS` runs to completion before `tty1::once:` gets a
look in, so syslogd, klogd, the random seed, the udev coldplug, zram's
`mkswap`, DHCP, the modem and crond were all standing between the user and the
home screen. `rcS` is ours now and splits: the four scripts the UI genuinely
needs run in front, everything else in one background subshell in its existing
order. `S11udevall` still takes 2.46 s — it just takes it at 8.7 s, on a phone
that has been usable since 5.9 s.

**2. A flat two-second sleep in the initramfs.** `panel_start()` started the
display daemon and then `sleep 2`, because the daemon resets the panel and
forces fb0 to 32bpp and anything blitted before that lands in the wrong
format. The daemon now writes `NEODCT_DISPLAYD_READY` when it has actually
finished, and the initramfs polls for that in 20 ms steps — giving up at once
if the daemon exits, which is what happens on QEMU where there is no SPI bus
to find. `PANEL_SETTLE` remains the ceiling, so the worst case is the
behaviour it replaced.

**3. udev's full coldplug, 2.8 s, blocking.** Replaying an add event for every
device in `/sys` and waiting for the last one. Almost none of it matters to the
UI — but one part does: `/dev/input/by-path/*-kbd` is how
`nd_evdev_discover()` picks the keypad, and devtmpfs alone gives
`/dev/input/event0` with no promise that event0 is the keyboard. Under QEMU
there is a virtio *tablet* on the same bus, so dropping udev entirely would
have booted a phone with no working keys. So `S10udevd` settles the input
subsystem only — a handful of devices, and on a Luckfox with a GPIO matrix
keypad, possibly none at all — and `S11udevall` does the rest in the
background.

**4. The one-second splash sleep**, already removed at the owner's request.

None of this was findable before `run_qemu.sh` learned `NEODCT_APPEND`:
`CONFIG_PRINTK_TIME` is off in both kernel configs, so a boot log carried no
timestamps and "which part is slow" had no answer that was not a guess.

```
NEODCT_APPEND="printk.time=1" NEODCT_DEBUG=1 neodct/tools/run_qemu.sh
```

### What is left

The 20 ms spawn floor and 12 ms of `FT_New_Face` — four faces, one font file,
opened four times. Neither is the difference between "instant" and "a slight
delay" any more; both are written down here so the next person does not have
to re-derive them.

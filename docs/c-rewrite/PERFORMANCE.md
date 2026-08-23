# Where the time actually goes

Measured before writing any C, the same way the memory was measured: run the real
Python and look, rather than assume.

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

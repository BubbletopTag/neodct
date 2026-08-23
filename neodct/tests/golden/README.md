# Golden frames — the reference the C port is verified against

Captured from the **Python build** with `neodct/tools/goldenframe.py`, before any C
was written. These 49 images are what NeoDCT looks like today, and the C rewrite is
correct exactly when it reproduces them pixel for pixel.

Regenerate (only when the Python UI intentionally changes):

    python3 neodct/tools/goldenframe.py --out neodct/tests/golden/

Check a C build against them:

    ./build/nd-shoot --out /tmp/frames
    python3 neodct/tools/goldenframe.py --compare neodct/tests/golden /tmp/frames

`manifest.json` holds each frame's dimensions and the SHA-256 of its **raw RGB
bytes** — not of the PNG file, because two encoders can write different bytes for
identical images and it is the pixels that matter.

Every clock and randomness source that can reach a pixel is pinned, so a capture is
reproducible on any machine: virtual time starting at a fixed epoch and advancing one
0.1 s tick per frame, TZ pinned to UTC, `random` seeded to a constant. Changing
`EPOCH`, `TICK` or `SEED` in `goldenframe.py` invalidates every image here.

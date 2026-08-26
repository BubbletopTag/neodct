# Golden frames — a regression net, no longer a conformance gate

Captured from the **Python build** with `neodct/tools/goldenframe.py`, before any C
was written. While the port was in progress these images were the definition of
correct: the C build was right exactly when it reproduced them pixel for pixel.

**That is over.** The port is done and applications are now being deliberately
redesigned — the Music library, the Messages Chat style — so a frame that no longer
matches is usually the *point*, not a failure. See CODING-STANDARDS.md section 7.

What they are for now: when you change one screen, these are a cheap check that you
did not disturb the other forty-seven. A frame you meant to change gets re-cut and
said so in the commit message. A screen that is genuinely new does **not** get a new
frame — its test is a unit test, not a picture of itself that can only ever agree
with it.

Regenerate (only when the UI intentionally changes):

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

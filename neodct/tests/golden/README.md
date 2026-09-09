# Golden frames — a historical baseline the test suite still hashes

Captured from the **Python build** with `neodct/tools/goldenframe.py` at 0.4.0a,
before any C was written. While the port was in progress these images were the
definition of correct: the C build was right exactly when it reproduced them pixel
for pixel.

**That is over.** These frames are from 0.4.0a; the tree is on 0.5.15a — twenty-six
releases, which at this project's alpha cadence is a matter of weeks. Do not read that
short calendar gap as freshness: drift here is counted in releases, not days.

The port is done and screens are deliberately redesigned now — the Music library, the
Messages Chat style — and they will be redesigned again. This set drifts from the truth
**by design**. It is not a description of what the UI should look like, and:

- a frame here is never a reason to leave a screen the way it is;
- it is never something to ask permission about before changing a screen;
- a mismatch against it is not a finding, not a bug, and not something to put in a
  summary.

**The check worth doing compares the tree against itself**, not against this
directory — snapshot before the work, snapshot after, diff the two. See
CODING-STANDARDS.md section 7.

This set is kept for one reason: a dozen tests (`test_appsel`, `test_widgets_*`,
`test_messages`, `test_dialer`, …) hash a rendered screen against `manifest.json` and
fail on a mismatch. When a screen change turns `make test` red, re-cut it as a chore
and give it one line in the commit — no deliberation, no paragraph:

    ./build/default/bin/nd-shoot --out /tmp/frames
    python3 neodct/tools/goldenframe.py --compare neodct/tests/golden /tmp/frames

then copy the changed PNGs here and update their `sha256` in `manifest.json`.

A screen that is genuinely new does **not** get a new frame — its test is a unit
test, not a picture of itself that can only ever agree with it. Refreshing the whole
set from the Python build is a deliberate, separate task:

    python3 neodct/tools/goldenframe.py --out neodct/tests/golden/

`manifest.json` holds each frame's dimensions and the SHA-256 of its **raw RGB
bytes** — not of the PNG file, because two encoders can write different bytes for
identical images and it is the pixels that matter.

Every clock and randomness source that can reach a pixel is pinned, so a capture is
reproducible on any machine: virtual time starting at a fixed epoch and advancing one
0.1 s tick per frame, TZ pinned to UTC, `random` seeded to a constant. Changing
`EPOCH`, `TICK` or `SEED` in `goldenframe.py` invalidates every image here.

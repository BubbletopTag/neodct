"""Deterministic frame capture -- the reference the C port is verified against.

`shoot_docs.py` renders genuine UI output, but not *reproducible* output: the
status bar reads the wall clock, the unread-mail envelope blinks off
`time.time()`, the text cursor blinks, and both games seed `random` from the
clock. Run it twice and you get two different sets of pixels. That is fine for
documentation and useless as a test oracle.

This module removes every one of those, so the same script always produces the
same bytes. Then "the C port is one-to-one" becomes a checksum comparison
instead of a judgement call:

    python3 neodct/tools/goldenframe.py --out golden/     # from the Python build
    ./build/nd-shoot --out frames/                        # from the C build
    python3 neodct/tools/goldenframe.py --compare golden/ frames/

How time is handled
-------------------
Freezing the clock outright would deadlock anything that waits for time to
pass -- the +CLIP grace period in `poll_modem`, the cursor blink, the modem
retry backoff. So the clock is *virtual* rather than frozen: it starts at a
fixed epoch and advances one UI tick (0.1 s, the real main-loop period) each
time a frame is drawn. Within a single frame time does not move, so a screen
composed of several draw calls cannot tear across a tick boundary.

That gives three properties at once, and we need all three:

  * deterministic  -- frame N is always at EPOCH + N*0.1, on every machine
  * monotonic      -- nothing that polls for elapsed time can hang
  * frame-aligned  -- one frame renders at exactly one instant

The epoch is 2024-01-01 12:34:56 UTC. TZ is pinned to UTC because
`time.strftime("%H:%M")` reads the local zone, and a reference rendered in
Dublin must match one rendered in a CI container.
"""

import argparse
import hashlib
import json
import os
import random
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
NEODCT = os.path.dirname(HERE)
for _p in (HERE, os.path.join(NEODCT, "overlay", "NeoDCT")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

# 2024-01-01 12:34:56 UTC. Arbitrary, but fixed forever: changing it
# invalidates every stored reference frame.
EPOCH = 1704112496.0

# One UI tick. Matches the 0.1 s timeout in core.main.run()'s read_keypress,
# so virtual time advances at the same rate the real main loop does.
TICK = 0.1

# Seed for `random`. The games call random.seed(time.time()); with virtual
# time that is already deterministic, but seeding here as well means a frame
# is reproducible even if a caller reorders the shot list.
SEED = 20240101


class VirtualClock:
    """A clock that only moves when a frame is drawn."""

    def __init__(self, epoch=EPOCH, tick=TICK):
        self.epoch = epoch
        self.tick = tick
        self.frame = 0

    def now(self):
        return self.epoch + self.frame * self.tick

    def advance(self):
        self.frame += 1


class _Frozen:
    """Patches every clock and randomness source that can reach a pixel.

    Restored on exit, so importing this module cannot affect an unrelated
    test in the same interpreter.
    """

    #: Every clock in `time` that can influence a pixel. perf_counter is on
    #: this list because CubeBench integrates its rotation angle over
    #: perf_counter deltas -- miss it and the cube lands at a different angle
    #: on a faster machine.
    PATCHED = ("time", "monotonic", "monotonic_ns", "time_ns",
               "perf_counter", "perf_counter_ns",
               "localtime", "gmtime", "strftime")

    def __init__(self, clock):
        self.clock = clock
        self._saved = {}
        self._saved_tz = None

    def __enter__(self):
        for name in self.PATCHED:
            self._saved[name] = getattr(time, name)

        # strftime() with no time tuple formats *now*, which is how the
        # status-bar clock is drawn (core/main.py:768). Route that through
        # the virtual clock; an explicit tuple is passed through untouched.
        real_strftime = self._saved["strftime"]
        real_gmtime = self._saved["gmtime"]
        clock = self.clock

        time.time = lambda: clock.now()
        time.monotonic = lambda: clock.now()
        time.perf_counter = lambda: clock.now()
        time.time_ns = lambda: int(clock.now() * 1e9)
        time.monotonic_ns = lambda: int(clock.now() * 1e9)
        time.perf_counter_ns = lambda: int(clock.now() * 1e9)
        time.gmtime = lambda secs=None: real_gmtime(clock.now() if secs is None else secs)
        time.localtime = lambda secs=None: real_gmtime(clock.now() if secs is None else secs)
        time.strftime = lambda fmt, t=None: real_strftime(
            fmt, real_gmtime(clock.now()) if t is None else t)

        # strftime honours TZ even when handed a gmtime tuple for %Z/%z, and
        # a reference rendered in one zone has to match one rendered in
        # another. Pin it.
        self._saved_tz = os.environ.get("TZ")
        os.environ["TZ"] = "UTC"
        if hasattr(time, "tzset"):
            time.tzset()

        random.seed(SEED)
        return self

    def __exit__(self, exc_type, exc, tb):
        for name, fn in self._saved.items():
            setattr(time, name, fn)
        if self._saved_tz is None:
            os.environ.pop("TZ", None)
        else:
            os.environ["TZ"] = self._saved_tz
        if hasattr(time, "tzset"):
            time.tzset()
        return False


def instrument(fb, clock):
    """Make `fb` advance `clock` by one tick per drawn frame.

    Wraps rather than subclasses so this works on the CapturingFramebuffer
    that StubUI already built.
    """
    real_update = fb.update

    def update(pil_image):
        result = real_update(pil_image)
        clock.advance()
        return result

    fb.update = update
    return fb


class DeterministicUI:
    """StubUI with the clock and randomness pinned.

    Same interface as `uistub.StubUI`, so anything written against that --
    including shoot_docs.py's shot functions -- works unchanged.
    """

    #: The genuine uistub.StubUI, bound once at class-creation time.
    #: `capture()` replaces the module attribute with this class, so looking
    #: StubUI up by name at call time would resolve to us and recurse.
    _stub_cls = None

    def __init__(self, **kwargs):
        cls = type(self)
        if cls._stub_cls is None:
            import uistub
            cls._stub_cls = uistub.StubUI

        self.clock = VirtualClock()
        self._frozen = _Frozen(self.clock)
        self._stub = cls._stub_cls(**kwargs)
        self._ui = None

    def __enter__(self):
        self._frozen.__enter__()
        try:
            self._ui = self._stub.__enter__()
        except BaseException:
            self._frozen.__exit__(*sys.exc_info())
            raise
        instrument(self._ui.fb, self.clock)
        return self._ui

    def __exit__(self, exc_type, exc, tb):
        try:
            return self._stub.__exit__(exc_type, exc, tb)
        finally:
            self._frozen.__exit__(exc_type, exc, tb)


# --- capture and comparison ------------------------------------------------

def frame_digest(image):
    """SHA-256 over raw RGB bytes.

    Raw pixels, not the PNG file: two encoders can write different bytes for
    identical images, and it is the pixels the port has to match.
    """
    rgb = image if image.mode == "RGB" else image.convert("RGB")
    h = hashlib.sha256()
    h.update(b"%d,%d|" % (rgb.width, rgb.height))
    h.update(rgb.tobytes())
    return h.hexdigest()


def write_manifest(out_dir, entries):
    """Record each frame's size and digest next to the images."""
    path = os.path.join(out_dir, "manifest.json")
    with open(path, "w") as fh:
        json.dump({
            "epoch": EPOCH, "tick": TICK, "seed": SEED,
            "frames": entries,
        }, fh, indent=2, sort_keys=True)
    return path


def load_manifest(d):
    with open(os.path.join(d, "manifest.json")) as fh:
        return json.load(fh)


def compare(reference_dir, candidate_dir, verbose=True):
    """Compare two capture directories. Returns (ok, differences)."""
    ref = load_manifest(reference_dir)
    cand = load_manifest(candidate_dir)

    ref_frames = {f["name"]: f for f in ref["frames"]}
    cand_frames = {f["name"]: f for f in cand["frames"]}

    diffs = []
    for name in sorted(set(ref_frames) | set(cand_frames)):
        r = ref_frames.get(name)
        c = cand_frames.get(name)
        if r is None:
            diffs.append((name, "extra", "not in reference"))
        elif c is None:
            diffs.append((name, "missing", "not rendered by candidate"))
        elif r["size"] != c["size"]:
            diffs.append((name, "size", f"{r['size']} vs {c['size']}"))
        elif r["sha256"] != c["sha256"]:
            diffs.append((name, "pixels", _describe_pixel_diff(
                os.path.join(reference_dir, name + ".png"),
                os.path.join(candidate_dir, name + ".png"))))

    if verbose:
        total = len(set(ref_frames) | set(cand_frames))
        if not diffs:
            print(f"identical: {total} frames match")
        else:
            print(f"{len(diffs)} of {total} frames differ:")
            for name, kind, detail in diffs:
                print(f"  {name:<28} {kind:<8} {detail}")
    return (not diffs), diffs


def _describe_pixel_diff(ref_path, cand_path):
    """Count differing pixels and give their bounding box.

    Whether the difference is three pixels of text antialiasing or the whole
    screen decides whether it is a bug or a rounding difference, so say which.
    """
    try:
        from PIL import Image, ImageChops
    except ImportError:
        return "differs"
    try:
        with Image.open(ref_path) as a_f, Image.open(cand_path) as b_f:
            a = a_f.convert("RGB")
            b = b_f.convert("RGB")
            if a.size != b.size:
                return f"size {a.size} vs {b.size}"
            delta = ImageChops.difference(a, b)
            box = delta.getbbox()
            if box is None:
                return "differs"
            n = sum(1 for px in delta.convert("L").tobytes() if px)
            pct = 100.0 * n / (a.width * a.height)
            return f"{n} px ({pct:.2f}%) in box {box}"
    except OSError as exc:
        return f"could not compare: {exc}"


def capture(out_dir):
    """Render the full reference set deterministically."""
    import shoot_docs

    os.makedirs(out_dir, exist_ok=True)
    entries = []

    # shoot_docs builds its screens with uistub.StubUI; swapping in the
    # deterministic wrapper makes every existing shot function reproducible
    # without touching shoot_docs itself.
    import uistub
    real_stub = uistub.StubUI
    # Bind the real class before shadowing the name, or DeterministicUI
    # constructs itself forever.
    DeterministicUI._stub_cls = real_stub
    uistub.StubUI = DeterministicUI
    shoot_docs.StubUI = DeterministicUI

    real_save = shoot_docs.save

    def save(image, name, out):
        path = real_save(image, name, out)
        rgb = image if image.mode == "RGB" else image.convert("RGB")
        entries.append({
            "name": name,
            "size": [rgb.width, rgb.height],
            "sha256": frame_digest(rgb),
        })
        return path

    shoot_docs.save = save
    # shoot_docs.main() takes no argv and parses sys.argv itself.
    real_argv = sys.argv
    sys.argv = ["shoot_docs", "--out", out_dir]
    try:
        shoot_docs.main()
    finally:
        sys.argv = real_argv
        shoot_docs.save = real_save
        shoot_docs.StubUI = real_stub
        uistub.StubUI = real_stub

    entries.sort(key=lambda e: e["name"])
    write_manifest(out_dir, entries)
    return entries


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", help="capture the reference set into this directory")
    ap.add_argument("--compare", nargs=2, metavar=("REFERENCE", "CANDIDATE"),
                    help="compare two capture directories")
    ap.add_argument("--verify-determinism", metavar="DIR",
                    help="capture twice into DIR and confirm the runs are identical")
    args = ap.parse_args(argv)

    if args.compare:
        ok, _ = compare(args.compare[0], args.compare[1])
        return 0 if ok else 1

    if args.verify_determinism:
        base = args.verify_determinism
        a, b = os.path.join(base, "run-a"), os.path.join(base, "run-b")
        print("capture 1/2...")
        capture(a)
        print("capture 2/2...")
        capture(b)
        print()
        ok, _ = compare(a, b)
        print("\ndeterminism:", "PASS" if ok else "FAIL")
        return 0 if ok else 1

    if args.out:
        entries = capture(args.out)
        print(f"captured {len(entries)} frames -> {args.out}")
        return 0

    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())

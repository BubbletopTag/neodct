#!/usr/bin/env python3
"""parity_diff.py -- hold two nd-inventory captures against each other.

The product of this whole harness is a PULL REQUEST REVIEW, not a green tick.
So the comparator's job is not "are these two files the same" -- they must not
be, and a test that can never pass is a test that gets disabled -- but "is
every way in which they differ one somebody has written down a reason for".

============ WHAT IT REFUSES, AND WHY REFUSING IS THE POINT ============

A refusal is not a measurement. But a capture recorded as a measurement when
it is not one becomes a BASELINE, and then every future comparison is against
a fiction. So four kinds of capture are refused outright as a baseline or as
either side of a gating comparison:

  capture.root is not "/"        --root exists so the canonicaliser can be
                                 unit-tested against a synthetic tree with no
                                 image. It is also a way to manufacture a
                                 plausible-looking capture, and the guard is
                                 the comparator's rather than the format's --
                                 which is a real weakness, written down here
                                 rather than hidden.
  capture.sections is not "all"  a sectioned capture is a shorter file, and a
                                 shorter file compared against a whole one
                                 produces a diff about the operator's
                                 arguments.
  capture.method is not          the shell fallback and the cross-compiled
  "nd-inventory"                 no-libneodct probe build are both weaker
                                 instruments. A WEAKER INSTRUMENT MUST NEVER
                                 BE ABLE TO BECOME THE REFERENCE.
  the format versions differ     a capture whose format is not the tool's is
                                 not old data, it is data whose masks and
                                 family table are unknown.

And one precondition on the PAIR: capture.os_version_id must match on both
sides. Comparing a 0.5.8b phone against a 0.6.0a emulator is two unrelated
observations rather than a parity test. --force-version-skew exists, prints a
banner, and is for the case where there genuinely is no matching build.

============ THE ALLOWLIST IS AN ANNOTATION OF THE DIFF, NOT A COPY OF IT ===

One record per divergent key, five fields plus one this file added:

  key           the record. One segment may be [*].
  qemu          the value on the emulator side, verbatim, or ABSENT
  hw            the value on the phone side, verbatim, ABSENT, or ~<regex>
                where the value is genuinely a free variable (a board name)
  verdict       until-stage-N | until-image | permanent | must-differ
  hw_evidence   measured | unmeasured-claim
  why           the argument. Never empty.

`must-differ` is the one verdict that is NOT an annotation of a difference,
and it is checked against the two captures rather than against the diff --
see check_must_differ(). It used to be checked by nothing: diff_captures()
returns only keys that differ, so a must-differ record whose two sides had
come to AGREE matched nothing, was reported as stale, and the operator was
told to delete it. That is the failure D1 exists to prevent, announced as an
instruction to remove the guard.

`hw_evidence` is not in the design and is here because the design asks the
single-sided mode to say "0 of N records verified against hardware", and
nothing can COUNT that unless each record carries it as a field. Prose in
`why` cannot be counted, and a claim that is only a sentence is a claim that
turns into a fact the second time somebody reads the file.

`until-image` is the fourth verdict and it is here for the same reason: the
committed emulator-side artefact was captured from a busybox initramfs on the
repo's own kernel, because there is no Buildroot image yet. Records that are
properties of THAT ROOTFS rather than of the emulator cannot honestly be
called permanent or until-stage-N, and calling them either would be the
harness lying about its own evidence.
"""

import argparse
import hashlib
import re
import sys
from pathlib import Path

SENTINEL = "INV|"
FORMAT_SUPPORTED = 1
VERDICTS = ("permanent", "must-differ", "until-image")
STAGE_VERDICT = re.compile(r"^until-stage-\d+$")
EVIDENCE = ("measured", "unmeasured-claim")
FIELDS = ("key", "qemu", "hw", "verdict", "hw_evidence", "why")


class CaptureError(Exception):
    """A capture that cannot be parsed, or that is refused as a reference."""


class Capture:
    """One nd-inventory capture, parsed.

    `records` maps key -> value for COMPARED records only; `informational`
    holds the ~ ones. The split is the whole reason the marker is a column:
    the diff is over `records` and nothing else.
    """

    def __init__(self, path, preamble, records, informational, hashes, raw_body):
        self.path = path
        self.preamble = preamble
        self.records = records
        self.informational = informational
        self.hashes = hashes
        self.raw_body = raw_body

    @property
    def method(self):
        return self.preamble.get("capture.method", "")

    @property
    def root(self):
        return self.preamble.get("capture.root", "")

    @property
    def sections(self):
        return self.preamble.get("capture.sections", "")

    @property
    def version_id(self):
        return self.preamble.get("capture.os_version_id", "")

    @property
    def fmt(self):
        return self.preamble.get("format", "")


def parse_capture(path):
    """Parse a capture file. Raises CaptureError on anything malformed.

    Lines without the sentinel are DROPPED rather than rejected: the capture
    arrives down a serial console that interleaves printk, and the sentinel
    exists precisely so the inventory can be lifted out of a noisy log. What
    is not tolerated is a missing BEGIN/END or a hash that does not verify,
    because those mean the capture is incomplete rather than merely dirty.
    """
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    preamble, records, informational, hashes = {}, {}, {}, {}
    body_lines, seen_begin, seen_end, order = [], False, False, []
    transport_src = []

    for line in text.splitlines():
        line = line.rstrip("\r")
        if not line.startswith(SENTINEL):
            continue
        rest = line[len(SENTINEL):]
        if rest.startswith("#"):
            kv = rest[1:].strip()
            if "=" in kv:
                name, _, value = kv.partition("=")
                if name.startswith("sha256."):
                    hashes[name[len("sha256."):]] = value
                else:
                    preamble[name] = value
                    transport_src.append(line + "\n")
            continue
        if rest == "BEGIN":
            seen_begin, transport_src = True, transport_src + [line + "\n"]
            continue
        if rest == "END":
            seen_end = True
            transport_src.append(line + "\n")
            continue
        if not seen_begin or seen_end:
            continue
        transport_src.append(line + "\n")
        body_lines.append(line)
        informational_record = rest.startswith("~")
        if informational_record:
            rest = rest[1:]
        key, _, value = rest.partition(" ")
        if not key:
            raise CaptureError(f"{path}: a record with no key")
        target = informational if informational_record else records
        if key in target:
            raise CaptureError(f"{path}: duplicate record key {key!r}")
        target[key] = value
        order.append(key)

    if not seen_begin or not seen_end:
        raise CaptureError(f"{path}: the capture has no BEGIN/END framing")
    if order != sorted(order):
        raise CaptureError(f"{path}: records are not in LC_ALL=C key order")
    return Capture(path, preamble, records, informational, hashes, "".join(transport_src))


def verify_hashes(cap):
    """Recompute both hashes. Returns a list of complaints, empty when good.

    Two, because they answer different questions: an informational line
    changing must not invalidate a capture, and a printk chewing an
    informational line must still be caught.
    """
    problems = []
    # A MISSING HASH IS A FAILURE AND NOT A SKIP. These were guarded by
    # `if name in cap.hashes`, so a capture carrying no trailer at all
    # verified clean -- and deleting the two lines is easier than editing
    # them. Reproduced: strip the trailer from the committed capture, set
    # capture.method back to nd-inventory, change mem.total_mib from 52 to 96,
    # and the file passed parse_capture(), verify_hashes() and
    # refuse_as_reference() with nothing to say. A forged or hand-repaired
    # hardware capture is the only kind anyone will ever be tempted to
    # produce, since nobody on this branch has a phone.
    #
    # inventory-fallback.sh writes UNAVAILABLE(shell) into both slots rather
    # than omitting them, so it stays refused for the reason it is meant to be
    # refused -- a weaker instrument -- and not for a missing line.
    for name in ("transport", "compared"):
        if name not in cap.hashes:
            problems.append(
                f"{cap.path}: no sha256.{name} -- a capture with no hash is not a capture"
            )
    transport = hashlib.sha256(cap.raw_body.encode()).hexdigest()
    if "transport" in cap.hashes and cap.hashes["transport"] != transport:
        problems.append(
            f"{cap.path}: sha256.transport does not verify "
            f"(recorded {cap.hashes['transport'][:16]}..., recomputed {transport[:16]}...)"
        )
    compared = "".join(f"{SENTINEL}{k} {cap.records[k]}\n" for k in sorted(cap.records))
    digest = hashlib.sha256(compared.encode()).hexdigest()
    if "compared" in cap.hashes and cap.hashes["compared"] != digest:
        problems.append(
            f"{cap.path}: sha256.compared does not verify "
            f"(recorded {cap.hashes['compared'][:16]}..., recomputed {digest[:16]}...)"
        )
    return problems


def refuse_as_reference(cap):
    """Why this capture may not be a baseline or a gating side. [] means it may."""
    reasons = []
    if cap.fmt != str(FORMAT_SUPPORTED):
        reasons.append(
            f"format={cap.fmt!r} is not {FORMAT_SUPPORTED} -- the masks and the family "
            f"table behind this capture are not the ones in the tool"
        )
    if cap.root != "/":
        reasons.append(f"capture.root={cap.root!r} -- a synthetic tree, not a machine")
    if cap.sections != "all":
        reasons.append(f"capture.sections={cap.sections!r} -- a partial capture")
    if cap.method != "nd-inventory":
        reasons.append(
            f"capture.method={cap.method!r} -- a weaker instrument than the tool, and a "
            f"weaker instrument must never become the reference"
        )
    return reasons


# --------------------------------------------------------------------- #
# The diff, and the allowlist over it. Both are pure functions over text,
# which is why they can be unit-tested with no machine anywhere near them.
# --------------------------------------------------------------------- #


def diff_captures(qemu, hw):
    """[(key, qemu_value, hw_value)] for every COMPARED key that differs.

    A key present on one side only is a difference whose other side is
    ABSENT, and that is deliberate: a whole missing record is the loudest
    thing this harness can find -- a virtio_input silently refused for want
    of VIRTIO_F_VERSION_1 produces no line at all.
    """
    out = []
    for key in sorted(set(qemu.records) | set(hw.records)):
        a = qemu.records.get(key, "ABSENT")
        b = hw.records.get(key, "ABSENT")
        if a != b:
            out.append((key, a, b))
    return out


def parse_allowlist(path):
    """Parse allow.txt into a list of dicts. Raises CaptureError if malformed.

    Records are blank-line separated `field  value` blocks; a value may be
    continued on following indented lines, because `why` is an argument and an
    argument that has to fit on one line is an argument nobody makes.
    """
    records, current, field = [], None, None
    for lineno, raw in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        if raw.lstrip().startswith("#"):
            continue
        if not raw.strip():
            if current:
                records.append(current)
            current, field = None, None
            continue
        if raw[0].isspace():
            if current is None or field is None:
                raise CaptureError(f"{path}:{lineno}: a continuation with nothing to continue")
            current[field] += " " + raw.strip()
            continue
        name, _, value = raw.partition(" ")
        name = name.strip()
        if name not in FIELDS:
            raise CaptureError(f"{path}:{lineno}: unknown field {name!r}")
        if current is None:
            current = {}
        if name in current:
            raise CaptureError(f"{path}:{lineno}: {name!r} given twice in one record")
        current[name] = value.strip()
        field = name
    if current:
        records.append(current)
    for rec in records:
        missing = [f for f in FIELDS if f not in rec]
        if missing:
            raise CaptureError(f"{path}: record {rec.get('key', '?')} is missing {missing}")
        # Validated HERE, inside the try/except that main() wraps around
        # parsing, because key_matches() raises the same CaptureError from
        # apply_allowlist() -- which main() calls outside it, so a malformed
        # key came back as an uncaught traceback while every other allowlist
        # defect came back as one REFUSED line.
        if rec["key"].count("[*]") > 1:
            raise CaptureError(f"{path}: {rec['key']!r}: at most one [*] per key")
        for column in ("qemu", "hw"):
            if rec[column] == "~":
                raise CaptureError(
                    f"{path}: {rec['key']}: a bare '~' in the {column} column. It meant a "
                    f"total wildcard in one place and the empty regex in every other; "
                    f"write '~.*' if that is what you mean"
                )
    return records


def key_matches(pattern, key):
    """`pattern` may carry [*] in exactly one segment; it matches one segment.

    The design allowed [*] only in the last segment. That does not survive
    contact with the record keys: mtd.byname."NAND simulator partition 0".size
    puts the free variable in the MIDDLE, and a rule that cannot express the
    real divergence forces somebody to write eight records instead of one. One
    segment, anywhere, and never more than one -- so a pattern still names a
    shape rather than swallowing a subtree.
    """
    if "[*]" not in pattern:
        return pattern == key
    if pattern.count("[*]") != 1:
        raise CaptureError(f"{pattern!r}: at most one [*] per key")
    parts = pattern.split(".")
    keys = key.split(".")
    if len(parts) != len(keys):
        return False
    return all(p == "[*]" or p == k for p, k in zip(parts, keys))


def apply_allowlist(differences, allow):
    """Split a diff by the allowlist.

    Returns (unexplained, stale, satisfied). `stale` is the ratchet and the
    reason the file shrinks rather than accumulates: a record annotating a
    difference that no longer exists has nothing to annotate, and it fails
    until whoever closed the difference deletes it in the same pull request.
    """
    unexplained, satisfied, matched = [], [], set()
    for key, a, b in differences:
        hit = None
        for i, rec in enumerate(allow):
            if not key_matches(rec["key"], key):
                continue
            if not _value_ok(rec["qemu"], a):
                continue
            if not _value_ok(rec["hw"], b):
                continue
            hit = i
            break
        if hit is None:
            unexplained.append((key, a, b))
        else:
            matched.add(hit)
            satisfied.append((key, a, b, allow[hit]))
    # A must-differ record that matched nothing is NOT stale. `differences`
    # holds only keys that differ, so a must-differ record whose two sides
    # AGREE -- the exact event it exists to catch -- matched nothing, fell in
    # here, and main() printed "no longer differs; delete this record": the
    # tool told the operator to remove the one guard standing between a
    # QEMU-built .ndsw and a phone in somebody's pocket. The verdict cannot be
    # evaluated against a diff at all; check_must_differ() evaluates it
    # against the two captures.
    stale = [
        rec
        for i, rec in enumerate(allow)
        if i not in matched and rec["verdict"] != "must-differ"
    ]
    return unexplained, stale, satisfied


def check_must_differ(qemu, hw, allow):
    """[(key, value)] for every must-differ key the two machines AGREE on.

    Nothing read `verdict == "must-differ"` before this: VERDICTS was used
    only to spell-check the field, and the one record carrying it --
    platform.record.image, D1's discriminator -- was enforced by nothing at
    all. allow.txt says "The test fails when a must-differ key is equal" and
    the README says "if the two sides ever agree, a QEMU-built .ndsw becomes
    installable"; this is the code those two sentences describe.

    A key absent from BOTH captures reads ABSENT on both sides and is
    reported, which is right: a discriminator that is missing from both
    machines is not a discriminator.
    """
    violations = []
    keys = sorted(set(qemu.records) | set(hw.records))
    for rec in allow:
        if rec["verdict"] != "must-differ":
            continue
        for key in keys:
            if not key_matches(rec["key"], key):
                continue
            a = qemu.records.get(key, "ABSENT")
            b = hw.records.get(key, "ABSENT")
            if a == b:
                violations.append((key, a))
        if not any(key_matches(rec["key"], k) for k in keys):
            violations.append((rec["key"], "ABSENT"))
    return violations


def _value_ok(expected, actual):
    """`~<regex>` where the value is genuinely a free variable; else verbatim.

    A BARE `~` USED TO MEAN TWO DIFFERENT THINGS IN ONE FILE, and one of them
    failed open. apply_allowlist() short-circuited on `rec["qemu"] in ("~", a)`,
    so a bare `~` in the emulator column matched ANY value; here it compiles as
    the empty regex and matches only the empty string, which is what the hw
    column and the qemu-column host test always went through. So a reviewer
    writing `qemu ~` to mean "the emulator has nothing here" got a total
    wildcard that would silently absorb whatever the emulator grew later, and
    the same token in the hw column matched nothing at all. There is one
    meaning now and `~.*` is the wildcard; parse_allowlist() rejects the bare
    form so the ambiguity cannot be written down again.
    """
    if expected.startswith("~"):
        return re.fullmatch(expected[1:], actual) is not None
    return expected == actual


def propose(differences, allow):
    """Skeleton records for the unexplained keys, with an EMPTY why.

    Empty on purpose. The host test refuses an empty why, so the generated
    skeleton cannot be committed without somebody typing the argument -- which
    is what keeps the file a set of reasons instead of a set of hashes.

    EVERY RECORD IS PRECEDED BY A BLANK LINE, INCLUDING THE FIRST. The README
    documents `--propose >> allow.txt` as the one operation it asks a reviewer
    to perform, and allow.txt ends with a record and no trailing blank line --
    so a first record with no leading separator landed INSIDE the previous
    block, and parse_allowlist() then raised "'key' given twice in one record"
    pointing at a line the operator had not touched. Reproduced with one
    proposed key; every test in test_parity_allowlist.py failed on the result
    and the obvious recovery was to hand-edit the innocent record the error
    named.
    """
    unexplained, _, _ = apply_allowlist(differences, allow)
    out = []
    for key, a, b in unexplained:
        out.append(
            f"key          {key}\n"
            f"qemu         {a}\n"
            f"hw           {b}\n"
            f"verdict      until-stage-N\n"
            f"hw_evidence  measured\n"
            f"why          \n"
        )
    return "".join("\n" + rec for rec in out)


def main(argv=None):
    ap = argparse.ArgumentParser(description="compare two nd-inventory captures")
    ap.add_argument("--qemu", required=True)
    ap.add_argument("--hw", required=True)
    ap.add_argument("--allow", required=True)
    ap.add_argument("--propose", action="store_true")
    ap.add_argument("--force-version-skew", action="store_true")
    args = ap.parse_args(argv)

    try:
        qemu = parse_capture(args.qemu)
        hw = parse_capture(args.hw)
        allow = parse_allowlist(args.allow)
    except CaptureError as exc:
        print(f"REFUSED: {exc}", file=sys.stderr)
        return 2

    problems = []
    for cap in (qemu, hw):
        problems += verify_hashes(cap)
        problems += [f"{cap.path}: {r}" for r in refuse_as_reference(cap)]
    if problems:
        print("REFUSED -- these are not captures this tool may compare:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 2

    if qemu.version_id != hw.version_id:
        message = (
            f"the two captures are of different images: {qemu.version_id} against "
            f"{hw.version_id}. That is two unrelated observations, not a parity test."
        )
        if not args.force_version_skew:
            print(f"REFUSED: {message}", file=sys.stderr)
            return 2
        print(f"WARNING: --force-version-skew: {message}", file=sys.stderr)

    differences = diff_captures(qemu, hw)
    if args.propose:
        print(propose(differences, allow), end="")
        return 0

    unexplained, stale, satisfied = apply_allowlist(differences, allow)
    violations = check_must_differ(qemu, hw, allow)
    for key, value in violations:
        print(f"MUST-DIFFER VIOLATED  {key}: both sides say {value}")
    for key, a, b in unexplained:
        print(f"UNEXPLAINED  {key}\n  qemu {a}\n  hw   {b}")
    for rec in stale:
        print(f"STALE        {rec['key']} -- no longer differs; delete this record")
    print(
        f"\n{len(differences)} differing keys, {len(satisfied)} explained, "
        f"{len(unexplained)} unexplained, {len(stale)} stale records, "
        f"{len(violations)} must-differ violations"
    )
    return 1 if (unexplained or stale or violations) else 0


if __name__ == "__main__":
    sys.exit(main())

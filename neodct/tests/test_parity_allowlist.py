"""neodct/tests/parity/ -- the half of the parity harness that needs no boot.

Hardware is rare and kernel-config changes are not, so the common failure is
not "the phone and the emulator diverged" but "somebody changed something and
the emulator quietly drifted". That half is caught here, on every change, with
no phone in the room and no image built: the committed capture is checked
against itself and against the allowlist, and the allowlist's own rules -- no
empty argument, no stale record, pinned record counts, and no column that
matches every value a machine can report -- are enforced as tests rather than
as a convention.

What this file CANNOT do is notice that the committed capture has stopped
describing the emulator, because it only ever reads a file. That is
`make parity-probe` in neodct/src, which boots the kernel and requires a fresh
capture to equal the baseline byte for byte.

The comparison functions in neodct/tools/parity_diff.py are pure functions
over text, so they are tested here directly with synthetic captures. That is
the whole reason they are functions and not inline in main(): a comparator
whose behaviour can only be exercised by having two machines is a comparator
nobody exercises.

============ THIS SUITE CANNOT TELL YOU THE PHONE IS FINE ============

There is no luckfox-armv7.inventory, because nobody on this branch has a
Luckfox Pico Mini B. Every `hw` value in allow.txt is a claim, every record
says so in its own hw_evidence field, and test_zero_records_are_verified_
against_hardware asserts the count out loud so that a green run of this file
can never be mistaken for evidence about a phone.
"""

import hashlib
import os
import subprocess
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS_DIR = os.path.join(REPO, "neodct", "tools")
PARITY_DIR = os.path.join(REPO, "neodct", "tests", "parity")
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import parity_diff as pd  # noqa: E402

QEMU_BASELINE = os.path.join(PARITY_DIR, "qemu-armv7-probe.inventory")
# The capture from a BUILT image, which does not exist yet. QEMU_BASELINE
# points at the probe file until it does -- see the guard below.
IMAGE_BASELINE = os.path.join(PARITY_DIR, "qemu-armv7.inventory")
HW_BASELINE = os.path.join(PARITY_DIR, "luckfox-armv7.inventory")
ALLOW = os.path.join(PARITY_DIR, "allow.txt")

# Pinned so that adding one is a visible diff on an integer that a reviewer
# sees, rather than a quiet extra block in a long file. `permanent` is the
# escape hatch that lets an allowlist stop shrinking, and this is the only
# thing standing on it.
# It went from 5 to 10 in one change, and that is the biggest single move it
# will ever be allowed to make without an argument. The five are the four
# small hardware surfaces plus the GPIO stand-in: power_supply and thermal are
# EMPTY BY DECISION (CONFIG_TEST_POWER was removed, and it was the only source
# of both), leds is ABSENT by decision, gpio is a mockup chip that can never
# match the RV1103's controllers, and backlight moved from until-stage-5 to
# permanent because what is left of that divergence is a fact about the
# PHONE -- two populations, reflashed and not -- rather than about the
# emulator. Every one of them is a decision somebody made on purpose, which is
# what `permanent` is for; the count is here so the next five have to be
# argued for in a diff.
# The eleventh is fb0.fix.smem_len, and it is the panel stage paying for what
# it did NOT allowlist: the other ten fb0 records that a first hardware
# capture would have diverged on now AGREE, because neodct_displayd's
# force_mode() runs on both machines. smem_len is the one the ioctl cannot
# reach -- vfb's compile-time VIDEOMEMSIZE -- so it is argued as a FLOOR
# rather than explained away as a difference.
#
# TWELVE THROUGH FIFTEEN ARE THE STORAGE STAGE, AND ONE RECORD WENT AWAY TO
# PAY FOR THEM. `class.mtd` was deleted: the emulator's MTD listing is now
# [mtd0 mtd0ro ... mtd5 mtd5ro], the same key set the phone's six partitions
# produce, so there was nothing left to annotate -- the same reason
# cpufreq.cpu0 and cpufreq.policies were deleted when the surfaces stage
# landed. In its place are the five mtd.byname.[*].* records, which cover the
# emulator's THIRTY geometry keys where before there were five and none of
# them was listed at all. The phone's own thirty are the mirror image and are
# deliberately not written: they do not exist until somebody captures a phone.
#
# AND THE SIXTEENTH IS block.mtdblock5.size, which is the same three megabytes
# of bad-block slack landing in the one family where both machines produce the
# key -- so it lands as a differing VALUE and not as two disjoint key sets.
# It was found by a reviewer, not by this file, and it is here because the
# argument for it was already written down three records away, attached to the
# wrong key family.
PERMANENT_RECORDS_EXPECTED = 16

# `permanent` was the ONLY integer this suite pinned, which left every other
# way of growing the file uncounted. One appended record with a `[*]` key and
# two wildcard columns matched fifteen keys in the committed capture, explained
# every one of them, moved no counter and passed every test -- the whole class
# tree exempt in one block. So the total is pinned too, and so is
# `until-image`: those are the records the README says MUST be revisited the
# first time a built image is captured, and a promise that can be added to
# without a diff on an integer is a promise nobody is holding.
TOTAL_RECORDS_EXPECTED = 27
UNTIL_IMAGE_RECORDS_EXPECTED = 9

# ============ AND THE NUMBER THAT ACTUALLY MOVES, WHICH WAS UNPINNED =======
#
# The record count is a poor proxy for how much of a capture is excused,
# because one `[*]` record can cover thirty keys. Measured across this branch:
# the file went from 21 records covering 67 of the capture's 169 keys to 27
# covering 118 of 230 -- so the fraction of the capture exempt from parity
# went from 40% to 51% while the record count went up by six. Both directions
# are defensible and neither is visible in the other's integer, so both are
# pinned here.
#
# THE README SAID THIS FILE HAD SHRUNK AND IT HAD NOT. One `permanent` record
# (class.mtd) was deleted and six were added. What genuinely shrank is the
# work a first hardware diff has to do: ten fb0 records stopped needing an
# entry because force_mode() now runs on both machines, and thirty
# mtd.byname keys that were silently REQUIRED TO AGREE -- and could not --
# are now argued for. That is the honest sentence, and the README says it in
# those words now.
COVERED_KEYS_EXPECTED = 118

# A column matching every possible value. `~.*` and `~.+` are the two ways to
# write one; see the block in allow.txt's header for why there are none left.
TOTAL_WILDCARDS = ("~.*", "~.+")


@pytest.fixture(scope="module")
def allow():
    return pd.parse_allowlist(ALLOW)


@pytest.fixture(scope="module")
def qemu():
    return pd.parse_capture(QEMU_BASELINE)


# --------------------------------------------------------------------- #
# The committed capture, checked against itself
# --------------------------------------------------------------------- #


def test_baseline_framing_and_order(qemu):
    """BEGIN/END, no duplicate keys, and LC_ALL=C key order.

    parse_capture() raises on all three, so this asserts the file got here
    rather than asserting the parser works -- and the key order matters
    because a capture whose records are not sorted was produced by something
    that is not this tool.
    """
    assert qemu.records, "the committed capture has no compared records"
    keys = list(qemu.records)
    assert keys == sorted(keys)


def test_baseline_hashes_verify(qemu):
    """Both hashes, recomputed.

    The transport hash covers everything transmitted and the compared hash
    covers the compared subset alone, because they answer different
    questions: an informational line changing must not invalidate a capture,
    and a printk chewing an informational line must still be caught. A
    committed artefact whose hashes do not verify has been hand-edited, which
    is the one thing a baseline may never be.
    """
    assert pd.verify_hashes(qemu) == []


def test_baseline_format_matches_the_tool():
    """The capture's format= is the one nd_inventory.h defines.

    Read out of the header rather than duplicated here, so the two cannot
    drift: a bump in the tool that nobody recaptured for fails this.
    """
    header = os.path.join(REPO, "neodct", "src", "tools", "nd_inventory.h")
    with open(header, encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("#define ND_INV_FORMAT"):
                tool_format = line.split()[2]
                break
        else:
            pytest.fail("nd_inventory.h defines no ND_INV_FORMAT")
    cap = pd.parse_capture(QEMU_BASELINE)
    assert cap.fmt == tool_format


def test_the_probe_capture_is_refused_as_a_gating_reference(qemu):
    """And it must be, which is the point of committing it under that name.

    It was captured by a cross-compiled build with no libneodct, in a busybox
    initramfs rather than a NeoDCT image. It is a real measurement of a real
    armv7 machine and it is not a measurement of an image, so it may inform
    the allowlist and may never be a side of a gating comparison. A weaker
    instrument must never be able to become the reference.
    """
    reasons = pd.refuse_as_reference(qemu)
    assert any("capture.method" in r for r in reasons), reasons


def test_the_measured_records_are_the_ones_the_findings_name(qemu):
    """A handful of records asserted by hand, because a file of 170 lines that
    nobody has ever read a line of is not evidence of anything.

    Each of these is a number EMPIRICAL-FINDINGS reached by booting, and each
    would be silently wrong if a collector regressed.
    """
    r = qemu.records
    assert r["uname.machine"] == "armv7l"
    assert r["mem.total_mib"] == "52"
    # 53,820 kB before this capture started passing -dtb. The kernel reserves
    # fdt_totalsize() and a dtc-produced tree is 8 KB against the 1 MiB QEMU
    # pads its own to, so handing over a SMALLER tree gives ~1 MB back and
    # MemTotal RISES. The emulator is now ~800 kB above the phone's ~54 MB
    # instead of ~180 kB below it, and this line is the only thing in the
    # suite that notices when that number moves.
    assert qemu.informational["mem.total_kb"] == "54812"
    # Finding 11: nandsim reproduces the Pico Mini's part exactly, and it is
    # the write size that matters -- mtdram reports 1, so every LEB size and
    # VID header offset computed on it is arithmetic the phone never does.
    name = 'mtd.byname."NAND\\x20simulator\\x20partition\\x200"'
    assert r[f"{name}.erasesize"] == "131072"
    assert r[f"{name}.writesize"] == "2048"
    assert r[f"{name}.oobsize"] == "64"
    # 262144 and not 134217728: `nandsim.parts=2,2,4,128,64` now cuts the chip
    # into PARTITIONS.md's six partitions, so partition 0 is the 256 KiB `env`
    # partition rather than the whole 128 MB part. The CHIP is unchanged -- the
    # erase, write and OOB sizes above are the ones finding 11 measured, and
    # they are per-partition properties every partition reports identically.
    assert r[f"{name}.size"] == "262144"
    # The phone's userdata partition, at the phone's own mtd number, which is
    # what makes `neodct.user=ubi1:userdata` reachable in the emulator at all.
    userdata = 'mtd.byname."NAND\\x20simulator\\x20partition\\x204"'
    assert r[f"{userdata}.size"] == "8388608"
    assert r["mtd.node.mtd4.name"] == "NAND simulator partition 4"
    # The listing the phone produces too, which is why class.mtd stopped being
    # an allow.txt record.
    assert r["class.mtd"] == (
        "[mtd0 mtd0ro mtd1 mtd1ro mtd2 mtd2ro mtd3 mtd3ro mtd4 mtd4ro mtd5 mtd5ro]"
    )
    # The four small hardware surfaces, which are the whole reason a device
    # tree is built at run time. Two of them are PRESENT and it took a .dtsi;
    # two are ABSENT and it took removing a kernel symbol -- and this block
    # is the only place both halves are asserted side by side.
    assert r["class.backlight"] == "[backlight]"
    assert r["class.backlight.backlight.max_brightness"] == "10"
    assert r["cpufreq.cpu0"] == "present"
    assert r["cpufreq.policies"] == "[policy0]"
    assert r["cpufreq.available"] == "408000 600000 816000 1008000 1200000"
    # Empty, not populated: CONFIG_TEST_POWER put test_ac, test_battery and
    # test_usb in the first and -- through psy_register_thermal(), which names
    # a zone after the supply -- a thermal_zone0 typed `test_battery` in the
    # second. Both measured before the symbol came out.
    assert r["class.power_supply"] == "[]"
    assert r["class.thermal"] == "[]"
    # ABSENT and not empty. LEDS_CLASS is what would create the directory;
    # CONFIG_NEW_LEDS on its own creates nothing, which is why it came out.
    assert r["class.leds"] == "ABSENT"
    # export and unexport ARE the legacy sysfs GPIO interface, and GPIO_SYSFS
    # is `bool ... if EXPERT` with no default -- one olddefconfig from gone.
    # gpiochip0 is the mockup at ranges=0,64, which is what puts gpio53 where
    # ND_BL_GPIO_PIN says it is; gpiochip512 is -M virt's own eight-line
    # pl061, on which none of this phone's pin numbers exists.
    assert r["class.gpio"] == "[export gpiochip0 gpiochip512 unexport]"
    # Trap 1: MD is the gate under the whole immutable-rootfs design, and the
    # check is /dev/mapper/control rather than `grep verity /proc/devices`,
    # because verity is a DM target and not a device.
    assert r["dm.control"] == "present"
    # Finding 10: without -global virtio-mmio.force-legacy=false there is no
    # line here at all, silently, at every loglevel.
    assert r["input.node.event0.name"] == "QEMU Virtio Keyboard"
    # THE RECORD THAT GATES THE WHOLE IMAGE DESIGN, and it used to contain
    # neither of the two filesystems its own comment names. /proc/filesystems
    # writes "nodev\t<name>" for a virtual filesystem and "\t<name>" for a
    # device-backed one, and the collector selected column 1 -- so it held the
    # nodev entries and nothing else, and a kernel built without CONFIG_SQUASHFS
    # and CONFIG_EXT4_FS produced a byte-identical baseline. squashfs is the
    # verity root and ext4 is the only writable partition; an image that can
    # mount neither is an image that cannot boot.
    filesystems = r["proc.filesystems"]
    for fs in ("squashfs", "ext4", "ubifs"):
        assert f" {fs} " in f" {filesystems.strip('[]')} ", (
            f"{fs} is missing from proc.filesystems: {filesystems}"
        )


def test_the_framebuffer_is_the_phones_because_the_phones_code_put_it_there(qemu):
    """fb0 in the phone's mode, set by the phone's own force_mode().

    THIS TEST USED TO ASSERT 640 AND 8, and documented an ABSENCE: there was
    no S90display in a busybox initramfs, nothing under the emulator issued
    FBIOPUT_VSCREENINFO, and the eleven framebuffer records described vfb's
    built-in default rather than anything NeoDCT would ever see. It now
    documents a PARITY, and the difference between those two sentences is the
    whole of the panel stage.

    What makes it worth asserting is HOW it became true. The mode is not set
    by the capture script, by a kernel parameter or by a device model: the
    probe runs neodct_displayd -- the same binary, the same force_mode(), the
    same 32-bpp-then-16-bpp fallback -- with a backend that is not the SPI
    panel. So these numbers are evidence about the daemon's start-up path on a
    real armv7 boot, and not a description somebody typed.

    line_length 960 is 240 * 4, and visual 2 is TRUECOLOR where vfb's 8-bpp
    default was 3, PSEUDOCOLOR. red at offset 0 with blue at 16 is what
    convert_rect() reads to decide fb_swap_rb -- the bug its comment block is
    about -- so it is asserted here rather than left implicit.
    """
    r = qemu.records
    assert r["fb0.var.xres"] == "240"
    assert r["fb0.var.yres"] == "175"
    assert r["fb0.var.xres_virtual"] == "240"
    assert r["fb0.var.yres_virtual"] == "175"
    assert r["fb0.var.bits_per_pixel"] == "32"
    assert r["fb0.fix.line_length"] == "960"
    assert r["fb0.fix.visual"] == "2"
    assert r["fb0.var.red"] == "0/8"
    assert r["fb0.var.green"] == "8/8"
    assert r["fb0.var.blue"] == "16/8"
    assert r["fb0.var.transp"] == "24/8"
    # And the one force_mode() cannot move, which is why it is the only fb0
    # record in allow.txt. 168,000 is 960 * 175: below it vfb_check_var()
    # refuses the 32-bpp mode outright and the daemon silently runs a
    # different pixel pipeline.
    assert int(r["fb0.fix.smem_len"]) >= 960 * 175


# --------------------------------------------------------------------- #
# The allowlist's own rules
# --------------------------------------------------------------------- #


def test_allowlist_parses_and_every_field_is_filled(allow):
    assert allow, "the allowlist is empty"
    for rec in allow:
        for field in pd.FIELDS:
            assert rec[field], f"{rec.get('key', '?')}: {field} is empty"
        assert rec["why"].strip(), f"{rec['key']}: no argument"
        assert "UNASSIGNED" not in rec["why"]


def test_every_verdict_is_one_of_the_four(allow):
    for rec in allow:
        verdict = rec["verdict"]
        assert (
            verdict in pd.VERDICTS or pd.STAGE_VERDICT.match(verdict)
        ), f"{rec['key']}: {verdict!r} is not a verdict"
        # The skeleton --propose writes carries until-stage-N literally, so a
        # generated record that nobody finished cannot pass.
        assert verdict != "until-stage-N", f"{rec['key']}: an unedited --propose skeleton"


def test_no_key_is_annotated_twice(allow):
    keys = [rec["key"] for rec in allow]
    assert len(keys) == len(set(keys))


def test_the_record_counts_are_pinned(allow):
    """Total and `until-image`, beside `permanent`.

    Every one of these is a way for the file to stop shrinking, and only one
    of them was counted. A record appended with no counter to move is a record
    no reviewer is asked to look at.
    """
    assert len(allow) == TOTAL_RECORDS_EXPECTED, (
        f"the allowlist is now {len(allow)} records. Adding one means changing "
        f"TOTAL_RECORDS_EXPECTED here, on purpose, where a reviewer sees it."
    )
    until_image = [rec["key"] for rec in allow if rec["verdict"] == "until-image"]
    assert len(until_image) == UNTIL_IMAGE_RECORDS_EXPECTED, (
        f"the until-image records are now {until_image}. Each one is a promise to "
        f"revisit a record the first time parity_capture_qemu.sh runs on a built "
        f"image; adding one is a diff on this integer."
    )


def test_how_much_of_the_capture_is_excused_is_pinned_too(allow, qemu):
    """The record count is a poor proxy, and this is the number it hides.

    One `[*]` record can cover thirty keys, so a file can grow its exempt
    surface by fifty keys while its record count moves by five -- which is
    exactly what happened on the branch that added the storage stage, and the
    README said the file had shrunk. It shrank in one sense (ten fb0 records
    became unnecessary, thirty mtd.byname keys stopped being silently required
    to agree) and grew in the other. Both are now integers a reviewer sees.
    """
    covered = [
        key for key in qemu.records
        if any(pd.key_matches(rec["key"], key) for rec in allow)
    ]
    assert len(covered) == COVERED_KEYS_EXPECTED, (
        f"{len(covered)} of the capture's {len(qemu.records)} records are now "
        f"excused by the allowlist, not {COVERED_KEYS_EXPECTED}. That is the "
        f"number the record count hides: change it here, deliberately, and say "
        f"in the commit which direction the file moved."
    )


def test_no_column_is_a_total_wildcard(allow):
    """Neither column may match every value the machine can report.

    Eight records carried `hw ~.*` or `~.+`, six of them permanent, so they
    pinned the emulator and nothing else. class.gpio is the case that shows
    what that costs: its own argument says the record exists to pin `export`
    and `unexport` -- the whole legacy sysfs GPIO interface, one olddefconfig
    from gone -- and the phone column pinned neither.

    Zero, rather than "not on a permanent record", because a total wildcard on
    an until-image record is the same silence with a nearer deadline.
    """
    wild = [
        (rec["key"], column)
        for rec in allow
        for column in ("qemu", "hw")
        if rec[column] in TOTAL_WILDCARDS
    ]
    assert wild == [], (
        f"{wild} match every value the machine can report. Where the exact value "
        f"cannot be predicted the SHAPE usually can -- a listing rather than "
        f"ABSENT, a mount record rather than a missing one. See allow.txt's header."
    )


def test_a_subtree_record_must_describe_at_least_one_side(allow):
    """A `[*]` key with a free value on both sides exempts a whole subtree.

    Measured: one appended block reading `key class.[*]` / `qemu ~.*` /
    `hw ~.*` matched fifteen keys in the committed capture and explained all
    fifteen, so the qemu-column bijection -- which asks a wildcard record to
    describe AT LEAST ONE matched key -- passed trivially, and apply_allowlist()
    went on to absorb class.leds, class.i2c-dev and class.power_supply as well.
    A pattern is allowed to name a shape; it is not allowed to name a shape and
    then decline to say anything about it.
    """
    for rec in allow:
        if "[*]" not in rec["key"]:
            continue
        assert not (
            rec["qemu"] in TOTAL_WILDCARDS and rec["hw"] in TOTAL_WILDCARDS
        ), f"{rec['key']}: a subtree pattern with a free value on both sides exempts the subtree"


def test_permanent_count_is_pinned(allow):
    permanent = [rec["key"] for rec in allow if rec["verdict"] == "permanent"]
    assert len(permanent) == PERMANENT_RECORDS_EXPECTED, (
        f"the permanent records are now {permanent}. `permanent` is the escape hatch "
        f"that lets this file stop shrinking; adding one means changing "
        f"PERMANENT_RECORDS_EXPECTED in this test, on purpose, where a reviewer sees it."
    )


def test_the_qemu_column_matches_the_committed_capture(allow, qemu):
    """Every allowlist record's `qemu` value is checked against the measurement.

    This is the half of the bijection that CAN run single-sided, and it is
    what stops the emulator column from becoming folklore: a record claiming
    class.backlight is [] when the capture says otherwise fails here.

    A wildcard record has to describe AT LEAST ONE record in the capture, not
    every record its pattern touches. `dev.[*]` is about the owner columns and
    reaches `dev.count` as well, and apply_allowlist() would not use it to
    explain a difference in that one -- a record is matched on its key AND on
    both values, so a pattern cannot silently cover a divergence whose values
    it does not describe. Requiring every touched key to match would force the
    pattern to be split into forty records saying the same thing.
    """
    for rec in allow:
        matching = [k for k in qemu.records if pd.key_matches(rec["key"], k)]
        described = [k for k in matching if pd._value_ok(rec["qemu"], qemu.records[k])]
        if not matching:
            # A key that is not in the capture at all is legitimate only when
            # the record says the emulator side is absent -- which is the
            # honest state of every `until-image` record here.
            assert rec["qemu"].startswith("~") or rec["qemu"] == "ABSENT", (
                f"{rec['key']}: names no record in the committed capture, but claims "
                f"the emulator value is {rec['qemu']!r}"
            )
            continue
        assert described, (
            f"{rec['key']} claims qemu={rec['qemu']!r}, and no record it matches in the "
            f"committed capture has that value: "
            f"{ {k: qemu.records[k] for k in matching[:4]} }"
        )


def test_the_surfaces_stage_closed_its_records(allow):
    """until-stage-5 was five records and is now none.

    This is the mechanism the header describes, asserted rather than
    described. cpufreq.cpu0, cpufreq.policies, class.backlight,
    class.power_supply and class.thermal all said "a named stage will close
    this". The stage closed it -- a device tree for the first three, a kernel
    symbol REMOVED for the last two -- and the two with nothing left to
    annotate were DELETED while the other three were rewritten to the facts
    that survive: two populations of phone for the backlight, and a deliberate
    absence for the other two.

    The test is pinned to the stage number rather than to a count so that it
    keeps meaning something: a record reintroduced as until-stage-5 would be a
    stage claiming to close what it has already closed.
    """
    stage5 = [rec["key"] for rec in allow if rec["verdict"] == "until-stage-5"]
    assert stage5 == [], f"{stage5} still promise a stage that has landed"


def test_zero_records_are_verified_against_hardware(allow):
    """The loud one.

    A single-sided harness must not pass quietly -- that is nd-selftest's own
    house rule about SKIP, applied to a file. When luckfox-armv7.inventory
    arrives, records get hw_evidence: measured one at a time and this
    assertion changes shape with them.
    """
    measured = [rec["key"] for rec in allow if rec["hw_evidence"] == "measured"]
    unmeasured = [rec["key"] for rec in allow if rec["hw_evidence"] == "unmeasured-claim"]
    assert not os.path.exists(HW_BASELINE), (
        "luckfox-armv7.inventory exists now, so this test must be replaced by the "
        "two-sided bijection: recompute the diff and require one record per "
        "differing key, no stale records, and every must-differ key differing."
    )
    assert measured == [], (
        f"{measured} claim hw_evidence: measured, but there is no hardware capture "
        f"in {PARITY_DIR} for them to have been measured against."
    )
    assert len(unmeasured) == len(allow)
    print(
        f"\nPARITY: 0 of {len(allow)} allowlist records are verified against hardware. "
        f"This allowlist is a claim, not a measurement."
    )


def test_every_evidence_field_is_one_of_the_two(allow):
    for rec in allow:
        assert rec["hw_evidence"] in pd.EVIDENCE, f"{rec['key']}: {rec['hw_evidence']!r}"


def test_no_image_capture_has_arrived_unnoticed():
    """The mirror of the hardware guard above, which this suite did not have.

    QEMU_BASELINE is the PROBE capture -- a busybox initramfs on the repo's own
    kernel, with no /NeoDCT, no os-release and no libneodct. The day somebody
    commits a capture from a built image, every `until-image` record here is
    annotating a rootfs nobody boots any more, and nothing would have said so:
    QEMU_BASELINE would still point at the probe file and the twelve promises
    the README calls out as "MUST be revisited" would keep passing against it.
    """
    assert not os.path.exists(IMAGE_BASELINE), (
        "qemu-armv7.inventory exists now. QEMU_BASELINE must move to it, and every "
        "until-image record in allow.txt must be re-argued against it -- that is what "
        "the verdict promises. Until then the emulator column is checked against a "
        "capture of a busybox initramfs."
    )


def test_the_documented_way_to_add_a_record_produces_a_parseable_file(tmp_path):
    """`parity_diff.py ... --propose >> allow.txt` is the one operation the
    README asks a reviewer to perform, and it used to corrupt the file.

    propose() separated records with a blank line but emitted none before the
    first, and allow.txt ends with a record and no trailing blank line -- so
    the appended `key` line landed inside the previous block and
    parse_allowlist() raised "'key' given twice in one record" pointing at a
    line the operator had not touched. This test is that command.
    """
    with open(ALLOW, encoding="utf-8") as handle:
        body = handle.read()
    appended = tmp_path / "allow.txt"
    appended.write_text(
        body + pd.propose([("class.newthing", "[]", "[a]")], []), encoding="utf-8"
    )
    records = pd.parse_allowlist(str(appended))
    assert len(records) == TOTAL_RECORDS_EXPECTED + 1
    assert records[-1]["key"] == "class.newthing"


def test_a_bare_tilde_is_refused_in_either_column(tmp_path):
    """One token, one meaning.

    `~` was a total wildcard inside apply_allowlist()'s qemu short-circuit and
    the empty regex everywhere else, so a reviewer writing it in the qemu
    column to mean "the emulator has nothing here" got a record that would
    explain that key against ANY emulator value the day the emulator grew one.
    """
    for column in ("qemu", "hw"):
        fields = {"qemu": "[]", "hw": "[a]"}
        fields[column] = "~"
        text = (
            f"key          x\nqemu         {fields['qemu']}\nhw           {fields['hw']}\n"
            f"verdict      permanent\nhw_evidence  measured\nwhy          because.\n"
        )
        path = _write(tmp_path, f"allow-{column}.txt", text)
        with pytest.raises(pd.CaptureError, match="bare"):
            pd.parse_allowlist(path)


def test_a_malformed_key_comes_back_as_a_refusal_and_not_a_traceback(tmp_path):
    """key_matches() raises CaptureError, and main() called it from
    apply_allowlist() -- outside the try that catches one. So a two-[*] key
    exited with a Python traceback while every other allowlist defect exited
    with one REFUSED line, and a malformed allowlist looked like a broken
    comparator. Validated in parse_allowlist() now, which is inside the try."""
    a = _write(tmp_path, "a.inventory", _capture_text({"x": "1"}))
    b = _write(tmp_path, "b.inventory", _capture_text({"x": "2"}))
    allow_path = _write(
        tmp_path,
        "allow.txt",
        "key          a.[*].[*].size\nqemu         1\nhw           2\n"
        "verdict      permanent\nhw_evidence  measured\nwhy          because.\n",
    )
    assert pd.main(["--qemu", a, "--hw", b, "--allow", allow_path]) == 2


# --------------------------------------------------------------------- #
# The comparator itself, on synthetic captures
# --------------------------------------------------------------------- #


def _capture_text(records, informational=(), **preamble):
    head = {
        "format": "1",
        "capture.method": "nd-inventory",
        "capture.root": "/",
        "capture.euid": "0",
        "capture.os_version_id": "0.6.0a",
        "capture.sections": "all",
    }
    head.update(preamble)
    lines = [f"INV|# {k}={v}" for k, v in head.items()]
    lines.append("INV|BEGIN")
    body = [(k, v, False) for k, v in records.items()]
    body += [(k, v, True) for k, v in dict(informational).items()]
    for key, value, raw in sorted(body):
        lines.append(f"INV|{'~' if raw else ''}{key} {value}")
    lines.append("INV|END")
    transport = "".join(line + "\n" for line in lines)
    compared = "".join(f"INV|{k} {records[k]}\n" for k in sorted(records))
    lines.append(f"INV|# sha256.transport={hashlib.sha256(transport.encode()).hexdigest()}")
    lines.append(f"INV|# sha256.compared={hashlib.sha256(compared.encode()).hexdigest()}")
    return "\n".join(lines) + "\n"


def _write(tmp_path, name, text):
    path = tmp_path / name
    path.write_text(text, encoding="utf-8")
    return str(path)


def test_a_capture_survives_printk_interleaved_into_it(tmp_path):
    """The sentinel earns its keep here.

    The capture comes back down a serial console that interleaves kernel
    messages. Lines without INV| are dropped rather than rejected; the hashes
    are what say whether anything of ours was lost.
    """
    text = _capture_text({"a": "1", "b": "2"})
    dirty = text.replace(
        "INV|BEGIN\n", "INV|BEGIN\n[   4.117] random: crng init done\n"
    )
    cap = pd.parse_capture(_write(tmp_path, "dirty.inventory", dirty))
    assert cap.records == {"a": "1", "b": "2"}
    assert pd.verify_hashes(cap) == []


def test_a_chewed_line_fails_its_hash(tmp_path):
    text = _capture_text({"a": "1", "b": "2"}).replace("INV|b 2", "INV|b 22")
    cap = pd.parse_capture(_write(tmp_path, "chewed.inventory", text))
    problems = pd.verify_hashes(cap)
    assert len(problems) == 2, problems
    assert any("transport" in p for p in problems)
    assert any("compared" in p for p in problems)


def test_an_informational_line_changing_does_not_invalidate_the_identity(tmp_path):
    """Which is the whole reason there are two hashes and not one."""
    text = _capture_text({"a": "1"}, informational={"note": "x"})
    text = text.replace("INV|~note x", "INV|~note y")
    cap = pd.parse_capture(_write(tmp_path, "note.inventory", text))
    problems = pd.verify_hashes(cap)
    assert any("transport" in p for p in problems)
    assert not any("compared" in p for p in problems)


def test_unsorted_records_are_refused(tmp_path):
    text = _capture_text({"a": "1", "b": "2"}).replace(
        "INV|a 1\nINV|b 2", "INV|b 2\nINV|a 1"
    )
    with pytest.raises(pd.CaptureError, match="LC_ALL=C"):
        pd.parse_capture(_write(tmp_path, "unsorted.inventory", text))


def test_missing_framing_is_refused(tmp_path):
    text = _capture_text({"a": "1"}).replace("INV|END\n", "")
    with pytest.raises(pd.CaptureError, match="BEGIN/END"):
        pd.parse_capture(_write(tmp_path, "truncated.inventory", text))


@pytest.mark.parametrize(
    "override,needle",
    [
        ({"capture.root": "/tmp/synthetic"}, "capture.root"),
        ({"capture.sections": "kernel,memory"}, "capture.sections"),
        ({"capture.method": "shell-fallback"}, "capture.method"),
        ({"capture.method": "nd-inventory-nolib"}, "capture.method"),
        ({"format": "2"}, "format"),
    ],
)
def test_weaker_captures_are_refused_as_a_reference(tmp_path, override, needle):
    """Four ways to produce a plausible-looking capture that is not a machine.

    --root manufactures one from a directory; a sectioned capture is a
    shorter file whose diff would be about the operator's arguments; the
    shell fallback and the no-libneodct probe build are weaker instruments;
    and a capture from another format version is not old data, it is data
    whose masks are unknown.
    """
    text = _capture_text({"a": "1"}, **override)
    cap = pd.parse_capture(_write(tmp_path, "weak.inventory", text))
    reasons = pd.refuse_as_reference(cap)
    assert any(needle in r for r in reasons), reasons


def test_diff_treats_a_missing_record_as_ABSENT(tmp_path):
    """A whole missing record is the loudest thing this harness can find."""
    a = pd.parse_capture(_write(tmp_path, "a.inventory", _capture_text({"x": "1", "y": "2"})))
    b = pd.parse_capture(_write(tmp_path, "b.inventory", _capture_text({"x": "1"})))
    assert pd.diff_captures(a, b) == [("y", "2", "ABSENT")]


def test_allowlist_explains_stale_and_unexplained(tmp_path):
    allow = [
        {
            "key": "class.backlight",
            "qemu": "[]",
            "hw": r"~\[.+\]",
            "verdict": "until-stage-5",
            "hw_evidence": "measured",
            "why": "Stage 5 gives QEMU a backlight device.",
        },
        {
            "key": "cpufreq.cpu0",
            "qemu": "ABSENT",
            "hw": "present",
            "verdict": "until-stage-5",
            "hw_evidence": "measured",
            "why": "-M virt has no OPP table.",
        },
    ]
    differences = [
        ("class.backlight", "[]", "[backlight0]"),
        ("uname.release", "6.12.47", "5.10.110"),
    ]
    unexplained, stale, satisfied = pd.apply_allowlist(differences, allow)
    assert [k for k, _, _ in unexplained] == ["uname.release"]
    assert [r["key"] for r in stale] == ["cpufreq.cpu0"]
    assert [s[0] for s in satisfied] == ["class.backlight"]


def test_a_record_whose_qemu_side_moved_stops_explaining(tmp_path):
    """The annotation is of a specific difference, not of a key.

    If the emulator side changes, the record no longer describes what is
    there and the difference goes back to being unexplained -- which is what
    stops an allowlist from silently covering a NEW divergence that happens
    to share a key with an old one.
    """
    allow = [
        {
            "key": "class.backlight",
            "qemu": "[]",
            "hw": r"~\[.+\]",
            "verdict": "until-stage-5",
            "hw_evidence": "measured",
            "why": "Stage 5.",
        }
    ]
    unexplained, _, _ = pd.apply_allowlist(
        [("class.backlight", "[backlight1]", "[backlight0]")], allow
    )
    assert [k for k, _, _ in unexplained] == ["class.backlight"]


def test_key_wildcard_matches_one_segment_only():
    assert pd.key_matches("dev.[*]", "dev.tty[N]")
    assert pd.key_matches("dev.[*]", "dev.mapper/control")
    assert pd.key_matches('mtd.byname.[*].writesize', 'mtd.byname."NAND sim".writesize')
    # One segment, not a subtree: a pattern still names a shape.
    assert not pd.key_matches("dev.[*]", "dev.snd.timer")
    assert not pd.key_matches("dev.[*]", "block.vda.size")
    with pytest.raises(pd.CaptureError):
        pd.key_matches("a.[*].[*]", "a.b.c")


def test_propose_writes_a_skeleton_nobody_can_commit():
    """--propose leaves `why` empty, and test_allowlist_parses_and_every_field_
    is_filled refuses an empty why. That is what keeps the file a set of
    reasons instead of a set of hashes."""
    text = pd.propose([("uname.release", "6.12.47", "5.10.110")], [])
    assert "key          uname.release" in text
    assert "verdict      until-stage-N" in text
    # The two things the host test above refuses: an empty why, and the
    # literal until-stage-N placeholder. Either one keeps a generated skeleton
    # out of the tree until somebody has typed the argument.
    assert text.rstrip().endswith("why")


def test_a_must_differ_key_that_agrees_is_a_violation_and_not_a_stale_record(tmp_path):
    """The event D1 exists to prevent, and it used to be reported as an
    instruction to delete the guard.

    diff_captures() returns only keys that DIFFER, so a must-differ record
    whose two sides AGREE matched nothing, fell into `stale`, and main()
    printed "no longer differs; delete this record" about the only thing
    standing between a QEMU-built .ndsw and a phone in somebody's pocket.
    Nothing anywhere read verdict == "must-differ"; VERDICTS was used to spell
    -check the field and for nothing else.
    """
    same = _write(
        tmp_path, "a.inventory", _capture_text({"platform.record.image": "qemu-armv7"})
    )
    also = _write(
        tmp_path, "b.inventory", _capture_text({"platform.record.image": "qemu-armv7"})
    )
    allow_path = _write(
        tmp_path,
        "allow.txt",
        "key          platform.record.image\nqemu         ~.+\nhw           ~.+\n"
        "verdict      must-differ\nhw_evidence  measured\nwhy          D1's discriminator.\n",
    )
    allow = pd.parse_allowlist(allow_path)
    qemu = pd.parse_capture(same)
    hw = pd.parse_capture(also)

    assert pd.check_must_differ(qemu, hw, allow) == [
        ("platform.record.image", "qemu-armv7")
    ]
    # And it is NOT reported as stale, which is what used to happen instead.
    _, stale, _ = pd.apply_allowlist(pd.diff_captures(qemu, hw), allow)
    assert stale == []
    assert pd.main(["--qemu", same, "--hw", also, "--allow", allow_path]) == 1

    # The same record over two captures that DO differ passes.
    other = _write(
        tmp_path, "c.inventory", _capture_text({"platform.record.image": "luckfox-armv7"})
    )
    assert pd.check_must_differ(qemu, pd.parse_capture(other), allow) == []
    assert pd.main(["--qemu", same, "--hw", other, "--allow", allow_path]) == 0


def test_a_capture_with_no_hash_trailer_is_refused(tmp_path):
    """Deleting the two sha256 lines is easier than editing them, and it used
    to turn the check off rather than fail it.

    Reproduced against the committed capture: strip the trailer, set
    capture.method back to nd-inventory, change mem.total_mib, and
    parse_capture(), verify_hashes() and refuse_as_reference() all had nothing
    to say. A forged hardware capture is the only kind anybody will ever be
    tempted to produce, since nobody on this branch has a phone.
    """
    text = _capture_text({"a": "1"})
    stripped = "\n".join(
        line for line in text.splitlines() if not line.startswith("INV|# sha256.")
    ) + "\n"
    cap = pd.parse_capture(_write(tmp_path, "nohash.inventory", stripped))
    problems = pd.verify_hashes(cap)
    assert len(problems) == 2, problems
    assert all("not a capture" in p for p in problems)


def test_version_skew_is_refused_rather_than_diffed(tmp_path):
    a = _write(tmp_path, "a.inventory", _capture_text({"x": "1"}))
    b = _write(
        tmp_path, "b.inventory", _capture_text({"x": "2"}, **{"capture.os_version_id": "0.5.8b"})
    )
    allow_path = _write(
        tmp_path,
        "allow.txt",
        "key          x\nqemu         1\nhw           2\nverdict      permanent\n"
        "hw_evidence  measured\nwhy          because.\n",
    )
    rc = pd.main(["--qemu", a, "--hw", b, "--allow", allow_path])
    assert rc == 2
    rc = pd.main(["--qemu", a, "--hw", b, "--allow", allow_path, "--force-version-skew"])
    assert rc == 0


def test_the_capture_script_refuses_rather_than_records():
    """parity_capture_probe.sh must not be able to write a capture from a boot
    that went wrong. Checked as text because running it takes a QEMU boot and
    a cross toolchain, neither of which belongs in this suite."""
    script = os.path.join(TOOLS_DIR, "parity_capture_probe.sh")
    with open(script, encoding="utf-8") as handle:
        body = handle.read()
    assert body.count("REFUSED:") >= 4
    assert "--self-check" in body
    assert "exit 1" in body


def test_the_shell_fallback_is_parseable_and_refused_for_its_own_reason(tmp_path):
    """The fallback's refusal path was unreachable, and for the wrong reason.

    It piped its whole body through `sort`, which sorts complete LINES -- and
    `~` is 0x7E, so every informational record sorted after every letter and
    landed at the bottom of the file. nd_inventory.h says in as many words
    that the marker is a COLUMN precisely so that cannot happen. The result
    was that parse_capture() raised "records are not in LC_ALL=C key order" on
    every capture the script has ever produced, so `capture.method=
    shell-fallback` -- the line that is supposed to refuse it as a weaker
    instrument -- was never read at all, and the error blamed key ordering
    rather than naming the instrument.
    """
    script = os.path.join(TOOLS_DIR, "inventory-fallback.sh")
    out = tmp_path / "fallback.inventory"
    with open(out, "w", encoding="utf-8") as handle:
        rc = subprocess.call(["sh", script], stdout=handle, stderr=subprocess.DEVNULL)
    assert rc == 0
    cap = pd.parse_capture(str(out))
    assert cap.records, "the fallback produced no compared records"
    assert "mem.total_kb" in cap.informational
    reasons = pd.refuse_as_reference(cap)
    assert any("capture.method" in r for r in reasons), reasons
    # And its hash slots say UNAVAILABLE(shell) rather than being absent, so it
    # is refused as a weaker instrument and not as a capture with no trailer.
    assert cap.hashes == {"transport": "UNAVAILABLE(shell)", "compared": "UNAVAILABLE(shell)"}


def test_the_tool_and_the_fallback_agree_about_the_sentinel():
    """One framing, whatever produced it -- which is what makes an ssh capture
    and a serial scrape produce byte-identical artefacts."""
    header = os.path.join(REPO, "neodct", "src", "tools", "nd_inventory.h")
    with open(header, encoding="utf-8") as handle:
        assert 'INV|' in handle.read()

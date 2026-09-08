"""neodct/board/qemu/nd-virt-additions.dtsi -- checked against what it copies.

The emulator's device tree exists so that nd_backlight.c's PWM tier and every
line of nd_cpufreq.c run somewhere other than a phone somebody is holding.
That only works while the numbers in it are the phone's numbers, and they are
COPIES: the brightness table comes from the device-tree node recorded in
docs/HARDWARE_NOTES.md, and the operating points are the five nd_cpufreq.h
names. Two copies of one fact drift, and the drift here is silent -- the
emulator boots, the backlight dims, and it is dimming a panel with a different
table from the one in somebody's pocket.

So this file is the join. It parses the .dtsi and the two documents it copies
from, and fails when they stop agreeing. Editing either side on purpose means
editing both, which is the point.

============ WHAT THIS CANNOT CHECK ============

That the PHONE has this table. docs/HARDWARE_NOTES.md is the record of an SDK
patch applied to a tree that is not in this repository, so the phone column is
documentation and not a measurement -- the same standing every `hw` value in
neodct/tests/parity/allow.txt has, and for the same reason. What is measured
is the emulator: booting this .dtsi gives /sys/class/backlight/backlight with
max_brightness 10 and scaling_available_frequencies "408000 600000 816000
1008000 1200000", and neodct/tools/test_qemu_surfaces.sh asserts both in a
guest.
"""

import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DTSI = os.path.join(REPO, "neodct", "board", "qemu", "nd-virt-additions.dtsi")
HARDWARE_NOTES = os.path.join(REPO, "docs", "HARDWARE_NOTES.md")
CPUFREQ_H = os.path.join(REPO, "neodct", "src", "include", "nd_cpufreq.h")


def read(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def strip_comments(text):
    """The .dtsi is mostly comment, and the comments quote the very numbers
    this file compares -- the measured 24x PWM slowdown, the four bl_power
    boots. Matching inside them would make the test pass on prose."""
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def dtsi_property(name):
    body = strip_comments(read(DTSI))
    match = re.search(r"\b" + re.escape(name) + r"\s*=\s*<([^>]*)>\s*;", body)
    assert match, "%s is not in %s" % (name, DTSI)
    return match.group(1).split()


def notes_backlight_node():
    """The ```dts block in docs/HARDWARE_NOTES.md, which is the phone's."""
    text = read(HARDWARE_NOTES)
    match = re.search(r"```dts\n(.*?)```", text, flags=re.S)
    assert match, "no ```dts block in %s" % HARDWARE_NOTES
    node = match.group(1)
    assert "pwm-backlight" in node, "the first dts block is no longer the backlight"
    return node


def notes_property(name):
    match = re.search(r"\b" + re.escape(name) + r"\s*=\s*<([^>]*)>\s*;", notes_backlight_node())
    assert match, "%s is not in the HARDWARE_NOTES.md node" % name
    return match.group(1).split()


def test_brightness_levels_match_the_phones_node():
    assert dtsi_property("brightness-levels") == notes_property("brightness-levels")


def test_the_table_is_the_eleven_step_one_and_not_a_range():
    """Eleven entries means max_brightness 10, and max_brightness is the whole
    reason this table is copied rather than left at a driver default: it is
    what nd_backlight.c's round-half-even level arithmetic divides by. A
    0-255 panel would exercise that arithmetic against a shape no NeoDCT phone
    has -- measured in the guest, max_brightness reads 10."""
    levels = dtsi_property("brightness-levels")
    assert len(levels) == 11
    assert levels[0] == "0"


def test_default_brightness_level_matches_the_phones_node():
    assert dtsi_property("default-brightness-level") == notes_property("default-brightness-level")


def test_the_backlight_node_has_no_label():
    """A phandle is what pwm_backlight_initial_power_state() reads as "some
    display driver will unblank this", and NeoDCT's panel daemon is userspace
    and never will. Measured: labelled and compiled with `dtc -@`, the guest
    boots bl_power=4 with brightness=10 -- the fault docs/HARDWARE_NOTES.md
    records. run_qemu.sh does not pass -@, so a label alone is inert today;
    this asserts the node anyway, because the two halves of that fault should
    not be one edit apart."""
    body = strip_comments(read(DTSI))
    assert re.search(r"^\tbacklight\s*\{", body, flags=re.M), \
        "the backlight node is gone, renamed, or has grown a label"


def test_the_pwm_period_is_the_measured_one_and_not_the_phones():
    """The one number here that deliberately differs from the phone's node.
    pwm-gpio toggles a line from an hrtimer, so the phone's 25000 ns period
    costs a 24x guest slowdown whenever the panel is DIMMED -- measured, 0.96 s
    against 0.04 s for the same loop, with "hrtimer: interrupt took 217872 ns"
    beside it -- and dimming is what somebody boots the emulator to test. No
    consumer reads the period: nd_backlight.c opens max_brightness, brightness
    and bl_power and nothing else."""
    pwms = dtsi_property("pwms")
    assert pwms[2] == "1000000", "the period moved; read the block above it before changing this"
    assert notes_property("pwms")[2] == "25000", \
        "the phone's period moved, so this divergence needs re-arguing rather than re-pinning"


def test_operating_points_are_the_five_nd_cpufreq_h_names():
    """nd_cpufreq.h's ND_CPUFREQ_MAX_STEPS comment names the RV1103's table,
    test_cpufreq.c parses that exact string, and the guest reads it back out of
    scaling_available_frequencies. Three places, one table."""
    body = strip_comments(read(DTSI))
    hz = re.findall(r"opp-hz\s*=\s*/bits/\s*64\s*<(\d+)>", body)
    khz = sorted(int(value) // 1000 for value in hz)
    assert khz == [408000, 600000, 816000, 1008000, 1200000]

    header = read(CPUFREQ_H)
    match = re.search(r"five entries \(([^)]*)\)", header)
    assert match, "nd_cpufreq.h no longer names the table"
    mhz = sorted(int(value) for value in re.findall(r"\d+", match.group(1)))
    assert [value * 1000 for value in mhz] == khz

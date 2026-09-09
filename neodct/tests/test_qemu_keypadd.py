"""nd-i2c-keypadd's PCF8575, checked against a second reading of the datasheet.

The emulator's keypad is modelled on the HOST, by neodct/tools/nd-i2c-keypadd,
and the guest's nd_matrix.c talks to it over a real i2c bus. That makes the
model the thing every keypad claim under QEMU rests on: if it is wrong in the
same way the scanner is wrong, both agree and neither is right.

So the model is a SECOND IMPLEMENTATION -- written from the chip's electrical
behaviour rather than from what nd_matrix.c expects to read -- and this file
checks it two ways, which are not equally strong and should not be read as if
they were.

`expected_port()` below is a CROSS-CHECK OF THE TRANSCRIPTION and not a third
reading of the datasheet. It is the same rule, and it is deliberately a
DIFFERENT ALGORITHM from chip_read16()'s union-find: it floods outward from
the pins being driven low, level by level, until nothing new is reached. Two
shapes computing one rule can disagree about the cases where the shapes differ
-- a component that contains no driven pin, a pin driven low with nothing
shorted to it, two driven pins in one component -- and that is what it buys.
What it cannot catch is a shared misreading of the CHIP, because both sides
start from the same sentence.

The independent reading is the assertions written out BY HAND beside it: the
`0xFFFF & ~(1 << ROW_PINS[1]) & ~(1 << COL_PINS[1])` spellings, and the
four-bit popcount in the ghosting test. Those are the ones that would survive
being wrong about the rule, and st7789_replay.py -- which decodes from the
ST7789 command set rather than from the encoder -- is the thing this file is
NOT. Worth knowing before trusting a green bar here further than it goes.

============ THE RULE, IN ONE SENTENCE ============

The PCF8575 is quasi-bidirectional with no direction register and no command
byte. A pin written 1 is released to a weak internal pull-up and reads high; a
pin written 0 is driven hard low. A pressed key is a switch shorting two pins.
Therefore a read returns 0xFFFF with every pin cleared that shares a connected
component with a pin being driven low, where the components are computed over
the graph whose edges are the pressed keys.

Three behaviours are consequences of that rule rather than cases in it, and
all three are tested here because all three are load-bearing somewhere:

  * with NOTHING pressed a read during a scan is 0xFFFF with ONLY the driven
    row bit low. A model that starts at 0xFFFF and clears column bits returns
    a flat 0xFFFF instead, which is a lie about the chip that nothing in the
    scanner would ever catch -- validate_pins() forbids a pin being both a row
    and a column, so nd_matrix.c never looks at that bit;
  * after write16(0xFFFF) every pin reads high EVEN WITH A KEY HELD, which is
    the reason nd_pcf8575_close() writes it: a restart mid-scan must not leave
    a row driven low against a pressed key;
  * GHOSTING. Three keys in an L short a fourth into the same component and
    the fourth reads pressed. spec-hw-input.md and nd_matrix.c both say the
    matrix has no diodes and that "nothing in software compensates for that
    and nothing should start to", so a model that could not ghost would make
    nd_kpsetup_wait_new_pair()'s documented handling of "a phantom third pair
    from a ghosting three-key press" untestable anywhere.

============ AND WHY IT DRIVES THE BINARY RATHER THAN A COPY OF IT ============

`nd-i2c-keypadd --check` reads the same key-command grammar the running daemon
reads and answers `read` with the port word through the same chip_read16().
There is no test-only model: what is exercised below is the code a guest
transfer reaches.
"""

import os
import re
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "neodct", "tools", "nd-i2c-keypadd.c")

# The tree's own ND_ROW_PINS_DEFAULT / ND_COL_PINS_DEFAULT
# (spec-hw-input.md:485-486), read out of the daemon below rather than trusted
# from here -- see test_the_pin_table_is_the_trees_own.
ROW_PINS = [0, 1, 2, 3]
COL_PINS = [4, 5, 6, 7]

# nd_kpsetup_targets[] order, row-major, which is the layout the daemon
# carries and the order the first-boot wizard prompts in.
KEYS = [
    ["navikey", "clear", "up", "down"],
    ["num_1", "num_2", "num_3", "num_4"],
    ["num_5", "num_6", "num_7", "num_8"],
    ["num_9", "num_0", "star", "hash"],
]


@pytest.fixture(scope="module")
def keypadd(tmp_path_factory):
    """The daemon, built with the compiler this host has.

    A skip and not a failure when there is no cc: this suite runs on machines
    with no toolchain, and a red bar there would train people to ignore the
    colour. Everything else in the file is then untested and says so by not
    running, which is the honest shape.
    """
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no C compiler on this host")
    out = str(tmp_path_factory.mktemp("keypadd") / "nd-i2c-keypadd")
    # The project's own warning set, -Werror included. This binary is built by
    # qemu_machine.sh with plain -O2, so without this nothing anywhere would
    # notice it drifting out of CODING-STANDARDS.md section 6.
    proc = subprocess.run(
        [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wshadow", "-Wconversion",
         "-Wstrict-prototypes", "-Wmissing-prototypes", "-Wvla", "-O2", "-o", out, SRC],
        capture_output=True, text=True,
    )
    assert proc.returncode == 0, f"nd-i2c-keypadd does not build clean:\n{proc.stderr}"
    return out


def ports(keypadd, script):
    """Run a key-command script through `--check` and return every port word."""
    proc = subprocess.run(
        [keypadd, "--check"], input="\n".join(script) + "\n",
        capture_output=True, text=True, timeout=30,
    )
    assert proc.returncode == 0, proc.stderr
    return [int(m, 16) for m in re.findall(r"^PORT ([0-9A-F]{4})$", proc.stdout, re.M)]


def expected_port(latch, shorts):
    """The rule again, as a flood rather than as components.

    chip_read16() labels every pin's connected component with union-find and
    then asks whether the label is marked. This starts from the OTHER end: the
    pins the guest is driving low are the sources, and low-ness spreads across
    a short one hop at a time until a pass adds nothing. Same answer, opposite
    shape -- which is the point, because a bug in one of the two is unlikely
    to be a bug in the other. (A copy of the union-find would agree with the C
    even when both were wrong, which is what this used to be.)
    """
    neighbours = [[] for _ in range(16)]
    for a, b in shorts:
        neighbours[a].append(b)
        neighbours[b].append(a)

    low = [not (latch >> pin) & 1 for pin in range(16)]
    spreading = True
    while spreading:
        spreading = False
        for pin in range(16):
            if not low[pin]:
                continue
            for other in neighbours[pin]:
                if not low[other]:
                    low[other] = True
                    spreading = True

    value = 0xFFFF
    for pin in range(16):
        if low[pin]:
            value &= ~(1 << pin) & 0xFFFF
    return value


def key_short(row, col):
    return (ROW_PINS[row], COL_PINS[col])


# ------------------------------------------------------------------ #
# The rule
# ------------------------------------------------------------------ #

def test_an_idle_scan_leaves_only_the_driven_row_low(keypadd):
    """Not a flat 0xFFFF, which is what a lookup-table model returns.

    nd_matrix.c writes `0xFFFF & ~(1 << row_pins[r])` and then reads, so with
    nothing pressed the driven row's own bit is still low -- the pin is being
    held there by the master. The scanner never looks at it, which is exactly
    why a model that got it wrong could not be caught from the guest side.
    """
    script = []
    want = []
    for row in range(4):
        latch = 0xFFFF & ~(1 << ROW_PINS[row])
        script += [f"drive 0x{latch:04X}", "read"]
        want.append(latch)
    assert ports(keypadd, script) == want
    assert want != [0xFFFF] * 4


def test_a_press_only_shows_while_its_own_row_is_driven(keypadd):
    """The whole reason a scan has to walk the rows one at a time."""
    script = ["press num_2"]
    want = []
    for row in range(4):
        latch = 0xFFFF & ~(1 << ROW_PINS[row])
        script += [f"drive 0x{latch:04X}", "read"]
        want.append(expected_port(latch, [key_short(1, 1)]))
    got = ports(keypadd, script)
    assert got == want
    # And spelled out, so a reader does not have to run expected_port() in
    # their head: only the pass that drives row 1 sees column 1 go low.
    assert got[1] == 0xFFFF & ~(1 << ROW_PINS[1]) & ~(1 << COL_PINS[1])
    assert got[0] == 0xFFFF & ~(1 << ROW_PINS[0])


def test_releasing_every_pin_reads_high_even_with_a_key_held(keypadd):
    """nd_pcf8575_close() writes 0xFFFF for this reason.

    Nothing is driving anything, so nothing is pulled down through the switch
    and the pull-ups win. A restart mid-scan therefore cannot leave a row
    driven low against a pressed key.
    """
    assert ports(keypadd, ["press num_2", "drive 0xFFFF", "read"]) == [0xFFFF]


def test_two_keys_on_one_row_both_read_low_in_one_pass(keypadd):
    """Rollover. nd_matrix.c scans the WHOLE matrix every pass and never stops
    at the first hit, and this is the electrical fact that makes that useful.
    """
    latch = 0xFFFF & ~(1 << ROW_PINS[1])
    got = ports(keypadd, ["press num_1", "press num_3", f"drive 0x{latch:04X}", "read"])
    assert got == [expected_port(latch, [key_short(1, 0), key_short(1, 2)])]
    assert got[0] == 0xFFFF & ~(1 << ROW_PINS[1]) & ~(1 << COL_PINS[0]) & ~(1 << COL_PINS[2])


def test_three_keys_in_an_l_ghost_a_fourth(keypadd):
    """The matrix has no diodes and the model must not pretend it does.

    (1,0), (1,1) and (2,0) held. Driving row 2 pulls its own pin low; the
    switch at (2,0) drags column 0 down with it; (1,0) drags ROW 1 down from
    there; and (1,1) then drags column 1 down as well -- so column 1 reads
    pressed on row 2, where no key is held. That is the phantom
    nd_kpsetup_wait_new_pair() documents and nothing anywhere has executed.
    """
    latch = 0xFFFF & ~(1 << ROW_PINS[2])
    got = ports(keypadd, ["press num_1", "press num_2", "press num_5",
                          f"drive 0x{latch:04X}", "read"])[0]
    assert got == expected_port(latch, [key_short(1, 0), key_short(1, 1), key_short(2, 0)])
    # The fourth corner, which is not pressed:
    assert not (got >> COL_PINS[1]) & 1
    # ...and it really is a ghost and not a fourth key: the model was told
    # about three shorts.
    assert bin(0xFFFF ^ got).count("1") == 4


def test_the_whole_matrix_round_trips_through_a_simulated_scan(keypadd):
    """nd_matrix.c's raw_scan(), run here against the model.

    Every one of the sixteen keys, one at a time, found at exactly its own
    position and nowhere else. This is the property the guest boot asserts for
    a single key; doing all sixteen needs no boot and is where a pin-table
    mistake would show up as a transposition rather than as silence.
    """
    for row in range(4):
        for col in range(4):
            script = ["release all", f"press {KEYS[row][col]}"]
            for r in range(4):
                script += [f"drive 0x{0xFFFF & ~(1 << ROW_PINS[r]):04X}", "read"]
            words = ports(keypadd, script)
            found = {
                (r, c)
                for r in range(4)
                for c in range(4)
                if not (words[r] >> COL_PINS[c]) & 1
            }
            assert found == {(row, col)}, f"{KEYS[row][col]} read as {found}"


def test_a_short_between_pins_no_key_joins_is_visible(keypadd):
    """The first-boot wizard's world, which the shipping scanner never sees.

    nd_kpsetup_scan_pairs() drives EACH OF THE SIXTEEN PINS IN TURN and
    records every other pin that came back low, because it cannot know which
    pins are rows -- that is the entire point of it. A model expressed as
    "row -> columns" cannot answer that at all, so `short` exists and this is
    the test that it does.
    """
    latch = 0xFFFF & ~(1 << 9)
    got = ports(keypadd, ["short 9 14", f"drive 0x{latch:04X}", "read"])
    assert got == [expected_port(latch, [(9, 14)])]
    assert got[0] == 0xFFFF & ~(1 << 9) & ~(1 << 14)
    # Driving the OTHER end shows the same pair, which is what makes the
    # wizard's bipartition possible: a short has no direction.
    latch = 0xFFFF & ~(1 << 14)
    assert ports(keypadd, ["short 9 14", f"drive 0x{latch:04X}", "read"]) == [
        0xFFFF & ~(1 << 9) & ~(1 << 14)
    ]


def test_open_undoes_short_and_release_all_undoes_everything(keypadd):
    latch = 0xFFFF & ~(1 << 9)
    assert ports(keypadd, ["short 9 14", "open 9 14", f"drive 0x{latch:04X}", "read"]) == [latch]
    latch = 0xFFFF & ~(1 << ROW_PINS[0])
    assert ports(keypadd, ["press navikey", "press num_1", "release all",
                           f"drive 0x{latch:04X}", "read"]) == [latch]


# ------------------------------------------------------------------ #
# The constants, against the tree they are copied from
# ------------------------------------------------------------------ #

def test_the_pin_table_is_the_trees_own():
    """Copies drift. spec-hw-input.md names rows P00-P03 and columns P04-P07,
    apps/KeypadMapperI2C carries the same as its fallback, and the daemon has
    a third copy -- so this is the join, exactly as test_qemu_dtsi.py is the
    join for the backlight table.
    """
    with open(SRC, encoding="utf-8") as handle:
        text = handle.read()
    rows = re.search(r"ND_ROW_PINS\[ND_ROWS\] = \{([^}]*)\}", text)
    cols = re.search(r"ND_COL_PINS\[ND_COLS\] = \{([^}]*)\}", text)
    assert rows and cols, "the daemon's pin table has moved or been renamed"
    assert [int(v) for v in rows.group(1).split(",")] == ROW_PINS
    assert [int(v) for v in cols.group(1).split(",")] == COL_PINS

    spec = os.path.join(REPO, "docs", "c-rewrite", "spec-hw-input.md")
    with open(spec, encoding="utf-8") as handle:
        spec_text = handle.read()
    assert "#define ND_ROW_PINS_DEFAULT  {0, 1, 2, 3}" in spec_text
    assert "#define ND_COL_PINS_DEFAULT  {4, 5, 6, 7}" in spec_text


def test_the_key_names_are_the_wizards_own_order():
    """nd_kpsetup_targets[] is what the wizard prompts in, and the daemon's
    layout is that list read row-major. A name here that the wizard does not
    know would make a scripted key press unenrollable.
    """
    targets = os.path.join(REPO, "neodct", "src", "lib", "nd_keypadsetup.c")
    with open(targets, encoding="utf-8") as handle:
        block = handle.read()
    block = block[block.index("nd_kpsetup_targets[ND_KPSETUP_N_TARGETS] = {"):]
    block = block[: block.index("};")]
    names = re.findall(r'\{"([a-z0-9_]+)",', block)
    assert names == [name for row in KEYS for name in row]

    with open(SRC, encoding="utf-8") as handle:
        text = handle.read()
    table = text[text.index("ND_KEY_NAMES[ND_ROWS][ND_COLS] = {"):]
    table = table[: table.index("};")]
    assert re.findall(r'"([a-z0-9_]+)"', table) == names


def test_the_addresses_are_the_ones_the_tree_defaults_to():
    """0x20 is ND_I2C_ADDR_DEFAULT and ND_KPSETUP_PROBE_FIRST; 0x36 is the
    MAX17048 nd_battery.c opens. Both are in the daemon as literals, and both
    have to stay the tree's.
    """
    with open(SRC, encoding="utf-8") as handle:
        text = handle.read()
    assert "#define ND_KEYPAD_ADDR 0x20" in text
    assert "#define ND_GAUGE_ADDR  0x36" in text
    keypad_h = os.path.join(REPO, "neodct", "src", "include", "nd_keypad.h")
    with open(keypad_h, encoding="utf-8") as handle:
        assert "#define ND_I2C_ADDR_DEFAULT 0x20" in handle.read()


def test_the_gauge_answers_and_nothing_else_does():
    """The eight-address probe has to find EXACTLY ONE chip.

    nd_kpsetup_probe() walks 0x20..0x27 and takes the FIRST address that
    answers, so a model that ACKed everything would enrol 0x20 by accident and
    call it a keypad -- and one that ACKed nothing would leave the phone
    keyless. This is a source assertion rather than a boot, because the boot
    that proves it is test_qemu_i2c.sh; what it pins is that the NAK is the
    default branch and not a list somebody can extend.
    """
    with open(SRC, encoding="utf-8") as handle:
        text = handle.read()
    body = text[text.index("static uint8_t do_transfer("):]
    body = body[: body.index("\n}\n")]
    assert body.count("VIRTIO_I2C_MSG_ERR") == 1, (
        "the NAK must be the one fall-through at the end of do_transfer(); a "
        "second one means an address list somebody can extend by accident"
    )
    assert body.rstrip().endswith("return VIRTIO_I2C_MSG_ERR;")


# ------------------------------------------------------------------ #
# The lifecycle, out of the shell that owns it
# ------------------------------------------------------------------ #
#
# run_qemu.sh's keypad block cannot be exercised here -- it needs a Buildroot
# image, and a fresh checkout has none -- so what IS checked is the three
# structural properties that were got wrong at least once each on the storage
# stage, and whose failure mode is silence. The shared functions themselves
# ARE booted: test_qemu_i2c.sh, test_qemu_surfaces.sh and
# parity_capture_probe.sh all reach the guest through the same
# nd_keypadd_start() and nd_qemu_i2c_args().

RUN_QEMU = os.path.join(REPO, "neodct", "tools", "run_qemu.sh")
QEMU_MACHINE = os.path.join(REPO, "neodct", "tools", "qemu_machine.sh")


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def test_the_daemon_starts_before_qemu_and_not_after():
    """QEMU REFUSES TO START against a vhost-user socket that is not there --
    measured, `Failed to connect to '...': No such file or directory`. So the
    daemon is not a garnish that can be attached late; if the start moved
    below the qemu line the emulator would simply stop booting.
    """
    body = read(RUN_QEMU)
    start = body.index("nd_keypadd_start")
    launch = body.index("qemu-system-arm \"$@\" -append")
    assert start < launch, (
        "nd_keypadd_start must run before QEMU is launched; QEMU refuses to "
        "start when the socket is absent"
    )


def test_the_daemon_is_reaped_on_the_exit_trap():
    """Not after the QEMU line, which is the difference between a cleanup that
    happens and one that happens on the happy path only.

    Measured on the storage stage, twice: under `set -e` a non-zero QEMU exit
    terminates the script AT that line, and a `kill` of the script orphans
    QEMU and skips everything after it. A daemon left behind holds the socket,
    and the next boot's QEMU connects to a backend nobody is typing on.
    """
    body = read(RUN_QEMU)
    session_end = body.index("nd_session_end() {")
    trap = body.index("trap 'nd_session_end' EXIT")
    kill = body.index('kill "$KEYPADD_PID"')
    assert session_end < kill < trap, (
        "the daemon must be killed inside nd_session_end(), which is what the "
        "EXIT trap runs"
    )


def test_exec_is_refused_while_a_daemon_needs_reaping():
    """An exec'd shell has no EXIT trap at all.

    run_qemu.sh execs on the one mode with nothing to save and nothing to
    clean up. A keypad daemon is a third thing to clean up, so it has to be in
    that condition -- and the condition is the load-bearing part, not the
    comment above it.
    """
    body = read(RUN_QEMU)
    line = [ln for ln in body.splitlines() if "exec qemu-system-arm" in ln]
    assert len(line) == 1
    guard = body[: body.index(line[0])].rsplit("\nif ", 1)[-1]
    assert '-z "$KEYPADD_PID"' in guard, (
        "the exec branch must not be taken while a keypad daemon is running: "
        f"the guard is `{guard.strip()}`"
    )


def test_the_shared_recipe_is_where_the_three_callers_can_reach_it():
    """qemu_machine.sh exists because three recipes that hand-copy a machine
    drift, and the drift is invisible -- that already happened once, with the
    nandsim ID bytes. The keypad's QEMU arguments go in the same file for the
    same reason, and every caller has to be reading them from there.
    """
    machine = read(QEMU_MACHINE)
    assert "nd_qemu_i2c_args()" in machine
    assert "nd_keypadd_start()" in machine
    assert "memory-backend-memfd" in machine, (
        "vhost-user needs shareable guest memory; QEMU's default anonymous RAM "
        "cannot be mapped by the backend and every transfer silently does nothing"
    )
    for caller in ("run_qemu.sh", "test_qemu_surfaces.sh", "test_qemu_i2c.sh",
                   "parity_capture_probe.sh"):
        text = read(os.path.join(REPO, "neodct", "tools", caller))
        assert "nd_qemu_i2c_args" in text, f"{caller} does not use the shared recipe"
        # Comments are stripped first: three of these files EXPLAIN the memfd
        # backend and why it is not optional, and a test that could not tell
        # an explanation from a second copy would push people to stop writing
        # the explanation.
        code = "\n".join(
            line for line in text.splitlines() if not line.lstrip().startswith("#")
        )
        assert "memory-backend-memfd" not in code, (
            f"{caller} has its own copy of the machine arguments"
        )

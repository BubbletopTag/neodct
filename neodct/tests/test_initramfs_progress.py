"""The install-progress bar, driven through the real applier.

`apply_pending` now draws on the panel while it installs, because for the
whole of a ~51 MB write the screen showed one unchanging picture -- which is
pixel-for-pixel what a hung phone looks like. This file pins the two halves of
that:

  * THE NUMBERS ARE REAL. Three phases in order, each given the byte count it
    is actually about to move, and the bytes that cross the counting stage are
    the bytes the phase moved. Nothing here is sampled, estimated or faked, and
    a bar that lied would be worse than no bar because it would be a specific
    lie.

  * IT CANNOT BREAK AN INSTALL. With nd-bootbar unset, missing, not
    executable, or exiting non-zero, the update still installs and
    installed.prop is byte-identical. That is the regression that matters:
    a cosmetic feature must never be able to stop an operating system from
    being installed.

The stub stands in for nd-bootbar the way verifier_shim() stands in for
nd-verify: same argv, and `exec cat` for the filter, so the pipeline keeps its
shape. neodct/src/test/unit/test_bootbar.c is what pins the real binary.
"""

import os
import shutil
import subprocess

import pytest

from test_initramfs_apply import (APPLY_SH, gate_env, read_prop, stage_a_package,
                                  stage_an_update)

PANEL_SH = os.path.join(os.path.dirname(APPLY_SH), "ndsys-panel.sh")

BOOTBAR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "src", "build", "default", "bin", "nd-bootbar")


def bootbar_stub(tmp_path, log, exit_code=0, executable=True):
    """A stand-in for nd-bootbar: appends its argv to `log`, then acts.

    The filter mode has to `exec cat`, because it sits in the middle of the
    pipeline that carries the system image to the flash. A stub that did not
    would silently truncate every install in this file.
    """
    path = tmp_path / "nd-bootbar-stub"
    # One argv word per line and a sentinel between invocations: "$*" would
    # join them with spaces and "Update not installed" would come back as
    # three arguments.
    path.write_text(
        "#!/bin/sh\n"
        "{ printf '%s\\n' \"$@\"; echo '<<<end>>>'; } >> '" + str(log) + "'\n"
        "for a in \"$@\"; do\n"
        "    [ \"$a\" = --at ] && exit " + str(exit_code) + "\n"
        "    [ \"$a\" = --fail ] && exit " + str(exit_code) + "\n"
        "done\n"
        "exec cat\n"
    )
    path.chmod(0o755 if executable else 0o644)
    return path


def run_panel(state, sys_dev, tmp_path, card=None, bootbar=None, command="apply_pending",
              panel_up="1"):
    """apply_pending with ndsys-panel.sh sourced, as init sources it.

    PANEL_UP is what panel_start() sets once /dev/fb0 is worth drawing on;
    setting it here is the host stand-in for a panel having come up.
    """
    user = tmp_path / "user"
    (user / "logs").mkdir(parents=True, exist_ok=True)
    lines = [
        'STATE_DIR="%s"; MNT_USER="%s"; SYS_DEV="%s"; USER_MOUNTED=1' % (state, user, sys_dev),
    ]
    if card is not None:
        lines.append('MNT_SDCARD="%s"; NDSYS_CARD_PREMOUNTED=1' % card)
    if bootbar is not None:
        lines.append('NDSYS_BOOTBAR="%s"' % bootbar)
    lines.append(gate_env(tmp_path).rstrip("\n"))
    lines.append('. "%s"' % PANEL_SH)
    # PANEL_FB somewhere that is certainly not a character device, so that a
    # build host which happens to HAVE a /dev/fb0 cannot make panel_start()
    # succeed and set PANEL_UP behind the test's back.
    lines.append('PANEL_FB="%s"' % (tmp_path / "there-is-no-framebuffer"))
    lines.append('PANEL_UP="%s"' % panel_up)
    # PANEL_SPLASH_HOLD is what progress_fail sleeps for. Two seconds a
    # refusal would make this file take a minute; the hold itself is the
    # shell's, and the frame is drawn before it.
    lines.append('PANEL_SPLASH_HOLD=0')
    lines.append('. "%s"' % APPLY_SH)
    lines.append(command)
    return subprocess.run(["sh", "-c", "\n".join(lines) + "\n"],
                          capture_output=True, text=True)


def calls(log):
    """Every invocation's argv, in order."""
    if not os.path.exists(log):
        return []
    out = []
    argv = []
    for line in open(log).read().splitlines():
        if line == "<<<end>>>":
            out.append(argv)
            argv = []
        else:
            argv.append(line)
    return out


def flag(argv, name):
    """The value after --name, or None."""
    for i, word in enumerate(argv):
        if word == name and i + 1 < len(argv):
            return argv[i + 1]
    return None


def phases(log):
    """(step, phase, total) for every filter invocation, in order."""
    out = []
    for argv in calls(log):
        if "--at" in argv or "--fail" in argv:
            continue
        out.append((flag(argv, "--step"), flag(argv, "--phase"), flag(argv, "--total")))
    return out


# --- the three phases -----------------------------------------------------

def test_the_three_phases_are_drawn_in_order(tmp_path):
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    log = tmp_path / "bar.log"

    result = run_panel(state, device, tmp_path, bootbar=bootbar_stub(tmp_path, log))

    assert result.returncode == 0, result.stderr
    assert device.read_bytes()[:len(image)] == image
    assert [(step, phase) for step, phase, _ in phases(log)] == [
        ("Checking the update", "1"),
        ("Installing", "2"),
        ("Checking the phone", "3"),
    ]


def test_the_first_frame_is_drawn_before_the_signature_is_checked(tmp_path):
    """Something has to appear within a fraction of a second of the update
    starting, not after the first megabyte. The 0% frame is what does it, and
    it is drawn before the signature check, the size check and the hash."""
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    log = tmp_path / "bar.log"

    run_panel(state, device, tmp_path, bootbar=bootbar_stub(tmp_path, log))

    first = calls(log)[0]
    assert flag(first, "--at") == "0"
    assert flag(first, "--step") == "Checking the update"
    assert flag(first, "--phase") == "1"


def test_the_bar_is_full_before_the_sync(tmp_path):
    """A 51 MB sync with the bar sitting at 99% would look like the hang this
    whole thing exists to remove."""
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    log = tmp_path / "bar.log"

    run_panel(state, device, tmp_path, bootbar=bootbar_stub(tmp_path, log))

    argv = [c for c in calls(log) if flag(c, "--at") == "100"]
    assert len(argv) == 1, calls(log)
    assert flag(argv[0], "--step") == "Installing"
    # And it happens after the write and before the read-back, which is the
    # only place in the sequence where "the write is done" is true.
    order = [c for c in calls(log) if "--at" in c or "--step" in c]
    at_100 = order.index(argv[0])
    phase_3 = [i for i, c in enumerate(order) if flag(c, "--phase") == "3"][0]
    assert at_100 < phase_3


def test_phases_one_and_two_divide_by_the_image_size(tmp_path):
    """image_bytes out of pending.prop, which by then has been proved to
    agree with a manifest signed by the release key. Not a guess."""
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    log = tmp_path / "bar.log"

    run_panel(state, device, tmp_path, bootbar=bootbar_stub(tmp_path, log))

    by_phase = {phase: total for _, phase, total in phases(log)}
    assert by_phase["2"] == str(len(image))


def test_the_read_back_divides_by_what_dd_will_actually_emit(tmp_path):
    """hash_prefix computes blocks = bytes / 4096 and dd emits blocks * 4096.

    Passing image_bytes instead would leave the last bar topping out at 99%
    on any image that is not 4096-aligned -- and the fixture image is not.
    """
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    log = tmp_path / "bar.log"

    run_panel(state, device, tmp_path, bootbar=bootbar_stub(tmp_path, log))

    by_phase = {phase: total for _, phase, total in phases(log)}
    assert by_phase["3"] == str((len(image) // 4096) * 4096)


def test_the_loose_image_pre_write_hash_is_counted_too(tmp_path):
    """The staged-image path hashes through hash_prefix rather than through
    unzip, and it gets a bar for the same reason."""
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    log = tmp_path / "bar.log"

    run_panel(state, device, tmp_path, bootbar=bootbar_stub(tmp_path, log))

    by_phase = {phase: total for _, phase, total in phases(log)}
    assert by_phase["1"] == str((len(image) // 4096) * 4096)


def test_a_package_on_a_card_is_counted_through_unzip(tmp_path):
    """The phone's normal path: straight out of the .ndsw on the card, with
    no copy anywhere. Phase 1 then measures the zip member, so its total is
    image_bytes rather than a 4096-aligned prefix."""
    state, card, image, _ = stage_a_package(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * (len(image) + 8192))
    log = tmp_path / "bar.log"

    result = run_panel(state, device, tmp_path, card=card,
                       bootbar=bootbar_stub(tmp_path, log))

    assert result.returncode == 0, result.stderr
    assert device.read_bytes()[:len(image)] == image
    by_phase = {phase: total for _, phase, total in phases(log)}
    assert by_phase["1"] == str(len(image))
    assert by_phase["2"] == str(len(image))


def test_the_bytes_that_cross_the_counter_are_the_bytes_that_are_written(tmp_path):
    """The count is exact rather than sampled: the stage sits IN the pipeline,
    so what it sees is what reaches the flash. A stub that counts and passes
    through proves the shape of that, and the device proves the content."""
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    counted = tmp_path / "counted"
    stub = tmp_path / "counting-stub"
    stub.write_text(
        "#!/bin/sh\n"
        "for a in \"$@\"; do [ \"$a\" = --at ] && exit 0; "
        "[ \"$a\" = --fail ] && exit 0; done\n"
        "exec tee -a '" + str(counted / "stream") + "'\n"
    )
    stub.chmod(0o755)
    counted.mkdir()

    run_panel(state, device, tmp_path, bootbar=stub)

    # Three phases, each streaming the image once: phase 1 and 3 over the
    # 4096-aligned prefix, phase 2 over the whole thing.
    aligned = (len(image) // 4096) * 4096
    assert (counted / "stream").stat().st_size == aligned + len(image) + aligned


# --- it cannot break an install -------------------------------------------
#
# This is the regression that matters. Everything above is decoration; these
# are the tests that say a progress bar cannot cost somebody their phone.

def _install_without_progress(state, tmp_path, image):
    """The applier sourced ON ITS OWN, exactly as test_initramfs_apply.py
    sources it: no ndsys-panel.sh, so progress_filter is the `exec cat`
    no-op. This is the behaviour that must not have changed."""
    device = tmp_path / "reference.img"
    device.write_bytes(b"\0" * len(image))
    (tmp_path / "user" / "logs").mkdir(parents=True, exist_ok=True)
    script = (
        'STATE_DIR="%s"; MNT_USER="%s"; SYS_DEV="%s"; USER_MOUNTED=1\n'
        '%s'
        '. "%s"\napply_pending\n' % (state, tmp_path / "user", device,
                                       gate_env(tmp_path), APPLY_SH)
    )
    result = subprocess.run(["sh", "-c", script], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return (state / "installed.prop").read_bytes(), device


@pytest.mark.parametrize("how", ["unset", "missing", "not-executable", "fails"])
def test_the_install_is_identical_however_broken_the_progress_bar_is(tmp_path, how):
    """Byte-for-byte, against the same staged update installed the old way.

    The same update, not merely an equivalent one: format_hash_area() puts a
    fresh uuid4() in the verity superblock, so two calls to build_image()
    produce two different images and comparing across them would prove
    nothing.
    """
    state, image, _ = stage_an_update(tmp_path)
    reference_state = tmp_path / "reference" / ".ndsys"
    shutil.copytree(state, reference_state)

    reference, _ = _install_without_progress(reference_state, tmp_path, image)

    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    log = tmp_path / "bar.log"

    if how == "unset":
        bootbar = None
    elif how == "missing":
        bootbar = tmp_path / "there-is-no-nd-bootbar"
    elif how == "not-executable":
        bootbar = bootbar_stub(tmp_path, log, executable=False)
    else:
        bootbar = bootbar_stub(tmp_path, log, exit_code=3)

    result = run_panel(state, device, tmp_path, bootbar=bootbar)

    assert result.returncode == 0, result.stderr
    assert device.read_bytes()[:len(image)] == image
    assert (state / "installed.prop").read_bytes() == reference


def test_a_progress_bar_that_cannot_be_reached_leaves_no_trace_in_the_record(tmp_path):
    """`unset` is the case a phone with an older initramfs is actually in:
    ndsys-panel.sh present, nd-bootbar absent. progress_filter falls through
    to `exec cat`, which changes neither the pipeline's shape nor its exit
    status."""
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))

    result = run_panel(state, device, tmp_path,
                       bootbar=tmp_path / "definitely-not-here")

    assert result.returncode == 0, result.stderr
    assert read_prop(state / "last_result.prop", "result") == "ok"


def test_nothing_is_drawn_when_the_panel_never_came_up(tmp_path):
    """PANEL_UP empty is QEMU with no framebuffer and a phone whose daemon
    exited. progress_filter must not even exec the tool: `[ -n "$PANEL_UP" ]`
    is the first thing it looks at."""
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    log = tmp_path / "bar.log"

    result = run_panel(state, device, tmp_path, bootbar=bootbar_stub(tmp_path, log),
                       panel_up="")

    assert result.returncode == 0, result.stderr
    assert device.read_bytes()[:len(image)] == image
    assert calls(log) == []


# --- the failure screens --------------------------------------------------
#
# The single most important thing this feature has to fix, and it is not the
# bar. Every refusal in apply_pending logs to /dev/console -- a serial cable
# the owner does not have -- and then boots the old system, so an unsigned
# package is refused silently from the owner's point of view: they install an
# update, the phone restarts, and nothing has changed.

def fails(log):
    return [(flag(c, "--fail"), flag(c, "--reason")) for c in calls(log) if "--fail" in c]


def test_an_unsigned_package_says_so_on_the_screen(tmp_path):
    state, image, _ = stage_an_update(tmp_path, signature=b"not a signature")
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    log = tmp_path / "bar.log"

    result = run_panel(state, device, tmp_path, bootbar=bootbar_stub(tmp_path, log))

    assert result.returncode == 0, result.stderr
    assert device.read_bytes() == b"\0" * len(image), "an unsigned image reached the flash"
    assert fails(log) == [("Update refused", "Not signed by NeoDCT")]


def test_a_damaged_image_says_so_on_the_screen(tmp_path):
    state, image, _ = stage_an_update(tmp_path)
    (state / "pending.img").write_bytes(b"\xff" * len(image))
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    log = tmp_path / "bar.log"

    run_panel(state, device, tmp_path, bootbar=bootbar_stub(tmp_path, log))

    assert fails(log) == [("Update not installed", "The update is damaged")]


def test_a_truncated_image_says_so_on_the_screen(tmp_path):
    state, image, _ = stage_an_update(tmp_path)
    (state / "pending.img").write_bytes(image[:-1024])
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    log = tmp_path / "bar.log"

    run_panel(state, device, tmp_path, bootbar=bootbar_stub(tmp_path, log))

    assert fails(log) == [("Update not installed", "The update is damaged")]


def test_an_incomplete_record_says_so_on_the_screen(tmp_path):
    state, image, _ = stage_an_update(tmp_path)
    text = (state / "pending.prop").read_text()
    (state / "pending.prop").write_text(
        "\n".join(l for l in text.splitlines() if not l.startswith("sha256=")) + "\n")
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    log = tmp_path / "bar.log"

    run_panel(state, device, tmp_path, bootbar=bootbar_stub(tmp_path, log))

    assert fails(log) == [("Update not installed", "The update was incomplete")]


def test_a_write_that_does_not_land_says_it_will_try_again(tmp_path):
    """write_system is overridden rather than fed a read-only device: these
    tests run as root, where chmod 0444 does not stop dd, and SYS_DEV=/dev/null
    -- what test_initramfs_apply.py uses -- succeeds at the write and fails at
    the read-back instead. The branch under test here is the other one."""
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    log = tmp_path / "bar.log"

    result = run_panel(state, device, tmp_path, bootbar=bootbar_stub(tmp_path, log),
                       command='write_system() { cat > /dev/null; return 1; }\napply_pending')

    assert result.returncode == 0, result.stderr
    assert fails(log) == [("Install did not finish", "It will try again")]
    # Kept, not discarded: a failed write is retried on the next boot, which
    # is the whole reason the staged copy is not deleted first.
    assert (state / "pending.prop").exists()


def test_a_read_back_mismatch_says_it_will_try_again(tmp_path):
    """The device is a FIFO-free stand-in for flash that accepted the write
    and gave something else back: write to one file, read from another."""
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    log = tmp_path / "bar.log"

    # Overwrite the device between the write and the read-back by making
    # write_system a no-op, which leaves the old contents in place.
    result = run_panel(state, device, tmp_path, bootbar=bootbar_stub(tmp_path, log),
                       command='write_system() { cat > /dev/null; }\napply_pending')

    assert result.returncode == 0, result.stderr
    assert fails(log) == [("Install did not finish", "It will try again")]
    # And the update is kept, because the record says it can be tried again.
    assert (state / "pending.prop").exists()


def test_giving_up_after_three_attempts_says_so(tmp_path):
    """Not in spec-boot-progress.md's table and added anyway: the three boots
    that got here each ended on "It will try again", and this is the one where
    that stops being true."""
    state, image, _ = stage_an_update(tmp_path)
    text = (state / "pending.prop").read_text()
    (state / "pending.prop").write_text(text.replace("attempts=0", "attempts=3"))
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    log = tmp_path / "bar.log"

    run_panel(state, device, tmp_path, bootbar=bootbar_stub(tmp_path, log))

    assert fails(log) == [("Update not installed", "It has been given up on")]


def test_a_successful_install_holds_nothing(tmp_path):
    """The 100% frame stays on the panel by itself until the UI draws over
    it, so a successful update boot gets no slower. Nothing calls --fail, and
    so nothing sleeps."""
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    log = tmp_path / "bar.log"

    run_panel(state, device, tmp_path, bootbar=bootbar_stub(tmp_path, log))

    assert fails(log) == []


# --- the real binary ------------------------------------------------------

@pytest.mark.skipif(not os.path.exists(BOOTBAR),
                    reason="the C build is not present (make -C neodct/src)")
def test_the_real_nd_bootbar_copies_its_stdin_with_no_framebuffer(tmp_path):
    """The property the whole design rests on, checked on the shipped binary
    rather than on a stub: no /dev/fb0 here, and the stream still comes out
    byte for byte with a zero exit status."""
    payload = os.urandom(300000)

    result = subprocess.run(
        [BOOTBAR, "--fb", str(tmp_path / "no-such-framebuffer"),
         "--step", "Installing", "--phase", "2", "--total", str(len(payload))],
        input=payload, capture_output=True)

    assert result.returncode == 0, result.stderr
    assert result.stdout == payload


@pytest.mark.skipif(not os.path.exists(BOOTBAR),
                    reason="the C build is not present (make -C neodct/src)")
def test_the_real_nd_bootbar_drives_a_whole_install(tmp_path):
    """No stub anywhere: the applier, the real binary, and an ordinary file
    standing in for /dev/fb0 (--geom, because a regular file has no
    FBIOGET_VSCREENINFO to answer). The install still has to be exact."""
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * len(image))
    panel = tmp_path / "fb0.raw"
    panel.write_bytes(b"\0" * (240 * 175 * 4))

    wrapper = tmp_path / "bootbar-onto-a-file"
    wrapper.write_text("#!/bin/sh\nexec '%s' --fb '%s' --geom 240x175x32 \"$@\"\n"
                       % (BOOTBAR, panel))
    wrapper.chmod(0o755)

    result = run_panel(state, device, tmp_path, bootbar=wrapper)

    assert result.returncode == 0, result.stderr
    assert device.read_bytes()[:len(image)] == image
    assert read_prop(state / "last_result.prop", "result") == "ok"
    # Something was drawn: the frame is not still all zeroes.
    assert set(panel.read_bytes()) != {0}

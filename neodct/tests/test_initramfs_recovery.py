"""Recovery mode's install path, exercised on the host.

recovery_install_package is the one part of recovery that can destroy a
system, so it lives apart from the menus and is driven here with real .ndsw
files and a regular file standing in for the system device.

Recovery checks the release signature but, unlike the automatic applier, does
not refuse over it: its whole premise is a person standing in front of a phone
that will not boot, and an owner whose only image is an unsigned development
build still has to be able to get it running. What it does instead is ask a
different question, and there is a test for each. What these tests otherwise
pin down is that it never writes anything it has not hashed first, and that it
records what the next boot needs.
"""

import hashlib
import json
import os
import pty
import select
import shutil
import subprocess
import time
import zipfile

import pytest

from update_fixtures import build_image, make_ndsw, write_public_key

INITRAMFS = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "initramfs")
APPLY_SH = os.path.join(INITRAMFS, "ndsys-apply.sh")
RECOVERY_SH = os.path.join(INITRAMFS, "ndsys-recovery.sh")


def script_for(tmp_path, command):
    """Source both helper scripts, then run one command."""
    state = tmp_path / "state"
    return (
        'STATE_DIR="%s"; MNT_USER="%s"; USER_MOUNTED=1\n'
        '. "%s"\n. "%s"\n%s\n' % (state, tmp_path / "user", APPLY_SH,
                                  RECOVERY_SH, command)
    )


def run(tmp_path, command, env=None):
    """Source both helper scripts and run one command."""
    return subprocess.run(["sh", "-c", script_for(tmp_path, command)],
                          capture_output=True, text=True, env=env)


def make_package(tmp_path, name="UPDATE.ndsw", **kwargs):
    image, tree = build_image(blocks=8)
    body = make_ndsw(tmp_path / name, image=image, tree=tree, **kwargs)
    return tmp_path / name, image, body


def read_prop(path, key):
    for line in open(path):
        if line.startswith(key + "="):
            return line.split("=", 1)[1].strip()
    return None


def test_installs_a_good_package_onto_the_device(tmp_path):
    package, image, _ = make_package(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * (len(image) + 4096))

    result = run(tmp_path, 'recovery_install_package "%s" "%s"'
                 % (package, device))

    assert result.returncode == 0, result.stderr
    assert device.read_bytes()[:len(image)] == image


def test_records_what_the_next_boot_needs_to_verify(tmp_path):
    package, image, body = make_package(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * len(image))

    run(tmp_path, 'recovery_install_package "%s" "%s"' % (package, device))
    installed = tmp_path / "state" / "installed.prop"

    assert read_prop(installed, "version") == body["version"]
    assert read_prop(installed, "verity_root_hash") == body["verity"]["root_hash"]
    assert read_prop(installed, "verity_image_blocks") == \
        str(body["verity"]["image_blocks"])
    assert read_prop(installed, "image_bytes") == str(len(image))
    assert read_prop(installed, "sha256") == body["sha256"]


def test_refuses_a_corrupt_image_without_touching_the_device(tmp_path):
    """The hash is checked straight out of the zip, before any write."""
    package, image, body = make_package(tmp_path)
    tampered = bytearray(image)
    tampered[5000] ^= 0xFF
    with zipfile.ZipFile(package, "r") as original:
        manifest = original.read("manifest.json")
    with zipfile.ZipFile(package, "w") as rewritten:
        rewritten.writestr("rootfs.squashfs", bytes(tampered))
        rewritten.writestr("manifest.json", manifest)
    device = tmp_path / "system.img"
    device.write_bytes(b"ORIGINAL SYSTEM")

    result = run(tmp_path, 'recovery_install_package "%s" "%s"'
                 % (package, device))

    assert result.returncode != 0
    assert device.read_bytes() == b"ORIGINAL SYSTEM"
    assert not (tmp_path / "state" / "installed.prop").exists()


def test_refuses_a_package_with_no_manifest(tmp_path):
    package, image, _ = make_package(tmp_path,
                                     members=("rootfs.squashfs",))
    device = tmp_path / "system.img"
    device.write_bytes(b"ORIGINAL")

    result = run(tmp_path, 'recovery_install_package "%s" "%s"'
                 % (package, device))

    assert result.returncode != 0
    assert device.read_bytes() == b"ORIGINAL"


def test_refuses_a_package_that_is_not_a_zip(tmp_path):
    package = tmp_path / "UPDATE.ndsw"
    package.write_bytes(b"not a zip")
    device = tmp_path / "system.img"
    device.write_bytes(b"ORIGINAL")

    result = run(tmp_path, 'recovery_install_package "%s" "%s"'
                 % (package, device))

    assert result.returncode != 0
    assert device.read_bytes() == b"ORIGINAL"


def test_an_unsigned_package_is_installable_in_recovery(tmp_path):
    """Recovery is the last resort, so it takes what it can verify the
    integrity of. Physical access is the only gate."""
    package, image, _ = make_package(
        tmp_path, members=("rootfs.squashfs", "manifest.json"))
    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * len(image))

    result = run(tmp_path, 'recovery_install_package "%s" "%s"'
                 % (package, device))

    assert result.returncode == 0
    assert device.read_bytes()[:len(image)] == image


def test_a_write_that_does_not_land_is_reported_and_records_nothing(tmp_path):
    """/dev/null accepts the bytes but cannot be fsynced, so this fails at
    the write step rather than the read-back -- either way nothing may be
    recorded as installed."""
    package, image, _ = make_package(tmp_path)

    result = run(tmp_path, 'recovery_install_package "%s" /dev/null' % package)

    assert result.returncode != 0
    assert "failed" in result.stderr or "mismatch" in result.stderr
    assert not (tmp_path / "state" / "installed.prop").exists()


def test_the_pending_state_is_cleared_after_a_recovery_install(tmp_path):
    """Otherwise the applier would try to install the old staged image on
    top of the one recovery just wrote."""
    package, image, _ = make_package(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * len(image))
    state = tmp_path / "state"
    state.mkdir()
    (state / "pending.prop").write_text("version=old\n")
    (state / "pending.img").write_bytes(b"old image")

    run(tmp_path, 'recovery_install_package "%s" "%s"' % (package, device))

    assert not (state / "pending.prop").exists()
    assert not (state / "pending.img").exists()


def test_manifest_fields_are_read_out_of_real_mkupdate_output(tmp_path):
    """The extractor is line-oriented sed, so it has to match the exact
    formatting json.dumps(indent=2) produces."""
    package, _, body = make_package(tmp_path)
    manifest = tmp_path / "manifest.json"
    manifest.write_bytes(subprocess.run(
        ["unzip", "-p", str(package), "manifest.json"],
        capture_output=True, check=True).stdout)

    for field, expected in (("sha256", body["sha256"]),
                            ("version", body["version"]),
                            ("platform", body["platform"]),
                            ("root_hash", body["verity"]["root_hash"]),
                            ("image_blocks", str(body["verity"]["image_blocks"])),
                            ("block_size", str(body["verity"]["block_size"]))):
        result = run(tmp_path, 'cat "%s" | recovery_manifest_field %s'
                     % (manifest, field))
        assert result.stdout.strip() == str(expected), field


def test_the_member_size_comes_from_the_zip_listing(tmp_path):
    package, image, _ = make_package(tmp_path)

    result = run(tmp_path, 'recovery_member_size "%s" rootfs.squashfs' % package)

    assert result.stdout.strip() == str(len(image))


# --- entering recovery ---------------------------------------------------

def test_a_flag_file_requests_recovery(tmp_path):
    """The trigger that works without editing the kernel cmdline, which is
    the only practical route on real hardware."""
    state = tmp_path / "state"
    state.mkdir()
    (state / "boot_recovery").touch()

    result = run(tmp_path, "recovery_requested")

    assert result.returncode == 0


def test_the_flag_is_consumed_so_recovery_does_not_loop(tmp_path):
    """Rebooting out of recovery must land in the normal system."""
    state = tmp_path / "state"
    state.mkdir()
    (state / "boot_recovery").touch()

    run(tmp_path, "recovery_requested")

    assert not (state / "boot_recovery").exists()
    assert run(tmp_path, "recovery_requested").returncode != 0


def test_no_flag_means_a_normal_boot(tmp_path):
    (tmp_path / "state").mkdir()

    assert run(tmp_path, "recovery_requested").returncode != 0


def test_recovery_draws_on_the_framebuffer_console_by_default(tmp_path):
    """tty1 is the phone's own screen; /dev/console is the serial port."""
    result = run(tmp_path, "recovery_tty")

    assert result.stdout.strip() in ("/dev/tty1", "/dev/console")


def test_the_tty_can_be_forced_from_the_cmdline(tmp_path):
    """Insurance: if keys do not reach the VT, drive it over serial instead."""
    result = run(tmp_path, 'RECOVERY_TTY_OVERRIDE=/dev/console; recovery_tty')

    assert result.stdout.strip() == "/dev/console"


# --- menu navigation -----------------------------------------------------
# Arrow keys need the tty in raw mode. If that fails anywhere in the chain
# (typing into the other window, a terminal that line-buffers, no VT) the
# escape sequence is echoed as ^[[A and nothing moves. Digits have to work
# too: typing "3" then Enter behaves the same either way.

def menu(tmp_path, keys, items=("alpha", "beta", "gamma")):
    """Drive recovery_menu with a scripted key sequence."""
    keyfile = tmp_path / "keys"
    keyfile.write_text("".join(k + "\n" for k in keys))
    stub = ('read_key() { sed -n 1p "%s"; sed -i 1d "%s"; }\n'
            'TTY=/dev/null\n'
            'screen_clear() { :; }\n'
            'say() { :; }\n' % (keyfile, keyfile))
    return run(tmp_path, '%srecovery_menu "" %s'
               % (stub, " ".join('"%s"' % i for i in items)))


def test_enter_picks_the_first_item(tmp_path):
    assert menu(tmp_path, ["ENTER"]).stdout.strip() == "1"


def test_down_then_enter_picks_the_second(tmp_path):
    assert menu(tmp_path, ["DOWN", "ENTER"]).stdout.strip() == "2"


def test_up_wraps_to_the_last_item(tmp_path):
    assert menu(tmp_path, ["UP", "ENTER"]).stdout.strip() == "3"


def test_down_wraps_to_the_first_item(tmp_path):
    assert menu(tmp_path, ["DOWN", "DOWN", "DOWN", "ENTER"]).stdout.strip() == "1"


def test_a_digit_moves_the_selection(tmp_path):
    """Typing 3 then Enter works in a line-buffered terminal, where arrow
    keys cannot."""
    assert menu(tmp_path, ["3", "ENTER"]).stdout.strip() == "3"


def test_a_digit_beyond_the_menu_is_ignored(tmp_path):
    assert menu(tmp_path, ["9", "ENTER"]).stdout.strip() == "1"


def test_zero_is_ignored_rather_than_selecting_nothing(tmp_path):
    assert menu(tmp_path, ["0", "ENTER"]).stdout.strip() == "1"


def test_a_digit_can_be_followed_by_more_movement(tmp_path):
    assert menu(tmp_path, ["2", "DOWN", "ENTER"]).stdout.strip() == "3"


def test_the_tty_is_the_serial_console_when_there_is_no_screen(tmp_path):
    """Headless -- no VT -- has to keep working, and is what
    neodct.rectty=/dev/console selects deliberately."""
    result = run(tmp_path, 'RECOVERY_TTY_OVERRIDE=/dev/console; recovery_tty')

    assert result.stdout.strip() == "/dev/console"


# --- where recovery draws -------------------------------------------------
# Input is taken from every tty that might carry it, but the UI belongs on
# one: the phone's screen. Drawing it on the serial console as well means
# QEMU paints every frame twice, once in the display window and once in the
# terminal the emulator was started from.

def drawn(tmp_path, command):
    """Run a drawing command with a display tty and a serial tty, and report
    what each of them received."""
    display = tmp_path / "display"
    serial = tmp_path / "serial"
    run(tmp_path, 'TTY="%s"; TTYS="%s %s"\n%s' % (display, display, serial,
                                                  command))
    return (display.read_bytes() if display.exists() else b"",
            serial.read_bytes() if serial.exists() else b"")


def test_the_menu_is_drawn_on_the_display(tmp_path):
    display, _ = drawn(tmp_path, 'say "update system"')

    assert display == b"update system\r\n"


def test_the_menu_is_not_drawn_on_the_serial_console_as_well(tmp_path):
    _, serial = drawn(tmp_path, 'say "update system"')

    assert serial == b""


def test_clearing_the_screen_does_not_clear_the_serial_console(tmp_path):
    _, serial = drawn(tmp_path, "screen_clear")

    assert serial == b""


# --- byte-stream input ----------------------------------------------------
# read_key consumes bytes from fd 8. In the image that fd is a FIFO fed by
# one pump per tty (the user may be typing into the QEMU display window,
# tty1, or the serial terminal, /dev/console -- reading only one makes the
# other appear dead). Here a regular file stands in, which also proves the
# fd is opened once and read sequentially: reopening per call would restart
# at byte 0 and parse the same key forever.

def parse_stream(tmp_path, data, count):
    stream = tmp_path / "stream.bin"
    stream.write_bytes(data)
    script = 'exec 8< "%s"\n' % stream + "read_key\n" * count
    return run(tmp_path, script).stdout.split()


def test_arrow_sequences_parse_from_a_byte_stream(tmp_path):
    assert parse_stream(tmp_path, b"\x1b[A\x1b[B", 2) == ["UP", "DOWN"]


def test_application_mode_arrows_parse_too(tmp_path):
    """Some terminal emulators send ESC O A rather than ESC [ A."""
    assert parse_stream(tmp_path, b"\x1bOA\x1bOB", 2) == ["UP", "DOWN"]


def test_enter_parses_as_both_cr_and_lf(tmp_path):
    assert parse_stream(tmp_path, b"\r\n", 2) == ["ENTER", "ENTER"]


def test_a_full_interaction_parses_in_order(tmp_path):
    assert parse_stream(tmp_path, b"2\x1b[B\rk", 4) == ["2", "DOWN", "ENTER", "UP"]


def stty_calls(tmp_path, command, refuse=None):
    """Record how recovery asks for a terminal mode."""
    log = tmp_path / "stty.log"
    stub = ('stty() { echo "$*" >> "%s"; case "$*" in %s) return 1 ;; esac; '
            'return 0; }\n' % (log, refuse or "@@never@@"))
    result = run(tmp_path, stub + command)
    return result, (log.read_text().splitlines() if log.exists() else [])


def test_the_terminal_mode_is_asked_for(tmp_path):
    result, calls = stty_calls(tmp_path, "recovery_raw_tty")

    assert result.returncode == 0
    assert any("-icanon" in call and "-echo" in call for call in calls), calls


def test_the_mode_is_set_through_a_descriptor_that_stays_open(tmp_path):
    """`stty -F /dev/tty1` cannot work. The Linux VT driver sets
    TTY_DRIVER_RESET_TERMIOS, so a terminal goes back to its canonical,
    echoing default the moment the last descriptor on it closes -- and for
    `stty -F` that is stty's own exit, before the reader has opened it. The
    mode has to be set on a descriptor whose owner holds it, or arrows echo
    as ^[[A and the reader blocks for a whole line."""
    _, calls = stty_calls(tmp_path, "recovery_raw_tty")

    assert not any("-F" in call.split() for call in calls), calls


def test_the_mode_falls_back_to_the_raw_composite(tmp_path):
    """An stty that refuses the individual flags still gets asked for a
    terminal that delivers one character at a time and does not echo."""
    result, calls = stty_calls(tmp_path, "recovery_raw_tty", refuse="*-icanon*")

    assert result.returncode == 0
    assert any("raw" in call.split() for call in calls), calls


# --- real terminals -------------------------------------------------------
# The tests above drive the parser from files. These drive the whole input
# path -- terminal mode, pump, FIFO, read_key -- through a real pty, because
# the bug that froze the menu was never in the parser: the terminal was
# still in canonical mode with echo on, so arrows appeared as ^[[B and
# nothing was readable until Enter.
#
# A pty keeps whatever mode it is given, and a VT does not, so the shim
# below gives the pty the one behaviour of /dev/tty1 that matters here.

VT_STTY_SHIM = """#!/bin/sh
# Makes a pty behave like the phone's screen. The Linux VT console driver
# sets TTY_DRIVER_RESET_TERMIOS: when the last descriptor on /dev/ttyN
# closes, the terminal reverts to the canonical, echoing default. So a mode
# set with `stty -F DEVICE` -- by a process that opens the terminal, sets it
# and exits -- is already gone by the time anything reads. A mode set on an
# inherited descriptor sticks, because its owner never let go.
device=""
want_device=""
for word in "$@"; do
    if [ -n "$want_device" ]; then
        device="$word"
        want_device=""
        continue
    fi
    case "$word" in
        -F|--file) want_device=1 ;;
    esac
done
if [ -n "$device" ]; then
    %(real)s "$@"
    status=$?
    %(real)s -F "$device" sane 2>/dev/null
    exit $status
fi
exec %(real)s "$@"
"""


def vt_like_env(tmp_path):
    """PATH with an stty that resets a pty the way the VT driver would."""
    real = shutil.which("stty")
    assert real, "these tests need a real stty"
    shim_dir = tmp_path / "vtbin"
    shim_dir.mkdir()
    shim = shim_dir / "stty"
    shim.write_text(VT_STTY_SHIM % {"real": real})
    shim.chmod(0o755)
    return dict(os.environ, PATH="%s:%s" % (shim_dir, os.environ["PATH"]))


def typed_into_the_screen(tmp_path, keystrokes, count):
    """Type raw bytes at a terminal recovery is reading, and report the keys
    it made of them plus anything the terminal echoed back."""
    master, slave = pty.openpty()
    device = os.ttyname(slave)
    os.close(slave)          # only the pump holds the terminal, as on a VT
    ready = tmp_path / "ready"
    script = script_for(tmp_path,
                        'TTY="%s"\n'
                        'recovery_input_start || exit 9\n'
                        'touch "%s"\n'
                        '%srecovery_input_stop\n'
                        % (device, ready, "read_key\n" * count))
    # stdin is deliberately not the terminal: on the phone recovery is init,
    # whose stdin is the serial console, and the keys have to come off the
    # screen's VT regardless.
    proc = subprocess.Popen(["sh", "-c", script], stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, env=vt_like_env(tmp_path),
                            start_new_session=True)
    try:
        deadline = time.time() + 10
        while not ready.exists() and time.time() < deadline:
            time.sleep(0.05)
        time.sleep(0.3)      # let the pump reach its read()
        os.write(master, keystrokes)
        # Echo comes straight back from the line discipline, so collect it
        # before the pump exits and takes the other end of the pty with it.
        echoed = b""
        while select.select([master], [], [], 0.4)[0]:
            try:
                chunk = os.read(master, 4096)
            except OSError:      # the pump let go of the terminal
                break
            if not chunk:
                break
            echoed += chunk
        try:
            out, _ = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            out = ""         # still waiting for a newline that never comes
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, 9)
            proc.wait()
        os.close(master)
    return out.split(), echoed


def test_an_arrow_key_arrives_without_waiting_for_a_newline(tmp_path):
    """Canonical mode hands over nothing until Enter, which is what made
    every arrow press look like it had been swallowed."""
    keys, _ = typed_into_the_screen(tmp_path, b"\x1b[B", count=1)

    assert keys == ["DOWN"]


def test_keystrokes_are_not_echoed_onto_the_screen(tmp_path):
    """^[[B printed over the menu is the terminal echoing what it was sent."""
    _, echoed = typed_into_the_screen(tmp_path, b"\x1b[B", count=1)

    assert echoed == b""


def test_keys_come_from_the_display_and_not_from_the_shell_stdin(tmp_path):
    """recovery runs as init, whose stdin is /dev/console -- the serial port.
    Reading the keys from there is why arrows worked over serial and did
    nothing at all on the phone's own screen: the VT was never read."""
    keys, _ = typed_into_the_screen(tmp_path, b"\x1b[B", count=1)

    assert keys == ["DOWN"]


# --- what recovery says about the signature ------------------------------
#
# It used to say "Signature is NOT checked here", and that was honest: there
# was no crypto in the initramfs. There is now (nd-verify and the release
# key, packed for the gate in ndsys-apply.sh), so the panel has to stop
# saying it -- and the question a person answers has to reflect which of the
# two situations they are actually in.

def signature_gate(tmp_path, key=None):
    """The verifier and key init exports; nd-verify stood in for by openssl."""
    verifier = tmp_path / "nd-verify"
    verifier.write_text('#!/bin/sh\nexec openssl dgst -sha256 -verify "$3" '
                        '-signature "$2" "$1" >/dev/null 2>&1\n')
    verifier.chmod(0o755)
    return ('NDSYS_VERIFY_BIN="%s"; NDSYS_RELEASE_KEY="%s"; NDSYS_TMPDIR="%s"\n'
            % (verifier, key or write_public_key(tmp_path), tmp_path / "run"))


def ask_is_signed(tmp_path, package, gate=None):
    script = (
        '%s'
        '. "%s"\n'
        'if recovery_package_is_signed "%s"; then echo SIGNED; else echo NOT; fi\n'
        % (gate if gate is not None else signature_gate(tmp_path),
           RECOVERY_SH, package)
    )
    return subprocess.run(["sh", "-c", script], capture_output=True,
                          text=True).stdout.strip()


def test_a_release_signed_package_reads_as_signed(tmp_path):
    image, tree = build_image(blocks=4)
    package = tmp_path / "UPDATE.ndsw"
    make_ndsw(package, image=image, tree=tree)

    assert ask_is_signed(tmp_path, package) == "SIGNED"


def test_a_package_with_no_signature_member_reads_as_unsigned(tmp_path):
    image, tree = build_image(blocks=4)
    package = tmp_path / "UPDATE.ndsw"
    make_ndsw(package, image=image, tree=tree,
              members=("rootfs.squashfs", "manifest.json"))

    assert ask_is_signed(tmp_path, package) == "NOT"


def test_a_package_signed_by_another_key_reads_as_unsigned(tmp_path):
    image, tree = build_image(blocks=4)
    package = tmp_path / "UPDATE.ndsw"
    make_ndsw(package, image=image, tree=tree)
    other = tmp_path / "other.pub"
    subprocess.run(["openssl", "genpkey", "-algorithm", "RSA",
                    "-pkeyopt", "rsa_keygen_bits:2048",
                    "-out", str(tmp_path / "other.key")],
                   check=True, capture_output=True)
    subprocess.run(["openssl", "rsa", "-in", str(tmp_path / "other.key"),
                    "-pubout", "-out", str(other)],
                   check=True, capture_output=True)

    assert ask_is_signed(tmp_path, package,
                         gate=signature_gate(tmp_path, key=other)) == "NOT"


def test_no_verifier_reads_as_unsigned_rather_than_as_signed(tmp_path):
    """"Is this signed?" with nothing able to answer is not "yes"."""
    image, tree = build_image(blocks=4)
    package = tmp_path / "UPDATE.ndsw"
    make_ndsw(package, image=image, tree=tree)
    gate = ('NDSYS_VERIFY_BIN="%s"; NDSYS_RELEASE_KEY="%s"; NDSYS_TMPDIR="%s"\n'
            % (tmp_path / "nope", write_public_key(tmp_path), tmp_path / "run"))

    assert ask_is_signed(tmp_path, package, gate=gate) == "NOT"


def test_the_panel_no_longer_claims_signatures_are_unchecked():
    """The old text was true and is not any more. A phone that says the wrong
    thing about its own security is worse than one that says nothing."""
    source = open(RECOVERY_SH).read()

    assert "Signature is NOT checked here" not in source
    assert "Signed by the release key" in source
    assert "NOT SIGNED" in source


# --- the on-screen UI ----------------------------------------------------
#
# Recovery has drawn on the phone's framebuffer console for a long time. What
# it could not do was be DRIVEN: the sixteen keys are on a PCF8575 that no
# kernel driver binds, so no byte ever reaches the VT and read_key() is, on a
# phone, a dead end. nd-recui scans the expander itself and draws in the
# phone's own typeface.
#
# What these cases pin is the SEAM, not the C: that the shell delegates when
# the panel UI is available, that it falls back to the menu above when it is
# not, and -- the important one -- that the install pipelines are byte-
# identical either way. Every case above this line runs with no $RECUI_BIN on
# disk and exercises exactly the fallback, which is why they still pass.

def fake_recui(tmp_path, body):
    """A stand-in for nd-recui. RECUI_BIN is a variable precisely so a host
    test can substitute one."""
    path = tmp_path / "nd-recui"
    path.write_text("#!/bin/sh\n%s\n" % body)
    path.chmod(0o755)
    return path


def panel_env(tmp_path, body):
    """Preamble that makes recovery_panel_ui() true with the fake in place."""
    return ('RECUI_BIN="%s"; PANEL_UP=1; RECOVERY_TTY_OVERRIDE=""\n'
            'RECOVERY_KEYMAP="%s"\n' % (fake_recui(tmp_path, body),
                                        tmp_path / "keymap.json"))


def test_the_panel_ui_is_used_when_the_binary_and_the_panel_are_both_there(tmp_path):
    body = 'echo "$@" >> "%s"; echo 2' % (tmp_path / "argv")
    result = run(tmp_path, '%srecovery_menu "pick" alpha beta'
                 % panel_env(tmp_path, body))

    assert result.stdout.strip() == "2"
    argv = (tmp_path / "argv").read_text()
    assert "menu" in argv
    assert "--keymap" in argv
    assert "pick alpha beta" in argv


def test_no_binary_means_the_text_menu_exactly_as_before(tmp_path):
    """The predicate has three legs and each of them alone is enough."""
    for preamble in ('RECUI_BIN=/nope; PANEL_UP=1\n',
                     'RECUI_BIN=/bin/sh; PANEL_UP=""\n',
                     'RECUI_BIN=/bin/sh; PANEL_UP=1; '
                     'RECOVERY_TTY_OVERRIDE=/dev/console\n'):
        result = run(tmp_path, '%srecovery_panel_ui && echo PANEL || echo TTY'
                     % preamble)
        assert result.stdout.strip() == "TTY", preamble


def test_the_cmdline_override_still_wins(tmp_path):
    """neodct.rectty=/dev/console is a deliberate request for a text UI on a
    cable. A prettier menu on a screen nobody is looking at is not an
    improvement."""
    body = 'echo 1'
    result = run(tmp_path, '%sRECOVERY_TTY_OVERRIDE=/dev/console\n'
                 'recovery_panel_ui && echo PANEL || echo TTY'
                 % panel_env(tmp_path, body))

    assert result.stdout.strip() == "TTY"


def test_exit_two_falls_through_to_the_text_menu(tmp_path):
    """"I have no usable input device". Drawing a menu nobody can move is
    worse than console text that at least works over serial."""
    keyfile = tmp_path / "keys"
    keyfile.write_text("DOWN\nENTER\n")
    stub = ('read_key() { sed -n 1p "%s"; sed -i 1d "%s"; }\n'
            'TTY=/dev/null; screen_clear() { :; }; say() { :; }\n'
            'recovery_input_ensure() { :; }\n' % (keyfile, keyfile))

    result = run(tmp_path, '%s%srecovery_menu "" alpha beta gamma'
                 % (panel_env(tmp_path, "exit 2"), stub))

    assert result.stdout.strip() == "2"


def test_a_dead_recui_is_not_asked_again(tmp_path):
    """One exec per screen to be told the same thing would also leave the
    panel showing a menu while the tty draws another."""
    counter = tmp_path / "calls"
    body = 'echo x >> "%s"; exit 2' % counter
    keyfile = tmp_path / "keys"
    keyfile.write_text("ENTER\nENTER\n")
    stub = ('read_key() { sed -n 1p "%s"; sed -i 1d "%s"; }\n'
            'TTY=/dev/null; screen_clear() { :; }; say() { :; }\n'
            'recovery_input_ensure() { :; }\n' % (keyfile, keyfile))

    run(tmp_path, '%s%srecovery_menu "" a; recovery_menu "" a'
        % (panel_env(tmp_path, body), stub))

    assert counter.read_text().count("x") == 1


def test_confirm_maps_yes_and_no_onto_the_exit_status(tmp_path):
    yes = run(tmp_path, '%srecovery_confirm "erase?" && echo YES || echo NO'
              % panel_env(tmp_path, "exit 0"))
    no = run(tmp_path, '%srecovery_confirm "erase?" && echo YES || echo NO'
             % panel_env(tmp_path, "exit 1"))

    assert yes.stdout.strip() == "YES"
    assert no.stdout.strip() == "NO"


def test_the_meter_is_cat_when_there_is_no_panel(tmp_path):
    """The pipelines have to be byte-identical on a headless run. A meter that
    dropped bytes would truncate the image being written to the system
    partition."""
    result = subprocess.run(
        ["sh", "-c", script_for(tmp_path,
                                'RECUI_BIN=/nope; PANEL_UP=""\n'
                                'printf "abcdef" | recovery_meter "step" 6')],
        capture_output=True, env=None)

    assert result.stdout == b"abcdef"


def test_the_meter_passes_the_stream_through_the_panel_ui_too(tmp_path):
    """And with one: nd-recui progress is a pv-style filter, so what comes out
    of the pipeline is what went in."""
    result = subprocess.run(
        ["sh", "-c", script_for(tmp_path,
                                '%sprintf "abcdef" | recovery_meter "step" 6'
                                % panel_env(tmp_path, "cat"))],
        capture_output=True, env=None)

    assert result.stdout == b"abcdef"


def test_the_install_still_writes_the_right_bytes_with_a_meter_in_the_way(tmp_path):
    """The regression that matters. Three pipelines gained a stage each; a
    stage that dropped or reordered a byte would put a corrupt image on the
    flash, and the read-back pass is what would catch it."""
    package, image, _ = make_package(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * (len(image) + 4096))

    result = run(tmp_path, '%srecovery_install_package "%s" "%s"'
                 % (panel_env(tmp_path, "cat"), package, device))

    assert result.returncode == 0, result.stderr
    assert device.read_bytes()[:len(image)] == image


def test_a_meter_that_truncates_is_caught_by_the_read_back(tmp_path):
    """Why instrumenting these pipelines is safe at all. The pipeline's exit
    status is dd's, not the meter's, so a short write would go unnoticed there
    -- but pass 3 hashes image_bytes back off the device and refuses."""
    package, image, _ = make_package(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * (len(image) + 4096))

    # Truncate ONLY the write pass, so passes 1 and 2 both report success and
    # pass 3 is the thing that has to notice. $3 is the --step value.
    body = 'case "$3" in "Writing image") head -c 32 ;; *) cat ;; esac'
    result = run(tmp_path, '%srecovery_install_package "%s" "%s"'
                 % (panel_env(tmp_path, body), package, device))

    assert result.returncode != 0
    assert "read-back mismatch" in result.stdout + result.stderr
    assert not (tmp_path / "state" / "installed.prop").exists()


def test_hash_prefix_still_hashes_the_same_thing_with_a_filter(tmp_path):
    """The one line this change added to ndsys-apply.sh, which the automatic
    applier also calls. `cat` is the default, so every existing caller is
    byte-identical."""
    blob = tmp_path / "blob"
    blob.write_bytes(b"\xa5" * 8192)
    want = hashlib.sha256(b"\xa5" * 8192).hexdigest()

    plain = run(tmp_path, 'hash_prefix "%s" 8192' % blob)
    filtered = run(tmp_path, 'hash_prefix "%s" 8192 cat' % blob)

    assert plain.stdout.strip() == want
    assert filtered.stdout.strip() == want


def test_the_shell_option_survives_never_having_opened_a_descriptor(tmp_path):
    """recovery_main does not open fd 8 when the panel UI is in use, so the
    shell option's hand-back has nothing to hand back. It must not fail --
    dropping to a shell is how somebody checks `ls /dev/i2c-*` on real
    hardware, which is the one thing this whole change cannot verify."""
    result = run(tmp_path, 'RECOVERY_INPUT_UP=""; TTY=/dev/null\n'
                           'recovery_input_stop && echo OK')

    assert result.stdout.strip() == "OK"


def test_every_message_screen_goes_through_one_helper(tmp_path):
    """Seven copies of the same five-line sequence were seven places to get
    the delegation wrong."""
    source = open(RECOVERY_SH).read()

    assert source.count("recovery_say ") >= 6
    # The old spelling survives only inside recovery_say itself and in the
    # "Installing" screen, which deliberately does not wait for a key.
    assert source.count('say "Press a key"') == 1


def test_no_panel_and_no_terminal_drops_to_a_rescue_shell(tmp_path):
    """recovery_main used to give up here; the descriptor is now opened on
    first use, so the giving up moved with it. Returning an empty choice
    instead would spin recovery_main's loop forever on a phone nobody can
    talk to. /bin/sh is fed EOF so the exec'd shell exits at once."""
    script = script_for(tmp_path, 'RECUI_BIN=/nope; PANEL_UP=""\n'
                                  'TTY=/does/not/exist\n'
                                  'recovery_menu "" alpha')
    result = subprocess.run(["sh", "-c", script], capture_output=True,
                            text=True, stdin=subprocess.DEVNULL)

    assert "cannot open" in result.stdout + result.stderr
    # And emphatically not a menu index, which the caller would act on.
    assert result.stdout.strip() in ("", "recovery: cannot open /does/not/exist for input")


def test_an_unopenable_terminal_does_not_kill_the_shell(tmp_path):
    """`exec 8<> "$TTY" || return 1` reads as handled and is not: POSIX makes
    a redirection error on a special builtin fatal to a non-interactive
    shell, so both dash and busybox ash killed the initramfs outright and the
    `exec /bin/sh` rescue path was unreachable. The probe subshell contains
    it."""
    result = run(tmp_path, 'TTY=/does/not/exist\n'
                           'recovery_input_start && echo OPENED || echo REFUSED\n'
                           'echo STILL_RUNNING')

    assert "REFUSED" in result.stdout
    assert "STILL_RUNNING" in result.stdout

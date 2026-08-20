"""The boot-time applier, exercised on the host.

This is the code that can brick the phone: it dd's a new system image over
the running one's partition. It lives in neodct/initramfs/ndsys-apply.sh as
sourceable functions precisely so it can be driven here with real files
standing in for the block devices, rather than only ever being tested by
rebooting QEMU.

The pending state is written by the real Python staging module, so these
tests also pin the contract between the writer (SystemUpdate) and the
reader (busybox sh).
"""

import hashlib
import json
import os
import subprocess

import pytest

from System.core.UpdateService import manifest as manifest_mod
from System.core.UpdateService import staging

from update_fixtures import build_image, make_ndsw

APPLY_SH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "initramfs", "ndsys-apply.sh",
)


def stage_an_update(tmp_path, **overrides):
    """Stage a real update and return (state_dir, image_bytes, manifest)."""
    image, tree = build_image(blocks=8)
    body = make_ndsw(tmp_path / "src.ndsw", image=image, tree=tree, **overrides)
    parsed = manifest_mod.parse(json.dumps(body).encode())
    staged = tmp_path / "pending.img"
    staged.write_bytes(image)
    state = tmp_path / "user" / ".ndsys"
    staging.stage(parsed, staged, state)
    return state, image, parsed


def run(state, sys_dev, tmp_path, command="apply_pending"):
    user = tmp_path / "user"
    (user / "logs").mkdir(parents=True, exist_ok=True)
    script = (
        'STATE_DIR="%s"; MNT_USER="%s"; SYS_DEV="%s"; USER_MOUNTED=1\n'
        '. "%s"\n%s\n' % (state, user, sys_dev, APPLY_SH, command)
    )
    return subprocess.run(["sh", "-c", script], capture_output=True, text=True)


def read_prop(path, key):
    for line in open(path):
        if line.startswith(key + "="):
            return line.split("=", 1)[1].strip()
    return None


def test_writes_the_staged_image_onto_the_system_device(tmp_path):
    state, image, parsed = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * (len(image) + 8192))  # oversized partition

    result = run(state, device, tmp_path)

    assert result.returncode == 0, result.stderr
    assert device.read_bytes()[:len(image)] == image


def test_clears_the_pending_update_once_it_is_installed(tmp_path):
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * len(image))

    run(state, device, tmp_path)

    assert staging.read_pending(state) is None
    assert not (state / "pending.img").exists()


def test_records_what_is_now_installed_for_the_next_boot(tmp_path):
    state, image, parsed = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * len(image))

    run(state, device, tmp_path)
    installed = staging.read_installed(state)

    assert installed.version == "0.3.2a"
    assert installed.verity_root_hash == parsed.verity["root_hash"]
    assert installed.verity_image_blocks == parsed.verity["image_blocks"]
    assert installed.image_bytes == len(image)


def test_reports_success(tmp_path):
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * len(image))

    run(state, device, tmp_path)

    assert staging.read_result(state)["result"] == "ok"
    assert staging.read_result(state)["version"] == "0.3.2a"


def test_does_nothing_when_no_update_is_staged(tmp_path):
    state = tmp_path / "user" / ".ndsys"
    state.mkdir(parents=True)
    device = tmp_path / "system.img"
    device.write_bytes(b"original system")

    result = run(state, device, tmp_path)

    assert result.returncode == 0
    assert device.read_bytes() == b"original system"


def test_refuses_an_image_whose_hash_does_not_match_the_record(tmp_path):
    """Corruption on the SD card or user partition must not reach the flash."""
    state, image, _ = stage_an_update(tmp_path)
    tampered = bytearray(image)
    tampered[5000] ^= 0xFF
    (state / "pending.img").write_bytes(bytes(tampered))
    device = tmp_path / "system.img"
    device.write_bytes(b"original system")

    run(state, device, tmp_path)

    assert device.read_bytes() == b"original system"
    assert staging.read_result(state)["result"] == "failed"
    assert "sha256" in staging.read_result(state)["reason"]
    assert staging.read_pending(state) is None


def test_refuses_a_truncated_staged_image(tmp_path):
    state, image, _ = stage_an_update(tmp_path)
    (state / "pending.img").write_bytes(image[:-4096])
    device = tmp_path / "system.img"
    device.write_bytes(b"original system")

    run(state, device, tmp_path)

    assert device.read_bytes() == b"original system"
    assert staging.read_result(state)["result"] == "failed"


def test_keeps_the_pending_update_when_the_write_does_not_land(tmp_path):
    """A failed write is retried on the next boot -- that is the whole
    reason the staged copy is not deleted first."""
    state, image, _ = stage_an_update(tmp_path)

    run(state, "/dev/null", tmp_path)

    assert staging.read_pending(state) is not None
    assert staging.read_result(state) is None


def test_a_retry_after_a_failed_write_succeeds(tmp_path):
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * len(image))

    run(state, "/dev/null", tmp_path)   # boot 1: write goes nowhere
    run(state, device, tmp_path)        # boot 2: real device

    assert device.read_bytes()[:len(image)] == image
    assert staging.read_result(state)["result"] == "ok"


def test_counts_attempts_so_a_bad_image_cannot_loop_forever(tmp_path):
    state, image, _ = stage_an_update(tmp_path)

    run(state, "/dev/null", tmp_path)
    run(state, "/dev/null", tmp_path)

    assert staging.read_pending(state).attempts == 2


def test_gives_up_after_three_attempts(tmp_path):
    state, image, _ = stage_an_update(tmp_path)

    for _ in range(4):
        run(state, "/dev/null", tmp_path)

    assert staging.read_pending(state) is None
    assert staging.read_result(state)["result"] == "failed"
    assert "gave up" in staging.read_result(state)["reason"]


def test_a_changelog_full_of_shell_metacharacters_cannot_run_commands(tmp_path):
    """The records are parsed, never sourced. Prove it: a value that would
    delete the image if evaluated must be inert."""
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * len(image))
    canary = tmp_path / "canary"
    canary.write_text("intact")
    with open(state / "pending.prop", "a") as handle:
        handle.write("version=0.3.2a$(rm -f %s)`rm -f %s`\n" % (canary, canary))

    run(state, device, tmp_path)

    assert canary.read_text() == "intact"


def test_builds_the_verity_table_that_matches_the_image_layout(tmp_path):
    """The table is what dm-verity is handed; wrong maths here means every
    boot fails verification."""
    state, image, parsed = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * len(image))
    run(state, device, tmp_path)

    result = run(state, device, tmp_path, command="verity_table")

    blocks = parsed.verity["image_blocks"]
    assert result.stdout.split() == [
        "0", str(blocks * 4096 // 512), "verity", "1", str(device), str(device),
        "4096", "4096", str(blocks), str(blocks + 1), "sha256",
        parsed.verity["root_hash"], parsed.verity["salt"],
    ]


def test_the_verity_table_matches_what_the_python_side_computes(tmp_path):
    """Two implementations of the same layout: keep them agreeing."""
    from System.core.UpdateService import verity as verity_mod
    image, tree = build_image(blocks=8)
    state, _, parsed = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * len(image))
    run(state, device, tmp_path)

    shell_table = run(state, device, tmp_path, command="verity_table").stdout.strip()
    python_table = verity_mod.dm_table(tree, str(device),
                                       hash_offset=tree.data_blocks * 4096)

    assert shell_table == python_table


def test_a_missing_salt_becomes_a_dash_in_the_table(tmp_path):
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * len(image))
    run(state, device, tmp_path)
    installed = state / "installed.prop"
    body = "".join(line for line in open(installed)
                   if not line.startswith("verity_salt="))
    installed.write_text(body + "verity_salt=\n")

    result = run(state, device, tmp_path, command="verity_table")

    assert result.stdout.strip().endswith(" -")


# --- device identification ------------------------------------------------
# virtio-mmio devices are not enumerated in the order QEMU is given them, so
# the first real boot mounted the SD card as the root filesystem and died
# with "no /sbin/init in the system image". Names from the cmdline are a hint
# only; what makes a device the system image is that it starts with the
# squashfs magic.

def run_fn(tmp_path, command):
    script = ('STATE_DIR="%s"; MNT_USER="%s"; USER_MOUNTED=1\n. "%s"\n%s\n'
              % (tmp_path / "state", tmp_path / "user", APPLY_SH, command))
    return subprocess.run(["sh", "-c", script], capture_output=True, text=True)


def test_a_squashfs_image_is_recognised_by_its_magic(tmp_path):
    image = tmp_path / "system.img"
    image.write_bytes(b"hsqs" + b"\x00" * 8192)

    assert run_fn(tmp_path, 'is_squashfs "%s"' % image).returncode == 0


def test_an_ext4_partition_is_not_a_squashfs(tmp_path):
    other = tmp_path / "userdata.ext4"
    other.write_bytes(b"\x00" * 2048)

    assert run_fn(tmp_path, 'is_squashfs "%s"' % other).returncode != 0


def test_a_fat_card_is_not_a_squashfs(tmp_path):
    card = tmp_path / "sdcard.img"
    card.write_bytes(b"\xeb\x58\x90mkfs.fat" + b"\x00" * 1024)

    assert run_fn(tmp_path, 'is_squashfs "%s"' % card).returncode != 0


def test_a_missing_device_is_not_a_squashfs(tmp_path):
    assert run_fn(tmp_path, 'is_squashfs "%s"' % (tmp_path / "nope")).returncode != 0


def test_the_cmdline_hint_is_used_when_it_really_is_the_squashfs(tmp_path):
    image = tmp_path / "system.img"
    image.write_bytes(b"hsqs" + b"\x00" * 8192)

    result = run_fn(tmp_path, 'find_system_device "%s"' % image)

    assert result.stdout.strip() == str(image)


def test_a_wrong_cmdline_hint_is_overridden_by_scanning(tmp_path):
    """The exact first-boot failure: neodct.sys pointed at the wrong device."""
    wrong = tmp_path / "sdcard.img"
    wrong.write_bytes(b"\xeb\x58\x90mkfs.fat" + b"\x00" * 1024)
    real = tmp_path / "candidates" / "system.img"
    real.parent.mkdir()
    real.write_bytes(b"hsqs" + b"\x00" * 8192)

    result = run_fn(tmp_path, 'NDSYS_SCAN_GLOB="%s/*"; find_system_device "%s"'
                    % (real.parent, wrong))

    assert result.stdout.strip() == str(real)


def test_no_squashfs_anywhere_is_reported_as_failure(tmp_path):
    empty = tmp_path / "candidates"
    empty.mkdir()

    result = run_fn(tmp_path, 'NDSYS_SCAN_GLOB="%s/*"; find_system_device ""'
                    % empty)

    assert result.returncode != 0
    assert result.stdout.strip() == ""


# --- identity that survives a wipe ----------------------------------------
# Scanning for the squashfs magic works right up until the moment recovery
# zeroes the system image, and that is exactly the moment it is needed: with
# nothing on the device to recognise, "wipe system" left recovery unable to
# say which device to install onto. QEMU makes it worse by enumerating
# virtio-mmio devices in an order of its own -- given system, user, card it
# produces vda=card, vdb=user, vdc=system -- so the cmdline name is no help
# either.
#
# The disk serial is a property of the device rather than of anything written
# on it, so it survives being zeroed. QEMU sets it with
# `-device virtio-blk-device,serial=NDSYS` and the kernel publishes it at
# /sys/block/vdc/serial.

FAT_CARD = b"\xeb\x58\x90mkfs.fat" + b"\x00" * 1024
SQUASHFS = b"hsqs" + b"\x00" * 8192
WIPED = b"\x00" * 8192


def block_devices(tmp_path, devices):
    """A stand-in /sys/block and /dev, so serials can be driven on the host.

    `devices` maps kernel name to (serial or None, contents). Returns the
    shell prologue that points the script at them.
    """
    sysfs = tmp_path / "sys" / "block"
    nodes = tmp_path / "dev"
    nodes.mkdir(parents=True, exist_ok=True)
    for name, (serial, contents) in devices.items():
        entry = sysfs / name
        entry.mkdir(parents=True)
        if serial is not None:
            # No trailing newline, the way virtio_blk publishes it.
            (entry / "serial").write_text(serial)
        (nodes / name).write_bytes(contents)
    return ('NDSYS_SYSFS_BLOCK="%s"; NDSYS_DEV_DIR="%s"; '
            'NDSYS_SCAN_GLOB="%s/*"\n' % (sysfs, nodes, nodes))


def test_the_system_device_is_found_by_its_serial(tmp_path):
    prologue = block_devices(tmp_path, {
        "vda": ("NDCARD", FAT_CARD),
        "vdb": ("NDUSER", WIPED),
        "vdc": ("NDSYS", SQUASHFS),
    })

    result = run_fn(tmp_path, prologue + 'find_system_device /dev/vda')

    assert result.stdout.strip() == str(tmp_path / "dev" / "vdc")


def test_the_system_is_still_identified_after_it_has_been_wiped(tmp_path):
    """The whole point: "wipe system" leaves nothing to scan for, and the
    next boot still has to know where to install."""
    prologue = block_devices(tmp_path, {
        "vda": ("NDCARD", FAT_CARD),
        "vdb": ("NDUSER", WIPED),
        "vdc": ("NDSYS", WIPED),
    })

    result = run_fn(tmp_path, prologue + 'find_system_device /dev/vda')

    assert result.stdout.strip() == str(tmp_path / "dev" / "vdc")


def test_a_squashfs_on_the_card_does_not_win_over_the_serial(tmp_path):
    """Someone who has written a system image onto an SD card must not have
    the phone adopt the card as its root filesystem."""
    prologue = block_devices(tmp_path, {
        "vda": ("NDCARD", SQUASHFS),
        "vdb": ("NDUSER", WIPED),
        "vdc": ("NDSYS", WIPED),
    })

    result = run_fn(tmp_path, prologue + 'find_system_device ""')

    assert result.stdout.strip() == str(tmp_path / "dev" / "vdc")


def test_the_squashfs_scan_still_works_when_nothing_has_a_serial(tmp_path):
    """An image booted from an older command line has no serials to read."""
    prologue = block_devices(tmp_path, {
        "vda": (None, FAT_CARD),
        "vdb": (None, WIPED),
        "vdc": (None, SQUASHFS),
    })

    result = run_fn(tmp_path, prologue + 'find_system_device /dev/vda')

    assert result.stdout.strip() == str(tmp_path / "dev" / "vdc")


def test_the_user_partition_is_found_by_its_serial(tmp_path):
    prologue = block_devices(tmp_path, {
        "vda": ("NDCARD", FAT_CARD),
        "vdb": ("NDUSER", WIPED),
        "vdc": ("NDSYS", SQUASHFS),
    })

    result = run_fn(tmp_path, prologue + 'find_user_device /dev/vdb')

    assert result.stdout.strip() == str(tmp_path / "dev" / "vdb")


def test_an_unknown_serial_finds_nothing(tmp_path):
    prologue = block_devices(tmp_path, {"vda": ("NDCARD", FAT_CARD)})

    result = run_fn(tmp_path, prologue + 'device_by_serial NOSUCHTHING')

    assert result.returncode != 0
    assert result.stdout.strip() == ""


def test_the_image_is_found_even_though_the_record_names_a_device_path(tmp_path):
    """The record is written by the running system, where the user partition
    is at /NeoDCT/User -- but the initramfs has it at /mnt/user. An absolute
    path from one is meaningless in the other, so the image must be resolved
    against whichever state directory is being read.

    This is what made a real staged update vanish with "staged update is
    incomplete; discarding".
    """
    state, image, _ = stage_an_update(tmp_path)
    # Rewrite the record the way the phone writes it.
    record = state / "pending.prop"
    record.write_text("".join(
        "image=/NeoDCT/User/.ndsys/pending.img\n" if line.startswith("image=")
        else line
        for line in record.read_text().splitlines(keepends=True)))
    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * len(image))

    result = run(state, device, tmp_path)

    assert result.returncode == 0, result.stderr
    assert "incomplete" not in result.stderr
    assert device.read_bytes()[:len(image)] == image

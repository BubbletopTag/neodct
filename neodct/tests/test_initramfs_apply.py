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


# --- installing from the card ----------------------------------------------
#
# The phone has nowhere to stage a copy: the Luckfox `userdata` partition is
# 8 MiB and a system image is 51 MiB. So the applier installs the image
# straight out of the .ndsw where it sits on the card, and these pin the
# parts of that which can go wrong -- a card that is not in the phone, and
# a card that is not the one the update was staged from.

def stage_a_package(tmp_path, name="UPDATE.ndsw", **overrides):
    """Stage a .ndsw on a card, the way the Update app does."""
    image, tree = build_image(blocks=8)
    card = tmp_path / "sdcard"
    (card / "update").mkdir(parents=True, exist_ok=True)
    package = card / "update" / name
    body = make_ndsw(package, image=image, tree=tree, **overrides)
    parsed = manifest_mod.parse(json.dumps(body).encode())
    state = tmp_path / "user" / ".ndsys"
    import zipfile
    with zipfile.ZipFile(str(package)) as handle:
        member = handle.getinfo("rootfs.squashfs").file_size
    staging.stage_package(parsed, package, member, state)
    return state, card, image, parsed


def run_with_card(state, sys_dev, tmp_path, card, command="apply_pending"):
    user = tmp_path / "user"
    (user / "logs").mkdir(parents=True, exist_ok=True)
    script = (
        'STATE_DIR="%s"; MNT_USER="%s"; SYS_DEV="%s"; USER_MOUNTED=1\n'
        'MNT_SDCARD="%s"; NDSYS_CARD_PREMOUNTED=1\n'
        '. "%s"\n%s\n' % (state, user, sys_dev, card, APPLY_SH, command)
    )
    return subprocess.run(["sh", "-c", script], capture_output=True, text=True)


def test_the_image_is_installed_straight_from_the_package(tmp_path):
    state, card, image, parsed = stage_a_package(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * (len(image) + 8192))

    result = run_with_card(state, device, tmp_path, card)

    assert result.returncode == 0, result.stderr
    assert device.read_bytes()[:len(image)] == image, result.stderr
    assert staging.read_installed(state).version == parsed.version


def test_nothing_is_copied_onto_the_user_partition(tmp_path):
    """The fault being fixed: a copy here needs 51 MiB on an 8 MiB
    partition. There must not be one, not even a small one."""
    state, card, image, _ = stage_a_package(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * (len(image) + 8192))

    run_with_card(state, device, tmp_path, card)

    assert not (state / "pending.img").exists()
    for path in (tmp_path / "user").rglob("*"):
        if path.is_file():
            assert path.stat().st_size < 4096, path


def test_a_missing_card_waits_rather_than_giving_up(tmp_path):
    """Somebody took the card out between staging and rebooting. That is
    not a failed update, it is an update that has not happened yet."""
    state, card, image, _ = stage_a_package(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * (len(image) + 8192))
    (card / "update" / "UPDATE.ndsw").unlink()

    result = run_with_card(state, device, tmp_path, card)

    assert result.returncode == 0, result.stderr
    assert staging.read_pending(state) is not None, "the update was discarded"
    assert device.read_bytes() == b"\0" * (len(image) + 8192)


def test_the_update_installs_when_the_card_comes_back(tmp_path):
    state, card, image, _ = stage_a_package(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * (len(image) + 8192))
    kept = (card / "update" / "UPDATE.ndsw").read_bytes()
    (card / "update" / "UPDATE.ndsw").unlink()
    run_with_card(state, device, tmp_path, card)          # card absent

    (card / "update" / "UPDATE.ndsw").write_bytes(kept)   # card back
    result = run_with_card(state, device, tmp_path, card)

    assert result.returncode == 0, result.stderr
    assert device.read_bytes()[:len(image)] == image


def test_a_different_package_under_the_same_name_is_refused(tmp_path):
    """The card is removable, so the file named in the record is not
    necessarily the file that was signed. The hash is what settles it --
    the initramfs has no crypto and cannot check the signature itself."""
    state, card, image, _ = stage_a_package(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\0" * (len(image) + 8192))
    other, other_tree = build_image(blocks=8, seed=b"different")
    make_ndsw(card / "update" / "UPDATE.ndsw", image=other, tree=other_tree)

    result = run_with_card(state, device, tmp_path, card)

    assert result.returncode == 0, result.stderr
    assert device.read_bytes() == b"\0" * (len(image) + 8192), "it installed!"
    assert staging.read_pending(state) is None
    assert staging.read_result(state)["result"] == "failed"


# --- UBI, which is what the real phone actually has ----------------------
#
# On the Luckfox the system partition is raw NAND behind UBI, and the cmdline
# hands the initramfs a ubiblock device:
#
#     ubi.mtd=4 ubi.block=0,system neodct.sys=/dev/ubiblock0_0
#
# ubiblock is READ-ONLY -- the kernel registers that disk read-only on
# purpose. So `dd of=/dev/ubiblock0_0` cannot ever succeed, and the applier's
# own error path turns that into a silent no-op: it logs "write failed;
# retrying on the next boot" and boots the old system. From the outside the
# phone downloads an update, reboots, and is exactly where it started, which
# is the bug this whole file failed to catch -- every test above writes to an
# ordinary file, and dd is perfectly happy with those.
#
# Writing a UBI volume goes through the character device instead, and for a
# static volume ubiupdatevol is the only way: the update is a transaction
# opened with the final size, not a seek-and-write.


def fake_ubiupdatevol(tmp_path):
    """A stand-in that records its arguments and performs the write."""
    log = tmp_path / "ubiupdatevol.args"
    tool = tmp_path / "fake-ubiupdatevol"
    body = "\n".join([
        "#!/bin/sh",
        'echo "$*" >> "@LOG@"',
        '# -s SIZE DEVICE - : consume stdin into the volume',
        'size=""; dev=""',
        'while [ $# -gt 0 ]; do',
        '  case "$1" in',
        '    -s) size=$2; shift 2 ;;',
        '    -) shift ;;',
        '    *) dev=$1; shift ;;',
        '  esac',
        'done',
        'cat > "$dev"',
        "",
    ]).replace("@LOG@", str(log))
    tool.write_text(body)
    tool.chmod(0o755)
    return tool, log


def run_ubi(state, sys_dev, tmp_path, tool):
    user = tmp_path / "user"
    (user / "logs").mkdir(parents=True, exist_ok=True)
    script = (
        'STATE_DIR="%s"; MNT_USER="%s"; SYS_DEV="%s"; USER_MOUNTED=1\n'
        'NDSYS_UBIUPDATEVOL="%s"\n'
        '. "%s"\napply_pending\n' % (state, user, sys_dev, tool, APPLY_SH)
    )
    return subprocess.run(["sh", "-c", script], capture_output=True, text=True)


def test_a_ubiblock_system_device_is_written_with_ubiupdatevol(tmp_path):
    state, image, _ = stage_an_update(tmp_path)
    dev_dir = tmp_path / "dev"
    dev_dir.mkdir()
    # The volume character device is what may be written; the ubiblock disk
    # beside it is the read-only view the rest of the boot uses.
    volume = dev_dir / "ubi0_0"
    volume.write_bytes(b"")
    sys_dev = dev_dir / "ubiblock0_0"
    sys_dev.write_bytes(b"\x00" * len(image))
    tool, log = fake_ubiupdatevol(tmp_path)

    result = run_ubi(state, sys_dev, tmp_path, tool)

    assert result.returncode == 0, result.stderr
    assert log.exists(), "ubiupdatevol was never called; the applier still dd'd"
    args = log.read_text()
    assert str(volume) in args, "wrote to the wrong node: %r" % args
    assert "-s %d" % len(image) in args, "no explicit size: %r" % args
    assert volume.read_bytes()[:len(image)] == image

    # And it has to actually FINISH. returncode 0 is not evidence: every
    # "retrying on the next boot" path returns 0 too, which is the whole
    # reason a failed install looks like a plain reboot from the outside.
    # The volume holds the new image but the ubiblock disk beside it still
    # reads as zeros -- exactly the situation on hardware, where reading back
    # through a block device that was never told anything changed can serve
    # the old contents from its page cache.
    assert staging.read_result(state)["result"] == "ok", result.stdout
    assert staging.read_installed(state) is not None, "installed.prop not written"
    assert staging.read_pending(state) is None, "pending record not cleared"


def test_the_ubi_volume_is_derived_from_the_ubiblock_name(tmp_path):
    """/dev/ubiblock0_0 -> /dev/ubi0_0, and only for ubiblock names."""
    script = (
        '. "%s"\n'
        'ubi_volume_for /dev/ubiblock0_0 || echo NONE\n'
        'ubi_volume_for /dev/ubiblock1_3 || echo NONE\n'
        'ubi_volume_for /dev/vda || echo NONE\n'
        'ubi_volume_for /dev/mmcblk0p2 || echo NONE\n' % APPLY_SH
    )
    out = subprocess.run(["sh", "-c", script], capture_output=True, text=True)
    assert out.stdout.split() == [
        "/dev/ubi0_0", "/dev/ubi1_3", "NONE", "NONE"], out.stdout


def test_an_ordinary_block_device_is_still_written_with_dd(tmp_path):
    """The QEMU path must not start needing a tool that is not there."""
    state, image, _ = stage_an_update(tmp_path)
    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * len(image))
    tool, log = fake_ubiupdatevol(tmp_path)

    result = run_ubi(state, device, tmp_path, tool)

    assert result.returncode == 0, result.stderr
    assert not log.exists(), "a plain device should not go through ubiupdatevol"
    assert device.read_bytes()[:len(image)] == image


# --- growing the volume --------------------------------------------------
#
# mknand.sh gives the `system` volume no explicit size, so ubinize sizes it to
# exactly the image being flashed ("assume minimum to fit image"). A static
# UBI volume cannot take more than it was made for, and NeoDCT images grow --
# 0.4.4a added 2.7 MB when Bluetooth arrived. Proven against real UBI in
# neodct/tools/test_update_ubi.sh, where a 1.25 MB image into a 1 MB volume
# does not install at all.
#
# There is room to grow into: the partition is 100 MB and the image is under
# 50 MB. It just has to be asked for.


def fake_tool(tmp_path, name, body="exit 0"):
    log = tmp_path / (name + ".args")
    tool = tmp_path / ("fake-" + name)
    tool.write_text("#!/bin/sh\necho \"$*\" >> \"%s\"\n%s\n" % (log, body))
    tool.chmod(0o755)
    return tool, log


def fake_ubi_sysfs(tmp_path, volume, reserved_ebs, usable_eb_size):
    root = tmp_path / "sysfs"
    d = root / volume
    d.mkdir(parents=True, exist_ok=True)
    (d / "reserved_ebs").write_text("%d\n" % reserved_ebs)
    (d / "usable_eb_size").write_text("%d\n" % usable_eb_size)
    return root


def run_ubi_full(state, sys_dev, tmp_path, upd, rsvol, sysfs):
    user = tmp_path / "user"
    (user / "logs").mkdir(parents=True, exist_ok=True)
    script = (
        'STATE_DIR="%s"; MNT_USER="%s"; SYS_DEV="%s"; USER_MOUNTED=1\n'
        'NDSYS_UBIUPDATEVOL="%s"; NDSYS_UBIRSVOL="%s"; NDSYS_UBI_SYSFS="%s"\n'
        '. "%s"\napply_pending\n'
        % (state, user, sys_dev, upd, rsvol, sysfs, APPLY_SH)
    )
    return subprocess.run(["sh", "-c", script], capture_output=True, text=True)


def test_a_volume_too_small_for_the_image_is_grown_first(tmp_path):
    state, image, _ = stage_an_update(tmp_path)
    dev = tmp_path / "dev"
    dev.mkdir()
    volume = dev / "ubi0_0"
    volume.write_bytes(b"")
    sys_dev = dev / "ubiblock0_0"
    sys_dev.write_bytes(b"")
    # One eraseblock short of the image.
    leb = 4096
    sysfs = fake_ubi_sysfs(tmp_path, "ubi0_0", (len(image) // leb) - 1, leb)
    upd, _ = fake_ubiupdatevol(tmp_path)
    rsvol, rslog = fake_tool(tmp_path, "ubirsvol")

    run_ubi_full(state, sys_dev, tmp_path, upd, rsvol, sysfs)

    assert rslog.exists(), "the volume was never resized"
    args = rslog.read_text()
    assert "-n 0" in args, "wrong volume id: %r" % args
    assert "-s %d" % len(image) in args, "wrong size: %r" % args
    assert str(dev / "ubi0") in args, "resize must name the DEVICE: %r" % args


def test_a_volume_that_already_fits_is_left_alone(tmp_path):
    """Resizing a static volume rewrites its table; do not do it for nothing."""
    state, image, _ = stage_an_update(tmp_path)
    dev = tmp_path / "dev"
    dev.mkdir()
    volume = dev / "ubi0_0"
    volume.write_bytes(b"")
    sys_dev = dev / "ubiblock0_0"
    sys_dev.write_bytes(b"")
    leb = 4096
    sysfs = fake_ubi_sysfs(tmp_path, "ubi0_0", (len(image) // leb) + 8, leb)
    upd, _ = fake_ubiupdatevol(tmp_path)
    rsvol, rslog = fake_tool(tmp_path, "ubirsvol")

    result = run_ubi_full(state, sys_dev, tmp_path, upd, rsvol, sysfs)

    assert not rslog.exists(), "resized a volume that already fits"
    assert staging.read_result(state)["result"] == "ok", result.stdout

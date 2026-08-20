"""System/hw/neodct-sdcard: which devices count as a removable card.

The classification is the safety-critical part. The phone runs from the
devices named on the kernel cmdline, and "format this card for me" must
never be able to point mkfs.vfat at one of them -- including at a sibling
partition of the same disk.
"""

import os
import subprocess

import pytest

HELPER = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "overlay", "NeoDCT", "System", "hw", "neodct-sdcard",
)


def call(tmp_path, function, *args, cmdline="neodct.sys=/dev/vda neodct.user=/dev/vdb"):
    """Source the helper and call one of its functions."""
    cmdline_file = tmp_path / "cmdline"
    cmdline_file.write_text(cmdline + "\n")
    env = dict(os.environ,
               NEODCT_SDCARD_SOURCE_ONLY="1",
               NEODCT_CMDLINE=str(cmdline_file),
               NEODCT_RUN_DIR=str(tmp_path / "run"),
               NEODCT_SDCARD_MOUNT=str(tmp_path / "sdcard"))
    quoted = " ".join('"%s"' % a for a in args)
    script = '. "%s"\n%s %s\n' % (HELPER, function, quoted)
    return subprocess.run(["sh", "-c", script], capture_output=True, text=True,
                          env=env)


def test_the_system_partition_is_not_a_card(tmp_path):
    assert call(tmp_path, "is_system_device", "/dev/vda").returncode == 0


def test_the_user_partition_is_not_a_card(tmp_path):
    assert call(tmp_path, "is_system_device", "/dev/vdb").returncode == 0


def test_a_third_disk_is_a_card(tmp_path):
    assert call(tmp_path, "is_system_device", "/dev/vdc").returncode != 0


def test_a_partition_of_the_system_disk_is_not_a_card(tmp_path):
    """cmdline says /dev/mmcblk0; mmcblk0p1 is the same physical card."""
    result = call(tmp_path, "is_system_device", "/dev/mmcblk0p1",
                  cmdline="neodct.sys=/dev/mmcblk0 neodct.user=/dev/mmcblk0p2")

    assert result.returncode == 0


def test_the_parent_disk_of_a_system_partition_is_not_a_card(tmp_path):
    """cmdline says /dev/mmcblk0p1; the whole disk must be off limits too."""
    result = call(tmp_path, "is_system_device", "/dev/mmcblk0",
                  cmdline="neodct.sys=/dev/mmcblk0p1 neodct.user=/dev/mmcblk0p2")

    assert result.returncode == 0


def test_a_second_sd_card_is_still_a_card(tmp_path):
    result = call(tmp_path, "is_system_device", "/dev/mmcblk1p1",
                  cmdline="neodct.sys=/dev/mmcblk0p1 neodct.user=/dev/mmcblk0p2")

    assert result.returncode != 0


def test_formatting_the_system_disk_is_refused(tmp_path):
    """The check has to come before anything touches mkfs.vfat."""
    result = call(tmp_path, "do_format", "/dev/vda")

    assert result.returncode != 0
    assert "refusing" in result.stderr


def test_formatting_the_user_partition_is_refused(tmp_path):
    result = call(tmp_path, "do_format", "/dev/vdb")

    assert result.returncode != 0
    assert "refusing" in result.stderr


def test_formatting_something_that_is_not_a_device_is_refused(tmp_path):
    plain_file = tmp_path / "not-a-device"
    plain_file.write_bytes(b"x" * 1024)

    result = call(tmp_path, "do_format", str(plain_file))

    assert result.returncode != 0
    assert "not a block device" in result.stderr


def test_removing_a_card_publishes_the_absent_state(tmp_path):
    call(tmp_path, "do_remove")

    state = (tmp_path / "run" / "sdcard.prop").read_text()

    assert "state=absent" in state


def test_a_scan_with_no_card_publishes_the_absent_state(tmp_path):
    """Nothing in the slot is the normal case, not an error."""
    result = call(tmp_path, "do_scan")

    assert result.returncode == 0
    assert "state=absent" in (tmp_path / "run" / "sdcard.prop").read_text()


def test_the_state_file_is_what_storage_expects(tmp_path, monkeypatch):
    """Contract check against the Python reader."""
    from System.core import Storage

    call(tmp_path, "write_state", "mounted", "/dev/vdc", "vfat", "NEODCT")
    # monkeypatch, not assignment: these are module globals the whole suite
    # shares, and leaving a tmp path in them breaks every later test that
    # expects the real device paths.
    monkeypatch.setattr(Storage, "STATE_FILE", str(tmp_path / "run" / "sdcard.prop"))
    monkeypatch.setattr(Storage, "MOUNT_POINT", str(tmp_path / "sdcard"))
    for folder in Storage.FOLDERS:
        os.makedirs(os.path.join(Storage.MOUNT_POINT, folder), exist_ok=True)

    card = Storage.card()

    assert card.state == "ready"
    assert card.device == "/dev/vdc"
    assert card.fstype == "vfat"
    assert card.label == "NEODCT"


# --- content-based exclusion ---------------------------------------------
# The cmdline named /dev/vda and /dev/vdb, but virtio-mmio handed them out in
# the opposite order, so the helper skipped the real card and tried to mount
# the system image instead. Names are a hint; content and the mount table are
# the truth.

def call_env(tmp_path, function, *args, cmdline="neodct.sys=/dev/vda neodct.user=/dev/vdb",
             mounts="", boot_state=""):
    (tmp_path / "cmdline").write_text(cmdline + "\n")
    (tmp_path / "mounts").write_text(mounts)
    (tmp_path / "boot_state").write_text(boot_state)
    env = dict(os.environ,
               NEODCT_SDCARD_SOURCE_ONLY="1",
               NEODCT_CMDLINE=str(tmp_path / "cmdline"),
               NEODCT_MOUNTS=str(tmp_path / "mounts"),
               NEODCT_BOOT_STATE=str(tmp_path / "boot_state"),
               NEODCT_RUN_DIR=str(tmp_path / "run"),
               NEODCT_SDCARD_MOUNT=str(tmp_path / "sdcard"))
    quoted = " ".join('"%s"' % a for a in args)
    script = '. "%s"\n%s %s\n' % (HELPER, function, quoted)
    return subprocess.run(["sh", "-c", script], capture_output=True, text=True,
                          env=env)


def test_a_squashfs_device_is_never_treated_as_a_card(tmp_path):
    """This is what went wrong: the system image was the only candidate left
    after the cmdline names excluded the wrong two devices."""
    image = tmp_path / "vdc"
    image.write_bytes(b"hsqs" + b"\x00" * 4096)

    assert call_env(tmp_path, "is_reserved_device", str(image)).returncode == 0


def test_a_mounted_device_is_never_treated_as_a_card(tmp_path):
    """The user partition is already mounted; do not remount it as a card."""
    device = tmp_path / "vdb"
    device.write_bytes(b"\x00" * 2048)
    mounts = "%s /NeoDCT/User ext4 rw,noatime 0 0\n" % device

    assert call_env(tmp_path, "is_reserved_device", str(device),
                    mounts=mounts).returncode == 0


def test_the_device_the_initramfs_recorded_is_excluded(tmp_path):
    """The initramfs publishes what it actually resolved; trust that over
    the cmdline."""
    device = tmp_path / "vdc"
    device.write_bytes(b"\x00" * 2048)
    state = "state=enforced\nsys_device=%s\nuser_device=/dev/vdb\n" % device

    assert call_env(tmp_path, "is_reserved_device", str(device),
                    boot_state=state).returncode == 0


def test_a_plain_fat_device_is_a_card(tmp_path):
    card = tmp_path / "vda"
    card.write_bytes(b"\xeb\x58\x90mkfs.fat" + b"\x00" * 2048)

    assert call_env(tmp_path, "is_reserved_device", str(card)).returncode != 0


def test_a_cmdline_named_device_is_still_excluded(tmp_path):
    """Belt and braces: when the names happen to be right, honour them."""
    assert call_env(tmp_path, "is_reserved_device", "/dev/vda").returncode == 0


def test_a_wrongly_named_device_is_still_available_as_a_card(tmp_path):
    """The exact second-order failure: the cmdline said neodct.sys=/dev/vda,
    but vda was really the SD card. Using those names to exclude devices
    excluded the card itself, so no candidate was left and the scan silently
    reported no card. What the initramfs actually resolved wins."""
    card = tmp_path / "vda"
    card.write_bytes(b"\xeb\x58\x90mkfs.fat" + b"\x00" * 2048)
    system = tmp_path / "vdc"
    system.write_bytes(b"hsqs" + b"\x00" * 2048)
    state = "state=enforced\nsys_device=%s\nuser_device=%s\n" % (
        system, tmp_path / "vdb")

    result = call_env(tmp_path, "is_reserved_device", str(card),
                      cmdline="neodct.sys=%s neodct.user=%s" % (card,
                                                               tmp_path / "vdb"),
                      boot_state=state)

    assert result.returncode != 0, "the card must not be excluded"


def test_the_cmdline_is_still_used_when_nothing_was_recorded(tmp_path):
    """Without a boot record (older initramfs) the names are all there is."""
    result = call_env(tmp_path, "is_reserved_device", "/dev/vda",
                      boot_state="state=enforced\n")

    assert result.returncode == 0


# --- mounting ------------------------------------------------------------
# busybox's blkid has far fewer filesystem probers than util-linux's and
# reported nothing at all for a plain mkfs.vfat card, so the helper announced
# "no filesystem we can read" about a perfectly good FAT32 image. Whether a
# card can be mounted is a question for the kernel, not for blkid.

def call_with_fake_mount(tmp_path, device, succeed_on, cmdline="", boot_state=""):
    """Run try_mount with `mount` stubbed, recording every attempt."""
    (tmp_path / "cmdline").write_text(cmdline + "\n")
    (tmp_path / "boot_state").write_text(boot_state)
    (tmp_path / "mounts").write_text("")
    attempts = tmp_path / "attempts"
    env = dict(os.environ,
               NEODCT_SDCARD_SOURCE_ONLY="1",
               NEODCT_CMDLINE=str(tmp_path / "cmdline"),
               NEODCT_MOUNTS=str(tmp_path / "mounts"),
               NEODCT_BOOT_STATE=str(tmp_path / "boot_state"),
               NEODCT_RUN_DIR=str(tmp_path / "run"),
               NEODCT_SDCARD_MOUNT=str(tmp_path / "sdcard"))
    script = (
        '. "%s"\n'
        'mount() { echo "$*" >> "%s"; case "$*" in %s) return 0 ;; esac; return 1; }\n'
        'blkid() { return 1; }\n'          # exactly what busybox did here
        'try_mount "%s"\n' % (HELPER, attempts, succeed_on, device)
    )
    result = subprocess.run(["sh", "-c", script], capture_output=True, text=True,
                            env=env)
    tried = attempts.read_text().splitlines() if attempts.exists() else []
    state = {}
    state_file = tmp_path / "run" / "sdcard.prop"
    if state_file.exists():
        for line in state_file.read_text().splitlines():
            key, _, value = line.partition("=")
            state[key] = value
    return result, tried, state


def test_a_fat_card_mounts_even_when_blkid_says_nothing(tmp_path):
    """The reported bug, exactly: blkid blind, card perfectly mountable."""
    result, tried, state = call_with_fake_mount(tmp_path, "/dev/vda", '*"-t vfat"*')

    assert result.returncode == 0
    assert state["state"] == "mounted"
    assert state["fstype"] == "vfat"
    assert state["device"] == "/dev/vda"


def test_vfat_is_tried_first(tmp_path):
    """It is what a NeoDCT card is, so do not paw through ext4 first."""
    _, tried, _ = call_with_fake_mount(tmp_path, "/dev/vda", '*"-t vfat"*')

    assert "vfat" in tried[0]


def test_an_ext_card_still_mounts(tmp_path):
    """Someone else's ext4 card: mount it so nothing is lost, and let the UI
    offer to reformat."""
    result, tried, state = call_with_fake_mount(tmp_path, "/dev/vda", '*"-t ext4"*')

    assert result.returncode == 0
    assert state["fstype"] == "ext4"
    assert any("vfat" in attempt for attempt in tried)


def test_fat_only_options_are_not_passed_to_an_ext_mount(tmp_path):
    """utf8/flush are FAT options; ext4 refuses them and the mount fails."""
    _, tried, _ = call_with_fake_mount(tmp_path, "/dev/vda", '*"-t ext4"*')

    for attempt in tried:
        if "ext4" in attempt or "ext3" in attempt or "ext2" in attempt:
            assert "utf8" not in attempt
            assert "flush" not in attempt


def test_a_card_with_no_readable_filesystem_is_reported_unmountable(tmp_path):
    result, tried, state = call_with_fake_mount(tmp_path, "/dev/vda", "nothing")

    assert result.returncode != 0
    assert state["state"] == "unmountable"
    assert state["device"] == "/dev/vda"
    assert len(tried) >= 4          # it really did try several


def test_the_last_resort_lets_the_kernel_guess(tmp_path):
    """A filesystem we did not list but the kernel knows about."""
    result, tried, state = call_with_fake_mount(tmp_path, "/dev/vda",
                                                '*"rw,noatime /dev/vda"*')

    assert result.returncode == 0
    assert state["state"] == "mounted"

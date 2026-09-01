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

def call_with_fake_mount(tmp_path, device, succeed_on, cmdline="", boot_state="",
                         label=None):
    """Run try_mount with `mount` stubbed, recording every attempt.

    `label` is what blkid reports for the device. None keeps the historical
    stub -- blkid saying nothing at all, which is exactly what busybox's did
    for a perfectly good FAT card and the reason this helper asks the kernel
    whether something mounts rather than asking blkid what it is. A string
    makes blkid answer, which is what after_mount() consults to decide whether
    the card is one of ours.

    chown is stubbed throughout, and recorded: the layout pass runs on a card
    the helper recognises, and "did it take ownership of this card" is a
    question every case below has an answer to.
    """
    (tmp_path / "cmdline").write_text(cmdline + "\n")
    (tmp_path / "boot_state").write_text(boot_state)
    (tmp_path / "mounts").write_text("")
    attempts = tmp_path / "attempts"
    chowns = tmp_path / "chowns"
    env = dict(os.environ,
               NEODCT_SDCARD_SOURCE_ONLY="1",
               NEODCT_CMDLINE=str(tmp_path / "cmdline"),
               NEODCT_MOUNTS=str(tmp_path / "mounts"),
               NEODCT_BOOT_STATE=str(tmp_path / "boot_state"),
               NEODCT_RUN_DIR=str(tmp_path / "run"),
               NEODCT_SDCARD_MOUNT=str(tmp_path / "sdcard"))
    if label is None:
        blkid = 'blkid() { return 1; }\n'   # exactly what busybox did here
    else:
        blkid = 'blkid() { echo "$1: LABEL=\\"%s\\" TYPE=\\"fake\\""; }\n' % label
    script = (
        '. "%s"\n'
        'mount() { echo "$*" >> "%s"; case "$*" in %s) return 0 ;; esac; return 1; }\n'
        '%s'
        'chown() { echo "$*" >> "%s"; }\n'
        'try_mount "%s"\n' % (HELPER, attempts, succeed_on, blkid, chowns, device)
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
    state["_chowns"] = chowns.read_text() if chowns.exists() else ""
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
    """`unmountable` here, and `unformatted` from do_format's own refusal when
    the image has no mke2fs to make a card with. Two spellings of one thing,
    which is why nd_storage.c and the Python Storage both map either of them
    onto UNFORMATTED: the UI has one branch for "the only way forward is to
    reformat, and that erases the card"."""
    result, tried, state = call_with_fake_mount(tmp_path, "/dev/vda", "nothing")

    assert result.returncode != 0
    assert state["state"] == "unmountable"
    assert state["device"] == "/dev/vda"
    assert len(tried) >= 4          # it really did try several


def test_the_last_resort_lets_the_kernel_guess(tmp_path):
    """A filesystem we did not list but the kernel knows about."""
    result, tried, state = call_with_fake_mount(tmp_path, "/dev/vda",
                                                '*"/dev/vda"*"sdcard"*')

    assert result.returncode == 0
    assert state["state"] == "mounted"


# --- what a card is mounted WITH -----------------------------------------
# A card is the one filesystem on the phone whose contents were chosen by
# whoever last held it, and udev mounts it automatically on insertion. So
# "plug this in" is a path an attacker with thirty seconds of physical access
# controls end to end, and the mount options are the whole of the defence.
# SECURITY-PLAN.md section 0/1; SECURITY-AUDIT.md finding 9.

@pytest.mark.parametrize("fstype", ["vfat", "ext4", "ext2"])
def test_every_card_mount_is_nosuid_nodev(tmp_path, fstype):
    _, tried, _ = call_with_fake_mount(tmp_path, "/dev/vda", '*"-t %s"*' % fstype)

    for attempt in tried:
        assert "nosuid" in attempt, attempt
        assert "nodev" in attempt, attempt


def test_the_last_resort_mount_is_nosuid_nodev_too(tmp_path):
    """The kernel-guesses path is a mount like any other, and it is the one
    that reaches a filesystem CARD_FSTYPES never named."""
    _, tried, _ = call_with_fake_mount(tmp_path, "/dev/vda", '*"/dev/vda"*"sdcard"*')

    assert "nosuid" in tried[-1] and "nodev" in tried[-1], tried[-1]


# --- what KIND of card this is -------------------------------------------
# after_mount() runs on every successful mount and answers one question the
# state file did not used to carry: is this a card the phone can lay out?
#
# It can only be one if the filesystem records ownership, because the whole
# layout is ownership -- apps/ that an untrusted process may execute out of
# and not write to, untrusted/ that it may write, media that it cannot read.
# ext4 says all of that per inode. FAT says none of it: permissions on a FAT
# mount come from uid=/gid=/fmask=/dmask= applied to the entire filesystem, so
# a FAT card can hold the owner's music and nothing else.

def test_an_ext_card_of_ours_is_laid_out(tmp_path):
    """Ours: ext, and labelled NEODCT. The folders are created and the layout
    is applied on THIS mount rather than only at format time, because the card
    has been out of the phone since the last one and anything could have
    happened to it in between."""
    result, _, state = call_with_fake_mount(tmp_path, "/dev/vda", '*"-t ext4"*',
                                            label="NEODCT")

    assert result.returncode == 0
    assert state["state"] == "mounted"
    assert state["label"] == "NEODCT"
    for folder in ("apps", "untrusted", "music", "wallpapers", "tones",
                   "backup_db", "update"):
        assert (tmp_path / "sdcard" / folder).is_dir(), folder
    assert "ndusr:ndusr_ut" in state["_chowns"], state["_chowns"]


def test_an_ext_card_of_ours_publishes_where_a_download_may_go(tmp_path):
    """`untrusted=` is what nd_storage_untrusted_dir() hands the browser, and
    a caller with nothing there REFUSES the download rather than falling back
    to /NeoDCT/User -- which is 8 MiB on the phone, shared with the databases.
    So the field is only set for a card that really has the directory."""
    _, _, state = call_with_fake_mount(tmp_path, "/dev/vda", '*"-t ext4"*',
                                       label="NEODCT")

    assert state["untrusted"] == str(tmp_path / "sdcard" / "untrusted")
    assert (tmp_path / "sdcard" / "untrusted").is_dir()


def test_a_strangers_ext_card_is_mounted_and_left_alone(tmp_path):
    """An ext4 card that is not ours mounts -- nothing is lost -- and is not
    touched. Chowning it would be the phone quietly taking ownership of
    somebody's photographs, and it is a card with real ownership on it, so
    that would be a change they could see and could not undo on the phone."""
    _, _, state = call_with_fake_mount(tmp_path, "/dev/vda", '*"-t ext4"*',
                                       label="HOLIDAY")

    assert state["state"] == "mounted"
    assert state["untrusted"] == "", "it offered a download directory it never made"
    assert state["_chowns"] == "", state["_chowns"]
    assert not (tmp_path / "sdcard" / "apps").exists()


def test_a_neodct_card_in_the_old_fat_format_is_reported_legacy(tmp_path):
    """A card from before 0.5.0b: FAT32, labelled NEODCT, and perfectly good
    for the owner's music.

    `legacy` rather than `mounted` because the difference is worth a sentence
    in the UI: this card cannot hold an installed app and has nowhere for a
    download to land, and the remedy is a REFORMAT, which erases it. That is
    the owner's to accept -- the phone will not reformat a card it found -- so
    the state exists to let Settings say so and offer."""
    _, _, state = call_with_fake_mount(tmp_path, "/dev/vda", '*"-t vfat"*',
                                       label="NEODCT")

    assert state["state"] == "legacy"
    assert state["fstype"] == "vfat"
    assert state["device"] == "/dev/vda", "the format dialog names this"
    assert state["untrusted"] == "", "a FAT card has nowhere ndusr_ut may write"


def test_a_legacy_card_is_not_laid_out_or_chowned(tmp_path):
    """Nothing is created and nothing is chowned. chmod and chown do not fail
    on a FAT mount, they SUCCEED AND DO NOTHING -- the kernel's vfat driver
    accepts both and the mode comes back from the mount options regardless. A
    layout applied here would report success and mean nothing."""
    _, _, state = call_with_fake_mount(tmp_path, "/dev/vda", '*"-t vfat"*',
                                       label="NEODCT")

    assert state["_chowns"] == "", state["_chowns"]
    assert not (tmp_path / "sdcard" / "apps").exists()


def test_somebody_elses_fat_card_is_just_mounted(tmp_path):
    """The camera card, which is the common case and stays the fast path. Not
    legacy -- it was never one of ours -- so the UI offers to set it up rather
    than telling its owner their card is out of date."""
    _, _, state = call_with_fake_mount(tmp_path, "/dev/vda", '*"-t vfat"*',
                                       label="CANON")

    assert state["state"] == "mounted"
    assert state["_chowns"] == ""

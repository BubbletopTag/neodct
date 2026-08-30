"""Every writable filesystem is mounted nosuid,nodev -- and stays that way.

SECURITY-AUDIT.md section 2.4 is the finding: the user partition, /tmp,
/dev/shm and every SD mount carried no restrictions at all, and cards are
mounted automatically on insertion by udev. Today that changes nothing,
because everything on the phone is already root. SECURITY-PLAN.md section 5
puts the fix in Phase 0 for exactly that reason -- it costs nothing while it
is inert, and the alternative is shipping a window in which ndusr_ut exists
and these options do not.

These are assertions about boot configuration rather than about code, so they
read the real files the image is built from. The failure they exist to catch
is a well-meaning edit that rewrites a mount line and drops an option nobody
remembered was load-bearing.
"""

import os
import re
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FSTAB = os.path.join(ROOT, "overlay", "etc", "fstab")
INIT = os.path.join(ROOT, "initramfs", "init")
APPLY_SH = os.path.join(ROOT, "initramfs", "ndsys-apply.sh")


def fstab_rows():
    """(fs, mountpoint, type, options) for every real row."""
    rows = []
    for line in open(FSTAB):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) >= 4:
            rows.append((fields[0], fields[1], fields[2], fields[3].split(",")))
    return rows


# The writable ones. / is read-only squashfs and proc/sysfs/devpts are kernel
# filesystems with nothing to execute or own.
WRITABLE_MOUNTS = ("/tmp", "/dev/shm", "/run")


@pytest.mark.parametrize("mountpoint", WRITABLE_MOUNTS)
def test_every_writable_tmpfs_is_nosuid_nodev(mountpoint):
    rows = [r for r in fstab_rows() if r[1] == mountpoint]

    assert rows, "%s is missing from fstab entirely" % mountpoint
    options = rows[0][3]
    assert "nosuid" in options, "%s: %s" % (mountpoint, options)
    assert "nodev" in options, "%s: %s" % (mountpoint, options)


def test_the_root_filesystem_is_still_read_only():
    """The property everything else is built on; a regression here would make
    the rest of this file beside the point."""
    root = [r for r in fstab_rows() if r[1] == "/"]

    assert root and "ro" in root[0][3]


def test_nothing_in_fstab_became_a_remount_rw():
    body = [l for l in open(FSTAB) if l.strip() and not l.startswith("#")]

    assert not [l for l in body if "remount" in l]


# --- the user partition, which fstab never sees --------------------------
# It is mounted by the initramfs and mount --moved into the new root before
# switch_root, so /etc/fstab has no say in its options and the init script is
# the only place they can be set.

def test_the_user_partition_is_mounted_nosuid_nodev():
    source = open(INIT).read()
    # "mount --move" hands the already-mounted partition to the new root and
    # carries its options with it; only the mount that CHOOSES them matters.
    mounts = [m for m in re.findall(r'mount [^\n]*"\$MNT_USER"', source)
              if "--move" not in m]

    assert mounts, "the user partition mount moved; this test needs updating"
    for line in mounts:
        assert "$USER_OPTS" in line, line
    options = re.search(r'^USER_OPTS="([^"]*)"', source, re.M)
    assert options, "USER_OPTS is not set in initramfs/init"
    assert "nosuid" in options.group(1)
    assert "nodev" in options.group(1)


def test_the_user_partition_is_not_noexec():
    """Deliberate, and worth pinning so it is not "fixed" later: nd-apprun
    loads an app with dlopen(), which noexec refuses exactly as it refuses
    execve(), and /NeoDCT/User/apps is where user-installed apps are meant to
    land once confinement exists. SECURITY-PLAN.md section 1."""
    options = re.search(r'^USER_OPTS="([^"]*)"', open(INIT).read(), re.M)

    assert options and "noexec" not in options.group(1)


# --- the card, at boot ---------------------------------------------------
# The applier mounts a card to install an update straight off it. That card
# is the most hostile input in the boot path: it was written by whoever last
# held it, and the phone reads it before the UI exists.

def run_mount_card(tmp_path, script_tail):
    """Source the applier with mount() stubbed and run something."""
    attempts = tmp_path / "attempts"
    script = (
        'MNT_SDCARD="%s"; SYS_DEV=/dev/null; USER_DEV=/dev/null\n'
        'NDSYS_SCAN_GLOB="%s/card.img"\n'
        '. "%s"\n'
        'mount() { echo "$*" >> "%s"; return 1; }\n'
        'mountpoint() { return 1; }\n'
        '%s\n' % (tmp_path / "mnt", tmp_path, APPLY_SH, attempts, script_tail)
    )
    (tmp_path / "card.img").write_bytes(b"\xeb\x58\x90mkfs.fat" + b"\x00" * 2048)
    result = subprocess.run(["sh", "-c", script], capture_output=True, text=True)
    tried = attempts.read_text().splitlines() if attempts.exists() else []
    return result, tried


def test_the_applier_mounts_a_card_read_only_nosuid_nodev_noexec(tmp_path):
    _, tried = run_mount_card(tmp_path, "mount_card")

    assert tried, "mount_card tried nothing at all"
    for attempt in tried:
        for option in ("ro", "nosuid", "nodev", "noexec"):
            assert option in attempt, "%s missing from: %s" % (option, attempt)


def test_the_applier_never_mounts_a_card_writable(tmp_path):
    """A card pulled out mid-write is a card someone loses their photos from,
    and nothing in the boot path has any business writing to one."""
    _, tried = run_mount_card(tmp_path, "mount_card")

    for attempt in tried:
        assert not re.search(r"(^|[ ,])rw([ ,]|$)", attempt), attempt

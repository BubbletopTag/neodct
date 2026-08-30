"""A NeoDCT card is two FAT32 partitions, and that is the storage half of
the confinement design.

SECURITY-PLAN.md section 1, Option C. The constraint it solves: a FAT
filesystem has NO ownership. chown and chmod do nothing on one; permissions
come from mount options, and those apply to the whole filesystem at once. So
there is no way to give ndusr_ut a subdirectory of a card and deny it the
rest -- a card mounted for the browser is a card the browser owns entirely,
including any authorized_keys sitting on it.

The way out is that a card can carry more than one filesystem. Two
partitions, two independent sets of ownership, no kernel feature, no
filesystem change, and a card that still reads on any computer.

Two things here need proving rather than asserting, and each gets a test that
does the real thing:

  the partition table   written by hand, because there is no sfdisk, no
                        parted and no batch-mode fdisk in the image. So the
                        66 bytes are checked against the on-disk format, and
                        -- where the machine allows it -- fed to the actual
                        Linux partition scanner through a loop device.

  the mount options     the arrival partition is the only one with noexec,
                        and the media side is 0751-shaped so ndusr_ut can
                        traverse to reach it without listing the owner's
                        music.
"""

import os
import shutil
import struct
import subprocess

import pytest

HELPER = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "overlay", "NeoDCT", "System", "hw", "neodct-sdcard",
)
SECTOR = 512
MIB = 1024 * 1024


def sh(tmp_path, body, env=None, stubs=""):
    """Source the helper and run `body`."""
    (tmp_path / "cmdline").write_text("neodct.sys=/dev/vda neodct.user=/dev/vdb\n")
    (tmp_path / "mounts").write_text("")
    (tmp_path / "boot_state").write_text("")
    full = dict(os.environ,
                NEODCT_SDCARD_SOURCE_ONLY="1",
                NEODCT_CMDLINE=str(tmp_path / "cmdline"),
                NEODCT_MOUNTS=str(tmp_path / "mounts"),
                NEODCT_BOOT_STATE=str(tmp_path / "boot_state"),
                NEODCT_RUN_DIR=str(tmp_path / "run"),
                NEODCT_SDCARD_MOUNT=str(tmp_path / "sdcard"))
    full.update(env or {})
    script = '. "%s"\n%s\n%s\n' % (HELPER, stubs, body)
    return subprocess.run(["sh", "-c", script], capture_output=True, text=True,
                          env=full)


def parse_mbr(raw):
    """The four partition entries, as (type, first_lba, sectors)."""
    assert raw[510:512] == b"\x55\xaa", "no MBR signature"
    out = []
    for i in range(4):
        entry = raw[446 + i * 16:446 + (i + 1) * 16]
        ptype = entry[4]
        first, count = struct.unpack("<II", entry[8:16])
        out.append((ptype, first, count))
    return out


# --- the arithmetic ------------------------------------------------------

def plan(tmp_path, sectors):
    result = sh(tmp_path, 'partition_plan /dev/fake',
                stubs='device_sectors() { echo %d; }' % sectors)
    assert result.returncode == 0, result.stderr
    return result.stdout.split()


def test_a_normal_card_is_split_three_to_one(tmp_path):
    """Downloads and MMS attachments are small; media is not."""
    p1_start, p1, p2_start, p2 = (int(v) for v in plan(tmp_path, 256 * MIB // SECTOR))

    assert p1 > p2, "the media side is the bigger one"
    assert abs(p2 * SECTOR / MIB - 64) < 2, p2


def test_every_boundary_is_a_megabyte(tmp_path):
    """1 MiB alignment. An SD card's erase blocks want it, and a partition
    that straddles one performs like a floppy."""
    for size_mib in (64, 128, 256, 1024, 8192):
        values = [int(v) for v in plan(tmp_path, size_mib * MIB // SECTOR)]

        for value in values:
            assert value % (MIB // SECTOR) == 0, (size_mib, values)


def test_the_partitions_do_not_overlap_or_run_off_the_end(tmp_path):
    for size_mib in (64, 128, 256, 1024, 8192, 65536):
        total = size_mib * MIB // SECTOR
        p1_start, p1, p2_start, p2 = (int(v) for v in plan(tmp_path, total))

        assert p1_start >= MIB // SECTOR, "the table itself needs room"
        assert p1_start + p1 <= p2_start, "the media side overruns the arrival one"
        assert p2_start + p2 <= total, "the arrival side runs off the end"


def test_a_huge_card_does_not_hand_over_a_quarter_of_it(tmp_path):
    """A quarter of 64 GB is 16 GB of downloads nobody will ever make."""
    _, _, _, p2 = (int(v) for v in plan(tmp_path, 64 * 1024 * MIB // SECTOR))

    assert p2 * SECTOR <= 600 * MIB, p2


def test_a_tiny_card_is_left_whole(tmp_path):
    """Not an error: it gets exactly what every card got before this -- one
    filesystem, no table, and no arrival partition, which is the same answer
    a foreign card gives."""
    result = sh(tmp_path, 'partition_plan /dev/fake',
                stubs='device_sectors() { echo %d; }' % (8 * MIB // SECTOR))

    assert result.stdout.strip() == "superfloppy"


def test_a_device_with_no_readable_size_is_a_failure_not_a_guess(tmp_path):
    result = sh(tmp_path, 'partition_plan /dev/fake || echo REFUSED',
                stubs='device_sectors() { return 1; }')

    assert "REFUSED" in result.stdout


# --- the table itself ----------------------------------------------------

def test_the_written_table_is_a_partition_table(tmp_path):
    """66 bytes of a format that has not changed since 1983, written by hand
    because the image has no partitioner that can be scripted."""
    image = tmp_path / "card.img"
    image.write_bytes(b"\xee" * (2 * MIB))

    result = sh(tmp_path, 'write_mbr "%s" 2048 129024 131072 129024' % image)

    assert result.returncode == 0, result.stderr
    entries = parse_mbr(image.read_bytes()[:512])
    assert entries[0] == (0x0C, 2048, 129024), entries
    assert entries[1] == (0x0C, 131072, 129024), entries
    assert entries[2] == (0, 0, 0) and entries[3] == (0, 0, 0), "junk in 3 and 4"


def test_both_partitions_are_fat32_lba(tmp_path):
    """0x0C, not 0x0B. A card big enough to matter is addressed by LBA, and
    an 0x0B entry beyond the CHS range is one some tools refuse to read."""
    image = tmp_path / "card.img"
    image.write_bytes(b"\x00" * (2 * MIB))
    sh(tmp_path, 'write_mbr "%s" 2048 1024 4096 1024' % image)

    for ptype, _, _ in parse_mbr(image.read_bytes()[:512])[:2]:
        assert ptype == 0x0C


def test_writing_the_table_does_not_touch_anything_else(tmp_path):
    """conv=notrunc, and one sector. A format that also zeroed the card
    would take minutes and wear the flash for nothing."""
    image = tmp_path / "card.img"
    image.write_bytes(b"\xab" * (2 * MIB))

    sh(tmp_path, 'write_mbr "%s" 2048 1024 4096 1024' % image)

    body = image.read_bytes()
    assert len(body) == 2 * MIB, "the image was truncated"
    assert set(body[512:]) == {0xAB}, "it wrote past the first sector"


@pytest.mark.skipif(os.geteuid() != 0 or not os.path.exists("/dev/loop-control")
                    or shutil.which("losetup") is None
                    or shutil.which("blockdev") is None,
                    reason="needs root and loop devices to ask the real scanner")
def test_linux_itself_reads_the_table_back(tmp_path):
    """The one that matters. Everything above checks the bytes against the
    format as documented; this hands them to the actual partition scanner and
    asks what it found.

    Skipped rather than failed where the kernel reserves no partition minors
    for loop devices -- a container, usually -- because that is a fact about
    the machine and not about the table."""
    image = tmp_path / "card.img"
    with open(image, "wb") as handle:
        handle.truncate(256 * MIB)

    loop = subprocess.run(["losetup", "--show", "-f", "-P", str(image)],
                          capture_output=True, text=True, check=True).stdout.strip()
    try:
        result = sh(tmp_path, 'plan="$(partition_plan %s)" && '
                              'set -- $plan && write_mbr %s "$1" "$2" "$3" "$4" && '
                              'echo "$plan"' % (loop, loop))
        assert result.returncode == 0, result.stderr
        want = [int(v) for v in result.stdout.split()]

        subprocess.run(["blockdev", "--rereadpt", loop], capture_output=True)
        name = os.path.basename(loop)
        base = "/sys/block/%s/%sp1" % (name, name)
        if not os.path.isdir(base):
            pytest.skip("this kernel makes no partition nodes for loop devices")
        for index, (start, size) in enumerate(((want[0], want[1]),
                                               (want[2], want[3])), start=1):
            part = "/sys/block/%s/%sp%d" % (name, name, index)
            assert os.path.isdir(part), "the kernel found no partition %d" % index
            assert int(open(part + "/start").read()) == start
            assert int(open(part + "/size").read()) == size
    finally:
        subprocess.run(["losetup", "-d", loop], capture_output=True)


# --- the mount options, which are what the partitions are FOR ------------

_ATTEMPT_SEQ = [0]


def mount_attempts(tmp_path, function, *args, extra_stubs=""):
    # A file per call: two calls in one test would otherwise pool their
    # attempts and the second would assert about the first one's.
    _ATTEMPT_SEQ[0] += 1
    attempts = tmp_path / ("attempts-%d" % _ATTEMPT_SEQ[0])
    stubs = (
        'mount() { echo "$*" >> "%s"; return 0; }\n'
        'mountpoint() { return 1; }\n'
        'blkid() { return 1; }\n'
        'id() { case "$2" in ndusr) echo 1000 ;; ndusr_ut) echo 1001 ;; '
        '*) return 1 ;; esac; }\n'
        % attempts
    ) + extra_stubs
    quoted = " ".join('"%s"' % a for a in args)
    result = sh(tmp_path, "%s %s" % (function, quoted), stubs=stubs)
    tried = attempts.read_text().splitlines() if attempts.exists() else []
    return result, tried


def test_the_arrival_partition_is_the_only_one_with_noexec(tmp_path):
    """Split by provenance, not by trust. noexec refuses mmap(PROT_EXEC) as
    well as execve, so a blanket rule would block emulator cores loaded with
    dlopen() and nd-apprun's own app.so."""
    _, media = mount_attempts(tmp_path, "try_mount", "/dev/mmcblk1p1")
    _, arrival = mount_attempts(
        tmp_path, "mount_untrusted", "/dev/mmcblk1p1",
        extra_stubs='candidates() { echo /dev/mmcblk1p2; }\n'
                    'is_untrusted_partition() { return 0; }\n')

    assert arrival, "the arrival partition was never mounted"
    for attempt in arrival:
        assert "noexec" in attempt, attempt
    for attempt in media:
        assert "noexec" not in attempt, attempt


def test_the_media_side_can_be_traversed_but_not_listed(tmp_path):
    """dmask=0026 is 0751, the same trick /NeoDCT/User uses: ndusr_ut walks
    through "sdcard" to reach "untrusted" and `ls` of the music is EACCES."""
    _, tried = mount_attempts(tmp_path, "try_mount", "/dev/mmcblk1p1")
    vfat = [a for a in tried if "-t vfat" in a]

    assert vfat, tried
    assert "uid=1000" in vfat[0] and "gid=1000" in vfat[0], vfat[0]
    assert "dmask=0026" in vfat[0], vfat[0]
    assert "fmask=0137" in vfat[0], vfat[0]


def test_the_arrival_side_is_written_by_one_user_and_read_by_the_other(tmp_path):
    """uid=ndusr_ut so the browser can write, gid=ndusr so the core can read
    -- because the plan's own workflow is that installing a download is an
    explicit copy the owner performs through the UI, and the UI is ndusr."""
    _, tried = mount_attempts(
        tmp_path, "mount_untrusted", "/dev/mmcblk1p1",
        extra_stubs='candidates() { echo /dev/mmcblk1p2; }\n'
                    'is_untrusted_partition() { return 0; }\n')

    assert tried
    assert "uid=1001" in tried[0], tried[0]
    assert "gid=1000" in tried[0], tried[0]


def test_an_image_with_no_users_still_mounts_the_card(tmp_path):
    """No ndusr means no uid= option rather than a guessed number: the vfat
    driver takes numbers only, so a name would be ignored and the mount would
    belong to root while looking as though it did not."""
    attempts = tmp_path / "attempts"
    result = sh(tmp_path, 'try_mount /dev/mmcblk1p1',
                stubs='mount() { echo "$*" >> "%s"; return 0; }\n'
                      'mountpoint() { return 1; }\nblkid() { return 1; }\n'
                      'id() { return 1; }\n' % attempts)

    tried = attempts.read_text().splitlines()
    assert result.returncode == 0
    assert tried and "uid=" not in tried[0], tried[0]
    assert "nosuid" in tried[0] and "nodev" in tried[0], tried[0]


def test_ownership_is_not_forced_onto_an_ext_card(tmp_path):
    """An ext card carries real uids. Forcing uid= would make every file on
    it belong to ndusr whoever wrote it, and the kernel rejects the option
    there anyway -- which would turn "someone else's ext card" from
    "mountable" into "unmountable"."""
    _, tried = mount_attempts(tmp_path, "try_mount", "/dev/mmcblk1p1")

    for attempt in tried:
        if "ext4" in attempt or "ext3" in attempt or "ext2" in attempt:
            assert "uid=" not in attempt, attempt
            assert "dmask" not in attempt, attempt


# --- identifying the arrival partition -----------------------------------

def fat32_image(path, label, size=MIB):
    """A boot sector that says what mkfs.vfat's says, and nothing else."""
    body = bytearray(b"\x00" * size)
    body[0:3] = b"\xeb\x58\x90"
    body[71:82] = label.ljust(11).encode()
    body[82:90] = b"FAT32   "
    body[510:512] = b"\x55\xaa"
    path.write_bytes(bytes(body))
    return path


def test_the_label_is_read_out_of_the_boot_sector(tmp_path):
    """Not from blkid. Busybox's blkid reported NOTHING for a plain mkfs.vfat
    card once already, which is why try_mount asks the kernel rather than
    asking it -- the same reasoning applies here."""
    result = sh(tmp_path, 'fat_label "%s"'
                % fat32_image(tmp_path / "p2.img", "NEODCTUT"))

    assert result.stdout.strip() == "NEODCTUT"


def test_a_media_partition_is_not_mistaken_for_an_arrival_one(tmp_path):
    fat32_image(tmp_path / "p1.img", "NEODCT")

    result = sh(tmp_path, 'is_untrusted_partition "%s" && echo YES || echo NO'
                % (tmp_path / "p1.img"))

    assert result.stdout.strip() == "NO"


def test_something_that_is_not_fat32_has_no_label(tmp_path):
    """Eleven bytes at offset 71 of an arbitrary filesystem are eleven
    arbitrary bytes. Checking the FAT32 signature is what stops them reading
    as a volume label."""
    (tmp_path / "other.img").write_bytes(b"NEODCTUT" * 128)

    result = sh(tmp_path, 'is_untrusted_partition "%s" && echo YES || echo NO'
                % (tmp_path / "other.img"))

    assert result.stdout.strip() == "NO"


@pytest.mark.parametrize("disk,part,want", [
    ("/dev/mmcblk1", 1, "/dev/mmcblk1p1"),
    ("/dev/mmcblk1", 2, "/dev/mmcblk1p2"),
    ("/dev/sda", 1, "/dev/sda1"),
    ("/dev/vdc", 2, "/dev/vdc2"),
    ("/dev/loop0", 1, "/dev/loop0p1"),
])
def test_partition_paths_are_spelled_the_way_the_kernel_spells_them(
        tmp_path, disk, part, want):
    """mmcblk, nvme and loop take a "p"; sd and vd do not. Getting this wrong
    means mkfs writes to a device that does not exist -- or worse, one that
    does."""
    result = sh(tmp_path, 'partition_path %s %d' % (disk, part))

    assert result.stdout.strip() == want


def test_partition_path_is_the_inverse_of_parent_disk(tmp_path):
    """The two have to agree, because parent_disk() is what decides whether a
    device is off limits and partition_path() is what decides where mkfs
    writes."""
    for disk in ("/dev/mmcblk1", "/dev/sda", "/dev/vdc", "/dev/loop0"):
        result = sh(tmp_path, 'parent_disk "$(partition_path %s 1)"' % disk)

        assert result.stdout.strip() == disk


# --- the safety property that must survive all of this -------------------

def test_formatting_still_refuses_the_disk_the_phone_runs_from(tmp_path):
    """do_format now resolves what it was given to a whole DISK, because a
    partition table is written to one. That must not become a way to reach
    the system disk through one of its partitions."""
    for target in ("/dev/vda", "/dev/vda1", "/dev/vdb", "/dev/vdb2"):
        result = sh(tmp_path, 'do_format %s' % target)

        assert result.returncode != 0, target
        assert "refusing" in result.stderr, target

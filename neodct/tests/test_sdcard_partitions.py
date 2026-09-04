"""A NeoDCT card is ONE ext4 partition, and the ownership on it is the storage
half of the confinement design.

It was two FAT32 partitions until 0.5.0b, and the reason it is not any more is
worth stating because it is the reason this file lost half its tests. A FAT
filesystem has NO ownership: chown and chmod do nothing on one, permissions
come from uid=/gid=/fmask=/dmask= mount options, and those apply to the whole
filesystem at once. "Downloads are writable by ndusr_ut and the owner's music
is not" therefore needed two filesystems, because it could not be said inside
one. ext4 records owner, group and mode per inode and says all of it in a
single directory tree -- so the second partition, its NEODCTUT label, the
arithmetic that split the card three-to-one and the guard that stopped the
arrival side being mounted as the media side all went away together. What
replaced them is neodct-sdcard's CARD_LAYOUT, and that is tested next door in
test_sdcard_layout.py.

What is left here is the partition table, which still has to be written by
hand: there is no sfdisk, no parted and no batch-mode fdisk in the image, so
do_format() writes the 446th to 512th bytes of the card itself. That is 66
bytes of a format that has not changed since 1983, and the tests below check
them against it -- and, where the machine allows it, hand them to the actual
Linux partition scanner through a loop device and ask what it found.

The mount options are here too, for the one card shape that still has any: a
FAT card the phone did not make. An ext card gets none, because it carries its
own ownership, which is the entire point.
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
# The 1 MiB alignment every partition on a card starts and ends on, in
# sectors. The helper spells it ALIGN_SECTORS; this is the same number, and
# test_the_alignment_is_a_megabyte below pins the two together rather than
# trusting the copy.
ALIGN = MIB // SECTOR

# Root plus loop devices, which is what it takes to ask the kernel about a
# partition table rather than to read the bytes back ourselves. A container
# usually has neither, and that is a fact about the machine and not about the
# table, so those tests skip rather than fail.
NEEDS_LOOP = pytest.mark.skipif(
    os.geteuid() != 0 or not os.path.exists("/dev/loop-control")
    or shutil.which("losetup") is None or shutil.which("blockdev") is None,
    reason="needs root and loop devices to ask the real partition scanner",
)

# reread_partitions() calls partprobe, which belongs to parted and is not on
# every host -- it is not on this one. It is a wrapper around one ioctl and
# says nothing about the table, so it is stubbed with blockdev(8), which asks
# the kernel the same thing. What must not be stubbed is anything that decides
# WHERE the partition goes; that is the whole point of the test below.
REREAD_STUB = 'reread_partitions() { sync; blockdev --rereadpt "$1"; }\n'


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


# --- the arithmetic that is left ------------------------------------------
# partition_plan() is gone with the second partition. What do_format() does
# now is: start at ALIGN_SECTORS, and take everything to the end of the disk,
# rounded down. So the only arithmetic left is align_down(), and it is worth
# its own tests because a partition that ends past the end of the card is a
# card the kernel truncates reads on.

def align_down(tmp_path, value):
    result = sh(tmp_path, 'align_down %d' % value)
    assert result.returncode == 0, result.stderr
    return int(result.stdout.strip())


def test_the_alignment_is_a_megabyte(tmp_path):
    """Pinned rather than assumed. An SD card's erase blocks want 1 MiB and a
    partition that straddles one performs like a floppy -- and every number
    below is derived from this one, so a drift here would make the rest of
    this file agree with the helper about the wrong thing."""
    result = sh(tmp_path, 'echo $ALIGN_SECTORS')

    assert int(result.stdout.strip()) == ALIGN


@pytest.mark.parametrize("sectors", [
    2 * ALIGN,              # exactly two megabytes
    2 * ALIGN + 1,          # one sector over
    3 * ALIGN - 1,          # one sector short
    64 * MIB // SECTOR,
    256 * MIB // SECTOR,
    64 * 1024 * MIB // SECTOR,   # a 64 GB card, and 32-bit arithmetic
])
def test_alignment_rounds_down_and_never_up(tmp_path, sectors):
    """Down, and only down. Rounding up by one megabyte is a partition whose
    last sector is off the end of the card, which reads as a filesystem that
    was fine until the day something used the last of it."""
    got = align_down(tmp_path, sectors)

    assert got % ALIGN == 0, got
    assert got <= sectors, got
    assert sectors - got < ALIGN, "it rounded down further than it had to"


# --- the table itself ----------------------------------------------------

def test_the_written_table_is_a_partition_table(tmp_path):
    """66 bytes of a format that has not changed since 1983, written by hand
    because the image has no partitioner that can be scripted.

    THREE arguments now, not five. The second entry was the arrival partition
    and it is a directory on the ext4 filesystem instead, so entries two,
    three and four have to be empty -- a card whose table still claims a
    second partition is one a PC will offer to repair."""
    image = tmp_path / "card.img"
    image.write_bytes(b"\xee" * (2 * MIB))

    result = sh(tmp_path, 'write_mbr "%s" 2048 260096' % image)

    assert result.returncode == 0, result.stderr
    entries = parse_mbr(image.read_bytes()[:512])
    assert entries[0] == (0x83, 2048, 260096), entries
    assert entries[1] == (0, 0, 0), "a second partition that no longer exists"
    assert entries[2] == (0, 0, 0) and entries[3] == (0, 0, 0), "junk in 3 and 4"


def test_the_one_partition_says_linux_and_not_fat32(tmp_path):
    """0x83, and the byte is not decoration.

    It was 0x0C (FAT32 with LBA) when the card was FAT. A PC that reads the
    table decides what to try mounting from it, so an ext4 filesystem inside a
    partition that claims to be FAT32 is a card a desktop offers to REPAIR --
    which is a dialog box away from a card with nothing on it."""
    image = tmp_path / "card.img"
    image.write_bytes(b"\x00" * (2 * MIB))

    sh(tmp_path, 'write_mbr "%s" 2048 1024' % image)

    ptype, _, _ = parse_mbr(image.read_bytes()[:512])[0]
    assert ptype == 0x83, hex(ptype)


def test_the_table_is_exactly_one_sector_with_the_signature_on_the_end(tmp_path):
    """512 bytes: 446 of nothing, four 16-byte entries, and 0x55 0xAA.

    The count is asserted rather than assumed because the helper assembles the
    table in a temp file precisely so that it can check the length before
    anything touches the card -- see the comment on write_mbr(), and the pipe
    bug it is there to have caught. This is the same check, from outside."""
    image = tmp_path / "card.img"
    image.write_bytes(b"\x11" * (2 * MIB))

    sh(tmp_path, 'write_mbr "%s" 2048 1024' % image)

    body = image.read_bytes()
    assert body[510:512] == b"\x55\xaa", "no signature"
    # 446 zero bytes, then the one entry, then 48 more zeros. Everything the
    # helper wrote, counted.
    assert body[:446] == b"\x00" * 446, "boot code where there should be none"
    assert body[462:510] == b"\x00" * 48, "entries two to four are not empty"
    assert body[512] == 0x11, "it wrote past the first sector"


def test_a_table_that_did_not_come_out_512_bytes_is_not_written(tmp_path):
    """The guard that makes the one function here that can destroy a card
    refuse rather than half-succeed.

    The bug it exists for: dd reads a pipe with one read() per block and a
    read() from a pipe returns whatever happens to be in it, so `dd bs=512
    count=1` on the far end of the assembly took the first 446 bytes and
    stopped -- a partition table with no partitions in it, written to a card,
    silently. mbr_entry is stubbed short here to produce the same shape of
    wrongness on purpose."""
    image = tmp_path / "card.img"
    image.write_bytes(b"\xab" * (2 * MIB))

    result = sh(tmp_path, 'write_mbr "%s" 2048 1024' % image,
                stubs='mbr_entry() { printf "short"; }')

    assert result.returncode != 0, "it wrote a table it could not measure"
    assert set(image.read_bytes()) == {0xAB}, "it touched the card anyway"


def test_writing_the_table_does_not_touch_anything_else(tmp_path):
    """conv=notrunc, and one sector. A format that also zeroed the card would
    take minutes and wear the flash for nothing."""
    image = tmp_path / "card.img"
    image.write_bytes(b"\xab" * (2 * MIB))

    sh(tmp_path, 'write_mbr "%s" 2048 1024' % image)

    body = image.read_bytes()
    assert len(body) == 2 * MIB, "the image was truncated"
    assert set(body[512:]) == {0xAB}, "it wrote past the first sector"


# --- and what the kernel makes of it --------------------------------------

@NEEDS_LOOP
def test_linux_itself_reads_the_table_back(tmp_path):
    """The one that matters. Everything above checks the bytes against the
    format as documented; this hands them to the actual partition scanner and
    asks what it found.

    do_format() is driven for real as far as the table, with only the mkfs and
    the mount stubbed out -- so the arithmetic that decides where the
    partition starts and ends is the helper's own and not a copy of it here.
    That arithmetic is now three lines rather than a function, which is
    exactly why it is worth exercising through the thing that uses it.

    Skipped rather than failed where the kernel reserves no partition minors
    for loop devices -- a container, usually -- because that is a fact about
    the machine and not about the table."""
    image = tmp_path / "card.img"
    with open(image, "wb") as handle:
        handle.truncate(256 * MIB)

    loop = subprocess.run(["losetup", "--show", "-f", "-P", str(image)],
                          capture_output=True, text=True, check=True).stdout.strip()
    try:
        result = sh(tmp_path, 'do_format %s' % loop,
                    stubs=REREAD_STUB
                          + 'mkfs_ext4() { echo "$1" > "%s"; }\n'
                            'try_mount() { return 0; }\n' % (tmp_path / "mkfs"))
        assert result.returncode == 0, result.stderr

        name = os.path.basename(loop)
        part = "/sys/block/%s/%sp1" % (name, name)
        if not os.path.isdir(part):
            pytest.skip("this kernel makes no partition nodes for loop devices")

        start = int(open(part + "/start").read())
        size = int(open(part + "/size").read())
        assert start == ALIGN, "the table itself needs the first megabyte"
        assert size % ALIGN == 0, size
        assert start + size <= 256 * MIB // SECTOR, "the partition runs off the end"
        # Everything that is left, not a share of it: there is no second
        # partition to leave room for any more.
        assert 256 * MIB // SECTOR - (start + size) < ALIGN, size

        # ...and the filesystem went on the PARTITION, not on the whole disk.
        assert (tmp_path / "mkfs").read_text().strip() == loop + "p1"
        assert "one ext4 partition" in result.stderr, result.stderr
    finally:
        subprocess.run(["losetup", "-d", loop], capture_output=True)


@NEEDS_LOOP
def test_an_image_with_no_mke2fs_refuses_rather_than_making_a_fat_card(tmp_path):
    """An image built without e2fsprogs cannot make a NeoDCT card at all, and
    the honest answer is to say so.

    Falling back to mkfs.vfat would produce a card that MOUNTS -- and then
    cannot hold an app, cannot give a download anywhere to land, and reports
    itself as a working card while doing none of it. The failure being
    invisible is what makes it worth refusing out loud."""
    image = tmp_path / "card.img"
    with open(image, "wb") as handle:
        handle.truncate(32 * MIB)

    loop = subprocess.run(["losetup", "--show", "-f", str(image)],
                          capture_output=True, text=True, check=True).stdout.strip()
    try:
        result = sh(tmp_path, 'do_format %s' % loop,
                    stubs='command() { return 1; }\n')

        assert result.returncode != 0
        assert "no mke2fs in this image" in result.stderr, result.stderr
        state = (tmp_path / "run" / "sdcard.prop").read_text()
        assert "state=unformatted" in state, state
    finally:
        subprocess.run(["losetup", "-d", loop], capture_output=True)


# --- the mount options, for the one card shape that still has any ---------

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


def options_of(attempt):
    """The -o field of one recorded `mount` call, as a list.

    Split out rather than searched for in the whole line, because the line
    ends in the mountpoint -- which under pytest is a directory named after
    the test. A test called ..._is_noexec therefore made `"noexec" not in
    attempt` false by existing, which is a test that can only pass by being
    renamed."""
    fields = attempt.split()
    return fields[fields.index("-o") + 1].split(",")


def test_no_card_mount_is_noexec(tmp_path):
    """THE OPTION THAT MUST NOT COME BACK, and the one this file used to
    insist on.

    noexec was the arrival partition's, and the split it belonged to was by
    PROVENANCE: things the owner copied on could run, things that arrived on
    their own could not. That partition is a directory now, and a directory
    cannot carry its own mount options -- so the card is mounted once, for
    everything on it.

    It has to be mounted exec, and that is not a weakening. Apps the owner
    installed live at /NeoDCT/User/sdcard/apps, nd-apprun reaches app.so with
    dlopen(), and noexec refuses mmap(PROT_EXEC) exactly as it refuses
    execve() -- so a noexec card is a phone on which no installed app will
    start, including the emulator cores the media player loads the same way.

    What carries the boundary instead is ownership: apps/ is 0755 ndusr:ndusr,
    so an untrusted process can read and execute what is there and cannot add
    to it or change it. That is a stronger statement than noexec ever made,
    because it distinguishes running code from writing it. See
    test_sdcard_layout.py."""
    _, tried = mount_attempts(tmp_path, "try_mount", "/dev/mmcblk1p1")

    assert tried, "nothing was mounted at all"
    for attempt in tried:
        assert "noexec" not in options_of(attempt), attempt


def test_the_safety_options_are_on_every_attempt(tmp_path):
    """nosuid,nodev, on the one filesystem whose contents were chosen by
    whoever last held it and which udev mounts on insertion. FAT could not
    represent a setuid bit or a device node in the first place; ext4 can, so
    on a card the phone now formats these are the only thing between a crafted
    card and a root shell rather than defence in depth."""
    _, tried = mount_attempts(tmp_path, "try_mount", "/dev/mmcblk1p1")

    for attempt in tried:
        options = options_of(attempt)
        assert "nosuid" in options and "nodev" in options, attempt


def test_a_fat_card_is_still_mounted_traversable_but_not_listable(tmp_path):
    """dmask=0026 is 0751, the same "traverse but do not list" trick
    /NeoDCT/User uses.

    Only FAT gets this. It is either somebody else's card or a NeoDCT card
    from before 0.5.0b, and in both cases mount options are the only way to
    say anything about it at all -- so the old shape is kept for the old
    filesystem rather than dropped along with the partition it was designed
    around."""
    _, tried = mount_attempts(tmp_path, "try_mount", "/dev/mmcblk1p1")
    vfat = [a for a in tried if "-t vfat" in a]

    assert vfat, tried
    options = options_of(vfat[0])
    assert "uid=1000" in options and "gid=1000" in options, vfat[0]
    assert "dmask=0026" in options, vfat[0]
    assert "fmask=0137" in options, vfat[0]


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
    assert tried, "nothing was mounted at all"
    options = options_of(tried[0])
    assert not [o for o in options if o.startswith("uid=")], tried[0]
    assert "nosuid" in options and "nodev" in options, tried[0]


def test_ownership_is_not_forced_onto_an_ext_card(tmp_path):
    """An ext card carries real uids, which is now the whole reason the phone
    formats one. Forcing uid= would make every file on it belong to ndusr
    whoever wrote it -- and the kernel rejects the option there anyway, which
    would turn a NeoDCT card from "mounted" into "unmountable"."""
    _, tried = mount_attempts(tmp_path, "try_mount", "/dev/mmcblk1p1")

    for attempt in tried:
        if not [t for t in ("ext4", "ext3", "ext2") if ("-t " + t) in attempt]:
            continue
        for option in options_of(attempt):
            assert not option.startswith("uid="), attempt
            assert not option.startswith("dmask"), attempt
            assert not option.startswith("fmask"), attempt


# --- telling one kind of card from another --------------------------------

def fat32_image(path, label, size=MIB):
    """A boot sector that says what mkfs.vfat's says, and nothing else."""
    body = bytearray(b"\x00" * size)
    body[0:3] = b"\xeb\x58\x90"
    body[71:82] = label.ljust(11).encode()
    body[82:90] = b"FAT32   "
    body[510:512] = b"\x55\xaa"
    path.write_bytes(bytes(body))
    return path


def card_is_ours(tmp_path, setup):
    """Run card_is_ours() against a mountpoint `setup` has prepared."""
    mount = tmp_path / "sdcard"
    mount.mkdir(parents=True, exist_ok=True)
    setup(mount)
    result = sh(tmp_path, 'card_is_ours && echo YES || echo NO',
                env={"NEODCT_SDCARD_MOUNT": str(mount)})
    return result.stdout.strip()


def test_a_card_of_ours_is_known_by_its_marker(tmp_path):
    """And NOT by its volume label.

    This decision was gated on `label_of`, which is blkid -- the reader this
    helper's own comments record as having returned NOTHING for a perfectly
    good FAT32 card, which is why try_mount asks the kernel whether something
    mounts rather than asking blkid what it is.

    Depending on it again, for the decision that applies the whole layout,
    would mean a card whose label blkid could not read silently kept whatever
    modes it arrived with -- and on removable media that is the difference
    between the confinement being enforced and merely being intended. The card
    is already mounted when this is asked, so the filesystem is right there.
    """
    assert card_is_ours(tmp_path, lambda m: (m / ".neodct").write_text("")) == "YES"


def test_a_card_from_an_older_neodct_is_known_by_its_folders(tmp_path):
    """The marker is new, so a card the phone formatted before 0.5.0b does not
    carry one -- nor does one a person made on a computer by following the SD
    card help. Recognising the folder set is what stops those being treated as
    strangers' cards and left unlaid-out."""
    def older_card(m):
        for folder in ("wallpapers", "tones", "music"):
            (m / folder).mkdir()
    assert card_is_ours(tmp_path, older_card) == "YES"


def test_a_strangers_card_is_not_ours(tmp_path):
    """The case that matters in the other direction. Chowning a card the phone
    merely found would be it quietly taking ownership of somebody's
    photographs, so an empty card, and a card with unrelated things on it, must
    both answer no."""
    assert card_is_ours(tmp_path, lambda m: None) == "NO"

    def holiday_photos(m):
        (m / "DCIM").mkdir()
        (m / "music").mkdir()      # one NeoDCT-ish name is not the set
    assert card_is_ours(tmp_path, holiday_photos) == "NO"


def test_a_freshly_formatted_card_is_ours_before_anything_is_on_it(tmp_path):
    """do_format's case, and the one moment neither test above can cover: the
    filesystem is seconds old and empty, so there is no marker and no folder
    set to recognise. FRESHLY_FORMATTED is how the phone says it knows,
    because it has just made the thing."""
    mount = tmp_path / "sdcard"
    mount.mkdir(parents=True, exist_ok=True)
    result = sh(tmp_path, 'FRESHLY_FORMATTED=1; card_is_ours && echo YES || echo NO',
                env={"NEODCT_SDCARD_MOUNT": str(mount)})
    assert result.stdout.strip() == "YES"


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
    does. Only partition 1 is ever made now, but the function is still asked
    for arbitrary numbers by reread_partitions() and by the tests, so both
    spellings stay pinned."""
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

def test_formatting_the_card_works_while_the_card_is_mounted(tmp_path):
    """The state a real card is ALWAYS in when somebody asks to format it.

    is_reserved_device() refuses anything mounted, which is right for
    candidates() -- looking for a card to mount, and uninterested in one that
    already is -- and was catastrophic for do_format. A card is mounted; that
    is what makes it a card rather than a slot. So the phone answered

        refusing to format /dev/vda1: the phone is running from it

    to every format of a working card, and the owner saw "Formatting failed.
    The card may be write protected."

    Which took the FAT-to-ext4 migration with it: the legacy card the UI
    itself offers to convert is, necessarily, mounted.

    The whole host suite missed it because every do_format test drives an
    unmounted loop device -- the one state a real card is never in at the
    moment of asking. So this test's entire point is the mount table.
    """
    card = tmp_path / "card.img"
    card.write_bytes(b"\0" * (8 * 1024 * 1024))
    table = tmp_path / "mounts-card"
    table.write_text("%s %s vfat rw 0 0\n" % (card, tmp_path / "sdcard"))

    result = sh(tmp_path, 'do_format %s' % card,
                env={"NEODCT_MOUNTS": str(table)},
                stubs='mkfs_ext4() { return 0; }\n'
                      'try_mount() { return 0; }\n'
                      'reread_partitions() { return 0; }\n'
                      'umount() { return 0; }\n')

    # It gets PAST the reserved gate, which is the whole property. It then
    # stops at "not a block device", because a regular file is not one -- and
    # that is the proof: the refusal that used to come first no longer does.
    assert "refusing" not in result.stderr, result.stderr
    assert "not a block device" in result.stderr, result.stderr


def test_formatting_still_refuses_a_device_mounted_somewhere_else(tmp_path):
    """The half of the mounted check that was worth keeping. A device in use
    at a mountpoint that is NOT the card's is something the phone is actually
    running on, and pointing mkfs at it is the accident the check exists for.
    """
    card = tmp_path / "card.img"
    card.write_bytes(b"\0" * (8 * 1024 * 1024))
    table = tmp_path / "mounts-elsewhere"
    table.write_text("%s /NeoDCT/User ext4 rw 0 0\n" % card)

    result = sh(tmp_path, 'do_format %s' % card,
                env={"NEODCT_MOUNTS": str(table)})

    assert result.returncode != 0
    assert "refusing" in result.stderr


def test_formatting_still_refuses_the_disk_the_phone_runs_from(tmp_path):
    """do_format resolves what it was given to a whole DISK, because a
    partition table is written to one. That must not become a way to reach the
    system disk through one of its partitions."""
    for target in ("/dev/vda", "/dev/vda1", "/dev/vdb", "/dev/vdb2"):
        result = sh(tmp_path, 'do_format %s' % target)

        assert result.returncode != 0, target
        assert "refusing" in result.stderr, target

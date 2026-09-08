"""The emulator's NAND: the parts of it a host can check without booting.

Everything here has a booted counterpart -- run_qemu.sh's NAND mode was proven
end to end on this kernel, twice, including a session boundary -- and these are
the halves that can run in the pytest suite on a machine with no QEMU: the
de-interleave the persistence rests on, the constants that have to agree with
docs/PARTITIONS.md, and the several ways the QEMU-only flasher could quietly
stop being QEMU-only.
"""

import gzip
import importlib.util
import os
import re
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(REPO, "neodct", "tools")
RUN_QEMU = os.path.join(TOOLS, "run_qemu.sh")
MKNAND = os.path.join(TOOLS, "mknand.sh")
QEMU_MACHINE = os.path.join(TOOLS, "qemu_machine.sh")
NDFLASH = os.path.join(REPO, "neodct", "initramfs", "qemu", "ndflash")
UBIATTACH_C = os.path.join(REPO, "neodct", "src", "tools", "nd_ubiattach.c")
SRC_MAKEFILE = os.path.join(REPO, "neodct", "src", "Makefile")


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mkqemuflash = _load("mkqemuflash", os.path.join(TOOLS, "mkqemuflash.py"))


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


# --------------------------------------------------------------------------
# The de-interleave, which is what /NeoDCT/User's persistence rests on
# --------------------------------------------------------------------------

def test_a_fully_programmed_partition_comes_back_byte_for_byte(tmp_path):
    """The plain case: every page written, so the image is the data planes.

    This was checked by hand against a real cache file first -- the same
    de-interleave over the system partition reproduced mknand.sh's system.ubi
    exactly -- and a hand check is not a gate.
    """
    stride = mkqemuflash.PAGE_STRIDE
    pages = 8
    original = bytes((i * 7 + 1) % 251 for i in range(pages * mkqemuflash.PAGE_BYTES))
    cache = tmp_path / "cache.raw"
    with open(cache, "wb") as handle:
        for page in range(pages):
            data = original[page * mkqemuflash.PAGE_BYTES:(page + 1) * mkqemuflash.PAGE_BYTES]
            handle.write(data + b"\xa5" * mkqemuflash.OOB_BYTES)
    assert cache.stat().st_size == pages * stride
    with open(cache, "rb") as handle:
        image, written = mkqemuflash.deinterleave(handle, 0, pages)
    assert written == pages
    assert image == original


def test_a_page_that_was_never_programmed_comes_back_as_0xff(tmp_path):
    """The rule the whole mechanism turns on.

    The cache file starts sparse, so a page nandsim never wrote reads as 2112
    zero bytes. Handing those back as 0x00 does not give UBI an empty erase
    block -- it gives it one whose erase-counter header is garbage, which UBI
    reports as corrupted. 0xFF is what blank NAND reads as and what UBI is
    looking for.
    """
    stride = mkqemuflash.PAGE_STRIDE
    cache = tmp_path / "cache.raw"
    with open(cache, "wb") as handle:
        handle.write(b"\x00" * stride)                       # never programmed
        handle.write(b"UBI#" + b"\x00" * (stride - 4))       # programmed
        handle.write(b"\x00" * stride)                       # never programmed
    with open(cache, "rb") as handle:
        image, written = mkqemuflash.deinterleave(handle, 0, 3)
    assert written == 1
    page = mkqemuflash.PAGE_BYTES
    assert image[:page] == b"\xff" * page
    assert image[page:page + 4] == b"UBI#"
    assert image[2 * page:] == b"\xff" * page


def test_a_short_cache_file_is_flash_that_was_never_touched(tmp_path):
    """A sparse file can simply end early, and everything past it is blank."""
    cache = tmp_path / "cache.raw"
    cache.write_bytes(b"UBI#" + b"\x00" * (mkqemuflash.PAGE_STRIDE - 4))
    with open(cache, "rb") as handle:
        image, written = mkqemuflash.deinterleave(handle, 0, 4)
    assert written == 1
    assert len(image) == 4 * mkqemuflash.PAGE_BYTES
    assert image[mkqemuflash.PAGE_BYTES:] == b"\xff" * (3 * mkqemuflash.PAGE_BYTES)


def test_save_refuses_to_write_a_partition_nothing_ever_programmed(tmp_path):
    """An all-blank save would look like a session that lost its files.

    It is not an error -- a boot that never mounted anything really does leave
    the chip blank -- so the answer is to leave the previous save alone and
    say so, which is what run_qemu.sh prints.
    """
    cache = tmp_path / "cache.raw"
    cache.write_bytes(b"")
    out = tmp_path / "userdata.img"
    path, written = mkqemuflash.save_partition(str(cache), str(out), "userdata")
    assert path is None and written == 0
    assert not out.exists()


# --------------------------------------------------------------------------
# The geometry, against docs/PARTITIONS.md and mknand.sh
# --------------------------------------------------------------------------

def test_the_partition_span_is_the_phones_table():
    """userdata is the fifth partition and 64 erase blocks of 128 KiB.

    Getting this wrong does not fail loudly: the save would lift somebody
    else's partition out of the cache file, hand it back as userdata.ubi, and
    UBI would refuse to attach a `boot` partition as a userdata volume with no
    line anywhere saying which file was wrong.
    """
    first, count = mkqemuflash.partition_span("userdata")
    assert first == (2 + 2 + 4 + 128) * 64
    assert count == 64 * 64
    assert count * mkqemuflash.PAGE_BYTES == 8 * 1024 * 1024


def test_the_cmdline_cuts_the_chip_the_way_the_table_says():
    """nandsim.parts= and mkqemuflash.py's table are the same six partitions.

    They are two spellings of docs/PARTITIONS.md, in two languages, and a boot
    where they disagree saves the wrong region.
    """
    machine = read(QEMU_MACHINE)
    match = re.search(r"nandsim\.parts=([0-9,]+)", machine)
    assert match, "nd_qemu_append() no longer sets nandsim.parts"
    sizes = [int(n) for n in match.group(1).split(",")]
    assert sizes == [pebs for _, pebs in mkqemuflash.PARTITION_PEBS]
    # FIVE and not six. Six produces seven partitions -- nandsim gives the
    # 3 MiB of bad-block slack to a trailing one the phone has no name for --
    # and class.mtd then has a cardinality the phone can never match.
    assert len(sizes) == 5


def test_mknand_leb_size_is_the_peb_less_two_headers():
    """LEB_SIZE, PEB_SIZE and VID_OFFSET are three constants with one degree
    of freedom, and the one that matters is measurable: 126,976 is what a real
    attach of a real mknand.sh image reports."""
    body = read(MKNAND)
    def const(name):
        match = re.search(r"^%s=(\S+)" % name, body, re.M)
        assert match, name
        return int(match.group(1), 0)
    assert const("PEB_SIZE") == 131072
    assert const("MIN_IO") == 2048
    assert const("VID_OFFSET") == 2048
    assert const("LEB_SIZE") == const("PEB_SIZE") - 2 * const("VID_OFFSET")
    assert const("LEB_SIZE") == 126976


def test_userdata_max_leb_is_what_ubi_actually_leaves():
    """It was 56 for the whole life of the immutable design, and it is 40.

    UBI takes 24 of the 8 MiB partition's 64 erase blocks -- 2 for the layout
    volume, 20 for the bad-PEB reserve, 2 for wear levelling -- so an
    autoresizing volume tops out at 40. Measured on a real attach:

        ubi1: volume 0 ("userdata") re-sized from 13 to 40 LEBs
        ubi1: total reserved PEBs: 64, PEBs reserved for bad PEB handling: 20

    A ubifs superblock built with -c 56 claims a maximum its volume cannot
    have. It mounts, which is why nobody saw it.
    """
    body = read(MKNAND)
    max_leb = int(re.search(r"^USERDATA_MAX_LEB=(\d+)", body, re.M).group(1))
    userdata_bytes = 8 * 1024 * 1024
    assert max_leb == 40
    # The partition really is 64 erase blocks, which is the half of this that
    # follows from PARTITIONS.md and can be asserted from a file.
    assert userdata_bytes // 131072 == 64
    # The other half -- that UBI keeps 24 of them -- is NOT asserted against a
    # constant here, and that is the correction. `max_leb == 64 - 24` reads
    # like arithmetic and is really the emulator kernel's
    # CONFIG_MTD_UBI_BEB_LIMIT and layout-volume reserve written down twice; a
    # phone whose UBI reserves a different number would make this test agree
    # with a wrong file. The measurement lives in mknand.sh's comment, where
    # it is labelled as this kernel's, next to the A/B that shows -c 56 and
    # -c 40 mount and fill identically on this geometry.


# --------------------------------------------------------------------------
# The flasher cannot collide with the phone, and cannot reach a phone
# --------------------------------------------------------------------------

def test_the_flashing_drives_do_not_wear_the_system_or_user_serial():
    """find_system_device()'s first rule is device_by_serial("NDSYS") and it
    outranks everything else, so a raw UBI image labelled NDSYS would be
    mounted as the system partition. NDUSER is the same trap one level down.
    """
    body = read(RUN_QEMU)
    # EVERY serial in the storage block, not just the ones that already start
    # NDNAND. The loop here used to be
    #
    #     flashing = re.findall(r"serial=(NDNAND\w*)", body)
    #     for serial in flashing: assert serial not in ("NDSYS", ...)
    #
    # which cannot fail: findall had already filtered to names beginning
    # NDNAND, so none of them could equal NDSYS. It read like the guard and
    # asserted nothing; a rename to NDSYS would have been caught only by the
    # membership test above it going missing, which is a different failure
    # with a different message.
    blocks = [m.group(1) for m in
              re.finditer(r"^case \"\$STORAGE\" in$(.*?)^esac$", body, re.M | re.S)
              if "virtio-blk-device" in m.group(1)]
    assert len(blocks) == 1, (
        "expected exactly one `case \"$STORAGE\"` block attaching drives, "
        "found %d" % len(blocks))
    serials = re.findall(r"serial=(\w+)", blocks[0])
    flashing = [name for name in serials if name not in ("NDSYS", "NDUSER")]
    assert set(flashing) == {"NDNANDSYS", "NDNANDUSR"}, (
        "the drives holding raw UBI images are %s; every one of them must "
        "carry a serial find_system_device() will not match" % sorted(set(flashing))
    )
    # And the cache drive, which is attached outside the case block.
    assert re.search(r"serial=NDNAND\b", body), "the cache drive lost its serial"


def test_the_flashing_images_cannot_be_mistaken_for_a_squashfs():
    """The other half of the same guard: find_system_device()'s later rules
    scan block devices for the squashfs magic. A ubinize output begins "UBI#",
    which is why the scan cannot pick one up -- and mknand.sh is where that
    stays true."""
    body = read(MKNAND)
    assert "ubinize" in body
    # ubinize writes an EC header first and its magic is UBI#. The scan looks
    # for hsqs. This is a documentation assertion with teeth only in that it
    # fails if the images stop being ubinize output.
    assert "mode=ubi" in body


def test_the_cache_drive_is_the_last_virtio_blk_on_the_command_line():
    """nandsim opens /dev/vda by literal path at device_initcall, and QEMU
    enumerates virtio-mmio in reverse, so the cache has to be attached last.
    A new -drive appended below it would silently take the name."""
    body = read(RUN_QEMU)
    cache_at = body.index("serial=NDNAND\n")
    # EVERY virtio-blk on the line, found rather than listed. Comparing the
    # positions of five names nobody has added yet cannot see the sixth: a new
    # -device with a new serial appended below the cache is exactly the change
    # this rule exists to stop, and a hard-coded list is blind to it.
    later = [m.group(1) for m in re.finditer(r"virtio-blk-device,[^\"\n]*serial=(\w+)", body)
             if m.start() > cache_at]
    assert later == [], (
        "%s is attached after the cache drive, which takes /dev/vda away from "
        "nandsim -- see the block comment above it in run_qemu.sh" % later
    )
    for other in ("serial=NDSYS", "serial=NDUSER", "serial=NDCARD",
                  "serial=NDNANDSYS", "serial=NDNANDUSR"):
        assert body.index(other) < cache_at, (
            "%s is attached after the cache drive, which takes /dev/vda away "
            "from nandsim" % other)


def test_the_flasher_refuses_before_it_programs_anything():
    """The serial check has to come before the first dd, because that is what
    makes a reordered -device list harmless rather than destructive."""
    body = read(NDFLASH)
    assert body.index("NDNAND") < body.index("dd if=")


def test_the_cache_node_is_checked_before_devtmpfs_hides_it():
    """The worse of the two cache_file failures, and the only guard that can
    catch it.

    `nandsim.cache_file=` is opened with O_CREAT, so a path that does not exist
    gets a REGULAR FILE created there -- in the initramfs, which is RAM. The
    chip then works, writes run at 5 MB/s, no slab appears and nothing says a
    word, while the whole chip sits in the guest's own memory. Measured.

    /sys/block/vda/serial cannot catch it: sysfs is populated by the driver
    whatever nandsim opened. The node can, but only before devtmpfs is mounted
    over /dev -- after that, the file nandsim made is hidden and a real vda is
    in its place. So the test is on the ORDER of two lines.
    """
    body = read(NDFLASH)
    guard = body.index("[ ! -b /dev/vda ]")
    assert guard < body.index("mount -t devtmpfs")
    assert "OOM-panic" in body[guard:guard + 600]


def test_the_flasher_is_not_packed_into_the_phones_initramfs():
    """mkinitramfs.py copies FILES out of neodct/initramfs/ and skips
    directories, which is the whole reason the flasher lives in a
    subdirectory. A phone whose NAND is already flashed does not need a NAND
    flasher, and QEMU-only code inside the artefact the parity harness exists
    to keep honest is exactly what that harness is for.
    """
    mkinitramfs = read(os.path.join(REPO, "neodct", "scripts", "mkinitramfs.py"))
    assert "if os.path.isfile(source):" in mkinitramfs
    assert os.path.isdir(os.path.dirname(NDFLASH))
    assert os.path.basename(os.path.dirname(NDFLASH)) == "qemu"


def test_nd_ubiattach_is_built_and_never_installed():
    """It does one ioctl the phone has no use for. The Makefile builds it so
    that it gets the project's warnings and the target toolchain; installing
    it would put a NAND attach tool on a phone."""
    body = read(SRC_MAKEFILE)
    assert "$(BINDIR)/nd-ubiattach" in body
    install = body[body.index("\ninstall: all"):]
    assert "nd-ubiattach" not in install


def test_nd_ubiattach_reports_the_number_the_kernel_wrote_back():
    """UBI_IOCATT returns 0 and put_user()s the assigned device number into
    the first field of the caller's struct. Printing the return value instead
    says "attached as ubi0" for an attach dmesg records as ubi1, and somebody
    then builds neodct.user=ubi0:userdata on it."""
    body = read(UBIATTACH_C)
    assert "req.ubi_num" in body.split("printf(")[-1]


# --------------------------------------------------------------------------
# The overlay archive
# --------------------------------------------------------------------------

def test_the_overlay_carries_a_dev_vda_block_node(tmp_path):
    """nandsim opens the cache file before devtmpfs is mounted, so the node
    has to be in the archive -- and cpio(1) cannot create one without
    CAP_MKNOD, which is the reason this packer exists at all.
    """
    flasher = tmp_path / "ndflash"
    flasher.write_text("#!/bin/sh\nexec /init\n")
    tool = tmp_path / "nd-ubiattach"
    tool.write_bytes(b"\x7fELF")
    out = tmp_path / "ndflash.cpio.gz"
    mkqemuflash.build_overlay(str(out), str(flasher), str(tool))

    raw = gzip.decompress(out.read_bytes())
    assert b"dev/vda\0" in raw
    assert b"ndflash\0" in raw
    assert b"bin/nd-ubiattach\0" in raw
    assert raw.rstrip(b"\0").endswith(b"TRAILER!!!")

    # The header for dev/vda: mode says block device, rdevmajor is virtio_blk's.
    offset = raw.index(b"dev/vda\0") - 110
    header = raw[offset:offset + 110].decode("ascii")
    assert header[:6] == "070701"
    mode = int(header[14:22], 16)
    assert mode & 0o170000 == 0o60000, "dev/vda is not a block device"
    assert int(header[78:86], 16) == mkqemuflash.VIRTIO_BLK_MAJOR
    assert int(header[86:94], 16) == 0


def test_the_overlay_is_byte_identical_for_identical_input(tmp_path):
    """run_qemu.sh rebuilds it on every boot. An archive that differed run to
    run would make the machine look changed to anything that hashes it."""
    flasher = tmp_path / "ndflash"
    flasher.write_text("#!/bin/sh\nexec /init\n")
    tool = tmp_path / "nd-ubiattach"
    tool.write_bytes(b"\x7fELF")
    first = tmp_path / "a.cpio.gz"
    second = tmp_path / "b.cpio.gz"
    mkqemuflash.build_overlay(str(first), str(flasher), str(tool))
    mkqemuflash.build_overlay(str(second), str(flasher), str(tool))
    assert first.read_bytes() == second.read_bytes()


def test_the_real_overlay_unpacks_with_cpio(tmp_path):
    """The one failure mode a hand-written newc writer has is a field in the
    wrong place, and the kernel does not diagnose it: it finds no /init and
    panics with "Unable to mount root fs", which reads as a broken kernel.
    Measured -- that is exactly what the first version of this packer did.
    """
    if not any(os.access(os.path.join(d, "cpio"), os.X_OK)
               for d in os.environ.get("PATH", "").split(os.pathsep)):
        pytest.skip("no cpio(1) on this host")
    tool = tmp_path / "nd-ubiattach"
    tool.write_bytes(b"\x7fELF\x01\x01\x01")
    out = tmp_path / "ndflash.cpio.gz"
    mkqemuflash.build_overlay(str(out), NDFLASH, str(tool))
    unpacked = tmp_path / "out"
    unpacked.mkdir()
    subprocess.run("gzip -dc %s | cpio -idm --quiet" % out, shell=True,
                   cwd=unpacked, check=True)
    assert (unpacked / "ndflash").read_text() == read(NDFLASH)
    assert (unpacked / "bin" / "nd-ubiattach").read_bytes() == tool.read_bytes()
    assert os.access(unpacked / "ndflash", os.X_OK)


# --------------------------------------------------------------------------
# The storage modes, and the thing that stops the fast path
# --------------------------------------------------------------------------

def test_the_default_storage_mode_is_the_nand():
    body = read(RUN_QEMU)
    assert 'STORAGE="${NEODCT_STORAGE:-nand}"' in body


def test_the_virtio_mode_says_what_it_is_not_testing():
    """A banner is the weakest of the four guards and it still has to exist:
    the other three are a capture that cannot be a baseline, a capture script
    that refuses, and verity_state.prop naming the device."""
    body = read(RUN_QEMU)
    marker = 'if [ "$STORAGE" = "virtio" ]; then'
    assert marker in body
    banner = body[body.index(marker):body.index(marker) + 800]
    assert "user_is_ubi()" in banner
    assert "cannot be a parity baseline" in banner


def test_the_capture_script_refuses_a_virtio_boot():
    body = read(os.path.join(TOOLS, "parity_capture_qemu.sh"))
    assert 'NEODCT_STORAGE:-nand' in body
    assert "REFUSED" in body[body.index("NEODCT_STORAGE:-nand"):][:600]


def test_nand_full_states_its_memory_floor_rather_than_oom_panicking():
    """51 MB of system.ubi is 54,953 kB of unreclaimable nandsim slab -- more
    than the whole 53,824 kB a -m 64 guest has. Measured: the OOM killer takes
    the flash partway through and the guest panics, which reads as a broken
    image."""
    body = read(RUN_QEMU)
    assert '[ "$MEMORY" -lt 128 ]' in body
    refusal = body[body.index('[ "$MEMORY" -lt 128 ]'):][:1200]
    assert "54,953 kB" in refusal
    assert "NOT THE PHONE'S MEMORY" in refusal

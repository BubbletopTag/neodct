#!/usr/bin/env python3
"""mkqemuflash.py -- the two host-side jobs the emulator's NAND needs and
neither cpio(1) nor the shell can do.

    mkqemuflash.py overlay  --out ndflash.cpio.gz --flasher PATH --ubiattach PATH
    mkqemuflash.py save     --cache nandcache.raw --out userdata.ubi.saved
    mkqemuflash.py geometry                       # print the table it uses

============ 1. THE OVERLAY, AND THE BLOCK DEVICE NODE IN IT ============

nandsim opens ``nandsim.cache_file=`` from a ``__init`` function at
device_initcall time.  The initramfs cpio is already unpacked by then --
populate_rootfs() is a rootfs_initcall -- but **devtmpfs is not mounted**:
CONFIG_DEVTMPFS_MOUNT only applies through prepare_namespace(), which an
initramfs boot skips, exactly as neodct/initramfs/init's own comment says.
So the path that parameter names has to be a device node baked into the
archive.  Measured working: a ``dev/vda`` block node at major 254, which is
what virtio_blk registers on this kernel, and virtio_blk initialises before
nandsim (dmesg order confirms it).

``cpio -o`` cannot create that node without CAP_MKNOD, and
neodct/scripts/mkinitramfs.py packs with plain cpio, so the archive is
written directly here instead.  A newc entry for a block device is a header
with rdevmajor/rdevminor and no body; there is nothing else to it.  No root,
no fakeroot, and byte-identical output for identical input.

============ 2. SAVING /NeoDCT/User ACROSS QEMU PROCESSES ============

The chip cannot persist by itself.  nandsim's pages_written bitmap is
vzalloc'd fresh on every boot, so next session every page reads 0xFF whatever
the cache file holds -- and a QEMU emulator where /NeoDCT/User resets on every
boot would silently make every boot a snapshot boot and kill the one workflow
the whole storage stack exists for: stage an update, reboot, let the initramfs
apply it.

The fix needs no guest tool, because the cache file is an exact page+OOB
interleave: nandsim writes at ``pos = row * pgszoob`` with pgszoob = 2048 + 64,
so lifting the first 2048 bytes of every 2112 gives back the partition image.
Verified against the system partition, where the same de-interleave reproduces
mknand.sh's own ``system.ubi`` byte for byte.

**A page that was never programmed has to come back as 0xFF, not 0x00.**  The
cache file starts sparse, so an untouched page reads as 2112 zero bytes, and
writing zeros into a UBI partition is not "empty" -- it is a PEB whose erase
counter header is garbage, which UBI reports as corrupted.  The rule used here
is that an all-zero 2112-byte record means "never programmed", and it is safe
because of the OOB half: the NAND core writes software ECC into every
programmed page's spare area, and nand_calculate_ecc() ends by INVERTING the
parity, so an all-zero data page carries 0xFF ECC bytes.  A programmed page
therefore cannot have an all-zero record.

Proven by round trip on this kernel, not by argument alone: a session wrote
marker.txt, 128 KiB of zeros and 256 KiB of random bytes into the ubifs, the
partition was saved with this code, and the next QEMU process restored it and
read all three back with identical checksums, 0 corrupted PEBs and UBI's erase
counters carried forward (max/mean 2/1).
"""

import argparse
import gzip
import os
import stat
import sys

# ---- the chip, from EMPIRICAL-FINDINGS 11 and docs/PARTITIONS.md ----------
#
# nandsim at 0x20/0xa1/0x00/0x15 is the Pico Mini's part: 128 MB, a 128 KiB
# erase block, a 2048-byte page and 64 bytes of OOB.  The partition table is
# the phone's, reproduced by `nandsim.parts=2,2,4,128,64` in qemu_machine.sh
# -- five values and not six, so the bad-block slack falls into the last
# partition instead of becoming an mtd6 the phone has no name for.
PAGE_BYTES = 2048
OOB_BYTES = 64
PAGE_STRIDE = PAGE_BYTES + OOB_BYTES        # nandsim's pgszoob
PEB_BYTES = 131072
PAGES_PER_PEB = PEB_BYTES // PAGE_BYTES     # 64

PARTITION_PEBS = [
    ("env", 2),
    ("idblock", 2),
    ("uboot", 4),
    ("boot", 128),
    ("userdata", 64),
    # rootfs takes the remainder; nandsim gives it whatever is left.
]

# virtio_blk's major on this kernel, read out of /proc/devices in a probe
# boot rather than assumed.  It is a fixed number for a built-in driver.
VIRTIO_BLK_MAJOR = 254


def partition_span(name):
    """(first page, page count) of one partition inside the cache file."""
    peb = 0
    for pname, pebs in PARTITION_PEBS:
        if pname == name:
            return peb * PAGES_PER_PEB, pebs * PAGES_PER_PEB
        peb += pebs
    raise KeyError("no partition called %r in the table" % name)


# ---- the newc cpio writer ------------------------------------------------

def _newc_entry(name, mode, data=b"", rdev_major=0, rdev_minor=0, ino=0):
    """One newc header plus its padded body.

    Field order is fixed by the format and getting it wrong is not diagnosed
    by the kernel: it simply finds no /init and panics with "Unable to mount
    root fs", which reads as a broken kernel rather than a broken archive.
    """
    fields = [
        "070701",
        "%08X" % ino,
        "%08X" % mode,
        "%08X" % 0,             # uid
        "%08X" % 0,             # gid
        "%08X" % 1,             # nlink
        "%08X" % 0,             # mtime -- zero, so the archive is reproducible
        "%08X" % len(data),
        "%08X" % 0,             # devmajor
        "%08X" % 0,             # devminor
        "%08X" % rdev_major,
        "%08X" % rdev_minor,
        "%08X" % (len(name) + 1),
        "%08X" % 0,             # check
    ]
    head = ("".join(fields) + name + "\0").encode()
    head += b"\0" * (-len(head) % 4)
    body = data + b"\0" * (-len(data) % 4)
    return head + body


def build_overlay(out_path, flasher, ubiattach):
    """Write the QEMU-only cpio the kernel concatenates onto initramfs.cpio.gz."""
    for path in (flasher, ubiattach):
        if not os.path.isfile(path):
            raise SystemExit("mkqemuflash: %s is missing" % path)

    blob = bytearray()
    ino = [100]

    def add(name, mode, data=b"", rdev=(0, 0)):
        ino[0] += 1
        blob.extend(_newc_entry(name, mode, data, rdev[0], rdev[1], ino[0]))

    # dev/ is created rather than assumed: the base initramfs has one, but a
    # concatenated archive is unpacked into whatever is there and an entry
    # whose parent is missing is skipped in silence.
    add("dev", stat.S_IFDIR | 0o755)
    add("dev/vda", stat.S_IFBLK | 0o600, rdev=(VIRTIO_BLK_MAJOR, 0))
    with open(flasher, "rb") as fh:
        add("ndflash", stat.S_IFREG | 0o755, fh.read())
    # In /bin because ndflash calls it by bare name, which means $PATH, which
    # means the same spelling works whether it came from a build tree or from
    # a one-off cross compile.
    add("bin", stat.S_IFDIR | 0o755)
    with open(ubiattach, "rb") as fh:
        add("bin/nd-ubiattach", stat.S_IFREG | 0o755, fh.read())
    blob.extend(_newc_entry("TRAILER!!!", 0, b""))

    # mtime=0 AND filename="" in the gzip header: run_qemu.sh rebuilds this on
    # every boot and a differing archive would look like a changed machine to
    # anything that hashes it. GzipFile(fileobj=...) stores fileobj.name in the
    # header unless told otherwise, so two builds of identical content into
    # differently-named files differ at byte 10. Caught by the test, not by
    # reading.
    with open(out_path, "wb") as fh:
        with gzip.GzipFile(filename="", fileobj=fh, mode="wb",
                           compresslevel=6, mtime=0) as gz:
            gz.write(bytes(blob))
    return out_path


# ---- the de-interleave ---------------------------------------------------

def deinterleave(cache, first_page, page_count):
    """Lift one partition's data planes out of a nandsim cache file.

    `cache` is a binary file object.  Returns the raw partition image, with
    0xFF for every page nandsim never programmed -- see this module's header
    for why that, and not 0x00, is the correct answer.
    """
    blank_record = bytes(PAGE_STRIDE)
    blank_page = b"\xff" * PAGE_BYTES
    out = bytearray()
    written = 0
    cache.seek(first_page * PAGE_STRIDE)
    for _ in range(page_count):
        record = cache.read(PAGE_STRIDE)
        if len(record) < PAGE_STRIDE:
            # A sparse file can simply end early. Everything past it is flash
            # that has never been touched.
            record = record.ljust(PAGE_STRIDE, b"\0")
        if record == blank_record:
            out += blank_page
        else:
            out += record[:PAGE_BYTES]
            written += 1
    return bytes(out), written


def save_partition(cache_path, out_path, partition):
    first, count = partition_span(partition)
    with open(cache_path, "rb") as fh:
        image, written = deinterleave(fh, first, count)
    if written == 0:
        # Nothing was ever programmed there. Writing the file anyway would
        # hand the next boot an all-0xFF partition, which is a correct empty
        # chip -- but saying so is what tells the caller the session did not
        # get as far as mounting anything.
        return None, 0
    # Written beside the destination and renamed, NEVER in place. `open(...,
    # "wb")` truncates the previous save before a byte of the new one exists,
    # so anything that interrupts an 8 MiB write -- ENOSPC on the images
    # directory, which shares a filesystem with the 132 MB sparse cache file,
    # or a signal -- leaves userdata.nand.img short. run_qemu.sh reports every
    # non-zero exit from here as "nothing was written ... left as it was",
    # which would then be exactly false: the next boot's `-nt` test still
    # picks the truncated file, ndflash dd's it onto mtd4, and the guest
    # reports corrupted PEBs or a failed ubifs mount two sessions after the
    # cause. os.replace() is atomic within a directory, so the previous save
    # survives every failure that is not a successful new one.
    tmp_path = out_path + ".tmp"
    try:
        with open(tmp_path, "wb") as fh:
            fh.write(image)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp_path, out_path)
    except BaseException:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        raise
    return out_path, written


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    ov = sub.add_parser("overlay", help="write the QEMU-only flasher cpio")
    ov.add_argument("--out", required=True)
    ov.add_argument("--flasher", required=True)
    ov.add_argument("--ubiattach", required=True)

    sv = sub.add_parser("save", help="lift a partition out of a nandsim cache file")
    sv.add_argument("--cache", required=True)
    sv.add_argument("--out", required=True)
    sv.add_argument("--partition", default="userdata")

    sub.add_parser("geometry", help="print the chip and partition table")

    args = ap.parse_args(argv)

    if args.cmd == "overlay":
        print(build_overlay(args.out, args.flasher, args.ubiattach))
        return 0

    if args.cmd == "save":
        if not os.path.isfile(args.cache):
            print("mkqemuflash: no cache file at %s" % args.cache, file=sys.stderr)
            return 1
        path, written = save_partition(args.cache, args.out, args.partition)
        if path is None:
            print("mkqemuflash: nothing was ever written to the %s partition;"
                  " leaving %s alone" % (args.partition, args.out), file=sys.stderr)
            return 1
        print("mkqemuflash: saved %s (%d programmed pages) -> %s"
              % (args.partition, written, path))
        return 0

    print("page %d + oob %d = stride %d, PEB %d = %d pages"
          % (PAGE_BYTES, OOB_BYTES, PAGE_STRIDE, PEB_BYTES, PAGES_PER_PEB))
    peb = 0
    for index, (name, pebs) in enumerate(PARTITION_PEBS):
        print("  mtd%d %-9s PEB %3d..%-3d  %9d bytes"
              % (index, name, peb, peb + pebs - 1, pebs * PEB_BYTES))
        peb += pebs
    print("  mtd%d %-9s PEB %3d..     the remainder"
          % (len(PARTITION_PEBS), "rootfs", peb))
    return 0


if __name__ == "__main__":
    sys.exit(main())

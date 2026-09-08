#!/bin/sh
# Turn a finished luckfox build into images flashable to raw NAND.
#
#   neodct/tools/mknand.sh [--userdata-only] <images-dir> <target-dir> [host-dir]
#
# Produces, in <images-dir>:
#   system.img     padded squashfs + appended dm-verity hash tree (as qemu)
#   system.ubi     system.img as a UBI static volume "system"  -> rootfs mtd
#   userdata.ubi   an empty ubifs volume "userdata"            -> userdata mtd
#
# --userdata-only stops after userdata.ubi and takes no <target-dir>: the
# emulator's default storage mode flashes that volume and leaves the system
# image where it is, and building a 50 MB squashfs and its verity tree to get
# a 2 MB ubifs would put ten seconds on every boot for nothing.
#
# ============ THIS FILE'S OUTPUT IS NOW BOOTED, NOT JUST BUILT ============
#
# run_qemu.sh's NAND storage mode calls this script and raw-flashes what it
# produces onto a simulated chip with the Pico Mini's geometry, so the
# constants below are checked by a machine rather than by reading. That is how
# USERDATA_MAX_LEB was found to be wrong -- see its comment -- and it had been
# wrong for the whole life of the immutable design.
#
# Why UBI at all: the Pico Mini has no block devices, only raw NAND behind
# MTD. UBI gives wear levelling and bad-block handling, and MTD_UBI_BLOCK
# then exposes a volume as /dev/ubiblockN_M -- a real block device, which is
# what squashfs and dm-verity both need. Nothing here runs on the phone:
# there is no ubiattach/ubinize in the target, so attaching is the kernel's
# job via ubi.mtd= and ubi.block= on the cmdline.
#
# Geometry must match the chip exactly or the volume will not attach. These
# are the same numbers buildroot uses for rootfs.ubi (see the luckfox
# defconfig): 128KiB erase blocks, 2048-byte pages and sub-pages.
set -eu

USERDATA_ONLY=""
SKEL_IN=""
while [ $# -gt 0 ]; do
    case "$1" in
        --userdata-only) USERDATA_ONLY=1; shift ;;
        # The caller's own userdata skeleton, so that the volume carries the
        # factory .ndsys/installed.prop. Without it dm-verity has no root hash
        # to check on first boot and an `enforce` boot goes to recovery, which
        # is why post-image-neodct.sh passes the same skeleton it puts inside
        # userdata.ext4 rather than letting this script build a bare one.
        --skel) SKEL_IN="${2:-}"; shift 2 ;;
        *) break ;;
    esac
done

IMAGES="${1:-}"
TARGET="${2:-}"
HOST="${3:-}"

HERE="$(cd "$(dirname "$0")" && pwd)"
NEODCT_DIR="$(dirname "$HERE")"

say() { echo "[mknand] $*"; }
die() { echo "[mknand] $*" >&2; exit 1; }

[ -n "$IMAGES" ] && [ -d "$IMAGES" ] \
    || die "usage: mknand.sh [--userdata-only] <images-dir> <target-dir> [host-dir]"
if [ -z "$USERDATA_ONLY" ]; then
    [ -n "$TARGET" ] && [ -d "$TARGET" ] || die "target dir '$TARGET' is not a directory"
fi
[ -n "$HOST" ] || HOST="$(dirname "$IMAGES")/host"

# ============ WHICH mtd-utils, AND WHY THAT DEPENDS ON THE CALLER ==========
#
# Buildroot's own host tools, always, for the images that go on a phone. This
# script is the assembler for the flash: `upgrade_tool di -rootfs system.ubi`
# over a maskrom cable, and a system volume that will not attach needs the
# cable back to fix (docs/FLASHING.md). Building it with whatever mtd-utils
# the build host happens to carry is not a trade this script gets to make on
# the operator's behalf -- check_fits() is a hard `die` for the same reason.
#
# THE $PATH FALLBACK IS FOR --userdata-only AND NOTHING ELSE. That is the
# emulator's call and it has exactly one caller: post-image-neodct.sh builds a
# 2 MB ubifs volume for a simulated chip, on a tree whose defconfig need not
# carry the UBIFS/UBI block that pulls host-mtd (D5 is about the LUCKFOX one)
# -- and refusing there would mean the emulator could not flash the phone's
# own userdata volume on a machine with perfectly good mtd-utils installed.
# Nothing that fallback builds ever reaches a phone; run_qemu.sh only reads
# the file, and says so itself when it is missing.
#
# It was unconditional for one revision, and the only signal was one `using
# ... (not buildroot's)` line in a wall of build output. Reproduced: with a
# host dir that did not exist AT ALL, a full run selected /usr/sbin/ubinize
# and carried on -- where the same command used to stop at "host dir not
# found". Hence the refusal below, restored, and scoped.
find_tool() {
    if [ -x "$HOST/sbin/$1" ]; then
        echo "$HOST/sbin/$1"
    elif [ -n "$USERDATA_ONLY" ] && command -v "$1" > /dev/null 2>&1; then
        command -v "$1"
    fi
}
if [ -z "$USERDATA_ONLY" ]; then
    [ -d "$HOST" ] || die "host dir '$HOST' not found -- pass it as the third argument"
fi
UBINIZE="$(find_tool ubinize)"
MKFS_UBIFS="$(find_tool mkfs.ubifs)"
if [ -n "$USERDATA_ONLY" ]; then
    [ -n "$UBINIZE" ] || die "no ubinize in $HOST/sbin and none on \$PATH (package mtd-utils)"
    [ -n "$MKFS_UBIFS" ] || die "no mkfs.ubifs in $HOST/sbin and none on \$PATH (package mtd-utils)"
    case "$UBINIZE" in
        "$HOST/sbin/"*) ;;
        *) say "using $UBINIZE (not buildroot's) -- userdata.ubi only, for the emulator" ;;
    esac
else
    [ -n "$UBINIZE" ] || die "no ubinize in $HOST/sbin. These images go on a phone, so
           the tools the rootfs was built with are the only ones accepted here;
           --userdata-only takes a system mtd-utils, the full run does not."
    [ -n "$MKFS_UBIFS" ] || die "no mkfs.ubifs in $HOST/sbin (see the note above)"
fi

# --- chip geometry --------------------------------------------------------
PEB_SIZE=0x20000        # erase block
MIN_IO=0x800            # page / minimum I/O
SUB_PAGE=2048
VID_OFFSET=2048
LEB_SIZE=0x1f000        # PEB minus two 2048-byte headers

# Partition sizes from mtdparts (see docs/HARDWARE_NOTES.md). The userdata
# volume's max LEB count has to suit *its* partition, not the rootfs one --
# a ubifs superblock claiming more LEBs than the volume has will not mount.
USERDATA_BYTES=$((8 * 1024 * 1024))
SYSTEM_BYTES=$((100 * 1024 * 1024))
# 8 MiB / 128 KiB = 64 erase blocks, less what UBI keeps for itself. It said
# 56 with the note "56 leaves room for both without cutting it fine", and that
# was a guess; 40 is the number UBI actually leaves, MEASURED on a real attach
# of this file's own output at this file's own geometry:
#
#   ubi1: volume 0 ("userdata") re-sized from 13 to 40 LEBs
#   ubi1: available PEBs: 0, total reserved PEBs: 64,
#         PEBs reserved for bad PEB handling: 20
#
# UBI takes 24 of the 64 PEBs -- 2 for the layout volume, 20 for the bad-PEB
# reserve and 2 for wear levelling -- so an autoresizing volume can never
# reach more than 40, and a superblock built -c 56 claims a maximum its volume
# cannot have.
#
# WHAT THAT COSTS IS NOTHING, AND THAT IS MEASURED TOO, because the first
# version of this comment said it was a functional defect and it is not. Two
# volumes built from the same skeleton, one -c 56 and one -c 40, flashed to
# the same nandsim mtd4 on this repo's own armv7 kernel and attached at VID
# offset 2048:
#
#                       -c 56                        -c 40
#   attach         re-sized from 13 to 40 LEBs   re-sized from 13 to 40 LEBs
#   FS size        3809280 bytes, 30 LEBs        3809280 bytes, 30 LEBs
#   superblock max 56 LEBs                       max 40 LEBs
#   journal        1269760 bytes (10 LEBs)       1142785 bytes (8 LEBs)
#   df             2636K total / 2608K avail     2636K total / 2608K avail
#   ENOSPC after   84 x 32K files                84 x 32K files
#
# So both mount, both give the same usable space and both fill at the same
# file. 40 is here because it is the truth and the superblock should not claim
# a maximum it cannot reach -- NOT because a phone in the field is broken. It
# is not, no reflash is warranted, and the earlier "BLAST RADIUS" paragraph
# saying otherwise would have sent devices through maskrom for nothing.
#
# AND THE 24 IS THIS KERNEL'S, NOT NECESSARILY THE PHONE'S. The bad-PEB
# reserve comes from CONFIG_MTD_UBI_BEB_LIMIT and the RV1103 SDK's 5.10 config
# is not in this repository (DECISIONS D6), so 64 - 24 is an emulator
# measurement carried across, in the same way every hw column in
# tests/parity/allow.txt is `hw_evidence: unmeasured-claim`. If the phone's
# UBI reserves more, -c 40 over-claims exactly as -c 56 did here -- with, on
# tonight's evidence, exactly as little effect. The first hardware attach
# settles it: the number is in the boot log as "max N LEBs".
USERDATA_MAX_LEB=40

WORK="$IMAGES/.mknand"
rm -rf "$WORK"
mkdir -p "$WORK"

# --- the userdata skeleton ------------------------------------------------
if [ -n "$SKEL_IN" ]; then
    [ -d "$SKEL_IN" ] || die "--skel '$SKEL_IN' is not a directory"
    SKEL="$SKEL_IN"
else
    SKEL="$WORK/userdata-skel"
    mkdir -p "$SKEL/db" "$SKEL/logs" "$SKEL/.ndsys" "$SKEL/.pycache" \
             "$SKEL/.seedrng" "$SKEL/sdcard" "$SKEL/tones" "$SKEL/wallpapers"
fi

# --- system.img: squashfs + verity ---------------------------------------
# mkupdate builds exactly the same image the qemu path flashes, so an update
# package and a freshly flashed phone are byte-identical systems.
if [ -z "$USERDATA_ONLY" ]; then
    SQUASHFS="$IMAGES/rootfs.squashfs"
    [ -f "$SQUASHFS" ] || die "no rootfs.squashfs in $IMAGES -- enable BR2_TARGET_ROOTFS_SQUASHFS"

    "$NEODCT_DIR/tools/mkupdate.py" \
        --images-dir "$IMAGES" \
        --target-dir "$TARGET" \
        --image-only \
        --installed-prop "$SKEL/.ndsys"

    [ -f "$IMAGES/system.img" ] || die "mkupdate did not produce system.img"
    say "system.img $(du -h "$IMAGES/system.img" | cut -f1)"
fi

# --- system.ubi -----------------------------------------------------------
# A static volume: its size is the image's size, so ubiblock presents exactly
# the squashfs plus its hash tree and dm-verity can read to the end.
if [ -z "$USERDATA_ONLY" ]; then
    cat > "$WORK/system.cfg" <<EOF
[system]
mode=ubi
vol_id=0
vol_type=static
vol_name=system
vol_alignment=1
image=$IMAGES/system.img
EOF

    "$UBINIZE" -o "$IMAGES/system.ubi" \
        -m "$MIN_IO" -p "$PEB_SIZE" -s "$SUB_PAGE" -O "$VID_OFFSET" \
        "$WORK/system.cfg"
    say "system.ubi $(du -h "$IMAGES/system.ubi" | cut -f1) (volume 'system', static)"
fi

# --- userdata.ubi ---------------------------------------------------------
# Dynamic and autoresizing so it grows into whatever the partition really is.
"$MKFS_UBIFS" -d "$SKEL" \
    -e "$LEB_SIZE" -c "$USERDATA_MAX_LEB" -m "$MIN_IO" -x none -F \
    -o "$WORK/userdata.ubifs"

cat > "$WORK/userdata.cfg" <<EOF
[userdata]
mode=ubi
vol_id=0
vol_type=dynamic
vol_name=userdata
vol_alignment=1
vol_flags=autoresize
image=$WORK/userdata.ubifs
EOF

"$UBINIZE" -o "$IMAGES/userdata.ubi" \
    -m "$MIN_IO" -p "$PEB_SIZE" -s "$SUB_PAGE" -O "$VID_OFFSET" \
    "$WORK/userdata.cfg"
say "userdata.ubi $(du -h "$IMAGES/userdata.ubi" | cut -f1) (volume 'userdata', ubifs)"

# --- fit checks -----------------------------------------------------------
# Flashing something larger than its partition is the one mistake here that
# is not recoverable over serial, so refuse rather than warn.
check_fits() {
    size="$(stat -c %s "$1")"
    if [ "$size" -gt "$2" ]; then
        die "$(basename "$1") is $size bytes, larger than its ${2}-byte partition"
    fi
    say "$(basename "$1"): $size bytes fits in $2"
}
[ -n "$USERDATA_ONLY" ] || check_fits "$IMAGES/system.ubi" "$SYSTEM_BYTES"
check_fits "$IMAGES/userdata.ubi" "$USERDATA_BYTES"

rm -rf "$WORK"
if [ -n "$USERDATA_ONLY" ]; then
    say "done -- userdata.ubi only, for run_qemu.sh's NAND storage mode"
else
    say "done -- flash system.ubi to the rootfs partition, userdata.ubi to userdata"
fi

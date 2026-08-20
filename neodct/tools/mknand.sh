#!/bin/sh
# Turn a finished luckfox build into images flashable to raw NAND.
#
#   neodct/tools/mknand.sh <images-dir> <target-dir> [host-dir]
#
# Produces, in <images-dir>:
#   system.img     padded squashfs + appended dm-verity hash tree (as qemu)
#   system.ubi     system.img as a UBI static volume "system"  -> rootfs mtd
#   userdata.ubi   an empty ubifs volume "userdata"            -> userdata mtd
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

IMAGES="${1:-}"
TARGET="${2:-}"
HOST="${3:-}"

HERE="$(cd "$(dirname "$0")" && pwd)"
NEODCT_DIR="$(dirname "$HERE")"

say() { echo "[mknand] $*"; }
die() { echo "[mknand] $*" >&2; exit 1; }

[ -n "$IMAGES" ] && [ -d "$IMAGES" ] || die "usage: mknand.sh <images-dir> <target-dir> [host-dir]"
[ -n "$TARGET" ] && [ -d "$TARGET" ] || die "target dir '$TARGET' is not a directory"
[ -n "$HOST" ] || HOST="$(dirname "$IMAGES")/host"
[ -d "$HOST" ] || die "host dir '$HOST' not found -- pass it as the third argument"

UBINIZE="$HOST/sbin/ubinize"
MKFS_UBIFS="$HOST/sbin/mkfs.ubifs"
[ -x "$UBINIZE" ] || die "no ubinize in $HOST/sbin"
[ -x "$MKFS_UBIFS" ] || die "no mkfs.ubifs in $HOST/sbin"

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
# 8MiB / 128KiB = 64 erase blocks, less UBI's layout volume and its bad-block
# reserve. 56 leaves room for both without cutting it fine.
USERDATA_MAX_LEB=56

WORK="$IMAGES/.mknand"
rm -rf "$WORK"
mkdir -p "$WORK"

# --- system.img: squashfs + verity ---------------------------------------
# mkupdate builds exactly the same image the qemu path flashes, so an update
# package and a freshly flashed phone are byte-identical systems.
SQUASHFS="$IMAGES/rootfs.squashfs"
[ -f "$SQUASHFS" ] || die "no rootfs.squashfs in $IMAGES -- enable BR2_TARGET_ROOTFS_SQUASHFS"

SKEL="$WORK/userdata-skel"
mkdir -p "$SKEL/db" "$SKEL/logs" "$SKEL/.ndsys" "$SKEL/.pycache" \
         "$SKEL/.seedrng" "$SKEL/sdcard" "$SKEL/tones" "$SKEL/wallpapers"

"$NEODCT_DIR/tools/mkupdate.py" \
    --images-dir "$IMAGES" \
    --target-dir "$TARGET" \
    --image-only \
    --installed-prop "$SKEL/.ndsys"

[ -f "$IMAGES/system.img" ] || die "mkupdate did not produce system.img"
say "system.img $(du -h "$IMAGES/system.img" | cut -f1)"

# --- system.ubi -----------------------------------------------------------
# A static volume: its size is the image's size, so ubiblock presents exactly
# the squashfs plus its hash tree and dm-verity can read to the end.
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
check_fits "$IMAGES/system.ubi" "$SYSTEM_BYTES"
check_fits "$IMAGES/userdata.ubi" "$USERDATA_BYTES"

rm -rf "$WORK"
say "done -- flash system.ubi to the rootfs partition, userdata.ubi to userdata"

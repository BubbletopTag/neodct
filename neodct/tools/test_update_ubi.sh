#!/bin/sh
# Drive the update applier at a REAL UBI volume, the way the phone has one.
#
#   neodct/tools/test_update_ubi.sh
#
# WHY. Updates never installed on real hardware. The phone's system partition
# is a static UBI volume published as a READ-ONLY ubiblock disk
# (ubi.block=0,system neodct.sys=/dev/ubiblock0_0), and the applier wrote it
# with dd -- which cannot work, and whose failure path logs "retrying on the
# next boot" and boots the old system. Downloaded, rebooted, nothing changed.
#
# test_initramfs_apply.py proves the applier now CHOOSES ubiupdatevol, but it
# proves that against files: a stand-in tool and a stand-in device. What it
# cannot prove is that the real tool, against a real UBI volume, actually
# works -- and that is precisely the class of thing that has been passing on
# this side while failing on the phone for the entire life of the feature.
#
# So this makes a real one. nandsim gives the Pico Mini's actual part: the
# four ID bytes below are 0x20 0xa1 0x00 0x15, and 0x15 decodes as bits[1:0]
# = 01 -> 2 KiB page, bit[2] = 1 -> 16 spare bytes per 512, bits[5:4] = 01 ->
# 128 KiB erase block, over a 1 Gbit x8 part. Measured in the guest:
# size=134217728 erase=131072 write=2048 oob=64. UBI attaches to it and a
# static volume is created exactly the way mknand.sh creates the phone's.
#
# ============ WHY NOT mtdram, WHICH IS WHAT THIS USED TO DO ============
#
# Because the number that matters is the page size, and mtdram has none.
# Measured, both devices side by side on the armv7 kernel:
#
#                 min I/O (writesize)   UBI LEB size
#   mtdram                          1        130,944
#   nandsim                      2048        129,024   <- the phone's
#
# UBI reads min_io straight off the MTD, so on mtdram every LEB size, every
# VID header offset and every ubinize -O argument is arithmetic the phone
# will never do -- and LEB arithmetic is exactly what section B below is
# about. A harness that gets 130,944 where the phone gets 129,024 is not
# testing the phone.
#
# It also costs nothing. mtdram keeps its whole backing store in kernel
# memory, which is why this script used to demand NEODCT_MEM=256 for a
# 32 MB chip; nandsim allocates lazily, so the phone's whole 128 MB part sits
# on a 64 MB guest with MemTotal unchanged at 53,824 kB. mtdram is switched
# off entirely (mtdram.total_size=0) rather than left at its built-in 4 MB
# default, because that 4 MB is vmalloc'd out of the phone's own RAM.
#
# THOSE PARAMETERS ARE NO LONGER PASSED FROM HERE. They were a NEODCT_APPEND
# on the launch below and they are now in run_qemu.sh's own default cmdline,
# because the 4 MB and the wrong 16 KiB geometry were costing every ordinary
# session too, not just this one -- and a machine definition kept in two
# places drifts. What is NOT delegated is the assertion: the writesize check
# below fails loudly if the chip this boots is not a 2048-byte-page part, so
# the day that default changes, this harness says so rather than quietly
# measuring the wrong LEB arithmetic.
#
# The ubiblock disk is deliberately NOT created: the applier only uses that
# name to decide which writer to use, and everything real happens through the
# volume character device. Nothing here needs the read-only half.
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SP="${NEODCT_UBI_WORK:-/tmp/claude-1000/update-ubi}"

say()  { echo "ubi: $*"; }
fail() { echo "ubi: FAIL -- $*" >&2; exit 1; }

QEMU_PID=""; HOLDER_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$HOLDER_PID" ] && kill "$HOLDER_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

rm -rf "$SP"; mkdir -p "$SP/images"
for f in zImage initramfs.cpio.gz system.img userdata.ext4; do
    cp -f "$REPO/buildroot/output/images/$f" "$SP/images/$f"
done

ser="$SP/serial.fifo"; log="$SP/serial.log"

# ============ HOW THE APPLIER GETS IN ============
#
# ndsys-apply.sh lives ONLY in the initramfs -- after switch_root it is gone,
# so the booted phone has no copy to source. It used to ride in on the SD
# card; the armv7 kernel has no VFAT_FS, so nothing in the guest can mount
# one and that route is closed.
#
# It rides in as a RAW DISK instead. No filesystem is involved: the file is
# padded to a 512-byte multiple (QEMU will not open a raw image that is not),
# attached as an ordinary virtio-blk device, and read back out with one dd of
# exactly the original length. Verified byte-for-byte below against the
# host's sha256 rather than assumed, because a transport that delivers most
# of a shell script is worse than one that delivers none.
#
# The device is found in the guest by its disk SERIAL, the same way the
# applier itself finds the system partition -- virtio-mmio enumeration is not
# the order the drives are given, so no /dev/vdX name can be written down
# here.
APPLIER_SRC="$REPO/neodct/initramfs/ndsys-apply.sh"
APPLIER_BYTES="$(wc -c < "$APPLIER_SRC" | tr -d ' ')"
APPLIER_SHA="$(sha256sum "$APPLIER_SRC" | cut -d' ' -f1)"
cp -f "$APPLIER_SRC" "$SP/applier.raw"
truncate -s $(( (APPLIER_BYTES + 511) / 512 * 512 )) "$SP/applier.raw" \
    || fail "could not pad the applier disk"

rm -f "$ser"; : > "$log"; mkfifo "$ser"
sh -c 'while :; do sleep 900; done' > "$ser" & HOLDER_PID=$!

# The phone's own memory, deliberately: nandsim costs the guest nothing (see
# the header), so there is no reason for this harness to run on a machine the
# phone does not have. NEODCT_SD=none because the card is neither needed nor
# mountable.
NEODCT_IMAGES="$SP/images" NEODCT_DISPLAY=offscreen NEODCT_SD=none \
    NEODCT_QEMU_EXTRA="-drive file=$SP/applier.raw,if=none,format=raw,id=ndapply,readonly=on -device virtio-blk-device,drive=ndapply,serial=NDAPPLY" \
    "$REPO/neodct/tools/run_qemu.sh" < "$ser" >> "$log" 2>&1 &
QEMU_PID=$!

waited=0
while [ "$waited" -lt 240 ]; do
    grep -aq 'login:' "$log" 2>/dev/null && break
    sleep 2; waited=$((waited + 2))
done
grep -aq 'login:' "$log" 2>/dev/null || { tail -20 "$log"; fail "never booted"; }
say "booted in ${waited}s"
sleep 2
printf '\n' > "$ser"; sleep 2
printf 'root\n' > "$ser"; sleep 4

ask() { printf '%s\n' "$1" > "$ser"; sleep "${2:-4}"; }
grab() { tail -c +"$1" "$log" | sed 's/\r$//' | sed -n "s/^$2=//p" | tail -1; }

# --- the applier, off its own raw disk ------------------------------------
# Found by serial, read by length, and checked against the host's sha256.
# The check is the point: a short read leaves a shell script that still
# sources, runs some of its function definitions, and fails much later
# somewhere that looks like the applier's fault.
M=$(wc -c < "$log")
ask 'D=""; for e in /sys/block/*/serial; do [ "$(cat $e)" = NDAPPLY ] && D=/dev/$(basename $(dirname $e)); done; echo APPLYDEV=$D' 5
APPLYDEV="$(grab "$M" APPLYDEV)"
[ -n "$APPLYDEV" ] || { tail -c +"$M" "$log" | tail -20; fail "no disk with serial NDAPPLY (did NEODCT_QEMU_EXTRA reach QEMU?)"; }

M=$(wc -c < "$log")
ask "dd if=$APPLYDEV bs=$APPLIER_BYTES count=1 of=/tmp/ndsys-apply.sh 2>/dev/null; \
     echo APPLYSHA=\$(sha256sum /tmp/ndsys-apply.sh | cut -d' ' -f1)" 6
[ "$(grab "$M" APPLYSHA)" = "$APPLIER_SHA" ] \
    || { tail -c +"$M" "$log" | tail -20; fail "the applier did not arrive intact off $APPLYDEV"; }
say "applier delivered on $APPLYDEV, sha matches the host's"

# --- a real UBI device on a simulated NAND chip ---------------------------
# The geometry is the phone's, from neodct/tools/mknand.sh: 128 KiB erase
# blocks and 2048-byte pages. Getting this wrong does not fail loudly -- it
# just makes a volume with different LEB arithmetic than the phone's, and the
# size behaviour under test is exactly LEB arithmetic. So the page size is
# ASKED FOR rather than assumed: writesize is where UBI gets min_io, and 1
# instead of 2048 is the whole difference between this harness and the one
# that came before it.
M=$(wc -c < "$log")
ask 'N=$(sed -n "s/^mtd\([0-9]*\):.*NAND simulator.*/\1/p" /proc/mtd | head -1); echo NANDMTD=$N; echo NANDWRITE=$(cat /sys/class/mtd/mtd$N/writesize 2>/dev/null)' 6
NANDMTD="$(grab "$M" NANDMTD)"
[ -n "$NANDMTD" ] || { tail -c +"$M" "$log" | tail -20; fail "no nandsim device -- are run_qemu.sh's nandsim.*_id_byte arguments still in its default cmdline?"; }
[ "$(grab "$M" NANDWRITE)" = "2048" ] \
    || fail "mtd$NANDMTD has a $(grab "$M" NANDWRITE)-byte page; the phone's is 2048"

M=$(wc -c < "$log")
ask "ubiattach -m $NANDMTD -d 0 /dev/ubi_ctrl >/dev/null 2>&1; echo UBI=\$(ls -d /sys/class/ubi/ubi0 2>/dev/null | wc -l); echo LEB=\$(cat /sys/class/ubi/ubi0/eraseblock_size 2>/dev/null)" 6
[ "$(grab "$M" UBI)" = "1" ] || { tail -c +"$M" "$log" | tail -20; fail "ubiattach failed"; }
say "UBI attached to mtd$NANDMTD (nandsim), LEB $(grab "$M" LEB) bytes"

# --- a static volume, sized to the image, exactly as mknand.sh does -------
IMG_SIZE=1048576
M=$(wc -c < "$log")
ask "ubimkvol /dev/ubi0 -N system -t static -s $IMG_SIZE >/dev/null 2>&1; echo VOL=\$(ls /dev/ubi0_0 2>/dev/null | wc -l)" 5
[ "$(grab "$M" VOL)" = "1" ] || { tail -c +"$M" "$log" | tail -20; fail "ubimkvol failed"; }
say "static volume /dev/ubi0_0 created at $IMG_SIZE bytes"

# --- a package the applier will accept ------------------------------------
# Built on the phone so the sha256 in the record is the phone's own arithmetic
# over the phone's own bytes; a host-built package would also be testing that
# two sha256 implementations agree, which is not what is in doubt.
M=$(wc -c < "$log")
ask "dd if=/dev/urandom of=/tmp/img bs=4096 count=$((IMG_SIZE / 4096)) 2>/dev/null; \
     echo SHA=\$(sha256sum /tmp/img | cut -d' ' -f1)" 8
SHA="$(grab "$M" SHA)"
[ -n "$SHA" ] || fail "could not build a test image on the phone"
say "test image: $IMG_SIZE bytes, sha ${SHA%${SHA#??????????}}..."

# --- the staging record, in the applier's own spelling --------------------
ask 'mkdir -p /tmp/state /tmp/user/logs && rm -f /tmp/state/*.prop'
for line in \
    "image=/tmp/img" \
    "image_bytes=$IMG_SIZE" \
    "sha256=$SHA" \
    "version=9.9.9z" \
    "buildtime=1" \
    "platform=qemu-armv7" \
    "verity_root_hash=deadbeef" \
    "verity_block_size=4096" \
    "verity_image_blocks=256" \
    "verity_salt=00" \
    "attempts=0"
do
    ask "echo '$line' >> /tmp/state/pending.prop" 1
done

# The image must sit beside the record: apply_pending resolves it as
# $STATE_DIR/$(basename image), never the path the record names.
ask 'cp /tmp/img /tmp/state/img && sed -i "s|^image=.*|image=img|" /tmp/state/pending.prop; sync'

# --- run the REAL applier at the REAL volume ------------------------------
APPLIER=/tmp/ndsys-apply.sh

M=$(wc -c < "$log")
ask "STATE_DIR=/tmp/state MNT_USER=/tmp/user SYS_DEV=/dev/ubiblock0_0 USER_MOUNTED=1 \
     sh -c '. $APPLIER; apply_pending' 2>&1 | tail -6" 25

M2=$(wc -c < "$log")
ask 'echo RESULT=$(sed -n "s/^result=//p" /tmp/state/last_result.prop 2>/dev/null)'
ask 'echo VOLSHA=$(dd if=/dev/ubi0_0 bs=4096 count=256 2>/dev/null | sha256sum | cut -d" " -f1)'
RESULT="$(grab "$M2" RESULT)"
VOLSHA="$(grab "$M2" VOLSHA)"

echo
say "result=${RESULT:-none}"
say "volume sha=${VOLSHA:-none}"
say "wanted   =$SHA"
[ "$RESULT" = "ok" ] || fail "applier did not report ok"
[ "$VOLSHA" = "$SHA" ] || fail "the UBI volume does not hold the image"
say "PASS (A) -- a real static UBI volume was written by the real applier"

# ============ B: an image BIGGER than the volume it must go into ==========
#
# mknand.sh gives the phone's `system` volume no explicit size, so ubinize
# "assume[s] minimum to fit image" -- the volume is exactly as big as the
# image that was flashed. A static volume cannot take more than it was made
# for, and NeoDCT images grow: 0.4.4a added 2.7 MB when BlueZ arrived. So the
# very first over-the-air update that is larger than the flashed build would
# stop here, on a phone where the write path is otherwise perfect.
#
# This asks the question directly rather than reasoning about it.
#
# The refusal is real at this geometry, and the numbers are worth having
# because they are the phone's. A 1,048,576-byte static volume on the
# simulated chip reserves 9 LEBs of 129,024 = 1,161,216 usable bytes, so the
# 1,310,720-byte image below does not fit; `ubiupdatevol` rejects it with
# `UBI_IOCVOLUP: Invalid argument`, `ubirsvol` takes it to 11 LEBs, and the
# same write then succeeds and reads back byte-identical. That is ubi_fit()'s
# arithmetic in ndsys-apply.sh, at the LEB size the phone actually has.
BIG=$((IMG_SIZE + 262144))
M=$(wc -c < "$log")
ask "ubimkvol /dev/ubi0 -N small -t static -s $IMG_SIZE >/dev/null 2>&1; echo VOL2=\$(ls /dev/ubi0_1 2>/dev/null | wc -l)" 5
[ "$(grab "$M" VOL2)" = "1" ] || fail "could not make the second volume"

M=$(wc -c < "$log")
ask "dd if=/dev/urandom of=/tmp/state/big bs=4096 count=$((BIG / 4096)) 2>/dev/null; \
     echo BSHA=\$(sha256sum /tmp/state/big | cut -d' ' -f1)" 8
BSHA="$(grab "$M" BSHA)"

ask 'rm -f /tmp/state/*.prop'
for line in \
    "image=big" \
    "image_bytes=$BIG" \
    "sha256=$BSHA" \
    "version=9.9.9y" \
    "buildtime=1" \
    "platform=qemu-armv7" \
    "verity_root_hash=deadbeef" \
    "verity_block_size=4096" \
    "verity_image_blocks=320" \
    "verity_salt=00" \
    "attempts=0"
do
    ask "echo '$line' >> /tmp/state/pending.prop" 1
done

M=$(wc -c < "$log")
ask "STATE_DIR=/tmp/state MNT_USER=/tmp/user SYS_DEV=/dev/ubiblock0_1 USER_MOUNTED=1 \
     sh -c '. $APPLIER; apply_pending' 2>&1 | tail -6" 25
M2=$(wc -c < "$log")
ask 'echo BRESULT=$(sed -n "s/^result=//p" /tmp/state/last_result.prop 2>/dev/null)'
ask "echo BVOLSHA=\$(dd if=/dev/ubi0_1 bs=4096 count=$((BIG / 4096)) 2>/dev/null | sha256sum | cut -d' ' -f1)"
BRESULT="$(grab "$M2" BRESULT)"
BVOLSHA="$(grab "$M2" BVOLSHA)"

echo
say "B: volume was $IMG_SIZE bytes, image is $BIG bytes"
say "B: result=${BRESULT:-none}"
[ "$BRESULT" = "ok" ] || fail "an image larger than the volume did not install (result=${BRESULT:-none}) -- the volume needs resizing first"
[ "$BVOLSHA" = "$BSHA" ] || fail "B: the volume does not hold the bigger image"
say "PASS (B) -- a larger image grew the volume and installed"

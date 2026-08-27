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
# So this makes a real one. mtdram gives a RAM-backed MTD with the Luckfox's
# geometry (128 KiB erase blocks, 2048-byte pages), UBI attaches to it, and a
# static volume is created exactly the way mknand.sh creates the phone's.
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
for f in Image initramfs.cpio.gz system.img userdata.ext4 sdcard.img; do
    cp -f "$REPO/buildroot/output/images/$f" "$SP/images/$f"
done

ser="$SP/serial.fifo"; log="$SP/serial.log"
# ndsys-apply.sh lives ONLY in the initramfs -- after switch_root it is gone,
# so the booted phone has no copy to source. It rides in on the card instead.
NEODCT_IMAGES="$SP/images" "$REPO/neodct/tools/sdcard.sh" new 16 >/dev/null 2>&1 \
    || fail "could not make a card"
NEODCT_IMAGES="$SP/images" "$REPO/neodct/tools/sdcard.sh" init >/dev/null 2>&1 || true
NEODCT_IMAGES="$SP/images" "$REPO/neodct/tools/sdcard.sh" put \
    "$REPO/neodct/initramfs/ndsys-apply.sh" >/dev/null \
    || fail "could not put the applier on the card"

rm -f "$ser"; : > "$log"; mkfifo "$ser"
sh -c 'while :; do sleep 900; done' > "$ser" & HOLDER_PID=$!

# 256 MB: mtdram's backing store is ordinary kernel memory and the phone's
# 72 MB has no room for a flash chip on top of a phone.
NEODCT_IMAGES="$SP/images" NEODCT_DISPLAY=offscreen NEODCT_SD=image NEODCT_MEM=256 \
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

# --- a real UBI device on a RAM-backed MTD --------------------------------
# The geometry is the phone's, from neodct/tools/mknand.sh: 128 KiB erase
# blocks and 2048-byte pages. Getting this wrong does not fail loudly -- it
# just makes a volume with different LEB arithmetic than the phone's, and the
# size behaviour under test is exactly LEB arithmetic.
M=$(wc -c < "$log")
ask 'modprobe mtdram total_size=32768 erase_size=128; echo MTD=$(cat /proc/mtd | wc -l)' 6
MTD="$(grab "$M" MTD)"
[ "${MTD:-0}" -ge 2 ] || { tail -c +"$M" "$log" | tail -20; fail "no mtdram device (modprobe failed?)"; }

M=$(wc -c < "$log")
ask 'ubiattach -m 0 -d 0 /dev/ubi_ctrl >/dev/null 2>&1; echo UBI=$(ls -d /sys/class/ubi/ubi0 2>/dev/null | wc -l)' 6
[ "$(grab "$M" UBI)" = "1" ] || { tail -c +"$M" "$log" | tail -20; fail "ubiattach failed"; }
say "UBI attached to the RAM-backed MTD"

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
    "platform=qemu-aarch64" \
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
M=$(wc -c < "$log")
APPLIER=/NeoDCT/User/sdcard/update/ndsys-apply.sh
M=$(wc -c < "$log")
ask "echo APPLIER=\$(ls $APPLIER 2>/dev/null | wc -l)" 3
[ "$(grab "$M" APPLIER)" = "1" ] || fail "the applier did not arrive on the card"

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
    "platform=qemu-aarch64" \
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

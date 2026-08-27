#!/bin/sh
# Install a real .ndsw on a real boot, and prove the phone came up on it.
#
#   neodct/tools/test_update_e2e.sh [package.ndsw]
#
# WHY THIS EXISTS. Updates never once installed on real hardware, and every
# host test passed the whole time. test_initramfs_apply.py drives the applier
# with ordinary files standing in for the system partition, and dd is happy to
# write a file -- so the one thing that mattered was the one thing no test
# could see: on the phone the system partition is /dev/ubiblock0_0, which the
# kernel registers READ-ONLY, and dd cannot write it at all. The applier's own
# error path then turns that into silence, logging "retrying on the next boot"
# and booting the old system. Downloaded, rebooted, nothing changed.
#
# So this test does the part unit tests structurally cannot: a genuine package
# on a genuine card, staged into the real state directory, applied by the real
# initramfs across a real reboot, and then the VERSION IS READ BACK off the
# running system. Nothing here stubs anything.
#
# It runs on the QEMU path, where the system device is /dev/vda. That does NOT
# exercise the ubiupdatevol branch -- test_initramfs_apply.py covers the choice
# of writer, and only hardware covers the rest -- but it does cover every other
# link in the chain, which is where the next bug will be.
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SP="${NEODCT_E2E_WORK:-/tmp/claude-1000/update-e2e}"
PKG="${1:-$REPO/buildroot/output/images/packages/UPDATE-qemu-aarch64-0.4.3a.ndsw}"

say()  { echo "e2e: $*"; }
fail() { echo "e2e: FAIL -- $*" >&2; exit 1; }

[ -f "$PKG" ] || fail "no package at $PKG"

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
say "images copied to $SP/images (the repo's own are not touched)"

# --- the package goes on the card, where the phone looks for it -----------
# A fresh card, sized for the package: the stock sdcard.img is small and a
# 63 MB .ndsw does not fit beside what is already on it ("Disk full", from
# mtools, which is easy to read as a harness bug rather than a full card).
PKG_MB=$(( ( $(wc -c < "$PKG") / 1048576 ) + 64 ))
NEODCT_IMAGES="$SP/images" "$REPO/neodct/tools/sdcard.sh" new "$PKG_MB" > /dev/null 2>&1 \
    || fail "could not make a ${PKG_MB}MB card"
NEODCT_IMAGES="$SP/images" "$REPO/neodct/tools/sdcard.sh" init > /dev/null 2>&1 || true
NEODCT_IMAGES="$SP/images" "$REPO/neodct/tools/sdcard.sh" put "$PKG" > /dev/null
CARD_NAME="$(basename "$PKG")"
say "put $CARD_NAME in update/ on the card"

# --- the staging record, from the package's own manifest ------------------
REC="$SP/pending.prop"
python3 - "$PKG" "$CARD_NAME" "$REC" <<'PY'
import json, sys, zipfile
pkg, name, out = sys.argv[1], sys.argv[2], sys.argv[3]
z = zipfile.ZipFile(pkg)
m = json.loads(z.read("manifest.json"))
size = z.getinfo("rootfs.squashfs").file_size
v = m["verity"]
open(out, "w").write(
    "package=%s\nimage_bytes=%d\nsha256=%s\nversion=%s\nbuildtime=%d\n"
    "platform=%s\nverity_root_hash=%s\nverity_block_size=%d\n"
    "verity_image_blocks=%d\nverity_salt=%s\nattempts=0\n" % (
        name, size, m["sha256"], m["version"], m["buildtime"], m["platform"],
        v["root_hash"], v["block_size"], v["image_blocks"], v.get("salt", "")))
print(m["version"])
PY
WANT="$(python3 -c "import json,zipfile,sys; print(json.loads(zipfile.ZipFile(sys.argv[1]).read('manifest.json'))['version'])" "$PKG")"
say "staging $WANT (the phone must come up reporting exactly this)"

ser="$SP/serial.fifo"; log="$SP/serial.log"
rm -f "$ser" "$log"; mkfifo "$ser"
sh -c 'while :; do sleep 900; done' > "$ser" & HOLDER_PID=$!

# boot <label> -- start QEMU and wait for a login prompt IN THE NEW OUTPUT.
#
# The offset matters. One log holds both boots, so grepping the whole file for
# "login:" matches the PREVIOUS boot's prompt and returns instantly ("booted in
# 0s"), after which every keystroke is typed into a console that is still in
# the initramfs. That reads as "the update did not apply" and is really just
# the harness racing itself -- the exact false negative this test exists to
# not produce.
boot() {
    boot_mark=$(wc -c < "$log" 2>/dev/null || echo 0)
    NEODCT_IMAGES="$SP/images" NEODCT_DISPLAY=offscreen NEODCT_SD=image \
        "$REPO/neodct/tools/run_qemu.sh" < "$ser" >> "$log" 2>&1 &
    QEMU_PID=$!
    waited=0
    while [ "$waited" -lt 300 ]; do
        tail -c +"$boot_mark" "$log" 2>/dev/null | grep -aq 'login:' && break
        sleep 2; waited=$((waited + 2))
    done
    tail -c +"$boot_mark" "$log" 2>/dev/null | grep -aq 'login:' \
        || { tail -20 "$log"; fail "$1: never booted"; }
    say "$1: booted in ${waited}s"
    # The prompt appearing is not the same as the console being ready for a
    # password; a boot that just wrote 63 MB is still settling.
    sleep 3
    printf '\n' > "$ser"; sleep 2
    printf 'root\n' > "$ser"; sleep 4
}

ask() {   # ask <shell line>
    printf '%s\n' "$1" > "$ser"
    sleep "${2:-4}"
}

# ============ boot 1: record the starting version, stage the update ========
boot "boot 1"

MARK=$(wc -c < "$log")
ask 'echo BEFORE=$(sed -n "s/^VERSION_ID=//p" /etc/os-release | tr -d \")'
BEFORE="$(tail -c +"$MARK" "$log" | sed 's/\r$//' | sed -n 's/^BEFORE=//p' | head -1)"
[ -n "$BEFORE" ] || fail "could not read the running version"
say "boot 1: running $BEFORE"
[ "$BEFORE" != "$WANT" ] || fail "already on $WANT -- pick a package with a different version"

# The record goes in by hand, one line at a time, because there is no shell
# on the phone that writes it: the Update app does, and its own contract with
# this file is pinned by test_update_staging.py. What is under test here is
# the APPLIER, so the record is given to it exactly as the app would leave it.
ask 'mkdir -p /NeoDCT/User/.ndsys && rm -f /NeoDCT/User/.ndsys/pending.prop'
while IFS= read -r line; do
    ask "echo '$line' >> /NeoDCT/User/.ndsys/pending.prop" 1
done < "$REC"
ask 'sync; echo STAGED=$(wc -l < /NeoDCT/User/.ndsys/pending.prop)'
STAGED="$(tail -c +"$MARK" "$log" | sed 's/\r$//' | sed -n 's/^STAGED=//p' | tail -1)"
[ "${STAGED:-0}" -ge 10 ] || fail "staging record is $STAGED lines, expected 11"
say "boot 1: staged $STAGED lines; rebooting"

MARK2=$(wc -c < "$log")
ask 'sync; reboot -f' 8
kill "$QEMU_PID" 2>/dev/null || true; QEMU_PID=""
sleep 3

# ============ boot 2: the initramfs applies it =============================
boot "boot 2"

say "--- what the initramfs said ---"
tail -c +"$MARK2" "$log" | sed 's/\r$//' | grep -a "ndsys" | tail -12 || true

MARK3=$(wc -c < "$log")
ask 'echo AFTER=$(sed -n "s/^VERSION_ID=//p" /etc/os-release | tr -d \")'
ask 'echo RESULT=$(sed -n "s/^result=//p" /NeoDCT/User/.ndsys/last_result.prop 2>/dev/null)'
ask 'echo PENDING=$(ls /NeoDCT/User/.ndsys/pending.prop 2>/dev/null | wc -l)'
AFTER="$(tail -c +"$MARK3" "$log" | sed 's/\r$//' | sed -n 's/^AFTER=//p' | head -1)"
RESULT="$(tail -c +"$MARK3" "$log" | sed 's/\r$//' | sed -n 's/^RESULT=//p' | head -1)"
PENDING="$(tail -c +"$MARK3" "$log" | sed 's/\r$//' | sed -n 's/^PENDING=//p' | head -1)"

echo
say "before=$BEFORE  after=$AFTER  wanted=$WANT  result=${RESULT:-none}"
[ "$AFTER" = "$WANT" ] || fail "phone is on $AFTER, not $WANT -- the update did not apply"
[ "$RESULT" = "ok" ] || fail "applier recorded result=${RESULT:-none}"
[ "${PENDING:-1}" = "0" ] || fail "pending.prop was not cleared"
say "PASS -- $BEFORE -> $AFTER across a real reboot, applied by the real initramfs"

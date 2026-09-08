#!/bin/sh
# parity_capture_probe.sh -- an nd-inventory capture from a real armv7 boot,
# with no Buildroot image anywhere in sight.
#
# ============ WHY THIS EXISTS BESIDE parity_capture_qemu.sh ============
#
# parity_capture_qemu.sh is the real thing and captures from a built image:
# run_qemu.sh, the NeoDCT rootfs, S90display having already called
# force_mode(), ndusr present, /NeoDCT/platform present. It is also the thing
# nobody can run until `cd buildroot && make` has finished, which is hours,
# and which has never been run in this container -- buildroot/output does not
# exist.
#
# So the committed QEMU-side artefact was captured by THIS script instead: the
# repo's own kernel (board/qemu/armv7-virt/linux.config, the exact zImage the
# defconfig builds), the exact QEMU machine run_qemu.sh assembles, the same
# kernel parameters -- and a busybox initramfs where the NeoDCT rootfs would
# be. That is a MEASUREMENT of a real armv7 machine and it is not a
# measurement of a NeoDCT image, and the two must never be confused:
#
#   * the tool is cross-compiled with -DND_INVENTORY_NO_LIBNEODCT, because
#     libneodct cannot be cross-compiled without freetype, sqlite, libpng and
#     libjpeg for the target. The six library-derived records therefore read
#     UNAVAILABLE(nolib) -- a hole you can see, never a shorter file;
#   * there is no /NeoDCT, no /etc/os-release, no ndusr and no eudev, so every
#     record about the IMAGE reads ABSENT and is a fact about this initramfs
#     rather than about the emulator;
#   * capture.method=nd-inventory-nolib goes into the preamble, and
#     parity_diff.py refuses such a capture as a gating baseline for exactly
#     the reason it refuses the shell fallback. A WEAKER INSTRUMENT MUST NEVER
#     BE ABLE TO BECOME THE REFERENCE.
#
# What it IS good for is everything below the image: the kernel's own identity,
# the memory number, the class trees, the MTD geometry, /proc/devices,
# /proc/filesystems, the input devices, the /dev families and the framebuffer
# ioctls. That is most of the file, it is measured, and it is why the committed
# allowlist is an observation rather than a desk exercise.
#
# ============ AND IT IS THE DRIFT GATE, WHICH IT WAS NOT ============
#
# The whole ratchet used to have a static file for an input. Nothing in the
# tree ever re-derived the emulator baseline: the host tests check allow.txt
# against qemu-armv7-probe.inventory and never against a machine, and the one
# gate the README named for drift -- parity_capture_qemu.sh --compare -- needs
# a Buildroot image that does not exist. Measured what that cost: delete
# `gpio-mockup.gpio_mockup_ranges=0,64` from run_qemu.sh as an obsolete-looking
# parameter and the emulator loses gpio53, 56 and 57 -- the backlight's GPIO
# tier and the panel's RST and DC -- while `make test`, the whole pytest suite
# and test_qemu_surfaces.sh all stay green and nothing anywhere says a word.
#
# So --compare is here, it costs one 4-second boot, and `make parity-probe` in
# neodct/src/Makefile is the caller. A change to
# board/qemu/armv7-virt/linux.config or to run_qemu.sh's machine means running
# it; both files say so in their own headers.
#
# Usage:
#   parity_capture_probe.sh --kernel <zImage> --rootfs <busybox rootfs dir> \
#                           [--out <file>] [--compare <baseline>] \
#                           [--work <dir>] [--force]
#
# It refuses to write the output if --self-check fails inside the guest, if
# the tool reports the capture incomplete, or if the framing did not arrive.
# --out over an existing file needs --force, and prints the diff instead.

set -eu

KERNEL=""
ROOTFS=""
OUT=""
BASELINE=""
FORCE=""
WORK=""
HERE="$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)"
SRC="$HERE/../src"
DTSI="$HERE/../board/qemu/nd-virt-additions.dtsi"
CROSS="${CROSS_COMPILE:-arm-linux-gnueabihf-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --kernel)  KERNEL="$2";   shift 2 ;;
        --rootfs)  ROOTFS="$2";   shift 2 ;;
        --out)     OUT="$2";      shift 2 ;;
        --compare) BASELINE="$2"; shift 2 ;;
        --work)    WORK="$2";     shift 2 ;;
        --force)   FORCE=1;       shift ;;
        -h|--help)
            sed -n '2,62p' "$0"
            exit 2 ;;
        *) echo "parity_capture_probe.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

[ -n "$KERNEL" ] && [ -n "$ROOTFS" ] && { [ -n "$OUT" ] || [ -n "$BASELINE" ]; } || {
    echo "usage: parity_capture_probe.sh --kernel zImage --rootfs DIR" >&2
    echo "         [--out FILE] [--compare BASELINE] [--force]" >&2
    exit 2
}

# REGENERATING THE BASELINE IS THE ONE PLACE THIS HARNESS HAD NO DELIBERATE-ACT
# PROTECTION. --out at the committed path overwrote it in place, with no diff
# and no confirmation, and the new file carries its own recomputed hashes -- so
# test_baseline_hashes_verify passes on whatever landed. About fifteen of the
# ~160 compared records are asserted by hand in the host suite; the other ~145
# could all move in one absent-minded command. The allowlist beside it has four
# real teeth and this had none.
if [ -n "$OUT" ] && [ -e "$OUT" ] && [ -z "$FORCE" ]; then
    echo "REFUSED: $OUT already exists. Recapturing over a committed baseline is" >&2
    echo "         a deliberate act: run with --compare to see what moved, and" >&2
    echo "         add --force only once you have read the diff and mean to" >&2
    echo "         commit it with the argument for every line of it." >&2
    exit 2
fi
[ -f "$KERNEL" ] || { echo "no kernel at $KERNEL" >&2; exit 2; }
[ -d "$ROOTFS" ] || { echo "no rootfs at $ROOTFS" >&2; exit 2; }
command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm not found" >&2; exit 2; }
command -v "${CROSS}gcc" >/dev/null || { echo "${CROSS}gcc not found" >&2; exit 2; }

# The device tree is not optional here, and that is the difference between
# this script and run_qemu.sh. run_qemu.sh degrades without dtc because a
# session with no backlight is still worth having; a CAPTURE with no backlight
# is a capture of a machine nobody boots, and it would become the baseline
# every later capture is diffed against. Refuse instead.
. "$HERE/qemu_machine.sh"

# `trap rm -rf "$WORK"` deletes whatever --work names, and `mkdir -p` succeeds
# silently on a directory that is already there -- so
# `--work ~/neodct-scratch` used to delete ~/neodct-scratch and everything
# under it, on the early exit paths too. "A place to put work" is the natural
# reading of the flag and it was not what it did.
WORK="${WORK:-${TMPDIR:-/tmp}/nd-parity-probe.$$}"
if [ -e "$WORK" ]; then
    echo "REFUSED: --work $WORK already exists, and this script deletes its work" >&2
    echo "         directory recursively when it finishes. Name one that does not." >&2
    exit 2
fi
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

nd_dtb_build "$DTSI" "$WORK" "$WORK/nd.dtb" || {
    echo "REFUSED: no device tree, so this would capture a machine run_qemu.sh" >&2
    echo "  does not assemble -- no backlight and no cpufreq." >&2
    exit 1; }

# The full warning set, including -Wconversion and -Werror. This build is not
# a lesser build of the tool -- it is the same two translation units under the
# same flags, minus the six records that need the library.
"${CROSS}gcc" \
    -std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion \
    -Wstrict-prototypes -Wmissing-prototypes -Wvla \
    -O2 -D_GNU_SOURCE -DND_INVENTORY_NO_LIBNEODCT \
    -I"$SRC/tools" -static \
    -o "$WORK/nd-inventory" \
    "$SRC/tools/nd_inventory.c" "$SRC/tools/nd_inventory_collect.c" 2>"$WORK/cc.log" || {
    echo "cross build failed:" >&2; cat "$WORK/cc.log" >&2; exit 1; }

cp -a "$ROOTFS" "$WORK/root"
cp "$WORK/nd-inventory" "$WORK/root/bin/nd-inventory"
"${CROSS}strip" "$WORK/root/bin/nd-inventory" 2>/dev/null || true

# The markers are what makes the scrape reconstructible from a log that also
# carries printk. The INV| sentinel does the same job line by line; these two
# bracket the region so a partial boot cannot look like a whole capture.
cat > "$WORK/root/init" <<'INIT'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts && mount -t devpts devpts /dev/pts
/bin/nd-inventory --self-check >/dev/null 2>/dev/null
echo "===PARITY-SELFCHECK=$?"
echo "===PARITY-BEGIN"
/bin/nd-inventory
echo "===PARITY-RC=$?"
echo "===PARITY-END"
poweroff -f
INIT
chmod +x "$WORK/root/init"
( cd "$WORK/root" && find . | cpio -o -H newc 2>/dev/null | gzip -9 ) > "$WORK/initramfs.cpio.gz"

# Two 8 MB blanks carrying the serials run_qemu.sh gives the real images, so
# block.byserial.NDSYS and block.byserial.NDUSER are exercised. They are never
# mounted and never written.
dd if=/dev/zero of="$WORK/ndsys.img"  bs=1M count=8 status=none
dd if=/dev/zero of="$WORK/nduser.img" bs=1M count=8 status=none

# The machine is run_qemu.sh's -- -M virt -cpu cortex-a7 -smp 1 -m 64 and
# force-legacy=false, without which virtio_input is refused SILENTLY and there
# is no keyboard at all (EMPIRICAL-FINDINGS 10).
#
# NEITHER THE DEVICE TREE NOR THE KERNEL PARAMETERS ARE COPIED. Both come from
# qemu_machine.sh -- nd_dtb_build() and nd_qemu_append() -- which run_qemu.sh
# and test_qemu_surfaces.sh also source, so a capture cannot be taken against
# a machine the emulator does not assemble. The parameters WERE copied, into
# three files, and the three copies had already drifted.
#
# What is added on top is this boot's own: console and rdinit, because a probe
# initramfs is not an image, `video=vfb:on` because run_qemu.sh's default
# display mode carries it and a capture with no framebuffer is a capture
# missing its eleven most valuable records, and neodct.devenv=1 because
# run_qemu.sh passes it by default.
timeout 300 qemu-system-arm \
    -M virt -cpu cortex-a7 -smp 1 -m 64 -nographic \
    -global virtio-mmio.force-legacy=false \
    -kernel "$KERNEL" \
    -initrd "$WORK/initramfs.cpio.gz" \
    -dtb "$WORK/nd.dtb" \
    -drive "file=$WORK/ndsys.img,if=none,format=raw,id=ndsys" \
    -device virtio-blk-device,drive=ndsys,serial=NDSYS \
    -drive "file=$WORK/nduser.img,if=none,format=raw,id=nduser" \
    -device virtio-blk-device,drive=nduser,serial=NDUSER \
    -device virtio-keyboard-device \
    -append "console=ttyAMA0 rdinit=/init panic=5 neodct.devenv=1 video=vfb:on \
$(nd_qemu_append)" \
    > "$WORK/boot.log" 2>&1 || true

# A capture is REFUSED rather than recorded whenever anything about the run
# was not what it should be. A refusal is not a measurement, but a recorded
# capture that is not a measurement is worse: it becomes a baseline.
selfcheck=$(sed -n 's/^===PARITY-SELFCHECK=\([0-9]*\).*/\1/p' "$WORK/boot.log" | tr -d '\r' | head -1)
rc=$(sed -n 's/^===PARITY-RC=\([0-9]*\).*/\1/p' "$WORK/boot.log" | tr -d '\r' | head -1)
[ "${selfcheck:-x}" = "0" ] || {
    echo "REFUSED: --self-check did not pass in the guest (got '${selfcheck:-nothing}')" >&2
    exit 1; }
[ "${rc:-x}" = "0" ] || {
    echo "REFUSED: nd-inventory reported the capture incomplete (exit ${rc:-nothing})" >&2
    grep -a 'nd-inventory:' "$WORK/boot.log" >&2 || true
    exit 1; }

# CR is stripped because the console turns LF into CRLF; nothing else is
# touched, which is what makes a serial scrape and an ssh capture produce the
# same bytes.
sed -n '/^===PARITY-BEGIN/,/^===PARITY-END/p' "$WORK/boot.log" \
    | tr -d '\r' \
    | grep '^INV|' > "$WORK/capture.txt"

grep -q '^INV|BEGIN$' "$WORK/capture.txt" || { echo "REFUSED: no BEGIN in the capture" >&2; exit 1; }
grep -q '^INV|END$'   "$WORK/capture.txt" || { echo "REFUSED: no END in the capture" >&2; exit 1; }

if [ -n "$OUT" ]; then
    mkdir -p "$(dirname "$OUT")"
    cp "$WORK/capture.txt" "$OUT"
    echo "captured $(grep -c '^INV|' "$OUT") framed lines into $OUT"
fi

# THE DRIFT GATE. A fresh capture must equal the committed baseline byte for
# byte. Hardware is rare and kernel-config changes are not, so this is the
# half that catches the common case -- somebody changes something and the
# emulator quietly drifts -- and it is the only gate in the tree that compares
# the allowlist's input against a MACHINE rather than against a file.
if [ -n "$BASELINE" ]; then
    if diff -u "$BASELINE" "$WORK/capture.txt"; then
        echo "the emulator has not drifted from $BASELINE"
    else
        echo "DRIFT: the emulator no longer matches $BASELINE (diff above)." >&2
        echo "       Either the change was intended -- recapture with --out ... --force" >&2
        echo "       and commit the baseline together with the argument for every line" >&2
        echo "       that moved -- or something moved that nobody meant." >&2
        exit 1
    fi
fi

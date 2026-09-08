#!/bin/sh
# parity_capture_qemu.sh -- the emulator side of the parity harness, from a
# BUILT IMAGE.
#
# ============ THIS SCRIPT HAS NEVER BEEN RUN ============
#
# Written down first because it is the kind of claim that quietly becomes
# untrue. buildroot/output does not exist in the tree this was written in and
# a full Buildroot build is hours, so nothing below has been executed against
# a real image. What HAS been executed is parity_capture_probe.sh, which boots
# the same kernel and the same QEMU machine with a busybox initramfs instead
# of the NeoDCT rootfs, and which produced the committed
# tests/parity/qemu-armv7-probe.inventory over two boots with a zero-byte
# diff. The first person to run THIS script should expect to fix it, and
# should treat every line below as a design rather than as a tested procedure.
#
# The pattern is test_update_ubi.sh's, verbatim rather than reinvented: a
# named pipe with a holder process so the fifo does not close when a writer
# exits, run_qemu.sh backgrounded, and byte offsets into the log so each read
# starts after the last.
#
# ============ TWO THINGS IT DOES THAT test_update_ubi.sh DOES NOT ==========
#
# NOTHING IS DELIVERED ON A DISK. The tool is in the image: `make install`
# puts nd-inventory in /NeoDCT/System/bin beside nd-selftest, and the argument
# for shipping it is in the Makefile's install rule. There is no artefact to
# hand the guest and no writable partition to leave one on.
#
# IT WAITS FOR READINESS, NOT FOR `login:`. The framebuffer records depend on
# something having called force_mode(); a capture taken before it would bake
# vfb's own 640x480x8 default into a baseline, which is exactly what the probe
# capture shows and exactly why that file is committed under a name that
# cannot be mistaken for this one. So: wait for /dev/fb0, run --self-check,
# and then check fb0.var.xres. A refusal is not a measurement, but a recorded
# capture that is not a measurement is worse, because it becomes the reference.
#
# ============ AND THAT CHECK IS A WARNING UNDER QEMU, NOT A REFUSAL ========
#
# It was a refusal, and it could never have passed. force_mode() lives in
# neodct/src/displayd/neodctDisplay.c and runs only when S90display starts
# that daemon -- and S90display sets PANEL_DAEMON=no on qemu, deliberately,
# because there is no SPI bus and the virtual framebuffer IS the screen.
# AGENTS.md says the same thing outright: "on the phone neodct_displayd does
# that and under QEMU nothing does it yet." So the first person to run this
# against a built image would have waited, been told the framebuffer was
# 640x480, and started debugging an image that was fine.
#
# The precondition is named instead of asserted: xres is 240 where a panel
# daemon is expected to have run, and until something under QEMU issues the
# ioctl -- which is the missing piece AGENTS.md already flags -- this says so
# on stderr and records the capture with vfb's default in it. The fb0 records
# are then a property of that gap, not of the emulator, and they are the
# reason the drift comparison is byte-for-byte against a baseline taken the
# same way rather than against the phone.

set -eu

OUT=""
TIMEOUT=180
BASELINE=""
REPO="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"

while [ $# -gt 0 ]; do
    case "$1" in
        --out)      OUT="$2";      shift 2 ;;
        --timeout)  TIMEOUT="$2";  shift 2 ;;
        --compare)  BASELINE="$2"; shift 2 ;;
        -h|--help)  sed -n '2,54p' "$0"; exit 2 ;;
        *) echo "parity_capture_qemu.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
[ -n "$OUT" ] || { echo "usage: parity_capture_qemu.sh --out FILE [--compare BASELINE]" >&2; exit 2; }

WORK="${TMPDIR:-/tmp}/nd-parity-qemu.$$"
mkdir -p "$WORK"
FIFO="$WORK/console"
LOG="$WORK/console.log"
mkfifo "$FIFO"

cleanup() {
    [ -n "${QEMU_PID:-}" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "${HOLD_PID:-}" ] && kill "$HOLD_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

# The holder. Without a second writer the fifo returns EOF the moment QEMU's
# own write side closes, and the reader sees a truncated capture rather than
# an error -- which is the failure mode this whole harness exists to refuse.
sleep "$((TIMEOUT + 30))" > "$FIFO" &
HOLD_PID=$!
cat "$FIFO" > "$LOG" &

# NEODCT_DISPLAY=offscreen because a capture does not need a window, and
# NEODCT_SD=none because a card would put block.byserial.NDCARD into the
# baseline and then every capture taken without one would differ from it.
#
# THE CONSOLE GOES THROUGH NEODCT_CONSOLE AND NOT THROUGH ARGV. This used to
# run `run_qemu.sh -serial "pipe:$FIFO" -display none`, and run_qemu.sh parses
# no positional arguments at all -- the `set --` that assembles its QEMU
# command line overwrites "$@" -- so both flags were discarded in silence, the
# console went to stdio, the fifo below never received a byte, and the wait
# for `login:` timed out after 180 s on a perfectly good image. -M virt wires
# one pl011, so a second -serial would not have been a second console either;
# run_qemu.sh names the chardev now instead of hard-coding stdio.
NEODCT_DISPLAY=offscreen NEODCT_SD=none NEODCT_CONSOLE="pipe:$FIFO" \
    "$REPO/tools/run_qemu.sh" &
QEMU_PID=$!

OFFSET=0
ask() {
    # Send a line and return everything printed after it. The byte offset is
    # what makes a second ask() not re-read the first one's output.
    OFFSET=$(wc -c < "$LOG")
    printf '%s\n' "$1" > "$FIFO"
    sleep "${2:-3}"
    tail -c "+$((OFFSET + 1))" "$LOG"
}

wait_for() {
    end=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        if grep -qa "$1" "$LOG" 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_for 'login:' "$TIMEOUT" || { echo "REFUSED: the guest never reached a login prompt" >&2; exit 1; }
ask 'root' 3 >/dev/null

# Readiness, in three steps, each of which is a separate way for a capture to
# be worthless.
wait_for '' 1
ask '[ -c /dev/fb0 ] && echo FB-NODE-PRESENT || echo FB-NODE-MISSING' 3 | grep -qa FB-NODE-PRESENT || {
    echo "REFUSED: no /dev/fb0 in the guest -- see the vfb note in linux.config" >&2; exit 1; }

# --self-check WITH NO SECTION. It used to say `--self-check kernel`, which is
# inv_collect_kernel() -- three strings from one uname(2) call, deterministic
# by construction, a gate that cannot fire. The collectors whose stability is
# actually at issue are the readdir-driven ones (class, dev, mtd, input,
# block, mount), and none of them was checked. Measured in the probe guest --
# the same kernel and the same machine, with a busybox initramfs -- the kernel
# section reports "ok (3 records, twice)" and the full check "ok (172 records,
# twice)", at no measurable cost.
ask '/NeoDCT/System/bin/nd-inventory --self-check >/dev/null 2>&1; echo SELFCHECK=$?' 5 \
    | grep -qa 'SELFCHECK=0' || {
    echo "REFUSED: --self-check did not pass in the guest, so this machine cannot" >&2
    echo "         produce a baseline: two collections in one process differed." >&2
    exit 1; }

CAPTURE=$(ask '/NeoDCT/System/bin/nd-inventory; echo CAPTURE-RC=$?' 20)
printf '%s\n' "$CAPTURE" | tr -d '\r' | grep -a '^INV|' > "$WORK/capture.txt" || true

grep -qa 'CAPTURE-RC=0' <<EOF || { echo "REFUSED: nd-inventory reported the capture incomplete" >&2; exit 1; }
$CAPTURE
EOF
grep -q '^INV|BEGIN$' "$WORK/capture.txt" || { echo "REFUSED: no BEGIN in the capture" >&2; exit 1; }
grep -q '^INV|END$'   "$WORK/capture.txt" || { echo "REFUSED: no END in the capture" >&2; exit 1; }

# THE ONE THAT MATTERS, AND THE ONE THAT CANNOT YET BE AN ASSERTION. vfb comes
# up 640x480 at 8 bpp and neodct_displayd puts it into the phone's mode with
# FBIOPUT_VSCREENINFO. A capture taken before that is a capture of a
# framebuffer no NeoDCT process ever sees, and the eleven numbers it would put
# in the baseline are the exact eleven that carry the whole Stage 3 claim.
#
# But S90display sets PANEL_DAEMON=no on qemu and nothing else issues the
# ioctl, so under the emulator this is a KNOWN gap rather than a bad capture,
# and refusing here refused every capture that will ever be taken. It says so
# loudly instead, and NEODCT_REQUIRE_PANEL=1 turns it back into a refusal for
# the day something under QEMU does set the mode.
xres=$(sed -n 's/^INV|fb0\.var\.xres \(.*\)$/\1/p' "$WORK/capture.txt")
if [ "$xres" != "240" ]; then
    echo "NOTE: fb0.var.xres is '${xres:-nothing}', not 240. Nothing under QEMU calls" >&2
    echo "      force_mode(): S90display sets PANEL_DAEMON=no here because there is no" >&2
    echo "      SPI bus and vfb IS the screen. So the fb0 records below describe vfb's" >&2
    echo "      own default and are a property of that gap, not of the emulator." >&2
    [ -z "${NEODCT_REQUIRE_PANEL:-}" ] || {
        echo "REFUSED: NEODCT_REQUIRE_PANEL is set and the panel is not in the phone's" >&2
        echo "         mode, so this capture is not of the machine you asked for." >&2
        exit 1; }
fi

mkdir -p "$(dirname "$OUT")"
cp "$WORK/capture.txt" "$OUT"
echo "captured $(grep -c '^INV|' "$OUT") framed lines into $OUT"

# The gate that runs on every build with no phone in the room: a fresh capture
# must equal the committed baseline byte for byte. Hardware is rare and kernel
# config changes are not, so this is the half that catches the common case --
# somebody changes something and the emulator quietly drifts.
if [ -n "$BASELINE" ]; then
    if diff -u "$BASELINE" "$OUT"; then
        echo "the emulator has not drifted from $BASELINE"
    else
        echo "DRIFT: the emulator no longer matches $BASELINE (diff above)." >&2
        echo "       Either the change was intended -- recapture and commit both the" >&2
        echo "       baseline and the reason -- or something moved that nobody meant." >&2
        exit 1
    fi
fi

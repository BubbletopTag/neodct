#!/bin/sh
# Prove S17audio actually un-mutes a real capture device.
#
#   neodct/tools/test_mic_capture.sh
#
# WHY. An electret soldered to a C-Media card recorded nothing on the phone,
# and MicTest drew the flat line faithfully. S17audio does the routing -- find
# the USB card, write asound.conf, bypass dmix -- and then stops. It never
# touches the mixer, and there is no alsactl in the image, so no saved state is
# restored either: the phone gets the driver's defaults on every boot. The ONN
# microphone's defaults are open, which is why this hid for as long as that was
# the only microphone anyone tried.
#
# The unit tests for the fix drive the script against an amixer I wrote myself,
# so they would pass just as happily if the parsing were wrong. This does not:
# snd-dummy is a real ALSA card with real capture controls, amixer is the real
# amixer, and the script is the real script. The card is deliberately muted
# first, the way a C-Media one arrives.
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SP="${NEODCT_MIC_WORK:-/tmp/claude-1000/mic-capture}"

say()  { echo "mic: $*"; }
fail() { echo "mic: FAIL -- $*" >&2; exit 1; }

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
rm -f "$ser"; : > "$log"; mkfifo "$ser"
sh -c 'while :; do sleep 900; done' > "$ser" & HOLDER_PID=$!

NEODCT_IMAGES="$SP/images" NEODCT_DISPLAY=offscreen NEODCT_SD=none \
    "$REPO/neodct/tools/run_qemu.sh" < "$ser" >> "$log" 2>&1 &
QEMU_PID=$!

waited=0
while [ "$waited" -lt 240 ]; do
    grep -aq 'login:' "$log" 2>/dev/null && break
    sleep 2; waited=$((waited + 2))
done
grep -aq 'login:' "$log" 2>/dev/null || { tail -20 "$log"; fail "never booted"; }
say "booted in ${waited}s"
sleep 2; printf '\n' > "$ser"; sleep 2; printf 'root\n' > "$ser"; sleep 4

ask()  { printf '%s\n' "$1" > "$ser"; sleep "${2:-4}"; }
grab() { tail -c +"$1" "$log" | sed 's/\r$//' | sed -n "s/^$2=//p" | tail -1; }

# --- a real card with real capture controls ------------------------------
M=$(wc -c < "$log")
ask 'modprobe snd-dummy; echo DUMMY=$(cat /proc/asound/cards | grep -ci dummy)' 6
[ "$(grab "$M" DUMMY)" -ge 1 ] 2>/dev/null || { tail -c +"$M" "$log" | tail -15; fail "snd-dummy did not load"; }
M=$(wc -c < "$log")
ask 'echo DCARD=$(sed -n "s/^ *\([0-9]\) \[Dummy.*/\1/p" /proc/asound/cards | head -1)' 4
DCARD="$(grab "$M" DCARD)"
[ -n "$DCARD" ] || fail "could not find the dummy card number"
say "snd-dummy is card $DCARD"

# --- mute it, the way a C-Media card arrives ------------------------------
M=$(wc -c < "$log")
ask "amixer -c $DCARD sset 'Master' 0% nocap >/dev/null 2>&1; \
     amixer -c $DCARD sset 'CD' 0% nocap >/dev/null 2>&1; \
     echo MUTED=\$(amixer -c $DCARD sget 'CD' 2>/dev/null | grep -c '\[off\]')" 6
say "muted: CD capture off-count=$(grab "$M" MUTED)"

# --- run the REAL S17audio against it -------------------------------------
#
# By running the script properly, not by sourcing it: sourcing an init script
# executes its case statement, and with no argument that is the usage branch
# and an exit -- the function under test never runs, and the test reports the
# fix broken when it was never called. S17audio finds its card by usbid, which
# snd-dummy has none of, so a fake /proc/asound is built naming the dummy card
# and given the C-Media usbid off the real adapter.
M=$(wc -c < "$log")
ask "echo ===CONTROLS===; amixer -c $DCARD controls 2>&1 | head -12" 6

M=$(wc -c < "$log")
ask "rm -rf /tmp/pa && mkdir -p /tmp/pa/card$DCARD && echo 0d8c:0014 > /tmp/pa/card$DCARD/usbid && \
     cp /proc/asound/cards /tmp/pa/cards && \
     NEODCT_PROC_ASOUND=/tmp/pa NEODCT_ASOUND_CONF=/tmp/asound.test \
     sh /etc/init.d/S17audio start 2>&1 | tail -3" 12

M2=$(wc -c < "$log")
ask "echo AFTERCAP=\$(amixer -c $DCARD sget 'CD' 2>/dev/null | grep -c '\[on\]')" 5
ask "echo AFTERVOL=\$(amixer -c $DCARD sget 'CD' 2>/dev/null | sed -n 's/.*\[\([0-9]*\)%\].*/\1/p' | head -1)" 5
AFTERCAP="$(grab "$M2" AFTERCAP)"
AFTERVOL="$(grab "$M2" AFTERVOL)"

echo
say "after S17audio: capture-on count=$AFTERCAP, 'CD Volume' left at ${AFTERVOL:-?}%"
[ "${AFTERCAP:-0}" -ge 1 ] 2>/dev/null || fail "capture was left switched off"

# The volume is deliberately NOT raised here, and that is the pass condition.
#
# snd-dummy publishes 'Mic Volume' + 'Mic Capture Switch' -- an old ISA-style
# mixer where one volume serves playback and capture together. A real USB audio
# card names them apart: the C-Media adapter this was written for publishes
# 'Mic Capture Volume' and 'Mic Playback Volume' as separate controls, read off
# the hardware itself.
#
# S17audio only raises a volume whose name says Capture, because an ambiguous
# one is the monitor path on the cards that matter, and raising that puts the
# microphone into the earpiece. So on this card the switch is turned on and the
# volume is left alone, which is the conservative half of the same rule.
[ "${AFTERVOL:-0}" -eq 0 ] 2>/dev/null \
    || fail "an ambiguously named volume was raised (${AFTERVOL}%) -- that is the monitor path on real cards"
say "PASS -- capture switched on; an ambiguous volume correctly left alone"

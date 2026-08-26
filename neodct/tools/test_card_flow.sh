#!/bin/sh
# "Copy keys from card", end to end, driven through the actual UI.
#
# test_remoteshell_e2e.sh seeds the phone's .remote directory directly, which
# tests the tunnel but skips the way a person actually sets this up: drop a
# folder on the SD card, press Copy keys from card, press Turn on. That path
# has its own failure modes -- wrong file names, a relay.conf that is not read,
# keys landing at the wrong mode -- and none of them are visible to the other
# test.
#
# So this one puts a card in the phone with NOTHING seeded, walks the menu with
# QEMU's sendkey, and then tries to log in. The keys the phone uses can only
# have come off the card.
#
#   neodct/tools/test_card_flow.sh
#
# The card it builds mirrors the layout of a real one exactly -- same four file
# names, same relay.conf -- but points at test_relay.sh rather than a VPS,
# because QEMU has no route to a real relay's IPv6 address.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
ROOT="$(dirname "$REPO")"
RELAY="$HERE/test_relay.sh"
SDCARD="$HERE/sdcard.sh"

WORK="${NEODCT_CARD_DIR:-$ROOT/build-cardtest}"
IMAGES="$WORK/images"
SRC_IMAGES="${NEODCT_SRC_IMAGES:-$ROOT/buildroot/output/images}"
R="${NEODCT_RELAY_DIR:-${TMPDIR:-/tmp}/neodct-test-relay}"
PHONE_PORT="${NEODCT_PHONE_PORT:-2222}"
MONITOR="$WORK/monitor.sock"

# Remote Shell is app id 9990. The menu is sorted by id, and with engineering
# mode on (the shipped default) it sits at this index, counting from zero.
REMOTE_INDEX="${NEODCT_REMOTE_INDEX:-19}"

say() { echo "card: $*"; }
die() { echo "card: $*" >&2; exit 1; }

QEMU_PID=""
HOLDER_PID=""
MON_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$HOLDER_PID" ] && kill "$HOLDER_PID" 2>/dev/null || true
    [ -n "${MON_PID:-}" ] && kill "$MON_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# One monitor connection for the whole run, held open on fd 9. A connection
# per key loses most of them: QEMU accepts the socket and the command races the
# close. Found the hard way -- three keys out of twenty-six arrived, and the
# ones that did went to the dialer.
mon_open() {
    rm -f "$WORK/mon.fifo"
    mkfifo "$WORK/mon.fifo"
    socat - "UNIX-CONNECT:$MONITOR" < "$WORK/mon.fifo" > "$WORK/mon.out" 2>&1 &
    MON_PID=$!
    exec 9> "$WORK/mon.fifo"
}

# Send one key and WAIT FOR THE PHONE TO SAY IT ARRIVED. nd_input logs every
# code it reads, so the guest itself is the acknowledgement -- no guessing at
# how long a redraw takes, and a dropped key fails here rather than three
# screens later.
# Two ways to press a key, because only ONE of them can be acknowledged.
#
# nd_main.c logs "Code: N" from the CORE's main loop. The moment a widget with
# its own read loop is on screen -- the AppSelector, a VerticalList, a dialog
# -- keys are consumed there and nothing is logged. So the guest can confirm
# the first keypress and nothing after it.
#
# key_probe is therefore used once, on the home screen, where it doubles as the
# readiness test: there is no log line that reliably says the UI is reading
# input yet, so the key is offered again and again until the phone says it took
# one. Everything after that is key_blind, paced, and the OUTCOME is what gets
# checked -- the tunnel coming up and a login working.
key_probe() {
    want="$2"
    # grep -c exits 1 on zero matches; "|| echo 0" would append a second
    # line and the arithmetic below would blow up on "0\n0".
    before=$(grep -ac "Code: $want" "$log" 2>/dev/null) || before=0
    attempt=0
    while [ "$attempt" -lt 40 ]; do
        printf 'sendkey %s\n' "$1" >&9
        tries=0
        while [ "$tries" -lt 12 ]; do
            now=$(grep -ac "Code: $want" "$log" 2>/dev/null) || now=0
            [ "$now" -gt "$before" ] && return 0
            sleep 0.25
            tries=$((tries + 1))
        done
        attempt=$((attempt + 1))
    done
    die "the phone never saw '$1' (code $want) -- the UI is not reading input"
}

key_blind() {
    printf 'sendkey %s\n' "$1" >&9
    sleep "${2:-0.5}"
}

# A picture of wherever it actually ended up, for when it ends up nowhere.
shot() {
    printf 'screendump %s\n' "$WORK/$1.ppm" >&9
    sleep 1
    [ -f "$WORK/$1.ppm" ] && say "screen saved: $WORK/$1.ppm"
}

command -v socat >/dev/null || die "socat is needed to drive the monitor"

mkdir -p "$IMAGES"
"$RELAY" up >/dev/null
say "relay up"

# ---- the card, laid out exactly as a real one ---------------------------
CARDDIR="$WORK/remote"
rm -rf "$CARDDIR"; mkdir -p "$CARDDIR"
cp "$R/phone_key"        "$CARDDIR/id_ed25519"
cp "$R/client_key.pub"   "$CARDDIR/authorized_keys"
cp "$R/known_hosts.guest" "$CARDDIR/known_hosts"
cat > "$CARDDIR/relay.conf" <<EOF
host=10.0.2.100
user=$(id -un)
port=$PHONE_PORT
EOF
chmod 600 "$CARDDIR/id_ed25519"

# ---- images: fresh userdata, NOTHING seeded -----------------------------
for f in Image initramfs.cpio.gz system.img userdata.ext4; do
    cp -f "$SRC_IMAGES/$f" "$IMAGES/$f"
done
cp -f "$SRC_IMAGES/sdcard.img" "$IMAGES/sdcard.img"

DEBUGFS="$ROOT/buildroot/output/host/sbin/debugfs"
if "$DEBUGFS" -R "ls /.remote" "$IMAGES/userdata.ext4" 2>/dev/null | grep -q 'state.prop'; then
    die "userdata already has a .remote -- this test must start with none"
fi
say "userdata is clean: no keys, no settings, nothing to fall back on"

for f in id_ed25519 authorized_keys known_hosts relay.conf; do
    NEODCT_SDCARD="$IMAGES/sdcard.img" "$SDCARD" put "$CARDDIR/$f" remote >/dev/null
done
say "card holds: $(NEODCT_SDCARD="$IMAGES/sdcard.img" "$SDCARD" ls remote | awk '/[a-z]/ {printf "%s ", $1}')"

# ---- boot ---------------------------------------------------------------
ser="$WORK/serial.fifo"
log="$WORK/serial.log"
rm -f "$ser" "$log" "$MONITOR"
mkfifo "$ser"
sh -c 'while :; do sleep 600; done' > "$ser" &
HOLDER_PID=$!

NEODCT_IMAGES="$IMAGES" NEODCT_DISPLAY=offscreen NEODCT_AUDIO=none \
NEODCT_MONITOR="$MONITOR" NEODCT_QEMU_EXTRA="$("$RELAY" qemu-args)" \
    "$HERE/run_qemu.sh" < "$ser" > "$log" 2>&1 &
QEMU_PID=$!
say "booting (qemu $QEMU_PID)"

waited=0
while [ "$waited" -lt 180 ]; do
    grep -aq 'Custom font loaded' "$log" 2>/dev/null && break
    sleep 2
    waited=$((waited + 2))
done
grep -aq 'Custom font loaded' "$log" 2>/dev/null || { tail -20 "$log"; die "the UI never started"; }
mon_open
say "UI up in ${waited}s"
sleep 5   # let the home screen finish its first draw

# ---- drive the menu -----------------------------------------------------
say "opening the menu (also the readiness probe)"
key_probe ret 28
i=0
while [ "$i" -lt "$REMOTE_INDEX" ]; do
    key_blind down 0.4
    i=$((i + 1))
done
say "selecting Remote Shell (entry $REMOTE_INDEX)"
key_blind ret 5

say "Copy keys from card"
key_blind 6 4    # item 6 of 7 -- VerticalList takes digits 1..9 directly
key_blind ret 3  # dismiss "Copied: ..."

say "Turn on"
key_blind 2 3    # item 2 -- the toggle
key_blind ret 6  # confirm the "Turn Remote Shell on?" dialog
key_blind ret 2  # dismiss "Remote Shell is on."

# ---- did it work? -------------------------------------------------------
say "waiting for the phone to dial out"
waited=0
while [ "$waited" -lt 120 ]; do
    ss -tln 2>/dev/null | grep -q "127.0.0.1:$PHONE_PORT " && break
    sleep 3
    waited=$((waited + 3))
done
if ! ss -tln 2>/dev/null | grep -q "127.0.0.1:$PHONE_PORT "; then
    shot failed
    echo "--- serial ---"; tail -25 "$log"
    die "the tunnel never came up"
fi
say "tunnel up in ${waited}s"

say "logging in with the key that came off the card"
if out="$("$RELAY" ssh 'echo CARD_FLOW_OK; ls -l /NeoDCT/User/.remote/ | grep -cE "relay_id|authorized|known"; cat /NeoDCT/User/.remote/state.prop' 2>&1)"; then
    echo "$out" | sed 's/^/  /'
    case "$out" in
        *CARD_FLOW_OK*) say "PASS -- the card set up a reachable phone" ;;
        *) die "logged in but got nothing back" ;;
    esac
else
    echo "$out" | sed 's/^/  /'
    die "could not log in"
fi

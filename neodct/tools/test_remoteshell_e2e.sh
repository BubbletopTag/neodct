#!/bin/sh
# Remote Shell, end to end, on this machine.
#
# Boots the phone in QEMU with test_relay.sh standing in for the VPS, waits
# for it to dial out, and then logs in THROUGH the tunnel the phone opened --
# which is the only thing that proves the feature works. Everything is real
# except the relay's address: real sshd on the phone, real ssh dialling out,
# real reverse forward, real login.
#
#   neodct/tools/test_remoteshell_e2e.sh              one run, fresh userdata
#   NEODCT_E2E_KEEP=1 ...                             keep userdata (2nd boot)
#   NEODCT_E2E_RUNS=5 ...                             run it five times
#
# Exits non-zero on the first run that fails, and leaves the phone's own
# remote.log on stdout when it does -- that log names the reason far more
# often than the client's "Permission denied" does.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
ROOT="$(dirname "$REPO")"
RELAY="$HERE/test_relay.sh"

WORK="${NEODCT_E2E_DIR:-$ROOT/build-qemutest}"
IMAGES="$WORK/images"
SRC_IMAGES="${NEODCT_SRC_IMAGES:-$ROOT/buildroot/output/images}"
RUNS="${NEODCT_E2E_RUNS:-1}"
BOOT_WAIT="${NEODCT_E2E_BOOT_WAIT:-180}"
TUNNEL_WAIT="${NEODCT_E2E_TUNNEL_WAIT:-120}"
PHONE_PORT="${NEODCT_PHONE_PORT:-2222}"

say() { echo "e2e: $*"; }
die() { echo "e2e: $*" >&2; exit 1; }

QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "${HOLDER_PID:-}" ] && kill "$HOLDER_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# A copy of the images, so a QEMU somebody else is running keeps its lock and
# its data. Every image is rewritten by a Buildroot rebuild anyway.
stage_images() {
    mkdir -p "$IMAGES"
    for f in Image initramfs.cpio.gz system.img sdcard.img; do
        [ -f "$SRC_IMAGES/$f" ] && cp -f "$SRC_IMAGES/$f" "$IMAGES/$f"
    done
    if [ -z "${NEODCT_E2E_KEEP:-}" ] || [ ! -f "$IMAGES/userdata.ext4" ]; then
        cp -f "$SRC_IMAGES/userdata.ext4" "$IMAGES/userdata.ext4"
    fi
}

one_run() {
    run_no="$1"
    say "run $run_no: staging images"
    stage_images

    "$RELAY" up >/dev/null
    NEODCT_IMAGES="$IMAGES" "$RELAY" seed "$IMAGES/userdata.ext4" >/dev/null
    say "run $run_no: relay up, userdata seeded"

    ser="$WORK/serial.fifo"
    log="$WORK/serial.log"
    rm -f "$ser" "$log"
    mkfifo "$ser"
    # Something has to hold the write end open or QEMU sees EOF at once.
    sh -c 'while :; do sleep 300; done' > "$ser" &
    HOLDER_PID=$!

    NEODCT_IMAGES="$IMAGES" NEODCT_DISPLAY=offscreen NEODCT_AUDIO=none \
    NEODCT_SD=none NEODCT_QEMU_EXTRA="$("$RELAY" qemu-args)" \
        "$HERE/run_qemu.sh" < "$ser" > "$log" 2>&1 &
    QEMU_PID=$!

    say "run $run_no: booting (qemu $QEMU_PID)"
    waited=0
    while [ "$waited" -lt "$BOOT_WAIT" ]; do
        grep -aq 'login:' "$log" 2>/dev/null && break
        sleep 2
        waited=$((waited + 2))
    done
    grep -aq 'login:' "$log" 2>/dev/null || { tail -20 "$log"; die "run $run_no: never booted"; }
    say "run $run_no: booted in ${waited}s"

    say "run $run_no: waiting for the phone to dial out"
    waited=0
    while [ "$waited" -lt "$TUNNEL_WAIT" ]; do
        ss -tln 2>/dev/null | grep -q "127.0.0.1:$PHONE_PORT " && break
        sleep 3
        waited=$((waited + 3))
    done
    if ! ss -tln 2>/dev/null | grep -q "127.0.0.1:$PHONE_PORT "; then
        echo "--- the phone's own remote.log ---"
        printf 'root\n' > "$ser"; sleep 2
        printf 'cat /NeoDCT/User/.remote/remote.log\n' > "$ser"
        sleep 3
        tail -40 "$log"
        die "run $run_no: the tunnel never came up"
    fi
    say "run $run_no: tunnel up in ${waited}s"

    say "run $run_no: logging in through it"
    if out="$("$RELAY" ssh 'echo READY; cat /etc/os-release | grep VERSION_ID' 2>&1)"; then
        echo "$out" | sed 's/^/  /'
        case "$out" in
            *READY*) say "run $run_no: PASS" ;;
            *)       die "run $run_no: logged in but got nothing back" ;;
        esac
    else
        echo "$out" | sed 's/^/  /'
        echo "--- the phone's own remote.log ---"
        printf 'root\n' > "$ser"; sleep 2
        printf 'cat /NeoDCT/User/.remote/remote.log\n' > "$ser"
        sleep 3
        tail -30 "$log"
        die "run $run_no: could not log in"
    fi

    kill "$QEMU_PID" 2>/dev/null || true
    kill "$HOLDER_PID" 2>/dev/null || true
    QEMU_PID=""
    HOLDER_PID=""
    sleep 2
}

n=1
while [ "$n" -le "$RUNS" ]; do
    one_run "$n"
    n=$((n + 1))
done
say "$RUNS run(s) passed"

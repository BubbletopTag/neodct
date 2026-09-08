#!/bin/sh
# parity_capture_hw.sh -- the phone side of the parity harness.
#
# ============ THIS SCRIPT HAS NEVER BEEN RUN EITHER ============
#
# Nobody working on this branch has a Luckfox Pico Mini B. There is no
# tests/parity/luckfox-armv7.inventory, test_parity_allowlist.py runs
# single-sided and says "0 of N records verified against hardware" on every
# run, and every `hw` value in allow.txt is a claim carrying
# hw_evidence: unmeasured-claim. This file is the procedure for turning that
# into a measurement, written now so that whoever first has a phone in hand
# does not also have to design the capture.
#
#     parity_capture_hw.sh --ssh neodct   --out tests/parity/luckfox-armv7.inventory
#     parity_capture_hw.sh --serial /dev/ttyUSB0 --out ...
#
# ============ WHY TWO TRANSPORTS AND ONE ARTEFACT ============
#
# Over Remote Shell the capture is one line. Over serial it is the same scrape
# the emulator side does, because REMOTE_SHELL.md section 7 records that the
# armv7 kernel has no NETDEVICES at all -- so the emulator has no NIC and
# cannot use ssh even in principle. That asymmetry is the reason the framing
# is transport-independent rather than the reason there are two formats: every
# body line carries the INV| sentinel, the body is bracketed, and the trailer
# carries two SHA-256s, so a capture lifted out of a printk-interleaved serial
# log and a capture read off a socket are the same bytes and either one can
# check itself.
#
# ============ IT IS SAFE TO RUN ON A PHONE SOMEBODY IS HOLDING ============
#
# nd-inventory never forks, never changes euid, performs no operation whose
# success is the answer, and opens /dev/fb0 O_RDONLY for two GET ioctls and
# nothing else under /dev. That last rule is not fastidiousness: opening
# /dev/ttyUSB2 raises DTR on a real SIM7600. It also reads no clock and no
# sensor, and nothing at all under /NeoDCT/User -- the phonebook, the SMS
# databases and Remote Shell's private key live there, and an inventory that
# walked it would put the owner's data into a git-committed file.

set -eu

MODE=""
TARGET=""
OUT=""
BIN=/NeoDCT/System/bin/nd-inventory

while [ $# -gt 0 ]; do
    case "$1" in
        --ssh)    MODE=ssh;    TARGET="$2"; shift 2 ;;
        --serial) MODE=serial; TARGET="$2"; shift 2 ;;
        --out)    OUT="$2";    shift 2 ;;
        -h|--help) sed -n '2,40p' "$0"; exit 2 ;;
        *) echo "parity_capture_hw.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
[ -n "$MODE" ] && [ -n "$OUT" ] || {
    echo "usage: parity_capture_hw.sh (--ssh HOST | --serial DEV) --out FILE" >&2
    exit 2; }

WORK="${TMPDIR:-/tmp}/nd-parity-hw.$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

case "$MODE" in
    ssh)
        # --self-check FIRST, and its failure is a refusal rather than a
        # warning. It is what turns "this format is byte-stable" from a claim
        # this design makes into a property the tool PROVES about every
        # machine it stands on -- including this one, which nobody here can
        # test any other way.
        #
        # WITH NO SECTION ARGUMENT, and the difference is the whole guarantee.
        # It used to say `--self-check kernel`: three strings from one uname(2)
        # call, deterministic by construction, a gate that cannot fire. The
        # collectors whose stability is at issue are the readdir-driven ones --
        # /sys/class, the /dev walk, /proc/mounts, /proc/bus/input/devices --
        # and the kernel section touches none of them. Measured in the probe
        # guest: the kernel section reports "ok (3 records, twice)" and the
        # full check "ok (172 records, twice)", at no measurable cost. On a
        # phone the concrete case is capturing before udev has settled or while
        # the modem re-enumerates a ttyUSB, and the capture that followed would
        # have been committed as luckfox-armv7.inventory.
        ssh "$TARGET" "$BIN --self-check >/dev/null" || {
            echo "REFUSED: --self-check failed on $TARGET -- two collections in one" >&2
            echo "         process differed, so this phone cannot produce a baseline." >&2
            exit 1; }
        ssh "$TARGET" "$BIN" > "$WORK/raw.txt" || {
            echo "REFUSED: nd-inventory exited non-zero on $TARGET; the capture is" >&2
            echo "         incomplete and must not be committed as a baseline." >&2
            exit 1; }
        ;;
    serial)
        command -v picocom >/dev/null 2>&1 || command -v cu >/dev/null 2>&1 || {
            echo "no picocom or cu; drive the console by hand and run:" >&2
            echo "    $BIN --self-check && $BIN" >&2
            echo "then pipe the log through: grep '^INV|' | tr -d '\\r'" >&2
            exit 2; }
        # 115200 8N1, root with no password -- AGENTS.md, Hardware access.
        {
            printf 'root\n'
            sleep 2
            printf '%s --self-check >/dev/null 2>&1; echo SELFCHECK=$?\n' "$BIN"
            sleep 5
            printf '%s; echo CAPTURE-RC=$?\n' "$BIN"
            sleep 20
        } | picocom -b 115200 -qrX "$TARGET" > "$WORK/raw.txt" 2>&1 || true
        grep -qa 'SELFCHECK=0' "$WORK/raw.txt" || {
            echo "REFUSED: --self-check did not pass on the phone" >&2; exit 1; }
        grep -qa 'CAPTURE-RC=0' "$WORK/raw.txt" || {
            echo "REFUSED: nd-inventory reported the capture incomplete" >&2; exit 1; }
        ;;
esac

# CR is stripped because a serial console turns LF into CRLF; nothing else is
# touched. That is the whole of the difference between the two transports, and
# it is why they produce byte-identical artefacts.
tr -d '\r' < "$WORK/raw.txt" | grep '^INV|' > "$WORK/capture.txt" || true
grep -q '^INV|BEGIN$' "$WORK/capture.txt" || { echo "REFUSED: no BEGIN in the capture" >&2; exit 1; }
grep -q '^INV|END$'   "$WORK/capture.txt" || { echo "REFUSED: no END in the capture" >&2; exit 1; }

method=$(sed -n 's/^INV|# capture\.method=\(.*\)$/\1/p' "$WORK/capture.txt")
[ "$method" = "nd-inventory" ] || {
    echo "REFUSED: capture.method is '${method:-nothing}'. Only the tool itself may" >&2
    echo "         produce a reference; the shell fallback and the no-libneodct probe" >&2
    echo "         build are weaker instruments and parity_diff.py will refuse them." >&2
    exit 1; }

mkdir -p "$(dirname "$OUT")"
cp "$WORK/capture.txt" "$OUT"
echo "captured $(grep -c '^INV|' "$OUT") framed lines into $OUT"
echo
echo "Now: parity_diff.py --qemu <the emulator capture> --hw $OUT \\"
echo "                    --allow neodct/tests/parity/allow.txt"
echo "and change hw_evidence to 'measured' on every record it confirms, one at"
echo "a time. That number is the only thing that turns this allowlist from a"
echo "claim into a measurement."

#!/bin/sh
# "Format this card", end to end, and the two things that made it fail.
#
#   neodct/tools/test_format_flow.sh              everything
#   neodct/tools/test_format_flow.sh --no-qemu    the host halves only
#
# ============ THE REPORT THIS EXISTS FOR ============
#
# On 0.5.9a, on real hardware, the owner wrote:
#
#   "I got this beautiful formatting progress bar. Waited for it to hit 100%,
#    then it told me that the format failed."
#
# Two separate faults, and neither of them could be reproduced on the 2 GB card
# the rest of the suite uses:
#
#   THE BAR'S DENOMINATOR WAS THE KILL DEADLINE. apps/Settings drew
#   `secs * BAR_TOTAL / ND_SVC_FORMAT_WAIT_S` and nd_svc.c stopped the helper
#   at `elapsed >= ND_SVC_FORMAT_WAIT_S`, the same flat 240 s in both. Filling
#   the bar and losing the card were one event, by construction.
#
#   AND THE FORMAT REALLY WAS TAKING LONGER THAN 240 s. mke2fs was writing
#   every inode table by hand -- one byte for every sixty-four bytes of card,
#   which is 32 MiB on the test card and 2 GiB on a 128 GB one. Small metadata
#   writes are the slowest thing an SD card does.
#
# So this script has three sections, and only the last one needs QEMU:
#
#   1. mke2fs is fast and the saving scales with the card, measured on a
#      32 GiB image against the helper's OWN mkfs_ext4 function. This is the
#      half that cannot be shown on a 2 GB card, because on a 2 GB card the
#      slow version is fast enough.
#   2. A format that is killed halfway leaves the card described honestly,
#      driven by sending the real helper a real SIGTERM mid-mke2fs.
#   3. The whole thing through the real UI: a blank card in a booted phone,
#      Settings walked with QEMU's `sendkey`, and the card usable afterwards.
#
# The third one is modelled on test_card_flow.sh, which already solved the
# boot, monitor and cleanup plumbing; the differences are noted where they are.
#
# What is NOT here, deliberately: a card big enough to time out. Proving the
# deadline scales needs no card at all -- it is arithmetic, and test_svc.c's
# test_a_full_bar_is_not_a_failure pins it across sizes from unknown to 2 TB,
# including that a full bar is nowhere near the kill. Section 1 below is the
# other half of that: the thing being timed is now bounded.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
ROOT="$(dirname "$REPO")"
HELPER="$REPO/overlay/NeoDCT/System/hw/neodct-sdcard"

WORK="${NEODCT_FORMAT_DIR:-$ROOT/build-cardtest/format}"
IMAGES="$WORK/images"
SRC_IMAGES="${NEODCT_SRC_IMAGES:-$ROOT/buildroot/output/images}"
MONITOR="$WORK/monitor.sock"
CARD_MB="${NEODCT_FORMAT_CARD_MB:-2048}"

# The size the mkfs measurement uses. Big enough that the old command's inode
# tables are unmistakable (size/64 = 512 MiB) and sparse, so it costs the disk
# only what mke2fs actually writes -- which is the number being measured.
PROBE_GIB="${NEODCT_FORMAT_PROBE_GIB:-32}"

WITH_QEMU=1
[ "${1:-}" = "--no-qemu" ] && WITH_QEMU=0

say()  { echo "format: $*"; }
pass() { echo "format: PASS  $*"; }
die()  { echo "format: FAIL  $*" >&2; exit 1; }

QEMU_PID=""
HOLDER_PID=""
MON_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$HOLDER_PID" ] && kill "$HOLDER_PID" 2>/dev/null || true
    [ -n "$MON_PID" ] && kill "$MON_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

mkdir -p "$WORK"
[ -r "$HELPER" ] || die "no helper at $HELPER"

# =====================================================================
# 1. mke2fs is fast, and the saving is the size of the card
# =====================================================================
#
# The helper's own mkfs_ext4 is called, sourced out of the shipped script, so
# this measures the command line that actually runs on the phone rather than a
# copy of it that can drift. NEODCT_SDCARD_SOURCE_ONLY is the script's existing
# "define the functions and do not dispatch" switch, which the pytest uses too.
#
# The measurement is ALLOCATED BLOCKS, not seconds. Seconds on a host with a
# page cache and an NVMe say nothing about an SD card; bytes written say
# everything, because the card's write rate is the constant this was always
# hostage to. A 32 GiB card whose format writes 5 MiB cannot take four minutes
# on any medium the phone will accept.
say "1. mke2fs on a ${PROBE_GIB} GiB card"

PROBE="$WORK/probe.img"
allocated_mib() { du -m "$1" | cut -f1; }

run_mkfs_variant() {   # run_mkfs_variant NAME COMMAND...
    _name="$1"; shift
    rm -f "$PROBE"
    truncate -s "${PROBE_GIB}G" "$PROBE"
    if ! "$@" >/dev/null 2>&1; then
        rm -f "$PROBE"
        echo "-1"
        return 0
    fi
    allocated_mib "$PROBE"
}

command -v mke2fs >/dev/null 2>&1 || die "no mke2fs on this host to measure"

# The command as it shipped in 0.5.9a, with the discard turned off so the
# number is the one a REAL CARD produces. On a file, mke2fs discards by
# punching a hole, sees that the hole reads back as zeros, and skips the inode
# table wipe it would otherwise do -- a shortcut no SD card offers, and one
# that would hide the entire fault being measured here.
OLD=$(run_mkfs_variant old mke2fs -t ext4 -F -m0 -L NEODCT \
        -O ^64bit,^metadata_csum -E nodiscard "$PROBE")
[ "$OLD" = "-1" ] && die "the 0.5.9a mke2fs command would not run here"

NEW=$(run_mkfs_variant new sh -c \
        "NEODCT_SDCARD_SOURCE_ONLY=1; export NEODCT_SDCARD_SOURCE_ONLY;
         . '$HELPER'; LABEL=NEODCT; mkfs_ext4 '$PROBE'")
[ "$NEW" = "-1" ] && die "the helper's mkfs_ext4 would not format $PROBE"

say "   0.5.9a command wrote ${OLD} MiB; the helper now writes ${NEW} MiB"

# A tenth is a wide margin around a change that is really two orders of
# magnitude (263 MiB -> 5 MiB when this was written). Wide on purpose: the
# claim is "the cost no longer scales with the card", and pinning the exact
# number would break on any e2fsprogs that sizes a journal differently.
[ "$NEW" -lt $((OLD / 10)) ] || \
    die "the new mke2fs writes ${NEW} MiB against ${OLD} MiB -- lazy init is not engaged"

# ============ AND THE FEATURE THAT MAKES IT WORK ============
#
# lazy_itable_init does NOTHING, silently, unless the group descriptors carry a
# checksum -- uninit_bg or metadata_csum. This command line turns metadata_csum
# off (an older e2fsprogs on the owner's PC has to be able to read the card), so
# uninit_bg is what the -E option rests on. Dropping it would leave a format
# that looks identical, passes every other check here, and writes every inode
# table again.
dumpe2fs -h "$PROBE" 2>/dev/null | grep -q "uninit_bg" || \
    die "the card has no uninit_bg: -E lazy_itable_init is being ignored"
dumpe2fs -h "$PROBE" 2>/dev/null | grep -q "metadata_csum" && \
    die "metadata_csum came back: an older e2fsprogs can no longer read the card"

# A fast format that produces a broken filesystem is not a fix.
e2fsck -fn "$PROBE" >"$WORK/fsck.log" 2>&1 || die "e2fsck on the new card failed; see $WORK/fsck.log"
rm -f "$PROBE"
pass "the format is bounded: ${NEW} MiB written for a ${PROBE_GIB} GiB card, and it fscks clean"

# =====================================================================
# 2. A format that is stopped halfway says so
# =====================================================================
#
# The core stops a helper that passes its deadline, and the owner can press
# Clear at any time; both arrive as SIGTERM. Until this was fixed nothing
# caught it: the script died mid-mke2fs and the state file still said
# `state=mounted device=...` for a card whose partition table had just been
# rewritten and whose filesystem was half made. The UI went on naming a device
# and offering folders that were not there.
#
# Driven with a real signal against the real helper functions, because the two
# things being checked are both about what a POSIX shell does under one: that
# the trap runs at all (it does not, if mke2fs is in the FOREGROUND -- hence
# run_mkfs), and that the mke2fs is killed rather than orphaned.
say "2. a format stopped halfway"

TRAPDIR="$WORK/trap"
rm -rf "$TRAPDIR"
mkdir -p "$TRAPDIR/bin" "$TRAPDIR/run"

cat > "$TRAPDIR/bin/mke2fs" <<'STUB'
#!/bin/sh
# Stands in for an mke2fs on a very slow card: it never finishes on its own.
exec sleep 300
STUB
chmod +x "$TRAPDIR/bin/mke2fs"

cat > "$TRAPDIR/run.sh" <<EOF
NEODCT_SDCARD_SOURCE_ONLY=1
export NEODCT_SDCARD_SOURCE_ONLY
PATH="$TRAPDIR/bin:\$PATH"
. "$HELPER"
STATE_DIR="$TRAPDIR/run"
STATE_FILE="\$STATE_DIR/sdcard.prop"
FORMAT_MARKER="\$STATE_DIR/.formatting"
LABEL=NEODCT
: > "\$FORMAT_MARKER"
write_state mounted /dev/fake1 ext4 NEODCT
trap 'format_interrupted /dev/fake1' TERM INT HUP
mkfs_ext4 /dev/fake1
EOF

sh "$TRAPDIR/run.sh" > "$TRAPDIR/out.log" 2>&1 &
TRAP_SH=$!
sleep 1
grep -q "^state=mounted" "$TRAPDIR/run/sdcard.prop" || die "the fixture did not start from a mounted card"

# The stub's pid, taken BEFORE the signal. Named rather than pattern-matched
# later: this checkout is worked in by several people at once and "any process
# whose command line mentions sleep" would find one of theirs.
MKFS_CHILD="$(pgrep -P "$TRAP_SH" | head -1 || true)"
[ -n "$MKFS_CHILD" ] || die "run_mkfs did not put the mke2fs in the background -- the trap cannot fire"

kill -TERM "$TRAP_SH" 2>/dev/null || true
waited=0
while [ "$waited" -lt 20 ]; do
    kill -0 "$TRAP_SH" 2>/dev/null || break
    sleep 0.5
    waited=$((waited + 1))
done
kill -0 "$TRAP_SH" 2>/dev/null && die "the helper ignored SIGTERM entirely"

grep -q "^state=unformatted" "$TRAPDIR/run/sdcard.prop" || {
    cat "$TRAPDIR/run/sdcard.prop" >&2
    die "a killed format left the old state behind -- the UI would still name a device"
}
grep -q "^device=/dev/fake1" "$TRAPDIR/run/sdcard.prop" || \
    die "the killed format forgot which device it was on; Settings could not offer to retry"
[ -e "$TRAPDIR/run/.formatting" ] && \
    die "the in-flight marker survived: udev's add/remove handlers stay disabled until reboot"

# The child, not just the script. The core signals the helper's pid and not its
# process group, so an mke2fs left running here would keep writing the card
# with its parent gone and the phone telling the owner it had stopped.
sleep 1
if kill -0 "$MKFS_CHILD" 2>/dev/null; then
    kill -KILL "$MKFS_CHILD" 2>/dev/null || true
    die "the mke2fs (pid $MKFS_CHILD) was orphaned rather than killed"
fi
pass "a stopped format publishes state=unformatted, clears its marker and takes its mke2fs with it"

if [ "$WITH_QEMU" -eq 0 ]; then
    say "--no-qemu: stopping before the end-to-end run"
    exit 0
fi

# =====================================================================
# 3. The whole thing, through the real UI
# =====================================================================
say "3. formatting a card from Settings, under QEMU"

command -v socat >/dev/null || die "socat is needed to drive the monitor"
for f in Image initramfs.cpio.gz system.img userdata.ext4; do
    [ -r "$SRC_IMAGES/$f" ] || die "no $f in $SRC_IMAGES -- build the image set first"
done

mkdir -p "$IMAGES"
for f in Image initramfs.cpio.gz system.img userdata.ext4; do
    cp -f "$SRC_IMAGES/$f" "$IMAGES/$f"
done

# ============ A BLANK CARD, AND WHY NOT THE STOCK ONE ============
#
# The stock sdcard.img is already a NeoDCT card, and show_memory_card() does not
# offer to format one of those -- correctly; a card that works is not a card to
# erase. The state that reaches the Format offer is ND_CARD_UNFORMATTED: there
# IS something in the slot and nothing on it will mount. Which is also the case
# the owner was in, and the one that exercises the whole of do_format --
# partition table, BLKRRPART, mke2fs, mount, folders, layout.
#
# Sparse, so a 2 GiB card of zeros costs the disk nothing until the format
# writes to it -- and what it then costs is a direct reading of what the format
# actually wrote.
rm -f "$IMAGES/sdcard.img"
truncate -s "${CARD_MB}M" "$IMAGES/sdcard.img"
say "card: ${CARD_MB} MiB of nothing at $IMAGES/sdcard.img"

ser="$WORK/serial.fifo"
log="$WORK/serial.log"
rm -f "$ser" "$log" "$MONITOR"
mkfifo "$ser"
sh -c 'while :; do sleep 600; done' > "$ser" &
HOLDER_PID=$!

NEODCT_IMAGES="$IMAGES" NEODCT_DISPLAY=offscreen NEODCT_AUDIO=none \
NEODCT_MONITOR="$MONITOR" "$HERE/run_qemu.sh" < "$ser" > "$log" 2>&1 &
QEMU_PID=$!
say "booting (qemu $QEMU_PID)"

waited=0
while [ "$waited" -lt 180 ]; do
    grep -aq 'Custom font loaded' "$log" 2>/dev/null && break
    sleep 2
    waited=$((waited + 2))
done
grep -aq 'Custom font loaded' "$log" 2>/dev/null || { tail -20 "$log"; die "the UI never started"; }
say "UI up in ${waited}s"

# One monitor connection for the whole run, held open on fd 9. A connection per
# key loses most of them -- test_card_flow.sh found that the hard way.
rm -f "$WORK/mon.fifo"
mkfifo "$WORK/mon.fifo"
socat - "UNIX-CONNECT:$MONITOR" < "$WORK/mon.fifo" > "$WORK/mon.out" 2>&1 &
MON_PID=$!
exec 9> "$WORK/mon.fifo"

# ============ WHY THE KEYS ARE HELD ============
#
# `sendkey down` on its own presses and releases in the same instant, and the
# guest drops some of them -- observed here as three arrows sent and two
# arriving, which opens the wrong app three screens later with nothing in any
# log to say why. QEMU's monitor takes a hold time in milliseconds as a second
# argument; 120 ms is roughly a human press and the loss goes away. Nothing
# downstream depends on it being exactly that.
key_blind() { printf 'sendkey %s 120\n' "$1" >&9; sleep "${2:-0.5}"; }
shot() {
    printf 'screendump %s\n' "$WORK/$1.ppm" >&9
    sleep 1
    [ -f "$WORK/$1.ppm" ] && say "   screen saved: $WORK/$1.ppm"
}

# The readiness probe, exactly as test_card_flow.sh does it: nd_main.c logs
# "Code: N" from the CORE's loop, and the moment a widget with its own read
# loop is on screen nothing is logged again -- so the first key is the only one
# the guest can acknowledge, and it doubles as "the UI is reading input now".
key_probe() {
    before=$(grep -ac "Code: $2" "$log" 2>/dev/null) || before=0
    attempt=0
    while [ "$attempt" -lt 40 ]; do
        printf 'sendkey %s 120\n' "$1" >&9
        tries=0
        while [ "$tries" -lt 12 ]; do
            now=$(grep -ac "Code: $2" "$log" 2>/dev/null) || now=0
            [ "$now" -gt "$before" ] && return 0
            sleep 0.25
            tries=$((tries + 1))
        done
        attempt=$((attempt + 1))
    done
    die "the phone never saw '$1' (code $2) -- the UI is not reading input"
}

sleep 5   # let the home screen finish its first draw
say "opening the menu"
key_probe ret 28

# Settings is app id 4 and the menu is sorted by id, so it is the fourth entry
# whatever else is installed: every engineering-mode app has a five-digit id
# and sorts after it. Counting from the top is therefore stable in a way that
# counting to an app near the bottom is not.
# ============ WALKING TO SETTINGS, AND CHECKING THAT IT ARRIVED ==========
#
# Settings is app id 4 and the menu is sorted by id, so it is the fourth entry
# whatever else is installed: every engineering-mode app has a five-digit id and
# sorts after it. Counting three down from the top is therefore stable in a way
# that counting to an app near the bottom is not.
#
# It is CHECKED rather than assumed, and then CORRECTED. nd_ui.c logs the
# selector's index on every launch -- the one acknowledgement available inside a
# widget that owns its own key loop -- and nd_appsel_init() puts the selection
# back at the top every time the menu is opened. So the walk is repeatable, a
# dropped arrow is measurable as a shortfall, and the fix is to press the
# difference again rather than to give up on a run that has already booted a
# phone.
SETTINGS_INDEX=3

launched_index() {
    grep -ao "Launching App ID: [0-9]*" "$log" 2>/dev/null | tail -1 | \
        sed 's/.*: //'
}

back_to_home() {
    # CLEAR is back on every screen and exits an app from its top level.
    # Six is more than the deepest place this walk can leave us, and extra
    # ones at the home screen do nothing.
    i=0
    while [ "$i" -lt 6 ]; do key_blind backspace 0.8; i=$((i + 1)); done
}

# key_probe left the menu OPEN -- the keypress it was waiting to be
# acknowledged is the one that opens it. The walk below starts from the home
# screen every time, so close it again rather than special-casing the first
# turn of a loop whose whole point is that it can be repeated.
back_to_home

say "Settings (entry $SETTINGS_INDEX, counting from the top)"
downs=$SETTINGS_INDEX
attempt=0
opened=0
while [ "$attempt" -lt 6 ]; do
    launches_before=$(grep -ac "Launching App ID:" "$log" 2>/dev/null) || launches_before=0
    key_blind ret 2          # the menu, from the home screen
    i=0
    while [ "$i" -lt "$downs" ]; do key_blind down 0.8; i=$((i + 1)); done
    key_blind ret 5
    launches_now=$(grep -ac "Launching App ID:" "$log" 2>/dev/null) || launches_now=0
    if [ "$launches_now" -gt "$launches_before" ]; then
        got=$(launched_index)
        [ "$got" = "$SETTINGS_INDEX" ] && { opened=1; break; }
        say "   opened entry $got, wanted $SETTINGS_INDEX -- $((SETTINGS_INDEX - got)) arrows were dropped"
        downs=$((downs + SETTINGS_INDEX - got))
        [ "$downs" -lt 1 ] && downs=$SETTINGS_INDEX
    else
        say "   nothing launched; the menu key itself was dropped"
    fi
    back_to_home
    attempt=$((attempt + 1))
done
if [ "$opened" -eq 0 ]; then
    shot failed-wrong-app
    grep -a "Launching App ID" "$log" | tail -3
    die "could not get to Settings: the menu keeps opening something else"
fi
shot 1-settings

say "Memory card (item 2 -- VerticalList takes digits directly)"
key_blind 2 4
shot 2-card

# ============ WALKING TO THE FORMAT WITH ENTER ALONE ============
#
# The route is a message dialog, then a help scroller of unknown length, then
# the format warning. ENTER pages the scroller and leaves it on the last page,
# and ENTER is the affirmative on both dialogs -- so pressing it until the core
# says a format has started needs no assumption about how many pages the help
# ran to. ENTER is also ignored by the progress screen (only Clear means
# anything there), so an extra press cannot cancel what it just started.
say "walking to Format"
started=0
i=0
while [ "$i" -lt 25 ]; do
    if grep -aq "App service: formatting" "$log" 2>/dev/null; then started=1; break; fi
    key_blind ret 1.2
    i=$((i + 1))
done
if [ "$started" -eq 0 ]; then
    shot failed-no-format
    tail -30 "$log"
    die "no format was ever started -- the card screen did not reach the offer"
fi
shot 3-formatting

# ============ THE DEADLINE IS THE CARD'S, NOT A CONSTANT ============
#
# The core logs the size it read and what it allowed before it spawns anything.
# On this 2 GiB card that is the floor, 90 s -- and the point is that it is a
# number derived from the card at all. 240 s for every card, which is what
# shipped, is the half of the owner's bug that a 2 GB test card can never show.
grep -a "a format is allowed" "$log" | tail -1 | sed 's/^/   /'
grep -aq "a format is allowed" "$log" || \
    die "the core did not derive a deadline from the card"
grep -a "a format is allowed" "$log" | grep -q "allowed 240s" && \
    die "the deadline is still the flat 240 s"

say "waiting for the format to finish"
waited=0
while [ "$waited" -lt 240 ]; do
    grep -aq "as a NeoDCT card" "$log" 2>/dev/null && break
    grep -aq "has not finished in" "$log" 2>/dev/null && break
    sleep 2
    waited=$((waited + 2))
done
shot 4-after

grep -aq "has not finished in" "$log" && die "the core stopped the format at its deadline"
grep -aq "was stopped before it finished" "$log" && die "the helper was interrupted"
grep -aq "formatted .* as a NeoDCT card" "$log" || {
    tail -40 "$log"
    die "the format never reported success"
}
say "format finished in about ${waited}s"

# The card is not just formatted, it is USABLE: do_format hands the new
# partition to try_mount, which mounts it, makes the five folders, writes the
# marker and applies the layout. That is the second half of "the card is usable
# afterwards", and a card that mkfs alone had touched would not have it. What
# the layout actually produced is checked against the image below; this is the
# phone saying it got that far.
grep -aq "mounted .* at /NeoDCT/User/sdcard" "$log" || {
    tail -40 "$log"
    die "the new filesystem was made but never mounted"
}

# And the UI's own user can read the record of it. nd_storage.c logs this line
# when the ndusr core can see the state file and not open it -- the 0.5.0b
# failure where a freshly formatted card read as "No memory card." until the
# next reboot. Its absence is the check.
grep -aq "cannot be read" "$log" && die "the core could not read the card state it had just written"
grep -aq "the card status file could not be read" "$log" && \
    die "Settings could not read the card state after the format"
pass "the phone formatted the card, mounted it and laid it out"

# ---- and the card itself, read from the host ---------------------------
#
# QEMU is stopped first: the image is being written by the guest and reading it
# underneath a running kernel would be reading a filesystem mid-update.
say "stopping the phone and reading the card"
kill "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""
sleep 3

DEBUGFS="$ROOT/buildroot/output/host/sbin/debugfs"
[ -x "$DEBUGFS" ] || DEBUGFS="$(command -v debugfs || true)"
[ -n "$DEBUGFS" ] || { say "no debugfs; skipping the on-card check"; exit 0; }

# One ext4 partition starting at 1 MiB -- what write_mbr lays down. e2fsprogs'
# unix_io takes the offset in the filename, so no loop device and no root.
CARD_FS="$IMAGES/sdcard.img?offset=1048576"
listing="$("$DEBUGFS" -R "ls -l /" "$CARD_FS" 2>/dev/null || true)"
[ -n "$listing" ] || die "there is no ext4 filesystem at 1 MiB on the card"
echo "$listing" | sed 's/^/   /'

# The five the UI hands out paths to, plus the two the layout makes and the
# marker card_is_ours() looks for. All of them, because "the card mounted" and
# "the card is a NeoDCT card" are different claims and only the second one is
# what the owner was promised.
for entry in wallpapers tones backup_db music update apps untrusted .neodct; do
    echo "$listing" | grep -q " $entry\$" || die "the formatted card has no $entry"
done

# ============ AND IT BELONGS TO THE PHONE'S USER ============
#
# An ext mount carries real numeric ownership and is deliberately given no
# uid=/gid= (neodct-sdcard's try_mount), so a card whose folders mke2fs left as
# root:root is one the ndusr core cannot write a byte to -- which is the
# ND_CARD_FOREIGN state, and was the "Could not write to the card. It may be
# locked or damaged." the owner used to get from a card the phone had made
# itself. The uids come from the table that creates them, not from `id -u`:
# this host's ndusr, if it has one, is not the phone's.
NDUSR_UID="$(awk '$1 == "ndusr" { print $2; exit }' "$REPO/configs/users-table.txt")"
NDUSR_UT_GID="$(awk '$1 == "ndusr_ut" { print $4; exit }' "$REPO/configs/users-table.txt")"
[ -n "$NDUSR_UID" ] || die "no ndusr in configs/users-table.txt"
for entry in wallpapers tones backup_db music update; do
    owner="$(echo "$listing" | awk -v e=" $entry\$" '$0 ~ e { print $4 }')"
    [ "$owner" = "$NDUSR_UID" ] || \
        die "$entry on the card belongs to uid ${owner:-?}, not to ndusr ($NDUSR_UID) -- the phone cannot write to its own card"
done
ut_gid="$(echo "$listing" | awk '$0 ~ " untrusted$" { print $5 }')"
[ "$ut_gid" = "$NDUSR_UT_GID" ] || \
    die "untrusted/ has gid ${ut_gid:-?}, not ndusr_ut ($NDUSR_UT_GID) -- a download would have nowhere to land"

pass "the card holds one ext4 partition, laid out and owned by the phone's own users"

say "ALL PASS -- screens and logs under $WORK"

#!/bin/sh
# inventory-fallback.sh -- the degraded inventory, for a phone whose rootfs
# cannot be rebuilt.
#
# ============ IT IS STRUCTURALLY FORBIDDEN FROM BEING A REFERENCE ==========
#
# This exists for exactly one case: a bring-up board or an unbuildable tree,
# where somebody has a serial console and no way to put nd-inventory on the
# machine. It is NOT a second implementation of the format and it must never
# become one, so its subordinate status is built into what it emits rather
# than written in a comment somebody can ignore:
#
#   * it uses the same framing, the same INV| sentinel and the same key names;
#   * it stamps capture.method=shell-fallback in the preamble, and
#     parity_diff.py REFUSES such a capture as a baseline or as either side of
#     a gating comparison. It will only ever be printed for a human.
#
# ============ AND IT IS A STRICT SUBSET, WHICH IT SAID IT WAS NOT ==========
#
# This claimed that "every record it CANNOT produce is emitted as
# UNAVAILABLE(shell) rather than omitted -- so the framebuffer block is four
# visible holes and not a shorter file". It is not true and it was never
# close: the C tool produces FIFTEEN fb0.* records and this emits four, and
# whole sections are missing outright --
#
#     cmdline.*  dev.*  mount.*  block.*  input.*  proc.*  mtd.*  ubi.*
#     os.*  zram0.disksize  mem-derived records beyond the two below
#
# -- so a capture from here carries about forty records against the tool's
# ~175. Held beside a real one, a human reads ~135 records as ABSENT on the
# phone: the file's own loudest signal ("a whole missing record is the loudest
# thing this harness can find") fired ~135 times by the INSTRUMENT rather than
# by the machine, which is exactly the reader-difference-as-machine-difference
# failure the design says it exists to prevent.
#
# Emitting a hole for every key the C tool can produce is the other fix and it
# is worse: the key list is not enumerable from a shell -- dev.* and mount.*
# are keyed by what is on the machine -- so it would be a hand-maintained copy
# of the collectors, drifting from them silently. So the honest thing is to
# say which sections are here and which are not, out loud, at the top.
#
# ============ WHY THE HOLES ARE WHERE THEY ARE ============
#
# FBIOGET_VSCREENINFO and FBIOGET_FSCREENINFO cannot be done from ash at all,
# and those are the most valuable records in the file: the panel divergence
# neodctDisplay.c documents is the PIXEL FORMAT, and after force_mode() both
# machines read back 240x175 bpp=32 line_len=960 with red at offset 0. That is
# the whole Stage 3 claim in eleven numbers and none of them is reachable from
# here.
#
# Neither is the compiled-in platform constant. overlay/bin/nd-platform's own
# header says what re-deriving it costs: "it will happily answer hw on an
# image the C has already refused to call anything." So this reads the RECORD
# in /NeoDCT/platform and reports the library's half as UNAVAILABLE(shell)
# rather than guessing -- a guess here would be a shell script overruling the
# one check that catches a mis-assembled image.
#
# And it is a bad instrument for byte-stability in its own right, which is the
# argument that decided the tool would be C. A symbolic mode encodes setuid
# and sticky ambiguously and busybox renders them differently between applet
# builds; `sort` without LC_ALL=C is locale-dependent; `printf` of a 32-bit
# size differs between builds. Every one of those is a READER difference that
# a diff would report as a MACHINE difference. LC_ALL=C is forced below and
# modes are printed in octal, which narrows it and does not close it.

LC_ALL=C
export LC_ALL

# The first field is the SORT key and is cut off after the sort; the second is
# the record as it goes on the wire. That is what keeps `~mem.total_kb` beside
# `mem.total_mib` instead of at the end of the file -- the marker is a column,
# and a sort over whole lines makes it part of the key.
say() { printf '%s\tINV|%s %s\n' "$1" "$1" "$2"; }
raw() { printf '%s\tINV|~%s %s\n' "$1" "$1" "$2"; }
hole() { printf '%s\tINV|%s UNAVAILABLE(shell)\n' "$1" "$1"; }

# Sorted, byte order, space separated, in brackets. "[]" for an empty
# directory and ABSENT for one that is not there: two different facts, and the
# distinction is the one EMPIRICAL-FINDINGS turns on.
listing() {
    if [ ! -d "$1" ]; then
        printf 'ABSENT'
        return
    fi
    printf '[%s]' "$(ls "$1" 2>/dev/null | sort | tr '\n' ' ' | sed 's/ $//')"
}

version_id=ABSENT
[ -r /etc/os-release ] && version_id=$(sed -n 's/^VERSION_ID=//p' /etc/os-release | tr -d '"')

printf 'INV|# format=1\n'
printf 'INV|# capture.method=shell-fallback\n'
printf 'INV|# capture.root=/\n'
printf 'INV|# capture.euid=%s\n' "$(id -u)"
printf 'INV|# capture.os_version_id=%s\n' "${version_id:-ABSENT}"
printf 'INV|# capture.sections=all\n'
printf 'INV|BEGIN\n'

# THE BODY IS SORTED, UNDER A FORCED LC_ALL=C, and the export at the top of
# this file is load-bearing rather than tidy: delete it and the ordering
# becomes locale-dependent, which is the exact failure this file's header
# warns about. The comment here used to say the records were "emitted in key
# order by hand, because `sort` over the whole body would be the
# locale-dependent step this file is trying to avoid" -- with the whole block
# piped through `sort` four lines below it.
#
# AND THE SORT KEY IS THE KEY, NOT THE LINE. `~` is 0x7E, so sorting whole
# lines put every informational record after every letter, i.e. at the bottom
# of the file -- which is precisely what nd_inventory.h says the marker is a
# COLUMN to prevent, and it made parse_capture() raise "records are not in
# LC_ALL=C key order" on every capture this script has ever produced.
# Reproduced: `INV|~mem.total_kb` landed after `INV|uname.sysname`, so the
# refusal path this file's header describes -- refused as a WEAKER INSTRUMENT,
# by capture.method -- was unreachable because the file could not be parsed at
# all. The marker is emitted as a separate sort field and cut off afterwards.
{
    for c in backlight block gpio graphics i2c-dev input leds misc mtd net power_supply rtc thermal tty ubi; do
        say "class.$c" "$(listing "/sys/class/$c")"
    done

    if [ -d /sys/devices/system/cpu/cpu0/cpufreq ]; then
        say cpufreq.cpu0 present
    else
        say cpufreq.cpu0 ABSENT
    fi
    say cpufreq.policies "$(listing /sys/devices/system/cpu/cpufreq)"
    # The OPP table, not just the directory. "Both machines have cpufreq" is a
    # much weaker statement than "both offer these five operating points", and
    # after the emulator grew a policy it is the only one left worth making.
    if [ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies ]; then
        # The trailing space is real -- the kernel writes one -- and the C
        # tool trims it, so this has to trim it too or the two instruments
        # would disagree about a machine they both read correctly.
        say cpufreq.available "$(sed 's/[[:space:]]*$//' \
            /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies)"
    else
        say cpufreq.available ABSENT
    fi

    if [ -e /dev/mapper/control ]; then say dm.control present; else say dm.control ABSENT; fi

    # The four holes that decided the tool would be C.
    hole fb0
    hole fb0.var.xres
    hole fb0.var.bits_per_pixel
    hole fb0.fix.line_length

    kb=$(sed -n 's/^MemTotal: *\([0-9]*\) kB/\1/p' /proc/meminfo)
    say mem.total_mib "$(( ( (kb / 1024) + 2 ) / 4 * 4 ))"
    raw mem.total_kb "${kb:-0}"

    # The library's half, refused rather than re-derived. The RECORD is a file
    # and is readable from here; the compiled constant is not, and the
    # disagreement between them is the whole of what D2 added.
    hole modem.board_expects_radio
    hole modem.cold_verdict
    hole platform.board
    hole platform.mismatch
    hole platform.resolved
    for f in board image platform; do
        v=ABSENT
        [ -r /NeoDCT/platform ] && v=$(sed -n "s/^$f=//p" /NeoDCT/platform)
        say "platform.record.$f" "${v:-ABSENT}"
    done

    say uname.machine "$(uname -m)"
    say uname.release "$(uname -r)"
    say uname.sysname "$(uname -s)"
} | sort | cut -f2-

printf 'INV|END\n'
printf 'INV|# sha256.transport=UNAVAILABLE(shell)\n'
printf 'INV|# sha256.compared=UNAVAILABLE(shell)\n'

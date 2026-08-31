# NeoDCT panel helpers for the initramfs -- sourced by init before both
# ndsys-apply.sh and ndsys-recovery.sh.
#
# Two things live here. The first is panel_start/panel_show/panel_stop, which
# used to be at the top of ndsys-recovery.sh; they moved because the update
# applier needs the same panel to draw install progress on, and recovery
# needing a screen is no reason for the applier to depend on the recovery
# menu. init already called panel_start() from panic() behind a `command -v`
# guard for exactly that reason. Nothing about them changed.
#
# The second is the progress helpers, which are new. They drive
# /bin/nd-bootbar (neodct/src/tools/nd_bootbar.c) so that an owner watching an
# update install sees how far along it is, instead of the boot logo -- which
# is pixel-for-pixel what a hung phone looks like.
#
# mkinitramfs.py copies every FILE in neodct/initramfs/ verbatim into the
# cpio, so this needed no build change, and test_initramfs_applets.py scans it
# automatically because script_paths() lists the directory.

# --- panel output ----------------------------------------------------------
# The phone's /dev/fb0 is vfb: the framebuffer console draws the menu below
# into it, but those pixels only reach the ST7789 if something mirrors them
# over SPI. The running system starts neodct_displayd for that; inside the
# initramfs it is ours to start -- and only once something has gone wrong.
# An ordinary boot must not pay for the panel reset, and two daemons must
# never drive the same SPI bus at once.
#
# Absent on QEMU and on any build whose daemon is the wrong architecture
# (mkinitramfs.py ships it only when it matches), so every step is optional:
# recovery still runs, just headless, which is what serial is for.
: "${PANEL_DAEMON:=/bin/neodct_displayd}"
: "${PANEL_BOOTLOGO:=/bootlogo.raw}"
: "${PANEL_SPLASH:=/splash.raw}"
: "${PANEL_FB:=/dev/fb0}"
: "${PANEL_SETTLE:=2}"
: "${PANEL_SPLASH_HOLD:=2}"
: "${PANEL_READY:=/panel.ready}"
PANEL_PID=""
PANEL_UP=""

# Bring the panel up.
#
# This used to start the daemon and then `sleep 2`, because the daemon
# resets the panel and forces fb0 to 32bpp on startup and anything written
# before that lands in whatever format the vfb happened to have. Two seconds
# was a guess, it was the single largest item in the boot, and it was paid
# on every boot including QEMU's, where there is no SPI bus and no daemon to
# wait for.
#
# The daemon now writes NEODCT_DISPLAYD_READY once it has finished exactly
# that setup, so this waits for the fact instead of for the guess -- and
# gives up the moment the daemon exits, which is what happens on QEMU where
# init_spi() finds no /dev/spidev. PANEL_SETTLE is still the ceiling, so the
# worst case is the behaviour this replaced.
#
# Returning 0 does NOT mean the daemon is running. It means /dev/fb0 is
# there and worth drawing on, which on QEMU is the whole story: the virtual
# framebuffer IS the screen, and the boot logo shows on it with no daemon
# involved at all.
panel_start() {
    [ -z "$PANEL_UP" ] || return 0
    [ -c "$PANEL_FB" ] || return 1

    if [ -x "$PANEL_DAEMON" ]; then
        rm -f "$PANEL_READY" 2>/dev/null
        NEODCT_DISPLAYD_READY="$PANEL_READY" "$PANEL_DAEMON" > /dev/null 2>&1 &
        PANEL_PID=$!

        # PANEL_SETTLE seconds, in 20 ms steps. Integer arithmetic: the shell
        # has no floats, and PANEL_SETTLE has always been whole seconds.
        _tries=0
        _max=$(( ${PANEL_SETTLE%%.*} * 50 ))
        while [ "$_tries" -lt "$_max" ]; do
            if [ -e "$PANEL_READY" ]; then
                PANEL_UP=1
                return 0
            fi
            # The daemon exited: no panel here. Do not sit out the timeout.
            if ! kill -0 "$PANEL_PID" 2>/dev/null; then
                PANEL_PID=""
                break
            fi
            sleep 0.02
            _tries=$((_tries + 1))
        done
    fi

    PANEL_UP=1
    return 0
}

# panel_show <raw> [hold] -- blit one pre-converted image and optionally
# sit on it. The blobs are built by mkinitramfs.py from the bitmaps and are
# already in the daemon's byte order, so this is a copy, not a conversion.
panel_show() {
    [ -n "$PANEL_UP" ] || return 1
    [ -r "$1" ] || return 1
    cat "$1" > "$PANEL_FB" 2>/dev/null || return 1
    [ -n "${2:-}" ] && [ "${2:-0}" != "0" ] && sleep "$2"
    return 0
}

# Must run before switch_root. The daemon keeps running across it -- its
# binary is gone but the process is not -- and the real system starts its
# own, so two of them would drive the same SPI bus at once.
panel_stop() {
    PANEL_UP=""
    [ -n "$PANEL_PID" ] || return 0
    kill "$PANEL_PID" 2>/dev/null
    PANEL_PID=""
    return 0
}

# --- install progress ------------------------------------------------------
#
# The bar's numerator is a counting stage in a pipeline that already existed,
# so the count is EXACT: it is the bytes that actually crossed the pipe, at
# the point they crossed it. Nothing is sampled and nothing is estimated. The
# denominator is image_bytes out of pending.prop, which by the time anything
# is drawn has been proved to agree with a manifest signed by the release key.
#
# Three phases -- hash the package, write the flash, hash it back -- each with
# its own bar and its own label, because they move the same 51 MB at three
# very different rates and one bar over 3x the bytes would sprint to 33% and
# then appear frozen for most of a minute. See docs/c-rewrite/
# spec-boot-progress.md section 1.2.
: "${NDSYS_BOOTBAR:=/bin/nd-bootbar}"

# progress_filter STEP PHASE TOTAL -- a pipeline stage.
#
# ONLY EVER CALL THIS INSIDE A PIPELINE. `exec` on both branches, which is
# what makes it free: it REPLACES the subshell the pipeline already forked
# rather than adding a process to it. Called on its own it would replace the
# applier's own shell, and the boot would end there.
#
# `exec cat` is the whole safety story. When nd-bootbar is missing, when the
# panel never came up, and -- this is the important one -- in every host test,
# where there is no /dev/fb0 and no built binary, the pipeline degrades to a
# cat that changes neither its shape nor its exit status. ash reports a
# pipeline's status from its last command and none of these scripts sets
# pipefail or set -e, so every existing test in test_initramfs_apply.py keeps
# passing untouched. That is the bar a cosmetic change to the applier has to
# clear, and it is why the tool is a filter rather than a poller.
progress_filter() {
    if [ -n "$PANEL_UP" ] && [ -x "$NDSYS_BOOTBAR" ]; then
        exec "$NDSYS_BOOTBAR" --step "$1" --phase "$2" --total "$3"
    fi
    exec cat
}

# progress_frame STEP PHASE PERCENT [TOTAL] -- one frame, no copying.
#
# The 0% frame is what makes something appear on the panel within a fraction
# of a second of the update starting, rather than after the first megabyte.
# The 100% frame is drawn before the sync that follows the write: a 51 MB sync
# with the bar sitting at 99% would look like the hang this is meant to
# remove.
progress_frame() {
    [ -n "$PANEL_UP" ] || return 0
    [ -x "$NDSYS_BOOTBAR" ] || return 0
    "$NDSYS_BOOTBAR" --step "$1" --phase "$2" --at "$3" \
        --total "${4:-0}" < /dev/null > /dev/null 2>&1
    return 0
}

# progress_fail HEADLINE REASON [HOLD] -- the refusal screen, held long
# enough to read.
#
# This is the half of the feature that matters most, and it is not the bar.
# Every refusal inside apply_pending() logs to /dev/console -- a serial cable
# the owner does not have -- and then boots the old system, so a package that
# is not signed by the release key is refused silently from the owner's point
# of view: they install an update, the phone restarts, and nothing has
# changed. The long reason still goes to last_result.prop and the Update app
# still shows it on the next launch; seeing it twice is correct.
progress_fail() {
    [ -n "$PANEL_UP" ] || return 0
    [ -x "$NDSYS_BOOTBAR" ] || return 0
    "$NDSYS_BOOTBAR" --fail "$1" --reason "$2" < /dev/null > /dev/null 2>&1
    sleep "${3:-$PANEL_SPLASH_HOLD}"
    return 0
}

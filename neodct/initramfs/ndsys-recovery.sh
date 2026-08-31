# NeoDCT recovery mode -- sourced by the initramfs init.
#
# Reached when the system cannot be booted (no system image found, verity
# refused it, no /sbin/init) or on request with neodct.recovery=1. Lets you
# install an update from an SD card without a working system, which is the
# one thing a rescue shell cannot talk a non-developer through.
#
# The UI is plain text on /dev/tty1 so it appears on the phone's screen (the
# kernel has CONFIG_FRAMEBUFFER_CONSOLE), falling back to /dev/console. The
# panel is 240x175, which is 30 columns by 10 rows in the 8x16 font, so
# every string here is written to fit in 30 columns.
#
# Menu structure follows the prototype in rec.py. Written in shell rather
# than python because there is no python in the initramfs -- adding one would
# cost more than the whole rest of the image.
#
# recovery_install_package() is the part that can destroy a system, so it is
# separate from the UI and unit tested on the host by
# neodct/tests/test_initramfs_recovery.py.

: "${RECOVERY_TITLE:=NeoDCT recovery}"
: "${RECOVERY_BYLINE:=by bubbletoptag}"
: "${MNT_SDCARD:=/mnt/sdcard}"
# One-shot request left by the running system: `touch` it and reboot. This is
# the only practical trigger on real hardware, where there is no way to edit
# the kernel cmdline and no keyboard in the initramfs.
: "${RECOVERY_FLAG:=$STATE_DIR/boot_recovery}"

ESC=$(printf '\033')
CR=$(printf '\r')

# --- the on-screen UI ------------------------------------------------------
#
# Recovery was never serial-only: it has drawn on /dev/tty1 -- the phone's own
# framebuffer console -- for as long as mkinitramfs.py has shipped the panel
# daemon. What it could not do was be DRIVEN. The sixteen keys are on a
# PCF8575 port expander that no kernel driver binds, so no byte ever reaches
# the VT and the read_key() path below is, on a phone, a dead end. nd-recui
# scans that expander itself and draws in the phone's own typeface; that is
# what this section is for, and the i2c half is the load-bearing half.
#
# Called through a variable, exactly as nd-verify is. Two payoffs: the host
# tests can substitute a stand-in, and test_initramfs_applets.py's scanner
# cannot see a variable, so EXTRA_BINARIES needs no edit.
: "${RECUI_BIN:=/bin/nd-recui}"
# The keymap the first-boot wizard wrote. Read once by nd-recui at startup,
# before any menu -- recovery_action_wipe_user deletes everything on this
# partition except .ndsys, INCLUDING keymap.json, so a later read would lose
# the keypad on the screen that says the data is gone.
: "${RECOVERY_KEYMAP:=$MNT_USER/keymap.json}"
# Latched when nd-recui reports it has no usable input device. There is no
# point re-execing it once per screen to be told the same thing, and doing so
# would leave the panel showing a menu while the tty draws another.
RECUI_DEAD=""

# True when the panel UI is available: the binary shipped, fb0 came up, and
# nobody asked for the serial console instead.
#
# neodct.rectty=/dev/console is a deliberate request for a text UI on a cable
# -- honour it. PANEL_UP is the right flag because panel_start()'s own comment
# says returning 0 means "/dev/fb0 is there and worth drawing on", which is
# true on QEMU with no daemon at all.
recovery_panel_ui() {
    [ -z "${RECOVERY_TTY_OVERRIDE:-}" ] || return 1
    [ -z "$RECUI_DEAD" ] || return 1
    [ -x "$RECUI_BIN" ] || return 1
    [ -n "$PANEL_UP" ] || return 1
    return 0
}

# Exit status 2 from any nd-recui verb means "I have no usable input device".
# Everything falls back to the tty menu, which is what recovery has always
# had; nothing here tries to soldier on with half a UI.
recovery_recui_gone() {
    RECUI_DEAD=1
    log "recovery: nd-recui has no usable input; falling back to the text menu"
}

# --- panel output ----------------------------------------------------------
#
# panel_start, panel_show, panel_stop and the PANEL_* defaults moved to
# ndsys-panel.sh, unchanged. The update applier needs the same panel to draw
# install progress on, and recovery needing a screen is no reason for the
# applier to depend on the recovery menu; init sources ndsys-panel.sh before
# this file, so on a real boot they are already defined by the time anything
# here runs.
#
# This file is also sourced ON ITS OWN, by neodct/tests/
# test_initramfs_recovery.py, which has no initramfs for /ndsys-panel.sh to be
# found in. Stub the three calls rather than leave the menu invoking functions
# that do not exist -- headless is what the real helpers report on a host
# anyway, since panel_start() begins with [ -c /dev/fb0 ].
: "${NDSYS_PANEL_SH:=/ndsys-panel.sh}"
if ! command -v panel_start > /dev/null 2>&1; then
    if [ -r "$NDSYS_PANEL_SH" ]; then
        . "$NDSYS_PANEL_SH"
    else
        : "${PANEL_SPLASH:=/splash.raw}"
        : "${PANEL_SPLASH_HOLD:=2}"
        panel_start() { return 1; }
        panel_show() { return 1; }
        panel_stop() { return 0; }
    fi
fi

# A literal newline: LF=$(printf '\n') would be the empty string, because
# command substitution strips trailing newlines -- so the Enter-as-LF case
# could never match and half the Enter presses were swallowed.
LF='
'

# --- terminal ------------------------------------------------------------

recovery_tty() {
    # Escape hatch: neodct.rectty=/dev/console drives recovery over the
    # serial port instead, for when keys are not reaching the VT.
    if [ -n "${RECOVERY_TTY_OVERRIDE:-}" ]; then
        echo "$RECOVERY_TTY_OVERRIDE"
        return 0
    fi
    # tty1 is the framebuffer console: the phone's own screen. Without it
    # (headless, or no VT) fall back to whatever /dev/console is.
    if [ -c /dev/tty1 ] && [ -w /dev/tty1 ]; then
        echo /dev/tty1
    else
        echo /dev/console
    fi
}

# Recovery drives one terminal, drawn on and read from: the phone's screen.
# Reading the serial console as well needs a second reader merged into one
# stream, and the pump-and-FIFO that took meant every frame was also painted
# into the terminal QEMU was started from. neodct.rectty=/dev/console moves
# the whole UI to the serial port instead, which is what a headless run wants
# anyway.

screen_clear() {
    printf '\033[2J\033[H' > "$TTY" 2>/dev/null
}

# \r\n rather than \n: the screen is a VT that may or may not be translating
# newlines, and a bare \n on one that is not leaves the next line indented by
# however long the last one was.
say() {
    printf '%s\r\n' "$*" > "$TTY" 2>/dev/null
}

# --- input ----------------------------------------------------------------
#
# The menu reads single bytes from one descriptor held open on the terminal
# it draws on. Two things had to be true before an arrow key could move
# anything, and neither was: the terminal has to be in character-at-a-time
# mode *and stay there*, and the keys have to be read from the screen's VT
# rather than from the shell's stdin, which is the serial port.

# Put the terminal *already open on stdin* into character-at-a-time mode.
#
# It has to be stdin rather than `stty -F /dev/tty1`, and that is the whole
# bug this replaced. The Linux VT console driver sets TTY_DRIVER_RESET_TERMIOS
# (drivers/tty/vt/vt.c), so when the last descriptor on /dev/ttyN closes the
# terminal reverts to its canonical, echoing default -- and for `stty -F` the
# last descriptor is stty's own, closed as it exits. The mode was gone before
# anything read a byte: every arrow echoed as ^[[A onto the menu and the read
# blocked for a whole line, so the screen looked frozen. Setting it through a
# descriptor whose owner keeps it open is what makes it stick.
#
# -icanon -echo in preference to `raw` because `raw` also turns off output
# post-processing, and log() writes progress to the serial console with plain
# newlines.
recovery_raw_tty() {
    stty -icanon -echo -icrnl min 1 time 0 2>/dev/null && return 0
    stty raw -echo 2>/dev/null
}

# Open the screen's terminal once, on fd 8, and keep it open.
#
# Holding it open is the whole trick, and `stty ... < /dev/tty1` is not the
# same thing: that opens the terminal, sets the mode and closes it again, and
# by the time the next command runs the mode is already gone (see
# recovery_raw_tty). fd 8 stays open for as long as recovery does, so the
# mode set through it stays put.
#
# Read-write rather than read-only because a VT opened read-only still works
# but leaves nothing to fall back on if the screen ever needs writing to
# through the same descriptor.
RECOVERY_INPUT_UP=""

recovery_input_start() {
    # Probe in a subshell BEFORE taking the descriptor.
    #
    # `exec 8<> "$TTY" || return 1` looks like it handles a terminal that
    # cannot be opened. It does not: POSIX makes a redirection error on a
    # special builtin fatal to a non-interactive shell, and both dash and
    # busybox ash oblige -- so the shell running the initramfs simply DIES,
    # and the `exec /bin/sh` rescue path this was written to reach was never
    # reachable. Inside a subshell the death is contained and the exit status
    # comes back. Found by the host test below, not on hardware, which is the
    # only reason it was ever going to be found.
    ( : <> "$TTY" ) 2>/dev/null || return 1
    exec 8<> "$TTY" || return 1
    recovery_raw_tty <&8
    RECOVERY_INPUT_UP=1
    return 0
}

recovery_input_stop() {
    [ -n "$RECOVERY_INPUT_UP" ] || return 0
    stty sane <&8 2>/dev/null
    exec 8<&-
    RECOVERY_INPUT_UP=""
    return 0
}

# Open fd 8 if it is not already open.
#
# recovery_main deliberately does NOT open it when the panel UI is in use: two
# readers of one VT leave stale bytes queued for whichever gets there second,
# and the VT's own echo would paint console text over the framebuffer
# nd-recui is drawing on. So the descriptor has to be opened by whoever first
# falls back to the text menu, which is here.
recovery_input_ensure() {
    [ -z "$RECOVERY_INPUT_UP" ] || return 0
    recovery_input_start
}

# One keypress from fd 8. Returns UP, DOWN, ENTER, a digit, or nothing.
#
# dd rather than `read -n 1`: busybox is built here without
# CONFIG_ASH_READ_NCHARS, so ash's read has no -n and swallows a whole line
# instead of a character -- which is why the menu only ever moved after
# Enter, and only on the serial console.
#
# The trailing X is not decoration: command substitution strips trailing
# newlines, so Enter would be indistinguishable from an empty read without a
# sentinel to protect it.
read_key() {
    raw="$(dd bs=1 count=1 <&8 2>/dev/null; echo X)"
    key="${raw%X}"
    if [ -z "$key" ]; then
        # EOF: do not busy-spin.
        sleep 1
        return 0
    fi
    case "$key" in
        "$ESC")
            raw="$(dd bs=1 count=1 <&8 2>/dev/null; echo X)"
            case "${raw%X}" in
                "["|O)   # ESC [ A is the console; ESC O A is app-cursor mode
                    raw="$(dd bs=1 count=1 <&8 2>/dev/null; echo X)"
                    case "${raw%X}" in
                        A) echo UP ;;
                        B) echo DOWN ;;
                    esac
                    ;;
            esac
            ;;
        "$CR"|"$LF") echo ENTER ;;
        [0-9]) echo "$key" ;;
        k) echo UP ;;      # for a serial console with no arrow keys
        j) echo DOWN ;;
    esac
}

# --- menus ---------------------------------------------------------------

# recovery_menu TITLE ITEM... -- echoes the chosen 1-based index, or 0.
#
# The panel version prints the same thing on the same stdout and exits, which
# is what makes it a drop-in. Only status 0 is an answer: 2 is "no input
# device", and anything else is a usage error on our part -- both fall through
# to the text menu below rather than returning an empty choice to the caller.
recovery_menu() {
    if recovery_panel_ui; then
        "$RECUI_BIN" menu --keymap "$RECOVERY_KEYMAP" --title "$RECOVERY_TITLE" -- "$@"
        _rc=$?
        [ "$_rc" = 0 ] && return 0
        [ "$_rc" = 2 ] && recovery_recui_gone
    fi
    # No panel and no terminal is the end of the road: returning an empty
    # choice would spin recovery_main's loop forever on a phone nobody can
    # talk to. This is where recovery_main used to give up, moved here
    # because that is now where the descriptor is first needed.
    if ! recovery_input_ensure; then
        log "recovery: cannot open $TTY for input"
        exec /bin/sh
    fi

    heading="$1"
    shift
    count=$#
    selected=1
    while :; do
        screen_clear
        say "$RECOVERY_TITLE"
        [ -n "$heading" ] && say "$heading"
        index=1
        for item in "$@"; do
            if [ "$index" = "$selected" ]; then
                say "> $item"
            else
                say "  $item"
            fi
            index=$((index + 1))
        done
        key="$(read_key)"
        case "$key" in
            UP)    selected=$((selected > 1 ? selected - 1 : count)) ;;
            DOWN)  selected=$((selected < count ? selected + 1 : 1)) ;;
            ENTER) echo "$selected"; return 0 ;;
            [1-9])
                # Digits move rather than select outright, so "3<Enter>"
                # reads the same whether the tty is raw or line-buffered --
                # and a stray Enter cannot pick something by accident.
                [ "$key" -le "$count" ] && selected="$key"
                ;;
        esac
    done
}

# A yes/no prompt that defaults to no, for the destructive choices.
#
# The panel version goes one further and opens with NEITHER answer lit, so a
# stray Enter on "WIPE SYSTEM?" cannot answer it at all. Status 0 is yes and 1
# is no, exactly as this function returns them; 2 falls through.
recovery_confirm() {
    if recovery_panel_ui; then
        "$RECUI_BIN" confirm --keymap "$RECOVERY_KEYMAP" -- "$1"
        _rc=$?
        [ "$_rc" = 0 ] && return 0
        [ "$_rc" = 1 ] && return 1
        recovery_recui_gone
    fi
    recovery_input_ensure || return 1

    selected=2
    while :; do
        screen_clear
        say "$RECOVERY_TITLE"
        say "$1"
        say ""
        if [ "$selected" = 1 ]; then
            say "> yes"
            say "  no"
        else
            say "  yes"
            say "> no"
        fi
        key="$(read_key)"
        case "$key" in
            UP|DOWN) selected=$((selected == 1 ? 2 : 1)) ;;
            1)       selected=1 ;;
            2)       selected=2 ;;
            ENTER)   [ "$selected" = 1 ] && return 0 || return 1 ;;
        esac
    done
}

# recovery_say LINE... -- one "here is what happened, press a key" screen.
#
# Seven of these were written out inline, each as the same five-line sequence.
# They are one function now because the panel version is a single process and
# repeating the delegation seven times would be seven places to get it wrong.
# The fallback body is the sequence it replaces, unchanged.
recovery_say() {
    if recovery_panel_ui; then
        "$RECUI_BIN" message --keymap "$RECOVERY_KEYMAP" -- "$@"
        _rc=$?
        [ "$_rc" = 0 ] && return 0
        [ "$_rc" = 2 ] && recovery_recui_gone
    fi
    recovery_input_ensure
    screen_clear
    say "$RECOVERY_TITLE"
    say ""
    for _line in "$@"; do
        say "$_line"
    done
    say ""
    say "Press a key"
    read_key > /dev/null
    return 0
}

# The bar for a long operation, as a pipeline stage -- `cat` when there is no
# panel UI, so every pipeline below is byte-identical to what it has always
# been on a headless run and under the host tests.
#
# A pv-style filter rather than polling dd: no signals, no busybox
# status=progress dependency, and one added stage per pass.
recovery_meter() {   # recovery_meter STEP TOTAL
    if recovery_panel_ui; then
        "$RECUI_BIN" progress --step "$1" --total "$2" \
            --header "${RECOVERY_METER_HEADER:-}"
    else
        cat
    fi
}

# The read-back pass runs inside hash_prefix, which takes the name of a filter
# and can therefore only pass a single word. Hence a zero-argument wrapper
# reading the two values out of the environment rather than a longer
# hash_prefix signature that every other caller would have to know about.
recovery_verify_meter() {
    recovery_meter "Verifying image" "${RECOVERY_METER_TOTAL:-0}"
}

# --- the card ------------------------------------------------------------

# Mount the first FAT filesystem that is not the system or user partition.
recovery_mount_card() {
    # One mounter for recovery and for the boot-time applier, in
    # ndsys-apply.sh. They used to be separate and disagreed: this one
    # tried vfat alone, so every exFAT card -- which is what a large card
    # is formatted as out of the box -- came up as "No SD card found" on a
    # phone whose running system mounted the same card without trouble.
    mount_card
}

recovery_find_packages() {
    for directory in "$MNT_SDCARD/update" "$MNT_SDCARD"; do
        [ -d "$directory" ] || continue
        for path in "$directory"/*.ndsw "$directory"/*.NDSW; do
            [ -f "$path" ] && echo "$path"
        done
    done
}

# --- installing ----------------------------------------------------------

# Pull one field out of the manifest. mkupdate writes it with
# json.dumps(indent=2), one field per line, so a line-oriented match is
# reliable -- and there is no JSON parser in an initramfs.
recovery_manifest_field() {
    sed -n 's/.*"'"$1"'"[[:space:]]*:[[:space:]]*"\{0,1\}\([^",]*\)"\{0,1\}.*/\1/p' \
        | head -n1
}

# True when the package's manifest carries a valid release signature.
#
# Same verifier, same key and the same detached signature the automatic
# applier checks, so "signed" means one thing on this phone. What differs is
# what happens next: the applier refuses, recovery only changes the question
# it puts on the panel. False also covers "there is no verifier", which is
# the honest answer to "is this signed" when nothing can tell.
recovery_package_is_signed() {   # recovery_package_is_signed NDSW
    _dir="${NDSYS_TMPDIR:-/run/ndsys}/recovery"

    [ -x "${NDSYS_VERIFY_BIN:-/bin/nd-verify}" ] || return 1
    [ -r "${NDSYS_RELEASE_KEY:-/neodct-release.pub}" ] || return 1
    rm -rf "$_dir" 2>/dev/null
    mkdir -p "$_dir" 2>/dev/null || return 1
    unzip -p "$1" manifest.json > "$_dir/manifest.json" 2>/dev/null
    unzip -p "$1" manifest.sig  > "$_dir/manifest.sig"  2>/dev/null
    [ -s "$_dir/manifest.json" ] && [ -s "$_dir/manifest.sig" ] || return 1
    "${NDSYS_VERIFY_BIN:-/bin/nd-verify}" "$_dir/manifest.json" \
        "$_dir/manifest.sig" "${NDSYS_RELEASE_KEY:-/neodct-release.pub}" \
        > /dev/null 2>&1
}

# The size of a member, from the zip's own listing.
recovery_member_size() {
    unzip -l "$1" "$2" 2>/dev/null | awk -v want="$2" '$NF == want {print $1; exit}'
}

# recovery_install_package NDSW SYSDEV [STATEDIR]
#
# Verifies the image against the manifest, writes it, reads it back, and
# records what is now installed. Never writes anything it has not hashed
# first. Returns 0 on success.
#
# The signature IS checked now (nd-verify and the release key are in the
# initramfs -- see the gate in ndsys-apply.sh), but unlike the automatic
# applier, recovery does not refuse over it. Recovery exists to rescue a
# phone that will not boot, its whole premise is a person standing in front
# of it pressing keys, and an owner whose only image is an unsigned
# development build must still be able to get the phone running. So the
# result is reported to the caller and the caller asks a different question.
# recovery_package_is_signed() below is what it asks.
recovery_install_package() {
    package="$1"
    device="$2"
    state="${3:-$STATE_DIR}"

    manifest="$(unzip -p "$package" manifest.json 2>/dev/null)"
    if [ -z "$manifest" ]; then
        log "recovery: no manifest.json in $(basename "$package")"
        return 1
    fi

    want_sha="$(echo "$manifest" | recovery_manifest_field sha256)"
    version="$(echo "$manifest" | recovery_manifest_field version)"
    platform="$(echo "$manifest" | recovery_manifest_field platform)"
    buildtime="$(echo "$manifest" | recovery_manifest_field buildtime)"
    root_hash="$(echo "$manifest" | recovery_manifest_field root_hash)"
    block_size="$(echo "$manifest" | recovery_manifest_field block_size)"
    image_blocks="$(echo "$manifest" | recovery_manifest_field image_blocks)"
    salt="$(echo "$manifest" | recovery_manifest_field salt)"
    image_bytes="$(recovery_member_size "$package" rootfs.squashfs)"

    if [ -z "$want_sha" ] || [ -z "$root_hash" ] || [ -z "$image_bytes" ]; then
        log "recovery: manifest is incomplete"
        return 1
    fi

    # This is where the time actually goes: three passes over ~48 MB. Wiping
    # the system, by contrast, is dd bs=1M count=1 and is instant -- a bar
    # that fills in 80 ms is a lie, so wipe gets a message and this gets the
    # bar. Each pass gains exactly one pipeline stage.
    #
    # A meter that failed mid-stream would truncate the write, and the
    # pipeline's status is dd's rather than the meter's -- but pass 3 hashes
    # image_bytes back off the device, so a short write is caught there and
    # nothing is recorded. That read-back is why instrumenting these
    # pipelines is safe at all.
    RECOVERY_METER_HEADER="$(basename "$package")"

    # Pass 1: hash the image straight out of the zip, before anything is
    # written. A corrupt package must never reach the flash.
    log "recovery: checking $version ($image_bytes bytes)"
    got_sha="$(unzip -p "$package" rootfs.squashfs 2>/dev/null \
        | recovery_meter "Checking image" "$image_bytes" \
        | sha256sum | cut -d' ' -f1)"
    if [ "$got_sha" != "$want_sha" ]; then
        log "recovery: image sha256 mismatch; refusing $version"
        return 1
    fi

    # Pass 2: write it.
    log "recovery: writing to $device"
    if ! unzip -p "$package" rootfs.squashfs 2>/dev/null \
            | recovery_meter "Writing image" "$image_bytes" \
            | dd of="$device" bs=1M conv=fsync 2>/dev/null; then
        log "recovery: write to $device failed"
        return 1
    fi
    sync

    # Pass 3: read back what landed. The meter goes INSIDE hash_prefix's own
    # pipeline -- the function returns a 64-character hash, so metering its
    # output would report 64 bytes against 48 MB.
    RECOVERY_METER_TOTAL="$image_bytes"
    if [ "$(hash_prefix "$device" "$image_bytes" recovery_verify_meter)" != "$want_sha" ]; then
        log "recovery: read-back mismatch on $device"
        return 1
    fi

    # Record it so the next boot can build the verity table.
    if [ -n "$state" ] && mkdir -p "$state" 2>/dev/null; then
        putprop_file "$state/installed.prop" <<EOF
sha256=$want_sha
image_bytes=$image_bytes
version=$version
buildtime=$buildtime
platform=$platform
verity_root_hash=$root_hash
verity_block_size=${block_size:-4096}
verity_image_blocks=$image_blocks
verity_salt=$salt
EOF
        rm -f "$state/pending.prop" "$state/pending.img" 2>/dev/null
    else
        log "recovery: cannot record installed.prop; verity will need"
        log "recovery: neodct.verity=permissive on the next boot"
    fi
    log "recovery: installed $version"
    return 0
}

# --- actions -------------------------------------------------------------

recovery_action_update() {
    if ! recovery_mount_card; then
        recovery_say "No SD card found." "Put a FAT32 card with" \
                     "update/UPDATE.ndsw in it."
        return
    fi

    packages="$(recovery_find_packages)"
    if [ -z "$packages" ]; then
        recovery_say "No .ndsw on the card." "Copy one into update/"
        return
    fi

    # One package is the normal case; with several, let them choose.
    count=0
    for path in $packages; do count=$((count + 1)); done
    if [ "$count" = 1 ]; then
        chosen="$packages"
    else
        # shellcheck disable=SC2086  # deliberate word splitting into args
        names=""
        for path in $packages; do names="$names $(basename "$path")"; done
        # shellcheck disable=SC2086
        index="$(recovery_menu "pick a file:" $names)"
        [ "$index" = 0 ] && return
        current=0
        for path in $packages; do
            current=$((current + 1))
            [ "$current" = "$index" ] && chosen="$path"
        done
    fi

    # Two different questions, because they carry two different risks and a
    # person is about to answer one of them.
    if recovery_package_is_signed "$chosen"; then
        _question="Install $(basename "$chosen")? Signed by the release key."
    else
        _question="$(basename "$chosen") is NOT SIGNED. Install it anyway?"
    fi
    if ! recovery_confirm "$_question"; then
        return
    fi

    # No recovery_say here, on purpose. This screen has no "press a key" and
    # is replaced within a second by the first progress bar; on the tty path
    # it is the last thing drawn before the install takes over. Between the
    # three passes the VT flips back to text for a moment as each nd-recui
    # exits and restores KD_TEXT -- what shows through is this screen, which
    # is why it is still worth drawing on the panel path too.
    screen_clear
    say "$RECOVERY_TITLE"
    say ""
    say "Installing. Do not"
    say "power off."
    say ""
    if recovery_install_package "$chosen" "$SYS_DEV"; then
        # Not recovery_say: a successful install reboots on its own after two
        # seconds and has always done so. Making somebody acknowledge it would
        # leave a phone sitting on a screen nobody is watching.
        say "Done. Rebooting."
        sleep 2
        recovery_reboot
    else
        recovery_say "FAILED. See the serial" "console for why."
    fi
}

recovery_action_wipe_user() {
    recovery_confirm "WIPE USER DATA? Contacts, messages and settings will be erased." \
        || return
    if [ -z "$USER_MOUNTED" ]; then
        recovery_say "No user partition."
        return
    fi
    # Keep .ndsys: it holds installed.prop, without which the next boot
    # cannot build the verity table and would land straight back here.
    for entry in "$MNT_USER"/* "$MNT_USER"/.[!.]*; do
        [ -e "$entry" ] || continue
        case "$(basename "$entry")" in
            .ndsys) continue ;;
        esac
        rm -rf "$entry" 2>/dev/null
    done
    mkdir -p "$MNT_USER/db" "$MNT_USER/logs" "$MNT_USER/.pycache" \
             "$MNT_USER/sdcard" "$MNT_USER/tones" "$MNT_USER/wallpapers" \
             2>/dev/null
    sync
    log "recovery: user data wiped"
    # The keymap this session is still using has just been deleted along with
    # everything else. nd-recui read it once at startup and holds it in
    # memory, so the keypad survives to the end of this session; the
    # first-boot wizard writes a new one on the next boot.
    recovery_say "User data wiped."
}

recovery_action_wipe_system() {
    recovery_confirm "WIPE SYSTEM? The phone will not boot until you install an update." \
        || return
    if [ -z "$SYS_DEV" ]; then
        recovery_say "No system device."
        return
    fi
    # Zeroing the first megabyte is enough to make it unmountable, and is
    # instant compared with erasing 134MB. That is also why this gets a
    # message screen and not a progress bar: a bar that fills in 80 ms is a
    # lie. The bar belongs on the install, which is three passes over ~48 MB.
    dd if=/dev/zero of="$SYS_DEV" bs=1M count=1 conv=fsync 2>/dev/null
    sync
    log "recovery: system image wiped"
    recovery_say "System wiped." "Install an update" "from an SD card."
}

recovery_reboot() {
    sync
    umount "$MNT_SDCARD" 2>/dev/null
    umount "$MNT_USER" 2>/dev/null
    reboot -f 2>/dev/null || reboot 2>/dev/null
    # If reboot is not available, at least stop cleanly.
    while :; do sleep 10; done
}

# --- entry point ---------------------------------------------------------

recovery_main() {
    TTY="$(recovery_tty)"
    printf '\033[?25l' > "$TTY" 2>/dev/null         # hide the cursor
    # Do NOT open fd 8 when the panel UI is in use. Two readers of the same VT
    # leave stale bytes queued for whichever gets there second, and -- worse --
    # the VT's own echo would paint console text over the framebuffer
    # nd-recui is drawing on. nd-recui's KDSETMODE handles the echo; not
    # opening the descriptor handles the stale bytes. recovery_input_ensure()
    # opens it on the first fall-through to the text menu.
    if ! recovery_panel_ui && ! recovery_input_start; then
        log "recovery: cannot open $TTY for input"
        exec /bin/sh
    fi

    reason="${1:-}"
    while :; do
        # 30x10 leaves no room for a legend, so the heading carries it on the
        # first screen only.
        choice="$(recovery_menu "${reason:-1-5 or arrows, Enter}" \
            "update system" "wipe user data" "wipe system" "reboot" "shell")"
        case "$choice" in
            1) recovery_action_update ;;
            2) recovery_action_wipe_user ;;
            3) recovery_action_wipe_system ;;
            4) recovery_reboot ;;
            5)
                # Hand the terminal back before the shell gets it: recovery's
                # descriptor has to go, or the two of them race for every
                # keystroke, and the shell needs the mode put back to
                # something it can be typed into. Both calls are no-ops when
                # the panel UI never opened one, which is the case this
                # option most needs to survive -- a developer dropping to a
                # shell to run `ls /dev/i2c-*`.
                _had_input="$RECOVERY_INPUT_UP"
                recovery_input_stop
                screen_clear
                say "exit to return to recovery"
                /bin/sh < "$TTY" > "$TTY" 2>&1
                [ -n "$_had_input" ] && recovery_input_start
                ;;
        esac
        # Only the first screen explains why recovery started.
        reason=""
    done
}

# True if the running system asked for recovery. The flag is consumed as it
# is read, so rebooting out of recovery returns to the normal system rather
# than looping straight back in.
recovery_requested() {
    [ -f "$RECOVERY_FLAG" ] || return 1
    rm -f "$RECOVERY_FLAG" 2>/dev/null
    sync
    return 0
}


# Used by init in place of a bare rescue shell.
recovery_or_panic() {
    log "$*"
    # Bring the screen up first: on hardware this is the only thing that
    # makes any of what follows visible. The face replaces whatever was on
    # the panel (usually the boot logo) so the failure is unmistakable.
    panel_start && panel_show "$PANEL_SPLASH" "$PANEL_SPLASH_HOLD"
    if [ -r /ndsys-recovery.sh ] || command -v recovery_main > /dev/null 2>&1; then
        recovery_main "$*"
    fi
    log "FATAL: $*"
    log "dropping to a rescue shell; the system may need reflashing"
    exec /bin/sh
}

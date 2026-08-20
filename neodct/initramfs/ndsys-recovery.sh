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
recovery_input_start() {
    exec 8<> "$TTY" || return 1
    recovery_raw_tty <&8
    return 0
}

recovery_input_stop() {
    stty sane <&8 2>/dev/null
    exec 8<&-
    return 0
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
recovery_menu() {
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
recovery_confirm() {
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

# --- the card ------------------------------------------------------------

# Mount the first FAT filesystem that is not the system or user partition.
recovery_mount_card() {
    mkdir -p "$MNT_SDCARD" 2>/dev/null
    mountpoint -q "$MNT_SDCARD" 2>/dev/null && return 0
    for device in $(candidate_devices); do
        [ "$device" = "$SYS_DEV" ] && continue
        [ "$device" = "$USER_DEV" ] && continue
        is_squashfs "$device" && continue
        if mount -t vfat -o ro "$device" "$MNT_SDCARD" 2>/dev/null; then
            return 0
        fi
    done
    return 1
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
# Note: recovery cannot check the *signature* -- there is no crypto in the
# initramfs. It is an integrity check only, which is why this path requires
# physical possession of the phone and says so on screen.
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

    # Pass 1: hash the image straight out of the zip, before anything is
    # written. A corrupt package must never reach the flash.
    log "recovery: checking $version ($image_bytes bytes)"
    got_sha="$(unzip -p "$package" rootfs.squashfs 2>/dev/null | sha256sum \
        | cut -d' ' -f1)"
    if [ "$got_sha" != "$want_sha" ]; then
        log "recovery: image sha256 mismatch; refusing $version"
        return 1
    fi

    # Pass 2: write it.
    log "recovery: writing to $device"
    if ! unzip -p "$package" rootfs.squashfs 2>/dev/null \
            | dd of="$device" bs=1M conv=fsync 2>/dev/null; then
        log "recovery: write to $device failed"
        return 1
    fi
    sync

    # Pass 3: read back what landed.
    if [ "$(hash_prefix "$device" "$image_bytes")" != "$want_sha" ]; then
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
        screen_clear
        say "$RECOVERY_TITLE"
        say ""
        say "No SD card found."
        say "Put a FAT32 card with"
        say "update/UPDATE.ndsw in it."
        say ""
        say "Press a key"
        read_key > /dev/null
        return
    fi

    packages="$(recovery_find_packages)"
    if [ -z "$packages" ]; then
        screen_clear
        say "$RECOVERY_TITLE"
        say ""
        say "No .ndsw on the card."
        say "Copy one into update/"
        say ""
        say "Press a key"
        read_key > /dev/null
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

    if ! recovery_confirm "Install $(basename "$chosen")? Signature is NOT checked here."; then
        return
    fi

    screen_clear
    say "$RECOVERY_TITLE"
    say ""
    say "Installing. Do not"
    say "power off."
    say ""
    if recovery_install_package "$chosen" "$SYS_DEV"; then
        say "Done. Rebooting."
        sleep 2
        recovery_reboot
    else
        say "FAILED. See the serial"
        say "console for why."
        say ""
        say "Press a key"
        read_key > /dev/null
    fi
}

recovery_action_wipe_user() {
    recovery_confirm "WIPE USER DATA? Contacts, messages and settings will be erased." \
        || return
    screen_clear
    say "$RECOVERY_TITLE"
    say ""
    if [ -z "$USER_MOUNTED" ]; then
        say "No user partition."
        say ""
        say "Press a key"
        read_key > /dev/null
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
    say "User data wiped."
    say ""
    say "Press a key"
    read_key > /dev/null
}

recovery_action_wipe_system() {
    recovery_confirm "WIPE SYSTEM? The phone will not boot until you install an update." \
        || return
    screen_clear
    say "$RECOVERY_TITLE"
    say ""
    if [ -z "$SYS_DEV" ]; then
        say "No system device."
    else
        # Zeroing the first megabyte is enough to make it unmountable, and
        # is instant compared with erasing 134MB.
        dd if=/dev/zero of="$SYS_DEV" bs=1M count=1 conv=fsync 2>/dev/null
        sync
        log "recovery: system image wiped"
        say "System wiped."
    fi
    say ""
    say "Press a key"
    read_key > /dev/null
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
    if ! recovery_input_start; then
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
                # something it can be typed into.
                recovery_input_stop
                screen_clear
                say "exit to return to recovery"
                /bin/sh < "$TTY" > "$TTY" 2>&1
                recovery_input_start
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
    if [ -r /ndsys-recovery.sh ] || command -v recovery_main > /dev/null 2>&1; then
        recovery_main "$*"
    fi
    log "FATAL: $*"
    log "dropping to a rescue shell; the system may need reflashing"
    exec /bin/sh
}

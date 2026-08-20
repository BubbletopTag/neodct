# NeoDCT update applier -- sourced by the initramfs init, and by the host
# test suite (neodct/tests/test_initramfs_apply.py) with regular files
# standing in for the block devices.
#
# Nothing here runs at source time: the caller sets SYS_DEV / STATE_DIR /
# MNT_USER and then calls apply_pending. That split is what makes the code
# that overwrites the system partition testable without rebooting anything.
#
# Records (pending.prop, installed.prop) are written by
# System/core/UpdateService/staging.py. They are read with sed and never
# sourced, so a changelog full of backticks cannot execute.

: "${MNT_USER:=/mnt/user}"
: "${STATE_DIR:=$MNT_USER/.ndsys}"
: "${LOG_FILE:=$MNT_USER/logs/update.log}"
: "${DM_NAME:=ndsys}"
: "${MAX_ATTEMPTS:=3}"

# The init script defines its own log(); provide one for standalone use.
if ! command -v log > /dev/null 2>&1; then
    log() {
        echo "[ndsys] $*" >&2
        [ -d "$(dirname "$LOG_FILE")" ] && \
            echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG_FILE" 2>/dev/null
        return 0
    }
fi

# --- finding the right block device --------------------------------------
#
# Device names from the kernel cmdline are a hint, nothing more. virtio-mmio
# devices are not enumerated in the order QEMU is given them, so the first
# real boot mounted the SD card as the root filesystem and died with "no
# /sbin/init in the system image". On real hardware the names differ again
# (mmcblk, ubiblock). What actually identifies the system image is that it
# starts with the squashfs magic, and the user partition is the one labelled
# NDUSER.

# Where to look for candidates. Overridable for the host tests.
: "${NDSYS_SCAN_GLOB:=/dev/vd[a-z] /dev/vd[a-z][0-9] /dev/mmcblk[0-9] \
/dev/mmcblk[0-9]p[0-9] /dev/sd[a-z] /dev/sd[a-z][0-9] /dev/ubiblock*}"

# Where the kernel publishes block-device identity, and where the nodes are.
# Both overridable for the host tests.
: "${NDSYS_SYSFS_BLOCK:=/sys/block}"
: "${NDSYS_DEV_DIR:=/dev}"
# The disk serials QEMU is told to hand out; see neodct/tools/run_qemu.sh.
: "${NDSYS_SERIAL:=NDSYS}"
: "${NDUSER_SERIAL:=NDUSER}"

# device_by_serial SERIAL -- echo the block device carrying that disk serial.
#
# This is the only identity that survives recovery's "wipe system": the
# serial belongs to the device, not to anything written on it, so zeroing the
# image cannot take it away -- whereas the squashfs magic the scan below
# looks for is precisely what a wipe destroys, leaving nothing to say where
# to install the replacement.
#
# It also settles QEMU's enumeration, which is not the order the drives are
# given on the command line: system, user, card comes up as vda=card,
# vdb=user, vdc=system, so neodct.sys=/dev/vda has always been wrong there.
#
# An eMMC partition has no disk serial, so on real hardware this finds
# nothing and the callers fall through to the scan and the cmdline hint --
# where the names are stable anyway, which is why this was only ever a QEMU
# problem.
device_by_serial() {
    [ -n "${1:-}" ] || return 1
    for entry in "$NDSYS_SYSFS_BLOCK"/*/serial; do
        [ -r "$entry" ] || continue
        # Command substitution strips the trailing newline, so this matches
        # whether or not the kernel wrote one.
        [ "$(cat "$entry" 2>/dev/null)" = "$1" ] || continue
        node="$NDSYS_DEV_DIR/$(basename "$(dirname "$entry")")"
        if [ -b "$node" ] || [ -f "$node" ]; then
            echo "$node"
            return 0
        fi
    done
    return 1
}

# "hsqs" is squashfs 4.0 little-endian, and conveniently printable, so a
# plain string comparison does the job with no od/hexdump.
is_squashfs() {
    [ -b "$1" ] || [ -f "$1" ] || return 1
    [ "$(dd if="$1" bs=4 count=1 2>/dev/null)" = "hsqs" ]
}

candidate_devices() {
    # shellcheck disable=SC2086  # the glob list is meant to be expanded
    for path in $NDSYS_SCAN_GLOB; do
        [ -b "$path" ] || [ -f "$path" ] || continue
        echo "$path"
    done
}

# find_system_device [hint] -- echo the device holding the squashfs or target system slot.
find_system_device() {
    # 0. A disk serial is an explicit statement of which device is the system
    #    slot, so it outranks everything else -- including a squashfs found
    #    somewhere it does not belong, such as an SD card someone wrote an
    #    image onto. It is also all there is to go on once the image has been
    #    wiped.
    device="$(device_by_serial "$NDSYS_SERIAL")" && [ -n "$device" ] && {
        echo "$device"
        return 0
    }

    # 1. If a valid squashfs is found via hint or scan, use it
    if [ -n "${1:-}" ] && is_squashfs "$1"; then
        echo "$1"
        return 0
    fi
    for device in $(candidate_devices); do
        if is_squashfs "$device"; then
            echo "$device"
            return 0
        fi
    done

    # 2. FALLBACK FOR WIPED SYSTEMS: Look for a partition labeled 'NDSYS' via blkid
    labelled="$(blkid 2>/dev/null | sed -n 's/^\([^:]*\):.*LABEL="NDSYS".*/\1/p' | head -n1)"
    if [ -n "$labelled" ]; then
        echo "$labelled"
        return 0
    fi

    # 3. Fallback to the environment hint if it was explicitly provided
    if [ -n "${1:-}" ] && [ -b "$1" ]; then
        echo "$1"
        return 0
    fi

    # 4. Last resort fallback to installed.prop reference if present
    if [ -r "$STATE_DIR/installed.prop" ] && [ -n "${SYS_DEV:-}" ] && [ -b "$SYS_DEV" ]; then
        echo "$SYS_DEV"
        return 0
    fi

    return 1
}

# find_user_device [hint] -- echo the NDUSER partition.
find_user_device() {
    # As above: the serial survives a reformat of the partition, the label
    # does not.
    device="$(device_by_serial "$NDUSER_SERIAL")" && [ -n "$device" ] && {
        echo "$device"
        return 0
    }
    labelled="$(blkid 2>/dev/null | \
        sed -n 's/^\([^:]*\):.*LABEL="NDUSER".*/\1/p' | head -n1)"
    if [ -n "$labelled" ]; then
        echo "$labelled"
        return 0
    fi
    # No label (or no blkid support): fall back to the hint, as long as it is
    # not the system image.
    if [ -n "${1:-}" ] && [ -b "$1" ] && ! is_squashfs "$1"; then
        echo "$1"
        return 0
    fi
    return 1
}

# Read one KEY=value out of a record file, without sourcing it.
getprop() {
    [ -r "$2" ] || return 1
    sed -n "s/^$1=//p" "$2" 2>/dev/null | head -n1
}

# Write a record atomically: temp file, rename, sync.
putprop_file() {
    mkdir -p "$(dirname "$1")" 2>/dev/null
    cat > "$1.new" && mv -f "$1.new" "$1" && sync
}

record_result() {   # record_result RESULT VERSION REASON
    [ -n "${USER_MOUNTED:-}" ] || return 0
    putprop_file "$STATE_DIR/last_result.prop" <<EOF
result=$1
version=$2
reason=$3
EOF
}

clear_pending() {
    rm -f "$STATE_DIR/pending.prop" "$STATE_DIR/pending.img" 2>/dev/null
    sync
    return 0
}

# sha256 of the first $2 bytes of $1, which may be a file or a block device.
hash_prefix() {
    blocks=$((${2} / 4096))
    dd if="$1" bs=4096 count="$blocks" 2>/dev/null | sha256sum | cut -d' ' -f1
}

# The dm-verity table for the installed image. Data and hash share one
# device: the tree starts right after the squashfs, and hash_start skips the
# verity superblock block. Must stay in step with
# UpdateService/verity.py dm_table() -- a test compares the two.
verity_table() {
    INSTALLED="$STATE_DIR/installed.prop"
    root_hash="$(getprop verity_root_hash "$INSTALLED")"
    block_size="$(getprop verity_block_size "$INSTALLED")"
    image_blocks="$(getprop verity_image_blocks "$INSTALLED")"
    salt="$(getprop verity_salt "$INSTALLED")"

    [ -n "$root_hash" ] || return 1
    [ -n "$block_size" ] || return 1
    [ -n "$image_blocks" ] || return 1
    [ -n "$salt" ] || salt="-"

    sectors=$((image_blocks * block_size / 512))
    hash_start=$((image_blocks + 1))
    echo "0 $sectors verity 1 $SYS_DEV $SYS_DEV $block_size $block_size" \
         "$image_blocks $hash_start sha256 $root_hash $salt"
}

# Install a staged update, if there is one. Safe to call on every boot and
# safe to interrupt: the staged image and its record are only removed once
# the write has been read back and verified.
apply_pending() {
    PENDING="$STATE_DIR/pending.prop"
    [ -r "$PENDING" ] || return 0

    # The record was written by the running system, where the user partition
    # is mounted at /NeoDCT/User; here it is at /mnt/user. Resolve the image
    # next to the record instead of trusting the path it names, or a real
    # staged update disappears as "incomplete".
    image="$STATE_DIR/$(basename "$(getprop image "$PENDING")" 2>/dev/null)"
    image_bytes="$(getprop image_bytes "$PENDING")"
    want_sha="$(getprop sha256 "$PENDING")"
    version="$(getprop version "$PENDING")"
    attempts="$(getprop attempts "$PENDING")"
    [ -n "$attempts" ] || attempts=0

    if [ -z "$image" ] || [ ! -f "$image" ] || [ -z "$image_bytes" ] \
            || [ -z "$want_sha" ]; then
        log "staged update is incomplete; discarding"
        record_result failed "$version" "incomplete staging record"
        clear_pending
        return 0
    fi

    if [ "$attempts" -ge "$MAX_ATTEMPTS" ]; then
        log "staged update failed $attempts times; gave up on $version"
        record_result failed "$version" "gave up after $attempts attempts"
        clear_pending
        return 0
    fi
    sed "s/^attempts=.*/attempts=$((attempts + 1))/" "$PENDING" > "$PENDING.new" \
        && mv -f "$PENDING.new" "$PENDING" && sync

    actual_size="$(wc -c < "$image" 2>/dev/null | tr -d ' ')"
    if [ "$actual_size" != "$image_bytes" ]; then
        log "staged image is $actual_size bytes, expected $image_bytes"
        record_result failed "$version" "staged image truncated"
        clear_pending
        return 0
    fi

    if [ "$(hash_prefix "$image" "$image_bytes")" != "$want_sha" ]; then
        log "staged image sha256 mismatch; refusing to install $version"
        record_result failed "$version" "image sha256 mismatch before write"
        clear_pending
        return 0
    fi

    log "installing $version ($image_bytes bytes) to $SYS_DEV"
    if ! dd if="$image" of="$SYS_DEV" bs=1M conv=fsync 2>/dev/null; then
        log "write to $SYS_DEV failed; retrying on the next boot"
        return 0
    fi
    sync

    # Read it back off the device. A write that reported success but landed
    # badly is exactly what verity would trip over on every later boot, and
    # by then the staged copy would be gone.
    if [ "$(hash_prefix "$SYS_DEV" "$image_bytes")" != "$want_sha" ]; then
        log "read-back mismatch on $SYS_DEV; retrying on the next boot"
        return 0
    fi

    putprop_file "$STATE_DIR/installed.prop" <<EOF
sha256=$want_sha
image_bytes=$image_bytes
version=$version
buildtime=$(getprop buildtime "$PENDING")
platform=$(getprop platform "$PENDING")
verity_root_hash=$(getprop verity_root_hash "$PENDING")
verity_block_size=$(getprop verity_block_size "$PENDING")
verity_image_blocks=$(getprop verity_image_blocks "$PENDING")
verity_salt=$(getprop verity_salt "$PENDING")
EOF
    clear_pending
    record_result ok "$version" "installed"
    log "installed $version"
    return 0
}

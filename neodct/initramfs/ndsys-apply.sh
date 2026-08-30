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
: "${NDSYS_UBIUPDATEVOL:=ubiupdatevol}"
: "${NDSYS_UBIRSVOL:=ubirsvol}"
: "${NDSYS_UBI_SYSFS:=/sys/class/ubi}"
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
    # A UBI volume is not a block device and has no blkid label, so none of
    # the above can find it. On raw NAND (Luckfox) the user partition is
    # ubifs, named "ubiN:volume" rather than given as a path -- take that
    # spelling from the cmdline as-is and let the mount decide.
    case "${1:-}" in
        ubi[0-9]*:?*) echo "$1"; return 0 ;;
    esac
    return 1
}

# Read one KEY=value out of a record file, without sourcing it.
getprop() {
    [ -r "$2" ] || return 1
    sed -n "s/^$1=//p" "$2" 2>/dev/null | head -n1
}

# --- the SD card ---------------------------------------------------------
#
# Where an update actually lives. The applier needs it because a .ndsw is
# installed straight off the card: the user partition is 8 MiB on the
# Luckfox and a system image is 51 MiB, so there is nowhere on the phone to
# put a copy, and making one was never necessary.

: "${MNT_SDCARD:=/mnt/sdcard}"

# The filesystems a card might carry, in the order it is likely to be.
# Must stay in step with CARD_FSTYPES in System/hw/neodct-sdcard: for a
# while this tried vfat alone, and every exFAT card -- which is what large
# cards are formatted as from the factory -- read as "no SD card found"
# while the running system had been mounting it happily all along.
: "${NDSYS_CARD_FSTYPES:=vfat exfat ext4 ext3 ext2}"

# Set when the card is already available at MNT_SDCARD and must not be
# mounted or unmounted here. The host tests use it to point the applier at
# a directory, the same way NDSYS_SCAN_GLOB points it at ordinary files.
: "${NDSYS_CARD_PREMOUNTED:=}"

# mount_card -- mount the first candidate that is not system or user.
# Read-only: nothing at boot has any business writing to the card, and a
# card pulled out mid-write is a card someone loses their messages from.
#
# nosuid,nodev,noexec as well, because this is the one mount in the whole boot
# path whose contents were chosen by whoever last held the card. Nothing here
# runs anything off it -- unzip reads the .ndsw and that is all -- so noexec
# costs nothing and closes the crafted-card path SECURITY-AUDIT.md finding 9
# describes. The card is FAT, which cannot represent a setuid bit or a device
# node in the first place; these options are what makes that true for an ext
# card somebody hands the phone as well.
: "${NDSYS_CARD_MOUNT_OPTS:=ro,nosuid,nodev,noexec}"

mount_card() {
    [ -n "$NDSYS_CARD_PREMOUNTED" ] && return 0
    mkdir -p "$MNT_SDCARD" 2>/dev/null
    mountpoint -q "$MNT_SDCARD" 2>/dev/null && return 0
    for device in $(candidate_devices); do
        [ "$device" = "$SYS_DEV" ] && continue
        [ "$device" = "${USER_DEV:-}" ] && continue
        is_squashfs "$device" && continue
        for fstype in $NDSYS_CARD_FSTYPES; do
            if mount -t "$fstype" -o "$NDSYS_CARD_MOUNT_OPTS" "$device" \
                    "$MNT_SDCARD" 2>/dev/null; then
                return 0
            fi
        done
    done
    return 1
}

umount_card() {
    [ -n "$NDSYS_CARD_PREMOUNTED" ] && return 0
    umount "$MNT_SDCARD" 2>/dev/null
    return 0
}

# ubi_volume_for DEV -- the UBI volume character device behind a ubiblock
# disk, or nothing at all for anything else.
#
# THE PHONE'S SYSTEM PARTITION IS ONE OF THESE. The Luckfox cmdline says
#
#     ubi.mtd=4 ubi.block=0,system neodct.sys=/dev/ubiblock0_0
#
# and ubiblock is READ-ONLY: the kernel registers that disk read-only on
# purpose, because it exists so squashfs and dm-verity have a block device to
# read. `dd of=/dev/ubiblock0_0` therefore cannot succeed for anyone, ever --
# and apply_pending's own error path turns that into a silent no-op, logging
# "write failed; retrying on the next boot" and booting the old system. What
# the owner sees is an update that downloads, reboots, and changes nothing.
#
# It was invisible to every host test in test_initramfs_apply.py because they
# all write to ordinary files, which dd is perfectly happy with, and invisible
# in QEMU because there the system device is /dev/vda.
#
# The writable side of a UBI volume is its character device. A STATIC volume
# in particular cannot be seek-and-written at all: an update is a transaction
# opened with the volume's final size, which is exactly what ubiupdatevol
# does and why no ordinary tool substitutes for it.
ubi_volume_for() {
    case "${1##*/}" in
        ubiblock[0-9]*_[0-9]*) ;;
        *) return 1 ;;
    esac
    echo "${1%/*}/ubi${1##*/ubiblock}"
}

# find_package NAME -- echo the path to NAME on the card, if it is there.
#
# By name, never by the path the record carries: the card is mounted
# somewhere else here than it was when the update was staged, which is the
# same reason a staged image is resolved against STATE_DIR.
find_package() {
    for directory in "$MNT_SDCARD/update" "$MNT_SDCARD"; do
        [ -f "$directory/$1" ] && echo "$directory/$1" && return 0
    done
    return 1
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
    rm -f "$STATE_DIR/pending.prop" "$STATE_DIR/pending.img" \
          "$STATE_DIR/pending.manifest.json" "$STATE_DIR/pending.manifest.sig" \
          2>/dev/null
    sync
    return 0
}

# sha256 of the image inside a .ndsw, read without unpacking it anywhere.
package_image_sha() {
    unzip -p "$1" rootfs.squashfs 2>/dev/null | sha256sum | cut -d' ' -f1
}

# The size of the image inside a .ndsw, from the zip's own listing.
package_image_size() {
    unzip -l "$1" rootfs.squashfs 2>/dev/null \
        | awk '$NF == "rootfs.squashfs" {print $1; exit}'
}

# sha256 of the first $2 bytes of $1, which may be a file or a block device.
hash_prefix() {
    blocks=$((${2} / 4096))
    dd if="$1" bs=4096 count="$blocks" 2>/dev/null | sha256sum | cut -d' ' -f1
}

# --- the release signature -----------------------------------------------
#
# SECURITY-AUDIT.md section 3, the critical finding, and the reason this
# section exists at all:
#
#     the running system checks the signature, writes pending.prop and
#     installed.prop to the WRITABLE partition, and the initramfs then
#     believes both.
#
# So a process that can write /NeoDCT/User stages its own image, records its
# own verity root hash, and every boot after that verifies cleanly against
# it. dm-verity was an integrity guarantee and not an authenticity one, and
# an update replaces only the rootfs, so nothing removes the result.
#
# The check belongs HERE because the initramfs is the one link in that chain
# an attacker cannot rewrite: it is built into the kernel image and replaced
# only by a reflash, and so are the verifier and the public key it carries.
# SECURITY-PLAN.md section 5 puts it in Phase 0.
#
# What is verified is manifest.json against manifest.sig -- the same detached
# RSA/SHA-256 signature the Update app checked, over the same bytes, with the
# same key. Then every field the record claims is compared against the field
# the SIGNED manifest actually carries, so pending.prop stops being a trust
# input and becomes a cache that has to agree with something signed.
: "${NDSYS_VERIFY_BIN:=/bin/nd-verify}"
: "${NDSYS_RELEASE_KEY:=/neodct-release.pub}"
: "${NDSYS_TMPDIR:=/run/ndsys}"

# Set from neodct.unsigned=1 on the kernel cmdline by init. The cmdline is
# the U-Boot environment on the phone and -append in QEMU: not writable from
# a running system, which is the whole point -- an escape hatch reachable
# from the partition being defended would not be one.
#
# It exists because engineering mode can install an unsigned package on
# purpose (apps/Update/main.c), and that is a real developer workflow. What
# it cannot be is the default.
: "${NDSYS_ALLOW_UNSIGNED:=}"

# One field out of a manifest.json, by exact line.
#
# The manifest is json.dumps(indent=2), so a value is always alone on its
# line at a known indent and a JSON string can never contain a raw newline.
# Anchoring on the indent and the key name is therefore exact, and no line
# other than the real one can match -- a changelog full of escaped quotes and
# braces is still one line beginning with "changelog".
#
# This runs only on a manifest whose signature has ALREADY been checked, so
# the parsing is of our own generator's output, not of a stranger's file.
manifest_str() {   # manifest_str FILE INDENT KEY
    sed -n "s/^$2\"$3\": \"\(.*\)\",\{0,1\}$/\1/p" "$1" 2>/dev/null | head -n1
}

manifest_num() {   # manifest_num FILE INDENT KEY
    sed -n "s/^$2\"$3\": \([0-9][0-9]*\),\{0,1\}$/\1/p" "$1" 2>/dev/null | head -n1
}

# Put manifest.json and manifest.sig somewhere readable and echo the
# directory. A .ndsw carries both; a loose staged image has them written
# beside it by the staging code that produced it.
manifest_pair() {   # manifest_pair IMAGE PACKAGE
    dir="$NDSYS_TMPDIR/manifest"
    rm -rf "$dir" 2>/dev/null
    mkdir -p "$dir" 2>/dev/null || return 1
    if [ -n "$2" ]; then
        unzip -p "$1" manifest.json > "$dir/manifest.json" 2>/dev/null
        unzip -p "$1" manifest.sig  > "$dir/manifest.sig"  2>/dev/null
    else
        cat "$STATE_DIR/pending.manifest.json" > "$dir/manifest.json" 2>/dev/null
        cat "$STATE_DIR/pending.manifest.sig"  > "$dir/manifest.sig"  2>/dev/null
    fi
    [ -s "$dir/manifest.json" ] || return 1
    [ -s "$dir/manifest.sig" ] || return 1
    echo "$dir"
}

# Every field of the record that the signature covers. A mismatch means the
# record was written by something other than the code that read this signed
# manifest -- which is exactly the attack.
#
# An extraction that comes back EMPTY is a mismatch too, for every field but
# the salt. The manifest is json.dumps(indent=2), so the patterns above are
# exact for what mkupdate.py writes; if that formatting ever changes, this
# fails closed and refuses the update rather than quietly comparing "" with
# "" and letting a forged record through.
field_agrees() {   # field_agrees KEY SIGNED-VALUE RECORD [may-be-empty]
    _got="$(getprop "$1" "$3")"
    if [ -z "$2" ] && [ -z "${4:-}" ]; then
        log "$1: nothing readable in the signed manifest"
        return 1
    fi
    if [ "$2" != "$_got" ]; then
        log "$1: the record says '$_got', the signed manifest says '$2'"
        return 1
    fi
    return 0
}

record_matches_manifest() {   # record_matches_manifest MANIFEST RECORD
    _bad=0
    field_agrees sha256 "$(manifest_str "$1" '  ' sha256)" "$2" || _bad=1
    field_agrees version "$(manifest_str "$1" '  ' version)" "$2" || _bad=1
    field_agrees platform "$(manifest_str "$1" '  ' platform)" "$2" || _bad=1
    field_agrees buildtime "$(manifest_num "$1" '  ' buildtime)" "$2" || _bad=1
    field_agrees verity_root_hash \
        "$(manifest_str "$1" '    ' root_hash)" "$2" || _bad=1
    field_agrees verity_block_size \
        "$(manifest_num "$1" '    ' block_size)" "$2" || _bad=1
    field_agrees verity_image_blocks \
        "$(manifest_num "$1" '    ' image_blocks)" "$2" || _bad=1
    # The one optional field: verity.salt may genuinely be absent, and then
    # both sides are empty and that agreement is the right answer.
    field_agrees verity_salt "$(manifest_str "$1" '    ' salt)" "$2" empty || _bad=1
    [ "$_bad" = 0 ]
}

# The gate. True means "this image is the one the release key signed, and the
# record describing it says what the signed manifest says".
release_signature_ok() {   # release_signature_ok IMAGE PACKAGE RECORD
    if [ ! -x "$NDSYS_VERIFY_BIN" ]; then
        log "no signature verifier at $NDSYS_VERIFY_BIN"
        return 1
    fi
    if [ ! -r "$NDSYS_RELEASE_KEY" ]; then
        log "no release key at $NDSYS_RELEASE_KEY"
        return 1
    fi
    dir="$(manifest_pair "$1" "$2")" || {
        log "the update carries no manifest and signature to check"
        return 1
    }
    if ! "$NDSYS_VERIFY_BIN" "$dir/manifest.json" "$dir/manifest.sig" \
            "$NDSYS_RELEASE_KEY" > /dev/null 2>&1; then
        log "manifest.sig is not a release signature over manifest.json"
        return 1
    fi
    record_matches_manifest "$dir/manifest.json" "$3" || return 1
    return 0
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

# write_system BYTES -- the image arrives on stdin and goes to the system
# partition, whichever kind it is. UBI_VOL is set by the caller.
#
# The size is passed rather than measured because ubiupdatevol needs it up
# front: a static volume update declares its final size when the transaction
# opens, and a stream has no length to ask for. dd does not care, and taking
# the argument anyway keeps one signature for both.
# ubi_fit VOLUME BYTES -- make sure the volume is big enough, growing it if
# it is not.
#
# The phone's system volume is created by mknand.sh with no size of its own,
# so ubinize sizes it to exactly the image being flashed. A static volume
# cannot then take a larger one, and NeoDCT images grow -- 0.4.4a added 2.7 MB
# when Bluetooth arrived. Without this, the first update bigger than the build
# on the phone fails to install and says nothing more than "retrying".
#
# There is room: the partition is 100 MB and the image is under 50 MB. UBI
# simply has to be asked, because a static volume's size is part of its table
# rather than something it discovers.
#
# Doing nothing when it already fits is not just an optimisation -- a resize
# rewrites the volume table, and that is not worth risking on every update
# that happens to be the same size or smaller.
ubi_fit() {
    vol=${1##*/}
    num=${vol#ubi}; num=${num%%_*}
    vid=${vol##*_}
    ebs="$(cat "$NDSYS_UBI_SYSFS/$vol/reserved_ebs" 2>/dev/null)" || return 0
    leb="$(cat "$NDSYS_UBI_SYSFS/$vol/usable_eb_size" 2>/dev/null)" || return 0
    [ -n "$ebs" ] && [ -n "$leb" ] || return 0
    have=$((ebs * leb))
    [ "$have" -ge "$2" ] && return 0
    log "volume $1 holds $have bytes and the image needs $2; growing it"
    "$NDSYS_UBIRSVOL" -n "$vid" -s "$2" "${1%/*}/ubi$num"
}

write_system() {
    if [ -n "${UBI_VOL:-}" ]; then
        if ! ubi_fit "$UBI_VOL" "$1"; then
            log "could not grow $UBI_VOL to $1 bytes"
            return 1
        fi
        "$NDSYS_UBIUPDATEVOL" -s "$1" "$UBI_VOL" -
    else
        dd of="$SYS_DEV" bs=1M conv=fsync 2>/dev/null
    fi
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
    package="$(basename "$(getprop package "$PENDING")" 2>/dev/null)"
    image="$STATE_DIR/$(basename "$(getprop image "$PENDING")" 2>/dev/null)"
    image_bytes="$(getprop image_bytes "$PENDING")"
    want_sha="$(getprop sha256 "$PENDING")"
    version="$(getprop version "$PENDING")"
    attempts="$(getprop attempts "$PENDING")"
    [ -n "$attempts" ] || attempts=0

    if [ -z "$image_bytes" ] || [ -z "$want_sha" ]; then
        log "staged update is incomplete; discarding"
        record_result failed "$version" "incomplete staging record"
        clear_pending
        return 0
    fi

    # A record naming a package installs from the card. Find it before
    # anything else: a missing card is worth another boot, not a discarded
    # update -- somebody may simply have taken it out.
    if [ -n "$package" ]; then
        if ! mount_card; then
            log "update $version waits for the card it is on"
            return 0
        fi
        image="$(find_package "$package")"
        if [ -z "$image" ]; then
            log "$package is not on this card; waiting"
            umount_card
            return 0
        fi
        log "installing $version from $image"
    elif [ -z "$image" ] || [ ! -f "$image" ]; then
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

    # Before the size and the hash, because both of those check the image
    # against the RECORD and the record is the thing being distrusted here.
    # See the section above: this is what stops /NeoDCT/User from choosing
    # which operating system the phone runs.
    #
    # A refusal clears the pending update rather than retrying. An unsigned
    # image will not become signed on the next boot, and leaving it staged
    # means three boots spent hashing 51 MB to reach the same answer.
    if release_signature_ok "$image" "$package" "$PENDING"; then
        :
    elif [ -n "$NDSYS_ALLOW_UNSIGNED" ]; then
        log "WARNING: neodct.unsigned=1 -- installing $version with no signature check"
    else
        log "refusing to install $version: not signed by the release key"
        record_result failed "$version" "not signed by the release key"
        umount_card
        clear_pending
        return 0
    fi

    if [ -n "$package" ]; then
        actual_size="$(package_image_size "$image")"
    else
        actual_size="$(wc -c < "$image" 2>/dev/null | tr -d ' ')"
    fi
    if [ "$actual_size" != "$image_bytes" ]; then
        log "staged image is $actual_size bytes, expected $image_bytes"
        record_result failed "$version" "staged image truncated"
        umount_card
        clear_pending
        return 0
    fi

    # Hash before writing, always. The running system checked the package's
    # signature before recording it; this checks that the bytes about to be
    # written are the ones that was said about. A card swapped between
    # staging and reboot fails here rather than installing.
    if [ -n "$package" ]; then
        got_sha="$(package_image_sha "$image")"
    else
        got_sha="$(hash_prefix "$image" "$image_bytes")"
    fi
    if [ "$got_sha" != "$want_sha" ]; then
        log "staged image sha256 mismatch; refusing to install $version"
        record_result failed "$version" "image sha256 mismatch before write"
        umount_card
        clear_pending
        return 0
    fi

    # UBI or an ordinary block device -- decided once, here, so the two
    # streaming callers below do not each have to know.
    if UBI_VOL="$(ubi_volume_for "$SYS_DEV")"; then
        log "installing $version ($image_bytes bytes) to $UBI_VOL (ubi volume)"
    else
        UBI_VOL=""
        log "installing $version ($image_bytes bytes) to $SYS_DEV"
    fi

    if [ -n "$package" ]; then
        # Straight from the zip to the partition. No copy is made anywhere:
        # there is nowhere on this phone to put one.
        if ! unzip -p "$image" rootfs.squashfs 2>/dev/null | write_system "$image_bytes"; then
            log "write to ${UBI_VOL:-$SYS_DEV} failed; retrying on the next boot"
            umount_card
            return 0
        fi
    elif ! write_system "$image_bytes" < "$image"; then
        log "write to ${UBI_VOL:-$SYS_DEV} failed; retrying on the next boot"
        return 0
    fi
    sync
    umount_card

    # Read it back off the device. A write that reported success but landed
    # badly is exactly what verity would trip over on every later boot, and
    # by then the staged copy would be gone.
    #
    # Read back through whatever was WRITTEN, which for UBI is the volume
    # character device and not the ubiblock disk. Two reasons, and either one
    # alone would be enough:
    #
    #   - The block device has a page cache that nothing invalidated. The
    #     kernel's only reaction to a static volume being updated is
    #     ubiblock_resize() (drivers/mtd/ubi/block.c, UBI_VOLUME_UPDATED), so
    #     if the new image happens to be the same size as the old one there
    #     is no capacity change and no reason for it to drop anything. The
    #     read-back would then hash the PREVIOUS system and report a mismatch
    #     on a write that was perfectly good.
    #   - Reading the character device is what the write did in reverse, so
    #     the check is over the same bytes through the same path.
    if [ "$(hash_prefix "${UBI_VOL:-$SYS_DEV}" "$image_bytes")" != "$want_sha" ]; then
        log "read-back mismatch on ${UBI_VOL:-$SYS_DEV}; retrying on the next boot"
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

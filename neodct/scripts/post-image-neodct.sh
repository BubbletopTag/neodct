#!/bin/sh
# Assemble the bootable NeoDCT image set from a finished buildroot tree.
#
# Produces, in $BINARIES_DIR:
#   system.img            padded squashfs + appended dm-verity hash tree
#   system.manifest.json  its version/verity parameters
#   initramfs.cpio.gz     the boot-time update applier
#   userdata.ext4         empty user partition, labelled NDUSER, carrying the
#                         installed.prop the initramfs needs on first boot
#   sdcard.img            a ready-made NeoDCT FAT32 card (only if absent, so
#                         a card you have put files on is never clobbered)
#
# `make update` packages the same system.img as an UPDATE.ndsw.
#
# Runs as a BR2_ROOTFS_POST_IMAGE_SCRIPT, so buildroot exports TARGET_DIR,
# HOST_DIR and BINARIES_DIR into the environment.
set -eu

BINARIES_DIR="${1:-${BINARIES_DIR:-}}"
: "${TARGET_DIR:?post-image: TARGET_DIR not set by buildroot}"
: "${HOST_DIR:?post-image: HOST_DIR not set by buildroot}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NEODCT_DIR="$(dirname "$SCRIPT_DIR")"

USERDATA_MB="${NEODCT_USERDATA_MB:-512}"
SDCARD_MB="${NEODCT_SDCARD_MB:-128}"

say() { echo "[post-image] $*"; }

# Prefer buildroot's own host tools over whatever the build machine has.
host_tool() {
    for candidate in "$HOST_DIR/sbin/$1" "$HOST_DIR/bin/$1"; do
        [ -x "$candidate" ] && { echo "$candidate"; return 0; }
    done
    command -v "$1" 2>/dev/null && return 0
    return 1
}

if [ ! -f "$BINARIES_DIR/rootfs.squashfs" ]; then
    say "no rootfs.squashfs -- enable BR2_TARGET_ROOTFS_SQUASHFS; skipping"
    exit 0
fi

# --- system.img (squashfs + verity tree) + factory installed.prop ---------
SKEL="$BINARIES_DIR/.userdata-skel"
rm -rf "$SKEL"
# These are the directories S00userdata would create on first boot; making
# them here means a fresh partition is usable even before it runs.
mkdir -p "$SKEL/db" "$SKEL/logs" "$SKEL/.ndsys" \
         "$SKEL/.seedrng" "$SKEL/sdcard" "$SKEL/tones" "$SKEL/wallpapers"

"$NEODCT_DIR/tools/mkupdate.py" \
    --images-dir "$BINARIES_DIR" \
    --target-dir "$TARGET_DIR" \
    --image-only \
    --installed-prop "$SKEL/.ndsys"

# --- initramfs -----------------------------------------------------------
# --verifier points at nd-verify, which the neodct package installs into
# BINARIES_DIR rather than into the rootfs: it is 4 MB of statically linked
# OpenSSL whose only caller is the initramfs, and the running system checks
# the same signature through libneodct, which is already mapped. mkinitramfs
# fails the build if it is not there -- an initramfs that cannot check an
# update signature would install anything staged for it.
#
# --recui points at the on-screen recovery UI, which install-boot puts beside
# nd-verify for the same reason: its only caller is the initramfs. Unlike the
# verifier it is optional -- without it recovery falls back to the text menu
# on tty1, which is what it has always had.
# --bootbar is the install-progress screen, from the same place and for the
# same reason. It is NOT required: without it an update installing at boot
# shows the logo and nothing else, which is where the phone was before it
# existed, and failing an image build over that would be the wrong trade.
"$NEODCT_DIR/scripts/mkinitramfs.py" \
    --target-dir "$TARGET_DIR" \
    --init "$NEODCT_DIR/initramfs" \
    --verifier "$BINARIES_DIR/nd-verify" \
    --recui "$BINARIES_DIR/nd-recui" \
    --bootbar "$BINARIES_DIR/nd-bootbar" \
    --output "$BINARIES_DIR/initramfs.cpio.gz"

# --- userdata.ext4 -------------------------------------------------------
MKE2FS="$(host_tool mke2fs)" || { say "mke2fs not found"; exit 1; }
USERDATA="$BINARIES_DIR/userdata.ext4"

if [ -f "$USERDATA" ] && [ -n "${NEODCT_KEEP_USERDATA:-}" ]; then
    # Keep contacts/messages/settings across a rebuild. installed.prop still
    # has to be refreshed: it records the root hash of the image that was
    # installed, and the image just changed -- a stale hash means dm-verity
    # refuses to boot the new system.
    if DEBUGFS="$(host_tool debugfs)"; then
        # The source file has to exist before any of this is worth doing.
        # It is written by mkupdate above; if that changed or failed, the
        # write below silently does nothing and the phone keeps booting the
        # previous image's root hash -- dm-verity then builds a device whose
        # blocks do not hash, and the boot dies at "cannot mount the system
        # image" with no clue why. That failure shipped once. Check first.
        if [ ! -f "$SKEL/.ndsys/installed.prop" ]; then
            say "no $SKEL/.ndsys/installed.prop -- mkupdate did not write it"
            say "  refusing to keep a userdata whose root hash would be stale"
            exit 1
        fi
        "$DEBUGFS" -w -R "mkdir /.ndsys" "$USERDATA" > /dev/null 2>&1 || true
        "$DEBUGFS" -w -R "rm /.ndsys/installed.prop" "$USERDATA" \
            > /dev/null 2>&1 || true
        # Deliberately NOT piped anywhere. An earlier version sent this
        # through `grep -q` to filter debugfs's banner, and grep exits at
        # its first match -- closing the pipe and SIGPIPEing debugfs in the
        # middle of the write. The image then carried the previous build's
        # root hash and would not boot. Capture to a file if the output is
        # wanted; never put a short-circuiting reader on the other end.
        "$DEBUGFS" -w -R \
            "write $SKEL/.ndsys/installed.prop .ndsys/installed.prop" \
            "$USERDATA" > "$BINARIES_DIR/.debugfs.log" 2>&1 || true
        # debugfs leaks the blocks of the file it unlinked; tidy up.
        if E2FSCK="$(host_tool e2fsck)"; then
            "$E2FSCK" -fy "$USERDATA" > /dev/null 2>&1 || true
        fi
        # Prove it landed rather than trusting the exit status: read the
        # hash back out and compare it to the image we just built.
        WANT=$(sed -n 's/^verity_root_hash=//p' "$SKEL/.ndsys/installed.prop")
        GOT=$("$DEBUGFS" -R "cat /.ndsys/installed.prop" "$USERDATA" 2>/dev/null \
              | sed -n 's/^verity_root_hash=//p')
        if [ -z "$GOT" ] || [ "$GOT" != "$WANT" ]; then
            say "installed.prop did not take: userdata says '${GOT:-nothing}',"
            say "  image is '$WANT'. That userdata cannot boot this image."
            exit 1
        fi
        say "userdata.ext4 kept, installed.prop refreshed (root ${WANT%%"${WANT#????????}"}...)"
    else
        say "debugfs not found; cannot refresh installed.prop in place."
        say "  Unset NEODCT_KEEP_USERDATA to rebuild the partition instead."
        exit 1
    fi
else
    rm -f "$USERDATA"
    # root_owner=0:0 because mke2fs -d otherwise copies the build user's uid,
    # and everything on the phone runs as root.
    "$MKE2FS" -q -F -t ext4 -b 4096 -L NDUSER \
        -d "$SKEL" -E root_owner=0:0 \
        "$USERDATA" "$((USERDATA_MB * 256))" > /dev/null
    say "userdata.ext4 (${USERDATA_MB}M, label NDUSER) -- user data reset"
    say "  (NEODCT_KEEP_USERDATA=1 keeps it across rebuilds)"
fi
rm -rf "$SKEL"

# --- sdcard.img ----------------------------------------------------------
#
# The emulator's card, and it is built the way the PHONE builds one: two
# FAT32 partitions, media and arrival, per SECURITY-PLAN.md section 1. The
# partition table comes from the phone's own helper rather than from a copy
# of its arithmetic -- `neodct-sdcard` is sourced for write_mbr() and
# partition_plan() -- so a card made here and a card made by Settings cannot
# drift apart. That matters more than it sounds: QEMU is the only place the
# two-partition path gets exercised before it reaches hardware.
#
# The two filesystems are made in separate files and dd'd into place, rather
# than with mkfs.vfat --offset. --offset arrived in dosfstools 4.2 and this
# has to work with whatever host-dosfstools buildroot pinned; writing at an
# offset is one dd either way.
build_sdcard_image() {   # build_sdcard_image PATH MKFS
    _img="$1"
    _mkfs="$2"
    _helper="$NEODCT_DIR/overlay/NeoDCT/System/hw/neodct-sdcard"

    rm -f "$_img"
    dd if=/dev/zero of="$_img" bs=1M count="$SDCARD_MB" status=none

    if [ ! -r "$_helper" ]; then
        say "no $_helper; making a single-filesystem card"
        "$_mkfs" -F 32 -n NEODCT "$_img" > /dev/null
        sdcard_folders "$_img" ""
        return 0
    fi

    # SOURCE_ONLY stops the helper dispatching on "$1"; it only defines
    # functions. Everything it needs from the environment it defaults.
    _plan="$(NEODCT_SDCARD_SOURCE_ONLY=1 sh -c \
        ". \"$_helper\"; partition_plan \"$_img\"" 2>/dev/null)" || _plan=""
    if [ -z "$_plan" ] || [ "$_plan" = "superfloppy" ]; then
        say "sdcard.img is too small to partition; one filesystem then"
        "$_mkfs" -F 32 -n NEODCT "$_img" > /dev/null
        sdcard_folders "$_img" ""
        return 0
    fi

    # shellcheck disable=SC2086  # four numbers, deliberately word-split
    set -- $_plan
    _p1_start="$1"; _p1_sectors="$2"; _p2_start="$3"; _p2_sectors="$4"

    NEODCT_SDCARD_SOURCE_ONLY=1 sh -c \
        ". \"$_helper\"; write_mbr \"$_img\" $_p1_start $_p1_sectors \
            $_p2_start $_p2_sectors" || {
        say "could not write a partition table into sdcard.img"
        return 1
    }

    # mkfs.vfat's block count is in 1 KiB units, so half the sector count.
    _tmp1="$_img.p1"
    _tmp2="$_img.p2"
    "$_mkfs" -F 32 -n NEODCT -C "$_tmp1" "$((_p1_sectors / 2))" > /dev/null
    "$_mkfs" -F 32 -n NEODCTUT -C "$_tmp2" "$((_p2_sectors / 2))" > /dev/null
    sdcard_folders "$_tmp1" ""
    dd if="$_tmp1" of="$_img" bs=512 seek="$_p1_start" conv=notrunc status=none
    dd if="$_tmp2" of="$_img" bs=512 seek="$_p2_start" conv=notrunc status=none
    rm -f "$_tmp1" "$_tmp2"

    say "sdcard.img (${SDCARD_MB}M): media $((_p1_sectors / 2048))M NEODCT," \
        "arrival $((_p2_sectors / 2048))M NEODCTUT"
}

# The five NeoDCT folders, in whichever FAT image is handed over.
sdcard_folders() {   # sdcard_folders IMAGE OFFSET-SUFFIX
    if MMD="$(host_tool mmd)"; then
        for folder in wallpapers tones backup_db music update untrusted; do
            # "untrusted" is the mountpoint the arrival partition goes on,
            # and it lives on the media side -- the same way sdcard/ itself
            # is a directory on /NeoDCT/User.
            MTOOLS_SKIP_CHECK=1 "$MMD" -i "$1$2" "::/$folder" 2>/dev/null || true
        done
    else
        say "  no mtools: the phone will offer to set the folders up"
    fi
}

# Never overwrite an existing card image: it is where the user drops their
# music, wallpapers and UPDATE.ndsw between builds.
if [ -f "$BINARIES_DIR/sdcard.img" ]; then
    say "sdcard.img exists, leaving it alone"
else
    if MKFSVFAT="$(host_tool mkfs.vfat)"; then
        build_sdcard_image "$BINARIES_DIR/sdcard.img" "$MKFSVFAT"
    else
        say "mkfs.vfat not found; skipping sdcard.img"
    fi
fi

say "image set ready in $BINARIES_DIR"

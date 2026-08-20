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
mkdir -p "$SKEL/db" "$SKEL/logs" "$SKEL/.ndsys" "$SKEL/.pycache" \
         "$SKEL/.seedrng" "$SKEL/sdcard" "$SKEL/tones" "$SKEL/wallpapers"

"$NEODCT_DIR/tools/mkupdate.py" \
    --images-dir "$BINARIES_DIR" \
    --target-dir "$TARGET_DIR" \
    --image-only \
    --installed-prop "$SKEL/.ndsys"

# --- initramfs -----------------------------------------------------------
"$NEODCT_DIR/scripts/mkinitramfs.py" \
    --target-dir "$TARGET_DIR" \
    --init "$NEODCT_DIR/initramfs" \
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
        # Not silenced: this is the write that matters.
        if ! "$DEBUGFS" -w -R \
                "write $SKEL/.ndsys/installed.prop .ndsys/installed.prop" \
                "$USERDATA" 2>&1 | grep -qv "^debugfs"; then
            :   # debugfs prints its banner on stderr even when it works
        fi
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
# Never overwrite an existing card image: it is where the user drops their
# music, wallpapers and UPDATE.ndsw between builds.
if [ -f "$BINARIES_DIR/sdcard.img" ]; then
    say "sdcard.img exists, leaving it alone"
else
    if MKFSVFAT="$(host_tool mkfs.vfat)"; then
        rm -f "$BINARIES_DIR/sdcard.img"
        dd if=/dev/zero of="$BINARIES_DIR/sdcard.img" bs=1M \
            count="$SDCARD_MB" status=none
        "$MKFSVFAT" -F 32 -n NEODCT "$BINARIES_DIR/sdcard.img" > /dev/null
        if MMD="$(host_tool mmd)"; then
            for folder in wallpapers tones backup_db music update; do
                MTOOLS_SKIP_CHECK=1 "$MMD" -i "$BINARIES_DIR/sdcard.img" \
                    "::/$folder" 2>/dev/null || true
            done
            say "sdcard.img (${SDCARD_MB}M FAT32 NEODCT, folders created)"
        else
            say "sdcard.img (${SDCARD_MB}M FAT32 NEODCT, no mtools: the phone"
            say "  will offer to set the folders up on first insert)"
        fi
    else
        say "mkfs.vfat not found; skipping sdcard.img"
    fi
fi

say "image set ready in $BINARIES_DIR"

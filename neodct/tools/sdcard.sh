#!/bin/sh
# Put files on the emulated SD card image without root or a loop mount.
#
#   neodct/tools/sdcard.sh ls                     list the card
#   neodct/tools/sdcard.sh ls update              list one folder
#   neodct/tools/sdcard.sh put UPDATE.ndsw        -> /update/UPDATE.ndsw
#   neodct/tools/sdcard.sh put song.mp3 music     -> /music/song.mp3
#   neodct/tools/sdcard.sh rm update/UPDATE.ndsw  delete one file
#   neodct/tools/sdcard.sh init                   create the NeoDCT folders
#   neodct/tools/sdcard.sh new 128                make a fresh blank card
#
# Do this while QEMU is NOT running: both writing to the image would corrupt
# the filesystem. (For live drag-and-drop, boot with NEODCT_SD=share instead
# and use the host folder.)
#
# ============ debugfs, BECAUSE THE CARD IS EXT4 NOW ============
#
# This used mtools, which edits a FAT filesystem as a data structure and so
# needed neither root nor a loop mount. A NeoDCT card is one ext4 partition
# since 0.5.0b -- nd_paths.h says why -- and debugfs is the same trick for ext:
# it opens the filesystem as a file and edits its inodes directly.
#
# Both properties matter here. A loop mount needs root, which a build should
# not need, and on a container without loop-partition minors it does not work
# at all. e2fsprogs is already a host dependency (BR2_PACKAGE_HOST_E2FSPROGS),
# and post-image-neodct.sh writes the card's ownership the same way.
#
# ============ AND WHY OWNERSHIP IS SET ON EVERY WRITE ============
#
# ext4 records an owner per file, and a file this tool creates would otherwise
# belong to whoever ran it -- which on a build host is not ndusr. The phone
# restates directory ownership on every mount (neodct-sdcard's apply_layout),
# but it deliberately does NOT touch file contents, so a file dropped here with
# the wrong owner stays wrong. The uids come from the users table rather than
# `id -u`, because the build host's ndusr is not the phone's.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
BUILDROOT="${NEODCT_BUILDROOT:-$(dirname "$REPO")/buildroot}"
IMAGES="${NEODCT_IMAGES:-$BUILDROOT/output/images}"
HOST_DIR="${NEODCT_HOST_DIR:-$BUILDROOT/output/host}"
CARD="${NEODCT_SDCARD:-$IMAGES/sdcard.img}"
USERS_TABLE="${NEODCT_USERS_TABLE:-$REPO/configs/users-table.txt}"

FOLDERS="wallpapers tones backup_db music update apps untrusted"

host_tool() {
    for candidate in "$HOST_DIR/bin/$1" "$HOST_DIR/sbin/$1"; do
        [ -x "$candidate" ] && { echo "$candidate"; return 0; }
    done
    command -v "$1" 2>/dev/null && return 0
    echo "sdcard: $1 not found. Build the target once so buildroot provides" >&2
    echo "  host-e2fsprogs, or install e2fsprogs on the host." >&2
    return 1
}

# The uid/gid the PHONE uses, read from the table that creates them. Field 2 is
# the uid and field 4 the gid; a missing table is not fatal, because the phone
# fixes directory ownership on the next mount anyway.
table_id() {   # table_id USER FIELD
    [ -r "$USERS_TABLE" ] || return 1
    awk -v u="$1" -v f="$2" '$1 == u { print $f; exit }' "$USERS_TABLE"
}
ND_UID="$(table_id ndusr 2 || true)"; ND_UID="${ND_UID:-1000}"
ND_GID="$(table_id ndusr 4 || true)"; ND_GID="${ND_GID:-1000}"
UT_GID="$(table_id ndusr_ut 4 || true)"; UT_GID="${UT_GID:-1001}"

# ============ WHERE THE FILESYSTEM STARTS ============
#
# A card the build or the phone made carries a partition table, so the ext4
# superblock is a megabyte in; a card somebody made with mkfs straight onto the
# device has none. debugfs takes "image?offset=N" for the first case, and
# guessing wrong opens garbage -- so read the table rather than assuming.
#
# Byte 450 is partition one's type. 0x83 is Linux, which is what write_mbr
# writes; anything else (or no 0x55AA signature) is treated as a whole-device
# filesystem.
card_spec() {
    _off="$(python3 - "$CARD" <<'PY' 2>/dev/null || echo 0
import struct, sys
d = open(sys.argv[1], "rb").read(512)
if len(d) == 512 and d[510:512] == b"\x55\xaa" and d[450] == 0x83:
    print(struct.unpack("<I", d[454:458])[0] * 512)
else:
    print(0)
PY
)"
    if [ "${_off:-0}" -gt 0 ]; then
        echo "$CARD?offset=$_off"
    else
        echo "$CARD"
    fi
}

# debugfs exits 0 even when every command in the script failed -- it prints
# "File not found" and carries on -- so the caller checks the output rather
# than the status. See post-image-neodct.sh, which learned the same thing.
run_debugfs() {   # run_debugfs WRITABLE < commands
    _dbg="$(host_tool debugfs)"
    if [ "$1" = "rw" ]; then
        "$_dbg" -w -f - "$(card_spec)" 2>&1 | grep -v '^debugfs [0-9]' || true
    else
        "$_dbg" -f - "$(card_spec)" 2>&1 | grep -v '^debugfs [0-9]' || true
    fi
}

# ============ AND IT HAS TO REFUSE A CARD IT CANNOT TOUCH ============
#
# debugfs edits ext filesystems and nothing else. Handed a FAT card it prints
# its complaints, exits 0 anyway -- which is what debugfs does -- and this
# script cheerfully reported
#
#     sdcard: copied UPDATE.ndsw to /update/ (ndusr, 0640)
#
# having copied nothing at all. The phone then said the package was "not on
# this card", and the lie was three steps upstream of the symptom. It cost a
# whole end-to-end update test to find, on a card that was FAT only because a
# fixture had been left behind.
#
# So: read the ext superblock magic before doing anything. It lives at offset
# 0x38 of the superblock, which starts 1024 bytes into the filesystem.
card_is_ext() {
    _spec="$(card_spec)"
    _off="${_spec##*?offset=}"
    [ "$_off" = "$_spec" ] && _off=0
    python3 - "$CARD" "$_off" <<'PY' 2>/dev/null
import sys
try:
    with open(sys.argv[1], "rb") as f:
        f.seek(int(sys.argv[2]) + 1024 + 0x38)
        sys.exit(0 if f.read(2) == b"\x53\xef" else 1)
except Exception:
    sys.exit(1)
PY
}

require_ext() {
    card_is_ext && return 0
    echo "sdcard: $CARD is not an ext filesystem." >&2
    echo "  A NeoDCT card is ext4 since 0.5.0b -- FAT cannot record who owns a" >&2
    echo "  file, which is what the confinement is written in. This tool edits" >&2
    echo "  ext with debugfs and can do nothing at all to a FAT card." >&2
    echo "  Make one with: $0 new 128   (this ERASES the image)" >&2
    exit 1
}

usage() {
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

[ $# -ge 1 ] || usage
ACTION="$1"
shift

if [ "$ACTION" != "new" ] && [ ! -f "$CARD" ]; then
    echo "sdcard: $CARD does not exist. Build the images, or make a blank" >&2
    echo "  card with: $0 new 128" >&2
    exit 1
fi

[ "$ACTION" = "new" ] || require_ext

case "$ACTION" in
    ls)
        printf 'ls -l /%s\n' "${1:-}" | run_debugfs ro
        ;;

    put)
        [ $# -ge 1 ] || usage
        SOURCE="$1"
        DEST_DIR="${2:-update}"
        [ -f "$SOURCE" ] || { echo "sdcard: no such file: $SOURCE" >&2; exit 1; }
        BASE="$(basename "$SOURCE")"
        # mkdir first in case the folder is absent, then write, then set the
        # owner -- debugfs's `write` makes the file owned by uid 0.
        {
            printf 'mkdir /%s\n' "$DEST_DIR"
            printf 'rm /%s/%s\n' "$DEST_DIR" "$BASE"
            printf 'write %s /%s/%s\n' "$SOURCE" "$DEST_DIR" "$BASE"
            printf 'sif /%s/%s uid %s\n' "$DEST_DIR" "$BASE" "$ND_UID"
            printf 'sif /%s/%s gid %s\n' "$DEST_DIR" "$BASE" "$ND_GID"
            printf 'sif /%s/%s mode 0100640\n' "$DEST_DIR" "$BASE"
        } | run_debugfs rw > /dev/null
        echo "sdcard: copied $BASE to /$DEST_DIR/ (ndusr, 0640)"
        ;;

    rm)
        [ $# -ge 1 ] || usage
        printf 'rm /%s\n' "$1" | run_debugfs rw > /dev/null
        echo "sdcard: deleted /$1"
        ;;

    init)
        for folder in $FOLDERS; do
            printf 'mkdir /%s\n' "$folder"
        done | run_debugfs rw > /dev/null
        # The layout, matching neodct-sdcard's CARD_LAYOUT. The phone restates
        # this on every mount; setting it here means a card is right before the
        # phone ever sees it, and a wrong one is visible at build time.
        {
            printf 'sif / uid %s\nsif / gid %s\nsif / mode 040751\n' "$ND_UID" "$ND_GID"
            printf 'sif /apps uid %s\nsif /apps gid %s\nsif /apps mode 040755\n' "$ND_UID" "$ND_GID"
            printf 'sif /untrusted uid %s\nsif /untrusted gid %s\nsif /untrusted mode 040770\n' \
                "$ND_UID" "$UT_GID"
            for folder in wallpapers tones backup_db music update; do
                printf 'sif /%s uid %s\nsif /%s gid %s\nsif /%s mode 040750\n' \
                    "$folder" "$ND_UID" "$folder" "$ND_GID" "$folder"
            done
            # The marker card_is_ours() looks for. Without it the phone falls
            # back to recognising the folder set, which works -- but a card the
            # build made should say so outright.
            printf 'mkdir /lost+found\n'
        } | run_debugfs rw > /dev/null
        echo "sdcard: NeoDCT folders and layout present on $CARD"
        ;;

    new)
        SIZE_MB="${1:-128}"
        MKE2FS="$(host_tool mke2fs)"
        dd if=/dev/zero of="$CARD" bs=1M count="$SIZE_MB" status=none
        # Same options the phone's own mkfs_ext4() uses, so a card made here
        # and a card made by Settings cannot drift. No partition table: a
        # whole-device filesystem mounts identically, and neodct-sdcard's
        # candidates() offers whole disks as well as partitions.
        "$MKE2FS" -t ext4 -F -m0 -L NEODCT -O ^64bit,^metadata_csum \
            "$CARD" > /dev/null 2>&1
        "$0" init > /dev/null
        echo "sdcard: new ${SIZE_MB}M ext4 card at $CARD (label NEODCT)"
        ;;

    *)
        usage
        ;;
esac

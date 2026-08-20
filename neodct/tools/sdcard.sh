#!/bin/sh
# Put files on the emulated SD card image without root or a loop mount.
#
# Uses the mtools buildroot builds as a host tool, so it works on the FAT32
# sdcard.img directly:
#
#   neodct/tools/sdcard.sh ls                     list the card
#   neodct/tools/sdcard.sh ls update              list one folder
#   neodct/tools/sdcard.sh put UPDATE.ndsw        -> ::/update/UPDATE.ndsw
#   neodct/tools/sdcard.sh put song.mp3 music     -> ::/music/song.mp3
#   neodct/tools/sdcard.sh rm update/UPDATE.ndsw  delete one file
#   neodct/tools/sdcard.sh init                   create the NeoDCT folders
#   neodct/tools/sdcard.sh new 128                make a fresh blank card
#
# Do this while QEMU is NOT running: both writing to the image would corrupt
# the filesystem. (For live drag-and-drop, boot with NEODCT_SD=share instead
# and use the host folder.)
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
BUILDROOT="${NEODCT_BUILDROOT:-$(dirname "$REPO")/buildroot}"
IMAGES="${NEODCT_IMAGES:-$BUILDROOT/output/images}"
HOST_DIR="${NEODCT_HOST_DIR:-$BUILDROOT/output/host}"
CARD="${NEODCT_SDCARD:-$IMAGES/sdcard.img}"

FOLDERS="wallpapers tones backup_db music update"

# mtools refuses to touch an image whose geometry it cannot make sense of.
export MTOOLS_SKIP_CHECK=1

host_tool() {
    for candidate in "$HOST_DIR/bin/$1" "$HOST_DIR/sbin/$1"; do
        [ -x "$candidate" ] && { echo "$candidate"; return 0; }
    done
    command -v "$1" 2>/dev/null && return 0
    echo "sdcard: $1 not found. Build the target once so buildroot provides" >&2
    echo "  host-mtools/host-dosfstools, or install mtools on the host." >&2
    return 1
}

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
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

case "$ACTION" in
    ls)
        MDIR="$(host_tool mdir)"
        "$MDIR" -i "$CARD" -/ "::/${1:-}"
        ;;

    put)
        [ $# -ge 1 ] || usage
        SOURCE="$1"
        DEST_DIR="${2:-update}"
        [ -f "$SOURCE" ] || { echo "sdcard: no such file: $SOURCE" >&2; exit 1; }
        MCOPY="$(host_tool mcopy)"
        MMD="$(host_tool mmd)"
        # -s so the folder is created if the card has not been set up yet.
        "$MMD" -i "$CARD" "::/$DEST_DIR" 2>/dev/null || true
        "$MCOPY" -i "$CARD" -o "$SOURCE" "::/$DEST_DIR/$(basename "$SOURCE")"
        echo "sdcard: copied $(basename "$SOURCE") to ::/$DEST_DIR/"
        ;;

    rm)
        [ $# -ge 1 ] || usage
        MDEL="$(host_tool mdel)"
        "$MDEL" -i "$CARD" "::/$1"
        echo "sdcard: deleted ::/$1"
        ;;

    init)
        MMD="$(host_tool mmd)"
        for folder in $FOLDERS; do
            "$MMD" -i "$CARD" "::/$folder" 2>/dev/null || true
        done
        echo "sdcard: NeoDCT folders present on $CARD"
        ;;

    new)
        SIZE_MB="${1:-128}"
        MKFS="$(host_tool mkfs.vfat)"
        MMD="$(host_tool mmd)"
        dd if=/dev/zero of="$CARD" bs=1M count="$SIZE_MB" status=none
        "$MKFS" -F 32 -n NEODCT "$CARD" > /dev/null
        for folder in $FOLDERS; do
            "$MMD" -i "$CARD" "::/$folder" 2>/dev/null || true
        done
        echo "sdcard: new ${SIZE_MB}M FAT32 card at $CARD (label NEODCT)"
        ;;

    *)
        usage
        ;;
esac

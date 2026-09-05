#!/bin/sh
# Put files on the emulated SD card image without root or a loop mount.
#
#   neodct/tools/sdcard.sh ls                     list the card
#   neodct/tools/sdcard.sh ls update              list one folder
#   neodct/tools/sdcard.sh put UPDATE.ndsw        -> /update/UPDATE.ndsw
#   neodct/tools/sdcard.sh put song.mp3 music     -> /music/song.mp3
#   neodct/tools/sdcard.sh put dist/qemu/PSX      -> /apps/PSX/, the whole tree
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

# ============ NEVER mkdir WHAT IS ALREADY THERE ============
#
# debugfs's mkdir of a name that exists says "Ext2 directory already exists"
# -- AFTER it has written a fresh directory inode and its block to the disk,
# and before it marks either of them allocated in the bitmaps. Nothing links
# the inode, the bitmaps say its slot is free, and the next allocation
# happens to overwrite it, which is why a run that mkdirs and then writes
# looks clean. A run that ENDS on such a mkdir leaves it there, and e2fsck
# reports an unconnected directory and two bitmap differences. `init` did
# exactly this on every card it ever touched -- its last line was `mkdir
# /lost+found`, which mke2fs had already made -- and the first `put` of a
# tree with a data/ directory in it did it once more.
#
# So the card is read before it is written: `ls -p` prints one
# /inode/mode/uid/gid/name/size/ line per entry, which unlike `ls -l` survives
# a name with a space in it. card_index() lists every directory it is given
# in one read-only run and prints DIR/NAME for each entry, and the writers
# consult that rather than trying and reading the complaint.
card_index() {   # stdin: one DIR per line -> "DIR/NAME" per entry, . and .. left out
    while IFS= read -r _d; do printf 'ls -p "%s"\n' "$_d"; done | run_debugfs ro | awk -F/ '
        /^debugfs: ls -p "/ {
            d = $0; sub(/^debugfs: ls -p "/, "", d); sub(/"$/, "", d)
            if (d == "/") d = ""
            next
        }
        /^\// && $6 != "." && $6 != ".." { print d "/" $6 }'
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

# ============ A DIRECTORY IS AN APP, AND IT GOES IN WHOLE ============
#
# `put dist/qemu/PSX` copies the tree to /apps/PSX, which is how an app the
# owner installs reaches the phone: the core scans /NeoDCT/User/sdcard/apps
# for it, and everything it needs -- app.so, its icon, and in that case a
# libretro core and a gigabyte of disc images -- lives inside that one
# directory, because it is the only directory a confined app can see.
#
# debugfs has no recursive write, so this is a walk: a mkdir or a write plus
# three sifs per entry, in one debugfs run. The modes are the ones
# apply_layout() gives an app on the phone -- 0755 directories and 0644 files,
# ndusr:ndusr, so the ndusr_ut process that dlopen()s app.so can read it and
# nothing in the app can rewrite its own code -- and data/ is 0770
# ndusr:ndusr_ut, the one part of an app that it may write. The phone restates
# the top level of that on every mount but deliberately never descends, so a
# games/ directory three levels down is only ever as right as it was written
# here. A tree put anywhere else gets the media folders' 0750/0640.
#
# `mkdir` takes a NAME exactly as `write` does (see put below): `mkdir /apps`
# from inside /apps/PSX creates /apps/PSX/apps and says nothing. So every
# entry is preceded by a `cd` to its parent, and only `sif` is handed a path.
#
# A tree already on the card is updated in place: each file is replaced and
# nothing absent from the source is removed. That is what keeps an app's
# saves in data/ across a reinstall, and it is also why a game deleted from
# the source stays on the card until `rm` takes it. Which directories and
# files are already there comes from card_index(), for the reason given
# above it: a directory must not be made twice, and a file that is not there
# need not be removed first.
put_tree() {   # put_tree SOURCE-DIR DEST-DIR
    _src="$1"; _dest="$2"
    _base="$(basename "$(cd "$_src" && pwd)")"
    _top="/$_dest/$_base"
    _out="${TMPDIR:-/tmp}/.neodct-sdcard.$$"

    # The names go to debugfs inside double quotes, which is what lets
    # "Crash Bandicoot.bin" through; a quote or a backslash in a name would
    # end the quoting early and there is no escaping to reach for, and a
    # newline would split the walk's own lines.
    _nl="$(printf '\nx')"; _nl="${_nl%x}"
    if find "$_src" \( -name '*"*' -o -name '*\\*' -o -name "*${_nl}*" \) |
            grep -q .; then
        echo "sdcard: a name under $_src contains a quote, a backslash or a" >&2
        echo "  newline, which debugfs cannot be told about. Rename it first." >&2
        exit 1
    fi
    if find "$_src" ! -type d ! -type f | grep -q .; then
        echo "sdcard: $_src holds something that is neither a file nor a" >&2
        echo "  directory (a symlink?); only those go on the card." >&2
        exit 1
    fi

    if [ "$_dest" = apps ]; then
        _dmode=040755; _fmode=0100644
        if [ ! -f "$_src/manifest.json" ] || [ ! -f "$_src/app.so" ]; then
            echo "sdcard: note: $_base has no manifest.json or no app.so, so" >&2
            echo "  the phone will not list it as an app." >&2
        fi
    else
        _dmode=040750; _fmode=0100640
    fi

    # Enough room, before a single block is written. "Free blocks" times
    # "Block size" is what the superblock says; the extent trees and the
    # directories cost blocks the file sizes do not show, so the check keeps a
    # margin rather than counting exactly. A card that is short is a card to
    # remake bigger, not one to fill to the last block and find out.
    _files="$(find "$_src" -type f | wc -l)"
    _bytes="$(find "$_src" -type f -printf '%s\n' |
        awk '{ s += $1 } END { printf "%.0f\n", s }')"
    _free="$(printf 'stats\n' | run_debugfs ro | awk -F': *' \
        '/^Free blocks:/ { f = $2 } /^Block size:/ { b = $2 }
         END { printf "%.0f\n", f * b }')"
    _need=$(( _bytes + _bytes / 20 + _files * 4096 ))
    if [ "$_need" -gt "${_free:-0}" ]; then
        echo "sdcard: $_base needs $(( _need / 1048576 )) MB and $CARD has" \
            "$(( ${_free:-0} / 1048576 )) MB free." >&2
        echo "  Make a bigger card with: $0 new SIZE_MB   (this ERASES the image)" >&2
        exit 1
    fi

    # What the card already holds under every directory the walk will touch.
    {
        printf '/\n/%s\n%s\n' "$_dest" "$_top"
        (cd "$_src" && find . -mindepth 1 -type d -printf '%P\n') |
        while IFS= read -r _d; do printf '%s/%s\n' "$_top" "$_d"; done
    } | card_index > "$_out.index"
    on_card() { grep -Fqx -- "$1" "$_out.index"; }

    {
        on_card "/$_dest" || printf 'cd /\nmkdir "%s"\n' "$_dest"
        on_card "$_top"   || printf 'cd "/%s"\nmkdir "%s"\n' "$_dest" "$_base"
        printf 'sif "%s" uid %s\nsif "%s" gid %s\nsif "%s" mode %s\n' \
            "$_top" "$ND_UID" "$_top" "$ND_GID" "$_top" "$_dmode"
        # find lists a directory before its contents, which is the order a
        # mkdir per line needs. %P is the path relative to the start.
        (cd "$_src" && find . -mindepth 1 \( -type d -o -type f \) -printf '%y %P\n') |
        while IFS= read -r _entry; do
            _kind="${_entry%% *}"; _rel="${_entry#* }"
            case "$_rel" in
                */*) _parent="$_top/${_rel%/*}"; _name="${_rel##*/}" ;;
                *)   _parent="$_top";            _name="$_rel" ;;
            esac
            _path="$_top/$_rel"
            printf 'cd "%s"\n' "$_parent"
            if [ "$_kind" = d ]; then
                on_card "$_path" || printf 'mkdir "%s"\n' "$_name"
                printf 'sif "%s" mode %s\n' "$_path" "$_dmode"
            else
                on_card "$_path" && printf 'rm "%s"\n' "$_name"
                printf 'write "%s" "%s"\nsif "%s" mode %s\n' \
                    "$_src/$_rel" "$_name" "$_path" "$_fmode"
            fi
            printf 'sif "%s" uid %s\nsif "%s" gid %s\n' \
                "$_path" "$ND_UID" "$_path" "$ND_GID"
        done
        if [ "$_dest" = apps ]; then
            # Made here for the reason apply_layout() gives: an app that could
            # create its own data directory could create siblings beside its
            # code instead. The phone would make it on the first mount anyway.
            on_card "$_top/data" || [ -d "$_src/data" ] ||
                printf 'cd "%s"\nmkdir data\n' "$_top"
            printf 'sif "%s/data" mode 040770\nsif "%s/data" uid %s\nsif "%s/data" gid %s\n' \
                "$_top" "$_top" "$ND_UID" "$_top" "$UT_GID"
        fi
    } > "$_out.debugfs"
    run_debugfs rw < "$_out.debugfs" > "$_out" 2>&1
    rm -f "$_out.index"

    # One "Allocated inode" per file, or it did not happen: debugfs exits 0
    # whatever it did. With the echo of each command taken out, what is left
    # is the actual complaint.
    _got="$(grep -c 'Allocated inode' "$_out" || true)"
    if [ "$_got" -ne "$_files" ]; then
        echo "sdcard: FAILED: wrote $_got of $_files files under $_top -- debugfs said:" >&2
        grep -v '^debugfs: \|^Allocated inode' "$_out" | sed 's/^/  /' >&2
        rm -f "$_out" "$_out.debugfs"
        exit 1
    fi
    rm -f "$_out" "$_out.debugfs"
    _extra=""
    [ "$_dest" = apps ] && _extra=", data/ 0770 ndusr:ndusr_ut"
    echo "sdcard: installed $_base to /$_dest/ ($_files files," \
        "$(( _bytes / 1048576 )) MB; ndusr, ${_dmode#04}/${_fmode#010}$_extra)"
}

usage() {
    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
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
        printf 'ls -l "/%s"\n' "${1:-}" | run_debugfs ro
        ;;

    put)
        [ $# -ge 1 ] || usage
        SOURCE="${1%/}"
        if [ -d "$SOURCE" ]; then
            put_tree "$SOURCE" "${2:-apps}"
            exit 0
        fi
        TMPOUT="${TMPDIR:-/tmp}/.neodct-sdcard.$$"
        DEST_DIR="${2:-update}"
        [ -f "$SOURCE" ] || { echo "sdcard: no such file: $SOURCE" >&2; exit 1; }
        BASE="$(basename "$SOURCE")"
        # mkdir first in case the folder is absent, then write, then set the
        # owner -- debugfs's `write` makes the file owned by uid 0.
        #
        # `cd` FIRST, and it is not decoration. debugfs's `write` takes a
        # destination NAME, not a path: `write src /update/x.ndsw` does not
        # write into /update, it creates a file in the CURRENT directory --
        # which is the root of the card -- and reports success either way.
        # The package then sat at the top of the card while the phone looked
        # for it in update/ and said it was not there.
        {
            printf 'mkdir /%s\n' "$DEST_DIR"
            printf 'cd /%s\n' "$DEST_DIR"
            printf 'rm %s\n' "$BASE"
            printf 'write %s %s\n' "$SOURCE" "$BASE"
            printf 'sif %s uid %s\n' "$BASE" "$ND_UID"
            printf 'sif %s gid %s\n' "$BASE" "$ND_GID"
            printf 'sif %s mode 0100640\n' "$BASE"
        } | run_debugfs rw > "$TMPOUT" 2>&1
        # debugfs exits 0 whatever happened, so the only way to know is to
        # look. A `put` that did not put is the bug this whole file just
        # spent an end-to-end update test finding.
        if ! grep -q "Allocated inode" "$TMPOUT"; then
            echo "sdcard: FAILED to copy $BASE -- debugfs said:" >&2
            sed 's/^/  /' "$TMPOUT" >&2
            rm -f "$TMPOUT"
            exit 1
        fi
        rm -f "$TMPOUT"
        echo "sdcard: copied $BASE to /$DEST_DIR/ (ndusr, 0640)"
        ;;

    rm)
        [ $# -ge 1 ] || usage
        printf 'rm "/%s"\n' "$1" | run_debugfs rw > /dev/null
        echo "sdcard: deleted /$1"
        ;;

    init)
        # Only the folders that are missing: see card_index() for what a
        # second mkdir of the same name leaves behind.
        HAVE="$(printf '/\n' | card_index)"
        for folder in $FOLDERS; do
            printf '%s\n' "$HAVE" | grep -Fqx -- "/$folder" ||
                printf 'mkdir /%s\n' "$folder"
        done | run_debugfs rw > /dev/null
        MARKER="${TMPDIR:-/tmp}/.neodct-sdcard-marker.$$"
        : > "$MARKER"
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
            # build made should say so outright. (This line was `mkdir
            # /lost+found`, a directory mke2fs had already made, so the marker
            # this comment promised was never written and nobody noticed,
            # because the fallback works.)
            printf 'cd /\n'
            printf '%s\n' "$HAVE" | grep -Fqx -- "/.neodct" && printf 'rm .neodct\n'
            printf 'write "%s" .neodct\n' "$MARKER"
            printf 'sif .neodct uid %s\nsif .neodct gid %s\nsif .neodct mode 0100644\n' \
                "$ND_UID" "$ND_GID"
        } | run_debugfs rw > /dev/null
        rm -f "$MARKER"
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

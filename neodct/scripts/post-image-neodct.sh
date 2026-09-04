#!/bin/sh
# Assemble the bootable NeoDCT image set from a finished buildroot tree.
#
# Produces, in $BINARIES_DIR:
#   system.img            padded squashfs + appended dm-verity hash tree
#   system.manifest.json  its version/verity parameters
#   initramfs.cpio.gz     the boot-time update applier
#   userdata.ext4         empty user partition, labelled NDUSER, carrying the
#                         installed.prop the initramfs needs on first boot
#   sdcard.img            a ready-made NeoDCT card -- one ext4 partition
#                         labelled NEODCT, laid out the way the phone lays
#                         one out (only if absent, so a card you have put
#                         files on is never clobbered)
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
#
# No apps/ among them any more. An app the owner installed lives at
# /NeoDCT/User/sdcard/apps -- on the CARD -- and the reasoning is in
# nd_paths.h: on the Luckfox this partition is eight megabytes shared with the
# databases, the settings, the logs and the browser profile, so an app
# directory here is a feature that fills the partition the phone needs in
# order to save anything at all. sdcard/ below is the mount point, and stays.
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
# The emulator's card, and it is built the way the PHONE builds one: ONE ext4
# partition, per SECURITY-PLAN.md section 1 as it stands now that the card has
# stopped being FAT.
#
# It was two FAT32 filesystems, media and arrival, and the split existed
# because FAT HAS no ownership: uid=, gid=, fmask= and dmask= are mount
# options and apply to a whole filesystem at once, so "the browser may write
# its downloads and may not read the owner's music" could only be said by
# mounting two filesystems. ext4 records owner, group and mode per inode, so
# one filesystem says both -- and the second partition, its NEODCTUT label and
# every line of code that told the two apart go away with it. It also makes
# apps on a card possible at all: an app.so on the old media side was 0640
# ndusr:ndusr, which is a file the ndusr_ut process that has to dlopen() it
# cannot read.
#
# The partition table still comes from the phone's own helper rather than from
# a copy of its arithmetic -- `neodct-sdcard` is sourced for write_mbr(),
# align_down() and ALIGN_SECTORS -- so a card made here and a card made by
# Settings cannot drift apart. That matters more than it sounds: QEMU is the
# only place this path is exercised before it reaches hardware. Note that
# write_mbr() now takes THREE arguments and stamps type 0x83; handing it the
# old five would write a table whose second entry describes a partition that
# does not exist.
#
# partition_plan() is gone from the helper, because one partition starting at
# the alignment and running to the end of the card is three lines rather than
# a function. Those three lines are below, and they run inside the sourced
# helper so that the alignment they use is the helper's own.
#
# The filesystem is made in a separate file and dd'd into place rather than
# written at an offset, the same way the two FAT ones were: mke2fs has no
# --offset at all, and writing at an offset is one dd either way.

# ============ THE OWNERSHIP GOES IN WITHOUT root, OR IT DOES NOT GO IN ====
#
# The card's confinement IS which uid owns which directory -- CARD_LAYOUT in
# the helper -- and none of the obvious ways of getting that into an image
# file are available to a build that must not need root:
#
#   chown on the staging tree   needs root, and a build step that needs root
#                               is a build step that is run differently by
#                               everyone who runs it
#   mke2fs -d on its own        copies the ids straight off the staging tree,
#                               which belongs to whoever typed `make`.
#                               -E root_owner= corrects exactly one inode
#   loop-mounting the image     needs root as well, and a kernel that has
#                               loop partitions -- neither of which a builder
#                               inside a container has
#
# So: `mke2fs -d` populates the tree, and then debugfs writes the numbers
# straight into the inodes with `sif`. debugfs edits an ext filesystem as a
# data structure rather than as a mount, from an unprivileged process, which
# is the same property that lets installed.prop be refreshed in a kept
# userdata above.
#
# ============ AND THE NUMBERS COME FROM users-table.txt ============
#
# NOT from `id -u ndusr`. Those two names exist ON THE PHONE. On a build
# machine they are usually absent and occasionally -- which is worse, because
# it is silent -- somebody else's account that happens to share the name. An
# ext4 image stores NUMERIC ids, so a card built against the host's idea of
# "ndusr" is a card whose apps/ belongs to a uid the phone has never issued.
# neodct/configs/users-table.txt is what buildroot feeds mkusers, so it is the
# one place that says what the numbers are, and it says 1000 and 1001.
#
# None of this is load-bearing on its own, and that is deliberate: a host with
# no debugfs gets a warning and not a failed build, because apply_layout() in
# the helper restates every mode and owner on EVERY mount, by design,
# precisely because a card can be written on any PC. What the pass here buys
# is an image that is already right BEFORE the phone has ever mounted it --
# which is what anything reading the card without booting it gets, and what
# makes a wrong layout visible at build time instead of at run time.

# One field of one row of the users table. Field 2 is the uid, field 4 the
# gid; a name that is not there yields the empty string, and the callers treat
# that as "leave the ids alone" rather than guessing a number.
phone_id() {   # phone_id NAME FIELD
    awk -v name="$1" -v field="$2" '$1 == name { print $field; exit }' \
        "$NEODCT_DIR/configs/users-table.txt" 2>/dev/null
}

CARD_UID="$(phone_id ndusr 2)"
CARD_GID="$(phone_id ndusr 4)"
CARD_UT_GID="$(phone_id ndusr_ut 4)"

# The NeoDCT folders, made in the staging tree that becomes the filesystem.
#
# They were mmd'd into a FAT image, because a FAT image cannot be populated
# any other way without mounting it. ext4 needs no such trick: `mke2fs -d`
# takes a directory tree, so the folders are ordinary mkdirs on the host.
#
# The list is written out here rather than read from the helper's FOLDERS on
# purpose -- this function also serves the path taken when the helper cannot
# be read at all, and a list sourced from a file that is missing is not a
# list. It is the same seven names, and unlike the layout below it does not
# have to stay in step to be safe: after_mount() mkdirs every one of FOLDERS
# on every mount of a NeoDCT card, so a name that falls behind here costs one
# directory that appears on the phone's first look at the card.
sdcard_folders() {   # sdcard_folders STAGING-DIR
    for folder in wallpapers tones backup_db music update apps untrusted; do
        mkdir -p "$1/$folder"
    done
}

# ---- the confinement test apps, and why they are NOT in a release ----
#
# neodct/tests/pentest-apps holds a set of apps that deliberately try things an
# app is not allowed to do: send a message as the owner, write to the root of
# the user partition, install an unsigned update, reboot the phone, and leave
# something behind that runs on the next boot. They exist to prove the
# confinement empirically rather than by argument, and each one REPORTS what
# stopped it rather than doing anything harmful.
#
# They ship only when NEODCT_PENTEST=1 is set for the build. A release image
# has an empty apps/ on its card, because an image that ships apps probing its
# own boundaries is an image whose boundaries have more to probe.
#
# They are staged onto the CARD and no longer onto the user partition: an
# installed app lives at /NeoDCT/User/sdcard/apps now, which is this image.
# One consequence to know before going to look for them -- an existing
# sdcard.img is never overwritten (below), so a card left over from an earlier
# build does not grow the probe apps when the flag is turned on. Delete
# sdcard.img and build again.
stage_pentest_apps() {   # stage_pentest_apps STAGING-APPS-DIR
    [ "${NEODCT_PENTEST:-0}" = "1" ] || return 0
    [ -d "$NEODCT_DIR/tests/pentest-apps" ] || return 0

    say "NEODCT_PENTEST=1: staging the confinement test apps into the card's apps/"
    cp -a "$NEODCT_DIR/tests/pentest-apps/." "$1/"
    # The .so is built by the neodct package; the staged tree carries only
    # the manifest and the icon, so pick each one out of the target.
    for d in "$1"/*/; do
        name=$(basename "$d")
        so="$TARGET_DIR/NeoDCT/System/apps/$name/app.so"
        [ -f "$so" ] && cp -a "$so" "$d/app.so"
        # The app's own storage, made HERE rather than by the app, for the
        # reason apply_layout() gives: an app that can create its own data
        # directory can create siblings beside its code instead. The phone
        # would make it on the first mount anyway; making it now is what keeps
        # the image and a mounted card the same shape.
        mkdir -p "$d/data"
    done
}

# Where the one partition goes, in the helper's own arithmetic: it starts at
# the alignment -- there has to be room for the table -- and runs to the last
# aligned sector of the card. Empty output means the card is too small to hold
# a partition at all, which is not an error here, only a card that gets a
# filesystem and no table.
sdcard_plan() {   # sdcard_plan HELPER IMAGE
    # SOURCE_ONLY stops the helper dispatching on "$1"; it only defines
    # functions. Everything else it needs from the environment it defaults.
    # The paths go in as positional parameters rather than interpolated into
    # the script text, so a path with a space in it stays one word.
    NEODCT_SDCARD_SOURCE_ONLY=1 sh -c '
        . "$1" || exit 1
        total="$(device_sectors "$2")" || exit 1
        start="$ALIGN_SECTORS"
        sectors="$(align_down $(( total - start )))"
        [ "$sectors" -gt 0 ] || exit 1
        echo "$start $sectors"
    ' sh "$1" "$2" 2>/dev/null
}

# The card's layout table, straight out of the helper, so that the modes this
# script writes and the modes the phone restates are one list in one file.
sdcard_layout_table() {   # sdcard_layout_table HELPER
    NEODCT_SDCARD_SOURCE_ONLY=1 sh -c \
        '. "$1" || exit 1; printf "%s\n" "$CARD_LAYOUT"' sh "$1" 2>/dev/null
}

# One ext4 filesystem, with the phone's own mkfs options.
mkfs_card() {   # mkfs_card MKE2FS FILE STAGING [BLOCKS]
    # -m0, -L NEODCT and -O ^64bit,^metadata_csum are mkfs_ext4()'s options in
    # the helper, verbatim: a card this script makes and a card Settings makes
    # have to be the same filesystem, and dropping those two features is what
    # keeps it readable by an older e2fsprogs on the owner's PC -- most of the
    # reason for picking a filesystem Linux reads everywhere.
    #
    # -b 4096 is the builder's own, and it is not optional here. The block
    # count below is in BLOCKS, and without -b mke2fs picks the size from
    # mke2fs.conf's size classes -- 1024 for a card this small -- which would
    # quietly make a filesystem a quarter of the partition it has to fill.
    #
    # root_owner= corrects the one inode mke2fs invents rather than copies. It
    # is redundant when the debugfs pass below runs, and it is the only thing
    # that gets the root of the card right when it does not.
    "$1" -q -F -t ext4 -b 4096 -m0 -L NEODCT -O ^64bit,^metadata_csum \
        -d "$3" -E "root_owner=${CARD_UID:-0}:${CARD_GID:-0}" \
        "$2" ${4:+"$4"} > /dev/null
}

# The modes and owners of CARD_LAYOUT, written into the inodes of a finished
# filesystem image. Never fatal: see the reasoning above.
apply_card_layout() {   # apply_card_layout FS-IMAGE STAGING LAYOUT
    if [ -z "$3" ] || [ -z "$CARD_UID" ] || [ -z "$CARD_UT_GID" ]; then
        say "  no layout table or no users table: leaving the card's ids as"
        say "  built; the phone sets them on the first mount"
        return 0
    fi
    if ! _dbg="$(host_tool debugfs)"; then
        say "  no debugfs on this host: the card's directories keep the build"
        say "  user's ids until the phone restates them on the first mount"
        return 0
    fi

    _script="$BINARIES_DIR/.sdcard-layout.debugfs"
    : > "$_script"

    # The table is parsed exactly as apply_layout() parses it, and the empty
    # name is the root of the card in both.
    #
    # "0751" becomes "040751" because debugfs's sif writes i_mode WHOLE, file
    # type bits included: a mode with no S_IFDIR in it turns a directory into
    # an inode of type zero, which fsck calls corruption and the kernel calls
    # nothing at all -- it simply stops being a directory.
    printf '%s\n' "$3" | while IFS= read -r entry; do
        [ -n "$entry" ] || continue
        name="${entry%%:*}"; rest="${entry#*:}"
        mode="${rest%%:*}"; rest="${rest#*:}"
        owner="${rest%%:*}"; group="${rest#*:}"
        printf 'sif /%s mode 04%s\nsif /%s uid %s\nsif /%s gid %s\n' \
            "$name" "$mode" \
            "$name" "$(phone_id "$owner" 2)" \
            "$name" "$(phone_id "$group" 4)"
    done >> "$_script"

    # Every staged app, in the shape apply_layout() gives one on the phone:
    # the directory and everything in it belong to the owner and are read-only
    # to the app -- which is what stops an app rewriting its own code -- and
    # data/ belongs to the untrusted group, which is the only part of it the
    # app may write.
    for appdir in "$2/apps"/*/; do
        [ -d "$appdir" ] || continue
        app="/apps/$(basename "$appdir")"
        printf 'sif %s mode 040755\nsif %s uid %s\nsif %s gid %s\n' \
            "$app" "$app" "$CARD_UID" "$app" "$CARD_GID" >> "$_script"
        for f in "$appdir"*; do
            [ -f "$f" ] || continue
            file="$app/$(basename "$f")"
            printf 'sif %s mode 0100644\nsif %s uid %s\nsif %s gid %s\n' \
                "$file" "$file" "$CARD_UID" "$file" "$CARD_GID" >> "$_script"
        done
        printf 'sif %s/data mode 040770\nsif %s/data uid %s\nsif %s/data gid %s\n' \
            "$app" "$app" "$CARD_UID" "$app" "$CARD_UT_GID" >> "$_script"
    done

    # Output to a file, deliberately, and not to a pipe: the installed.prop
    # write above says what a short-circuiting reader on the far end of
    # debugfs costs, and it costs the same here.
    "$_dbg" -w -f "$_script" "$1" > "$BINARIES_DIR/.sdcard-debugfs.log" 2>&1 \
        || true
    rm -f "$_script"

    # Read one of them back rather than trusting the exit status. debugfs
    # exits 0 having printed "File not found" for every line of a script aimed
    # at a filesystem laid out differently, so the status says nothing at all.
    # untrusted/ is the one worth checking: it is the only directory on the
    # card the untrusted set may write, so its being wrong is a hole rather
    # than an inconvenience.
    _got="$("$_dbg" -R "ls -l /" "$1" 2>/dev/null \
            | awk '$NF == "untrusted" { print $2 ":" $4 ":" $5 }')"
    _want="40770:$CARD_UID:$CARD_UT_GID"
    if [ "$_got" != "$_want" ]; then
        say "  the card layout did not take: untrusted/ is ${_got:-missing},"
        say "  wanted $_want. Not fatal -- apply_layout() restates it on every"
        say "  mount -- but the image is not what it should be. See"
        say "  $BINARIES_DIR/.sdcard-debugfs.log"
        return 0
    fi
    say "  layout: apps/ $CARD_UID:$CARD_GID 0755," \
        "untrusted/ $CARD_UID:$CARD_UT_GID 0770, media 0750"
}

build_sdcard_image() {   # build_sdcard_image PATH MKE2FS
    _img="$1"
    _mkfs="$2"
    _helper="$NEODCT_DIR/overlay/NeoDCT/System/hw/neodct-sdcard"
    _stage="$BINARIES_DIR/.sdcard-skel"
    _fs="$_img.p1"

    rm -f "$_img" "$_fs"
    rm -rf "$_stage"
    dd if=/dev/zero of="$_img" bs=1M count="$SDCARD_MB" status=none

    mkdir -p "$_stage"
    sdcard_folders "$_stage"
    stage_pentest_apps "$_stage/apps"

    _layout=""
    _plan=""
    if [ -r "$_helper" ]; then
        _layout="$(sdcard_layout_table "$_helper")"
        _plan="$(sdcard_plan "$_helper" "$_img")" || _plan=""
        [ -n "$_plan" ] || \
            say "sdcard.img is too small to partition; one filesystem then"
    else
        say "no $_helper; making a card with no partition table"
    fi

    # A card with no table is still a card: candidates() in the helper offers
    # whole disks as well as partitions, so an unpartitioned NEODCT filesystem
    # mounts exactly like a partitioned one. It is the fallback rather than
    # the shape because a partition table is what a desktop expects to find on
    # a card, and what do_format() writes.
    if [ -z "$_plan" ]; then
        mkfs_card "$_mkfs" "$_img" "$_stage" || {
            say "mke2fs could not make a filesystem in sdcard.img"
            return 1
        }
        apply_card_layout "$_img" "$_stage" "$_layout"
        rm -rf "$_stage"
        say "sdcard.img (${SDCARD_MB}M): one ext4 filesystem NEODCT, no table"
        return 0
    fi

    # shellcheck disable=SC2086  # two numbers, deliberately word-split
    set -- $_plan
    _p1_start="$1"; _p1_sectors="$2"

    NEODCT_SDCARD_SOURCE_ONLY=1 sh -c \
        '. "$1" || exit 1; write_mbr "$2" "$3" "$4"' \
        sh "$_helper" "$_img" "$_p1_start" "$_p1_sectors" || {
        say "could not write a partition table into sdcard.img"
        return 1
    }

    # count=0 seek=N makes a file of exactly the partition's length without
    # writing a byte of it, so the dd back into the card copies exactly the
    # sectors the table claims and nothing beyond them. The block count is an
    # eighth of the sector count because of the -b 4096 in mkfs_card, and the
    # plan is 1 MiB aligned, so that division is exact rather than nearly.
    dd if=/dev/zero of="$_fs" bs=512 count=0 seek="$_p1_sectors" status=none
    mkfs_card "$_mkfs" "$_fs" "$_stage" "$((_p1_sectors / 8))" || {
        say "mke2fs could not make the card's filesystem"
        rm -f "$_fs"
        return 1
    }
    apply_card_layout "$_fs" "$_stage" "$_layout"
    dd if="$_fs" of="$_img" bs=512 seek="$_p1_start" conv=notrunc status=none
    rm -f "$_fs"
    rm -rf "$_stage"

    say "sdcard.img (${SDCARD_MB}M): one ext4 partition of" \
        "$((_p1_sectors / 2048))M, label NEODCT"
}

# Never overwrite an existing card image: it is where the user drops their
# music, wallpapers and UPDATE.ndsw between builds.
#
# No tool check here any more. The card is ext4 now, so the tool is the same
# mke2fs the user partition needs, and the build has already stopped above if
# it is missing -- there is no image set without it, card or no card.
if [ -f "$BINARIES_DIR/sdcard.img" ]; then
    say "sdcard.img exists, leaving it alone"
else
    build_sdcard_image "$BINARIES_DIR/sdcard.img" "$MKE2FS"
fi

# ============ THE USERS MUST BE IN THE IMAGE, OR STOP ============
#
# Without ndusr and ndusr_ut in /etc/passwd, nd_priv_lookup() finds nothing,
# nd_priv_become() is a documented no-op, and EVERY APP RUNS AS ROOT --
# netsurf included. The privilege split is not degraded, it is absent.
#
# That shipped. An output/ tree older than the users table keeps its original
# .config through every rebuild, never gains BR2_ROOTFS_USERS_TABLES, and
# `make` says nothing; the only trace was one line in a boot log. It was found
# by running `top` on a real build and seeing netsurf as root.
#
# So it is checked here, and a build that would produce such an image FAILS.
# A broken phone you are told about beats a broken phone you are not.
#
# full_users_table.txt is what buildroot actually handed mkusers -- the
# defconfig's table plus every package's users, assembled in fs/common.mk. It
# is the right thing to test because it is the input to the only step that
# creates users; testing the built image instead would need unsquashfs or
# debugfs, which are not guaranteed on a build host.
check_users() {
    # BUILD_DIR is NOT among the variables buildroot exports to post-image
    # scripts -- BASE_DIR is (buildroot/Makefile:495) and BUILD_DIR is defined
    # as $(BASE_DIR)/build (:213). Taking ${BUILD_DIR:-} alone left the path as
    # "/buildroot-fs/..." , which does not exist, which took the "cannot
    # verify" branch and returned 0. A check that silently passes is the exact
    # thing this was written to stop, so it derives the path and treats a
    # missing table as a failure: post-image only runs after a rootfs has been
    # built, so fs/common.mk has always written this file by now.
    table="${BUILD_DIR:-${BASE_DIR:-}/build}/buildroot-fs/full_users_table.txt"

    if [ ! -f "$table" ]; then
        echo "" >&2
        echo "post-image: FATAL: cannot find $table" >&2
        echo "  so it cannot be verified that this image has the users that" >&2
        echo "  keep apps from running as root. Refusing to call it ready." >&2
        echo "" >&2
        exit 1
    fi
    for u in ndusr ndusr_ut; do
        if ! grep -qE "^[[:space:]]*$u[[:space:]]" "$table"; then
            echo "" >&2
            echo "post-image: FATAL: no '$u' in this image." >&2
            echo "" >&2
            echo "  Every app would run as ROOT -- the browser included." >&2
            echo "  nd_priv_lookup() has no user to find, so nd_priv_become()" >&2
            echo "  does nothing and the child keeps nd-core's uid 0." >&2
            echo "" >&2
            echo "  The users come from neodct/configs/users-table.txt, via" >&2
            echo "  BR2_ROOTFS_USERS_TABLES and via NEODCT_USERS in" >&2
            echo "  package/neodct/neodct.mk. Both have failed here." >&2
            echo "" >&2
            echo "  Try:  cd buildroot && make neodct_qemu_defconfig && make" >&2
            echo "  Then: nd-selftest on the phone reports the same thing." >&2
            echo "" >&2
            exit 1
        fi
    done
    say "users: ndusr and ndusr_ut are in this image"
}
check_users

say "image set ready in $BINARIES_DIR"

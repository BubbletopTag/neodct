#!/bin/sh
set -eu

TARGET_DIR="${1:-}"
if [ -z "$TARGET_DIR" ] || [ ! -d "$TARGET_DIR" ]; then
    echo "[post-build] Missing TARGET_DIR" >&2
    exit 1
fi
shift

# Platform id, e.g. luckfox-armv7 / qemu-aarch64.
#
# Buildroot calls post-build scripts as
#
#   script TARGET_DIR $BR2_ROOTFS_POST_SCRIPT_ARGS $BR2_ROOTFS_POST_BUILD_SCRIPT_ARGS
#
# and both defconfigs put the defconfig PATH in POST_SCRIPT_ARGS, because the
# qemu board's post-image script needs it. So $2 is a build-machine path and
# the platform id is LAST, not second. Reading $2 meant
# "${PLATFORM%%-*}" = "luckfox" never matched and etc/inittab.luckfox was
# never installed -- real hardware silently shipped the generic inittab.
# post-build-system-metadata.sh already takes the last argument; this now
# agrees with it.
PLATFORM="unknown"
for argument in "$@"; do
    PLATFORM="$argument"
done
case "$PLATFORM" in
    ""|*/*) PLATFORM="unknown" ;;
esac

rm -rf "$TARGET_DIR/tests"

# Drop apps that are no longer in the overlay.
#
# BR2_ROOTFS_OVERLAY copies *over* the target tree and never deletes, and
# buildroot does not rebuild target/ from scratch between builds. So an app
# removed from the overlay stays in every image built in that output
# directory until someone runs `make clean` -- it is still installed, still
# scanned by the launcher, still in the app grid. That is how a deleted app
# shipped in an update once already.
#
# Scoped deliberately to the app directories: those are populated purely by
# the overlay, so "in target but not in the overlay" is unambiguous there.
# The rest of the tree mixes overlay files with package and generated ones.
#
# Since the C rewrite those directories are no longer overlay-only: the neodct
# package installs app.so into each of them. That does not change the rule --
# an app dropped from the overlay should lose its app.so too -- but it does
# mean this loop can delete a freshly built binary, so it prints every removal
# and the coverage check below says which apps ended up with no app.so at all.
# Nothing else here touches .so files or /NeoDCT/System/{bin,lib}.
OVERLAY_ROOT="$(cd "$(dirname "$0")/../overlay" 2>/dev/null && pwd || true)"
if [ -n "$OVERLAY_ROOT" ]; then
    for apps_rel in NeoDCT/System/apps NeoDCT/System/engineering/apps; do
        target_apps="$TARGET_DIR/$apps_rel"
        overlay_apps="$OVERLAY_ROOT/$apps_rel"
        [ -d "$target_apps" ] && [ -d "$overlay_apps" ] || continue
        for path in "$target_apps"/*; do
            [ -d "$path" ] || continue
            name="$(basename "$path")"
            if [ ! -d "$overlay_apps/$name" ]; then
                if [ -f "$path/app.so" ]; then
                    echo "[post-build] dropping stale app (had a built app.so):" \
                         "$apps_rel/$name" >&2
                else
                    echo "[post-build] dropping stale app: $apps_rel/$name"
                fi
                rm -rf "$path"
            fi
        done
    done
fi

# Which apps got no app.so.
#
# The name lists that decide where the stub app.so is installed live in
# neodct/src/Makefile (STUB_STOCK_APPS / STUB_ENG_APPS) and are maintained by
# hand, so they can drift from the overlay's directory names. When they do,
# the app keeps its manifest.json and its icon -- it is still in the grid --
# and simply fails to start, which is a confusing thing to discover on the
# phone rather than here.
#
# A warning, not an error: during the port a half-populated tree is the normal
# state, and failing the image build over it would stop work. Set
# NEODCT_REQUIRE_APP_SO=1 to make it fatal for a release build.
if [ -n "$OVERLAY_ROOT" ]; then
    missing=""
    for apps_rel in NeoDCT/System/apps NeoDCT/System/engineering/apps; do
        target_apps="$TARGET_DIR/$apps_rel"
        [ -d "$target_apps" ] || continue
        for path in "$target_apps"/*; do
            [ -d "$path" ] || continue
            [ -f "$path/manifest.json" ] || continue
            [ -f "$path/app.so" ] && continue
            missing="$missing $apps_rel/$(basename "$path")"
        done
    done
    if [ -n "$missing" ]; then
        echo "[post-build] no app.so (app will not launch):$missing" >&2
        if [ "${NEODCT_REQUIRE_APP_SO:-0}" = "1" ]; then
            echo "[post-build] NEODCT_REQUIRE_APP_SO=1 -- failing the build" >&2
            exit 1
        fi
    fi
fi

# openssh's own boot script.
#
# The package installs /etc/init.d/S50sshd, which starts sshd at every boot
# with the stock config -- and the stock config listens on every interface.
# This phone has a public IPv6 address on mobile data, so that is an sshd
# facing the whole internet, always, whether or not anybody turned Remote
# Shell on. It also runs `ssh-keygen -A`, which writes into /etc/ssh on a
# read-only squashfs.
#
# System/core/RemoteShell decides when sshd runs, with a config it
# generates: loopback only, keys only, reachable solely through a tunnel
# the phone dialled out itself. Nothing else may start it.
rm -f "$TARGET_DIR/etc/init.d/S50sshd"

# Editor droppings. The overlay is a live working tree -- somebody has
# CHANGELOG.txt open in an editor while the release builds -- and whatever
# is sitting in it gets copied into the image and signed along with
# everything else. A LibreOffice lock file shipped to a phone this way
# once already.
find "$TARGET_DIR/NeoDCT" \
    \( -name '.~lock.*#' -o -name '*.swp' -o -name '*~' -o -name '.#*' \) \
    -type f -print -delete 2>/dev/null || true

# Apps the build was told to leave out.
#
#     NEODCT_EXCLUDE_APPS="Tube Foo" make
#
# The overlay is a live working tree and may hold an app somebody is still
# experimenting with. It has to stay in the tree for them to work on, and
# it must not be in a signed release built from that same tree. Naming it
# here keeps it out of the image without anyone moving directories around
# under a colleague mid-edit.
for name in ${NEODCT_EXCLUDE_APPS:-}; do
    for apps_rel in NeoDCT/System/apps NeoDCT/System/engineering/apps; do
        path="$TARGET_DIR/$apps_rel/$name"
        if [ -e "$path" ]; then
            echo "[post-build] excluding app: $apps_rel/$name"
            rm -rf "$path"
        fi
    done
done

find "$TARGET_DIR/NeoDCT" -name __pycache__ -type d -prune -exec rm -rf {} + 2>/dev/null || true

# Drop the Python from the image -- A BACKSTOP NOW, NOT THE MAIN EVENT.
#
# This used to do real work: the overlay carried the whole Python OS, 127
# files, and this pass is what kept them off the target. The overlay no
# longer carries any of it -- it lives in neodct/python-reference/, out of
# BR2_ROOTFS_OVERLAY entirely -- so on a clean tree the count below is zero.
#
# It stays because the overlay is a directory anybody can drop a file into,
# and a stray .py under /NeoDCT is not a small mistake: `ls
# /NeoDCT/System/core` showing ModemService/ full of .py and no sign of the
# service actually running is exactly the wrong thing for the next person
# debugging the phone over serial to find. There is also no interpreter on
# the image any more, so anything this catches could not have run regardless.
#
# Scoped to /NeoDCT so it cannot touch anything a package installed, and the
# now-empty service directories under System/core go with them -- that tree is
# t9.dict and directories, nothing else, so an empty directory there is a
# leftover and not something somebody meant.
# The icebox does not ship.
#
# /NeoDCT/Development is where work gets parked -- Icebox/MusicPlayer is a
# whole app with an icon and a manifest_disabled.disabled, sitting there
# waiting for somebody to come back to it. That is a working-tree thing.
# Nothing on the phone reads the directory (grep across src, overlay/etc,
# overlay/bin and scripts finds no reference at all), so shipping it just
# puts half-finished code on a device where it can only confuse whoever
# finds it. It stays in the repository, which is the point of an icebox.
rm -rf "$TARGET_DIR/NeoDCT/Development"

if [ "${NEODCT_KEEP_PYTHON:-0}" != "1" ] && [ -d "$TARGET_DIR/NeoDCT" ]; then
    py_count=$(find "$TARGET_DIR/NeoDCT" \
        \( -name '*.py' -o -name '*.pyc' -o -name '*.pyo' -o -name '*.py.old' \) \
        -type f | wc -l)
    find "$TARGET_DIR/NeoDCT" \
        \( -name '*.py' -o -name '*.pyc' -o -name '*.pyo' -o -name '*.py.old' \) \
        -type f -delete 2>/dev/null || true
    if [ -d "$TARGET_DIR/NeoDCT/System/core" ]; then
        find "$TARGET_DIR/NeoDCT/System/core" -mindepth 1 -type d -empty -delete \
            2>/dev/null || true
    fi
    echo "[post-build] dropped $py_count Python files from the image"
fi

# Strip the ELF files buildroot's own strip pass cannot reach.
#
# target-finalize runs its global strip BEFORE it copies BR2_ROOTFS_OVERLAY
# over the target tree and BEFORE it runs this script (buildroot/Makefile:
# strip at line 760, overlay at 791, post-build at 802). So anything the
# neodct package installed is already stripped by the time we get here and
# this is a no-op on it -- but any ELF file carried in the overlay has never
# been through strip at all, and never will be. That gap is what this closes.
# (It used to hold the 24KB neodct_displayd blob; the daemon is built from
# source now, but the gap is still there for the next one.)
#
# Deliberately narrow and deliberately unable to fail the build: scoped to
# /NeoDCT, skips anything that is not an ELF, and every failure is swallowed.
# A phone image is not worth losing to a strip that did not like a file.
if [ "${NEODCT_SKIP_STRIP:-0}" != "1" ] && [ -d "$TARGET_DIR/NeoDCT" ]; then
    # Honour BR2_STRIP_none: if the user asked for symbols, they get symbols.
    br_config=""
    for candidate in "${CONFIG_DIR:-}/.config" "${O:-}/.config"; do
        [ -f "$candidate" ] && { br_config="$candidate"; break; }
    done
    strip_wanted=1
    if [ -n "$br_config" ] && ! grep -q '^BR2_STRIP_strip=y$' "$br_config"; then
        strip_wanted=0
    fi

    # The cross strip, by whatever tuple this toolchain calls itself.
    #
    # It must come out of the buildroot host tree. An unset root would make
    # "$root"/bin/*-strip glob as /bin/*-strip and quietly select the BUILD
    # MACHINE's strip -- which on this host is llvm-strip, and llvm-strip is
    # multi-target, so it would cheerfully rewrite ARM binaries and nobody
    # would ever see it happen. Hence the explicit empty-root skip.
    #
    # HOST_DIR is not in buildroot's EXTRA_ENV, so in a real build it is O
    # that resolves this; HOST_DIR is only the fallback for running this
    # script by hand.
    strip_bin=""
    for root in "${O:-}/host" "${HOST_DIR:-}"; do
        [ -n "$root" ] && [ "$root" != "/host" ] || continue
        for candidate in "$root"/bin/*-strip; do
            [ -x "$candidate" ] || continue
            strip_bin="$candidate"
            break
        done
        [ -n "$strip_bin" ] && break
    done

    if [ "$strip_wanted" = "1" ] && [ -n "$strip_bin" ]; then
        find "$TARGET_DIR/NeoDCT" -type f \
             \( -perm -u+x -o -name '*.so' -o -name '*.so.*' \) -print \
        | while IFS= read -r f; do
            # Four bytes of ELF magic, or leave it alone. Shell scripts and
            # python live in here too, and `strip` on a text file is a mess.
            magic=$(dd if="$f" bs=4 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')
            [ "$magic" = "7f454c46" ] || continue
            # No chmod: everything here arrives u+w (the overlay rsync uses
            # --chmod=u=rwX and the package installs 0755), and silently
            # widening the mode of a file somebody made read-only on purpose
            # is a worse outcome than leaving its symbols in.
            [ -w "$f" ] || continue
            "$strip_bin" --strip-unneeded \
                --remove-section=.comment --remove-section=.note \
                "$f" 2>/dev/null || true
        done
    elif [ "$strip_wanted" = "1" ]; then
        echo "[post-build] no cross strip found; overlay binaries keep their" \
             "debug symbols" >&2
    fi
fi

# Luckfox-specific console config: replace generic inittab
# only when called with a luckfox platform id.
LUCKFOX_INITTAB="$TARGET_DIR/etc/inittab.luckfox"
if [ "${PLATFORM%%-*}" = "luckfox" ]; then
    if [ -f "$LUCKFOX_INITTAB" ]; then
        cp "$LUCKFOX_INITTAB" "$TARGET_DIR/etc/inittab"
    fi
fi
# Either way, don't ship the flavor-specific file itself
rm -f "$LUCKFOX_INITTAB"

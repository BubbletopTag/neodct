#!/bin/sh
set -eu

TARGET_DIR="${1:-}"
# Platform id, e.g. luckfox-armv7 / qemu-aarch64. This is the same string
# that lands in version.prop and in an update's manifest.json, so an image
# can never be installed on the wrong hardware.
PLATFORM="${2:-}"
if [ -z "$TARGET_DIR" ] || [ ! -d "$TARGET_DIR" ]; then
    echo "[post-build] Missing TARGET_DIR" >&2
    exit 1
fi

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
                echo "[post-build] dropping stale app: $apps_rel/$name"
                rm -rf "$path"
            fi
        done
    done
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

# Host-built .pyc files are useless on the target (host python != target
# python) and a read-only rootfs cannot replace them. Drop them; the
# runtime caches to /NeoDCT/User/.pycache instead (see run_neodct.sh).
find "$TARGET_DIR/NeoDCT" -name __pycache__ -type d -prune -exec rm -rf {} + 2>/dev/null || true

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

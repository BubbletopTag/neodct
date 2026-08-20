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

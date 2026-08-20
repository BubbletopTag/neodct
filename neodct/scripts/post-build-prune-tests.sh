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

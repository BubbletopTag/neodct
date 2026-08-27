#!/bin/sh
# Copy non-free firmware into the rootfs, from the one folder that holds it.
#
# The blobs do NOT live in neodct/overlay/. The overlay is the free part of the
# image and a reader should be able to trust that; anything without source sits
# in neodct/nonfree/ instead, beside the Rockchip DDR blob, so "what proprietary
# code does this phone run" is answered by listing one directory rather than by
# grepping the tree.
#
# That means the overlay mechanism cannot install them and this script does.
#
# ============ WHAT IS HERE AND WHY ============
#
# rtl_bt/  the TP-Link UB500's Bluetooth firmware. The RTL8761B carries no
#          usable on-chip BT firmware, so btrtl uploads this on every probe --
#          see drivers/bluetooth/btrtl.c, which is itself GPL-2.0-or-later and
#          mainline. Without it the dongle enumerates and then reports a null
#          BD_ADDR, which looks like broken hardware rather than missing
#          firmware.
#
#          The licence file ships with it because the licence requires it:
#          "Redistributions must reproduce the above copyright notice". Do not
#          drop it to save 2 KB.
#
# Files are copied rather than symlinked: the rootfs becomes a read-only
# squashfs and a link out of the build tree would dangle on the phone.
set -e

TARGET="${1:-$TARGET_DIR}"
[ -n "$TARGET" ] || { echo "post-build-nonfree-firmware: no target dir" >&2; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
NONFREE="$(dirname "$HERE")/nonfree"

[ -d "$NONFREE/rtl_bt" ] || exit 0

install -d "$TARGET/lib/firmware/rtl_bt"
for f in "$NONFREE"/rtl_bt/*.bin "$NONFREE"/rtl_bt/LICENCE.*; do
    [ -f "$f" ] || continue
    install -m 0644 "$f" "$TARGET/lib/firmware/rtl_bt/"
done

echo "[post-build] non-free firmware: $(ls "$TARGET/lib/firmware/rtl_bt" | tr '\n' ' ')"

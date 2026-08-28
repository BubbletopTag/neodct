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

# The same blob under the name THIS kernel asks for.
#
# btrtl picks a filename from what the chip reports about itself. The UB500
# answers lmp_subver=0x8761, hci_rev=0x0b, and the phone's 5.10 kernel maps
# that pair to "rtl8761b_fw.bin". Kernels from 5.15 on split the USB part out
# as its own entry and ask for "rtl8761bu_fw.bin" instead -- which is the name
# linux-firmware ships, and the name in neodct/nonfree/.
#
# Same silicon, same bytes, two spellings across kernel vintages. Installing
# both means the image does not care which kernel it is booted under, and the
# alternative -- patching the filename into btrtl -- would put a rootfs concern
# inside the kernel and break a genuine 8761B if one were ever attached.
#
# Copied, not linked: the rootfs is squashfs and a dangling link here is a
# firmware load that fails with -2 and a Bluetooth adapter with no address.
for pair in "rtl8761bu_fw.bin:rtl8761b_fw.bin" \
            "rtl8761bu_config.bin:rtl8761b_config.bin"; do
    src="$NONFREE/rtl_bt/${pair%%:*}"
    dst="$TARGET/lib/firmware/rtl_bt/${pair##*:}"
    [ -f "$src" ] && install -m 0644 "$src" "$dst"
done

echo "[post-build] non-free firmware: $(ls "$TARGET/lib/firmware/rtl_bt" | tr '\n' ' ')"

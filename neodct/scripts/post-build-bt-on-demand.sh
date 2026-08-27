#!/bin/sh
# Take out the init scripts that start dbus and bluetoothd at boot.
#
# Buildroot's dbus and bluez5_utils packages each install one, so a phone that
# ships them runs both daemons from boot whether or not anyone wants
# Bluetooth. Measured on this phone that is about 2.6 MB of PSS -- dbus 519 KB
# and bluetoothd 2.1 MB -- permanently, out of 54 MB.
#
# NeoDCT starts them when Settings enables Bluetooth and stops them when it is
# disabled, so the idle cost is nothing. Leaving these here would also mean two
# bluetoothd processes the moment the app started its own, and the second one
# fails in a way that reads as a broken adapter:
#
#   src/gatt-database.c:btd_gatt_database_new() Failed to start listening:
#   l2cap_bind: Address in use (98)
#   Failed to create GATT database for adapter
#   Unable to register new adapter
set -e

TARGET="${1:-$TARGET_DIR}"
[ -n "$TARGET" ] || { echo "post-build-bt-on-demand: no target dir" >&2; exit 1; }

for s in S30dbus S40bluetoothd; do
    if [ -e "$TARGET/etc/init.d/$s" ]; then
        rm -f "$TARGET/etc/init.d/$s"
        echo "[post-build] removed /etc/init.d/$s (started on demand instead)"
    fi
done

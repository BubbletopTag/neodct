#!/bin/sh
# install.sh -- drop the Bible app into a NeoDCT tree.
#
#   ./install.sh qemu-aarch64  /path/to/neodct/neodct/overlay   # then rebuild the image
#   ./install.sh luckfox-armv7 /mnt/neodct-root                 # a mounted rootfs
#   ./install.sh luckfox-armv7 --user /mnt/NDUSER               # data only, see below
#
# WHERE THIS CAN GO
#
# On a running phone "/" is a read-only squashfs under dm-verity, so the app
# itself cannot be copied onto a live device -- it has to go into the overlay
# and come back as part of an image or an .ndsw. Point this at
# neodct/overlay and rebuild.
#
# The PACK is different. web.ndb is data, the app looks for *.ndb on the user
# partition first, and /NeoDCT/User is writable. So `--user` copies only the
# pack, which is how you add a translation to a phone that is already flashed.

set -e
TARGET=$1
DEST=$2
HERE=$(cd "$(dirname "$0")" && pwd)

[ -n "$TARGET" ] && [ -n "$DEST" ] || { sed -n '2,20p' "$0"; exit 1; }

if [ "$TARGET" = "--user" ] || [ "$DEST" = "--user" ]; then
    DEST=$3
    [ -n "$DEST" ] || { echo "install.sh: --user needs a destination" >&2; exit 1; }
    mkdir -p "$DEST/Bible"
    cp "$HERE/Bible/web.ndb" "$DEST/Bible/"
    echo "pack -> $DEST/Bible/web.ndb"
    exit 0
fi

[ -d "$HERE/$TARGET" ] || { echo "install.sh: no such target '$TARGET'" >&2; exit 1; }

APPS="$DEST/NeoDCT/System/apps"
[ -d "$APPS" ] || APPS="$DEST/apps"
[ -d "$APPS" ] || { echo "install.sh: cannot find an apps/ directory under $DEST" >&2; exit 1; }

mkdir -p "$APPS/Bible"
cp "$HERE/Bible/manifest.json" "$HERE/Bible/icon.png" "$HERE/Bible/web.ndb" "$APPS/Bible/"
cp "$HERE/$TARGET/app.so" "$APPS/Bible/app.so"
chmod 0755 "$APPS/Bible/app.so"
echo "installed $TARGET -> $APPS/Bible"
ls -la "$APPS/Bible"

#!/bin/sh
# install.sh -- drop the Bible app into a NeoDCT tree.
#
#   ./install.sh luckfox-armv7 /path/to/neodct/neodct/overlay   # then rebuild the image
#   ./install.sh luckfox-armv7 /mnt/neodct-root                 # a mounted rootfs
#   ./install.sh luckfox-armv7 --user /mnt/NDUSER               # data only, see below
#
# ONE TARGET NOW COVERS BOTH MACHINES. The emulator is armv7 with the phone's
# musl hard-float NEON-VFPv4 ABI, so luckfox-armv7 is the build to install
# whichever one you are pointing this at. qemu-aarch64 is refused by name
# below, and that refusal is the point of this paragraph: the first usage line
# here used to say to install it into the QEMU overlay, this script's only
# check was that the directory existed, and nothing downstream would have
# objected -- the .so goes straight into the verity-covered squashfs, past
# mknap.py and past nd_nap_install(), so the first complaint would have been
# dlopen() failing in front of whoever opened the app.
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

[ -n "$TARGET" ] && [ -n "$DEST" ] || { sed -n '2,30p' "$0"; exit 1; }

# The same sentence mknap.py's RETIRED_ARCH_TAGS carries, said by name rather
# than worked out from the file. An ELF check here would be mknap's table
# written a second time in a second language, and the two copies would drift;
# what this path needs is the one thing mknap and nd_nap_install() already
# refuse, refused before the bytes reach an image that has no other gate.
case "$TARGET" in
    qemu-aarch64)
        echo "install.sh: $TARGET is no longer built; one armv7 package now" >&2
        echo "  serves both machines. Use luckfox-armv7 -- it is the build" >&2
        echo "  the emulator wants too." >&2
        exit 1
        ;;
esac

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

#!/bin/sh
# Graft the NeoDCT additions onto an extracted mpv tree.
#
# Run by the buildroot mpv package as a post-extract hook, the same way
# netsurf.mk grafts netsurf-neodct. Kept as a script rather than a patch
# because a patch against 325 lines of new file is unreadable, and because
# this way the source lives in the NeoDCT tree with its own unit tests.
#
#   apply.sh <path-to-extracted-mpv>
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
MPV="${1:?usage: apply.sh <mpv source dir>}"

[ -f "$MPV/video/out/vo.c" ] || {
    echo "apply.sh: $MPV does not look like an mpv tree" >&2
    exit 1
}

# Idempotent: buildroot re-runs post-extract hooks on a re-extract, and a
# second application must not double up the registration.
if [ -f "$MPV/video/out/vo_fbdev.c" ]; then
    echo "apply.sh: already applied"
    exit 0
fi

cp "$HERE/vo_fbdev.c" "$HERE/fbdev_format.c" "$HERE/fbdev_format.h" \
   "$MPV/video/out/"

# 1. Declare and register the driver. It goes next to vo_drm in the list,
#    after the windowed outputs, so autoprobing still prefers a real
#    display when there is one.
sed -i 's|^extern const struct vo_driver video_out_drm;|extern const struct vo_driver video_out_drm;\nextern const struct vo_driver video_out_fbdev;|' \
    "$MPV/video/out/vo.c"
sed -i 's|^    \&video_out_lavc,|    \&video_out_fbdev,\n    \&video_out_lavc,|' \
    "$MPV/video/out/vo.c"

# 2. Build it. Unconditional: it needs nothing but linux/fb.h, which is
#    part of the kernel headers every target here already has.
sed -i "s|^    'video/out/vo_null.c',|    'video/out/vo_null.c',\n    'video/out/vo_fbdev.c',\n    'video/out/fbdev_format.c',|" \
    "$MPV/meson.build"

# 3. Bindings that survive --no-config (see the file's own comment).
cat "$HERE/input-neodct.conf" >> "$MPV/etc/input.conf"

grep -q "video_out_fbdev" "$MPV/video/out/vo.c" || {
    echo "apply.sh: driver registration did not take" >&2
    exit 1
}
grep -q "vo_fbdev.c" "$MPV/meson.build" || {
    echo "apply.sh: meson source list did not take" >&2
    exit 1
}

echo "apply.sh: NeoDCT fbdev output added to $MPV"

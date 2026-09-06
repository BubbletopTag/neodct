#!/bin/sh
# Put /etc/neodct-devenv into the image, or make sure it is not there.
#
# ============ THE PROBLEM THIS SOLVES ============
#
# run_neodct.sh sources /NeoDCT/User/env.sh only when a developer environment
# is declared, and the gate is deliberately something the writable partition
# cannot set -- SECURITY-AUDIT.md section 4 Q5 vector 2 rated the ungated
# version Critical, because env.sh is arbitrary shell, as uid 0, from writable
# storage, on every boot, and it survives an update. Two things qualify as a
# gate and neither is a setting:
#
#   the kernel cmdline    U-Boot environment on the phone, -append in QEMU
#   a file in the rootfs  read-only squashfs under dm-verity
#
# That gate is correct. What was missing is that neither of them was reachable
# on the phone. run_qemu.sh adds `neodct.devenv=1` to -append BY DEFAULT, so
# every override -- NEODCT_T9, a redirected device path, and above all
# NEODCT_NO_DROP, which nd_main.c offers as the way to keep the old root
# behaviour "for a developer bisecting something" -- works in the emulator and
# is unreachable on the hardware. The phone's bootargs carry ubi.mtd, neodct.sys,
# neodct.user, neodct.verity and neodct.rectty and nothing else, and
# /etc/neodct-devenv did not exist in the overlay or in any built image.
#
# So the one target that is broken is the one where none of the tools for
# investigating it can be turned on, without a reflash.
#
# ============ AND WHY IT IS A BUILD FLAG AND NOT AN OVERLAY FILE ============
#
# Shipping the marker in neodct/overlay/etc/ would put it in EVERY image and
# delete the gate. Shipping it and deleting it here would mean the safe state
# depends on this script running, and a post-build script that is dropped from
# a defconfig fails open.
#
# So it is the other way round: absent unless asked for, and the ask is an
# environment variable at build time, which cannot be set from a running phone.
#
#     NEODCT_DEVENV_IMAGE=1 make
#
# and the resulting rootfs -- and any .ndsw cut from it, which is how this
# reaches a phone already in the field -- honours /NeoDCT/User/env.sh. The
# marker is inside the verity-covered squashfs, so it is still true that
# enabling it means replacing a signed image.
#
# Do not ship a release built with it. There is no way for the phone to tell
# afterwards, which is why nd-selftest prints the gate's state and why
# run_neodct.sh says out loud which of the two sources declared it.
set -eu

TARGET_DIR="${1:-${TARGET_DIR:-}}"
[ -n "$TARGET_DIR" ] || { echo "post-build: TARGET_DIR not set" >&2; exit 1; }

MARKER="$TARGET_DIR/etc/neodct-devenv"

case "${NEODCT_DEVENV_IMAGE:-0}" in
    1|y|Y|yes|on|ON)
        mkdir -p "$TARGET_DIR/etc"
        cat > "$MARKER" <<'MARKERBODY'
# The presence of this file is the whole content: /bin/run_neodct.sh checks for
# it and, when it is here, sources /NeoDCT/User/env.sh as root before starting
# nd-core. Built in by NEODCT_DEVENV_IMAGE=1. Not for a release image.
MARKERBODY
        echo "[post-build] /etc/neodct-devenv: DEVELOPER IMAGE -- env.sh will be sourced as root"
        ;;
    *)
        # Unconditional, not `if -e`: a stale target/ directory from a previous
        # NEODCT_DEVENV_IMAGE=1 build keeps every file it was given, and
        # buildroot will not remove one for you. A developer image that turned
        # itself back into a release image by accident is exactly the failure
        # this ordering exists to prevent.
        rm -f "$MARKER"
        ;;
esac

#!/bin/sh
# Refuse to call a build ready when the image has no ndusr.
#
# ============ WHY THIS IS ITS OWN FILE ============
#
# It was the last function in post-image-neodct.sh, and post-image-neodct.sh is
# wired into the QEMU defconfig's BR2_ROOTFS_POST_IMAGE_SCRIPT and nowhere
# else. The luckfox defconfig -- the one whose output goes on the phone --
# stops at `board/qemu/post-image.sh`, and the luckfox image is assembled
# afterwards by neodct/tools/mknand.sh, which has never mentioned ndusr.
#
# So the guard ran on exactly the target that does not ship, and the shipping
# target had none. That is the wrong way round for a check whose whole purpose
# is to stop a phone going out with every app running as root, and the error
# text even pointed the reader at `make neodct_qemu_defconfig`, which is the
# wrong command for the board it would have been telling about.
#
# Splitting it out means both defconfigs can list it directly, and it stays one
# implementation rather than two that can drift.
#
# ============ WHAT IT CHECKS ============
#
# full_users_table.txt is what buildroot actually handed mkusers -- the
# defconfig's table plus every package's users, assembled in fs/common.mk. It
# is the right thing to test because it is the input to the only step that
# creates users; testing the built image instead would need unsquashfs or
# debugfs, which are not guaranteed on a build host.
#
# Two things, and the second is new. Without ndusr and ndusr_ut,
# nd_priv_lookup() finds nothing, nd_priv_become() is a documented no-op and
# EVERY APP RUNS AS ROOT, netsurf included -- the privilege split is not
# degraded, it is absent. That shipped once: an output/ tree older than the
# users table keeps its original .config through every rebuild, never gains
# BR2_ROOTFS_USERS_TABLES, and `make` says nothing. It was found by running
# `top` on a real build and seeing netsurf as root.
#
# The GROUPS are the second thing, and they fail the other way round: quietly
# rather than absolutely. 61-neodct-devices.rules names i2c, ndusr, video and
# audio, and eudev resolves GROUP= at rule-parse time -- an unknown name logs
# one line and falls back to gid 0, which also drops the node to 0600. So a
# missing group is not a build error and not a boot error; it is a phone with
# no keypad, or no modem, or no sound, discovered by the owner. Checking the
# membership here is the only place it costs nothing.
set -eu

# Positional arguments are IGNORED on purpose. buildroot calls a post-image
# script as `script $(BINARIES_DIR) $(BR2_ROOTFS_POST_SCRIPT_ARGS)`, so $1 is
# the images directory and reading a table path out of it would find one only
# by accident. The override is an environment variable, which is what the host
# tests can set.
TABLE="${NEODCT_USERS_TABLE:-${BUILD_DIR:-${BASE_DIR:-}/build}/buildroot-fs/full_users_table.txt}"

# The groups ndusr must be in for the udev rules to mean anything, and what
# each one buys. Kept in step with neodct/configs/users-table.txt by hand.
#
#   i2c      the PCF8575 keypad matrix and the MAX17048 fuel gauge
#   dialout  the modem's AT and PCM ports
#   input    /dev/input/event*
#   video    /dev/fb0 and the backlight
#   audio    /dev/snd/*
NEODCT_NDUSR_GROUPS="i2c dialout input video audio"

die() {
    echo "" >&2
    while [ "$#" -gt 0 ]; do echo "$1" >&2; shift; done
    echo "" >&2
    exit 1
}

# BUILD_DIR is NOT among the variables buildroot exports to post-image scripts
# -- BASE_DIR is (buildroot/Makefile:495) and BUILD_DIR is defined as
# $(BASE_DIR)/build (:213). Taking ${BUILD_DIR:-} alone left the path as
# "/buildroot-fs/...", which does not exist, which took the "cannot verify"
# branch and returned 0. A check that silently passes is the exact thing this
# was written to stop, so a missing table is a failure: post-image only runs
# after a rootfs has been built, so fs/common.mk has always written it by now.
[ -f "$TABLE" ] || die \
    "post-image: FATAL: cannot find $TABLE" \
    "  so it cannot be verified that this image has the users that" \
    "  keep apps from running as root. Refusing to call it ready."

for u in ndusr ndusr_ut; do
    grep -qE "^[[:space:]]*$u[[:space:]]" "$TABLE" || die \
        "post-image: FATAL: no '$u' in this image." \
        "" \
        "  Every app would run as ROOT -- the browser included." \
        "  nd_priv_lookup() has no user to find, so nd_priv_become()" \
        "  does nothing and the child keeps nd-core's uid 0." \
        "" \
        "  The users come from neodct/configs/users-table.txt, via" \
        "  BR2_ROOTFS_USERS_TABLES and via NEODCT_USERS in" \
        "  package/neodct/neodct.mk. Both have failed here." \
        "" \
        "  Reconfigure from the defconfig for the board you are building" \
        "  (neodct_qemu_defconfig or luckfox_pico_mini_defconfig) and" \
        "  rebuild. nd-selftest on the phone reports the same thing."
done

# The groups column is the eighth field of the ndusr line. mkusers' format is
#   username uid group gid password home shell groups comment
# and a missing group here is a device the UI cannot open, not a build error.
groups="$(awk '$1 == "ndusr" { print $8; exit }' "$TABLE")"
for g in $NEODCT_NDUSR_GROUPS; do
    case ",$groups," in
        *",$g,"*) ;;
        *) die \
            "post-image: FATAL: ndusr is not in group '$g'." \
            "" \
            "  61-neodct-devices.rules hands device nodes to that group, and" \
            "  eudev resolves GROUP= at rule-parse time: an unknown or unheld" \
            "  group falls back to gid 0 and the node stays 0600. The phone" \
            "  boots and the UI cannot open the device -- no keypad, no modem" \
            "  or no sound, depending which one is missing." \
            "" \
            "  ndusr's groups here are: ${groups:-(none)}" \
            "  Expected all of: $NEODCT_NDUSR_GROUPS" \
            "  Fix neodct/configs/users-table.txt (and its copy check in" \
            "  neodct/tests/test_defconfig_copies.py)." ;;
    esac
done

echo "[post-image] users: ndusr ($groups) and ndusr_ut are in this image"

#!/bin/sh
# platform-id.sh -- the ONE table mapping an image tag to what the image is.
#
#   platform-id.sh <image-tag> word      qemu | hw
#   platform-id.sh <image-tag> board     qemu-virt | luckfox-pico-mini-b
#   platform-id.sh <image-tag> cdefine   ND_PLATFORM_QEMU | ND_PLATFORM_HW
#   platform-id.sh --tags                every tag it accepts, one per line
#
# Anything it does not recognise prints nothing and exits 1. There is no
# "unknown" output: a caller that gets nothing has to decide what to do about
# it, and both callers decide to fail the build.
#
# ============ WHY THIS IS A FILE AND NOT A `case` IN ITS CALLER ============
#
# The table used to live in post-build-system-metadata.sh, which writes
# /NeoDCT/platform. It now has a second reader: buildroot/package/neodct/
# neodct.mk compiles the same fact into libneodct as ND_BUILD_PLATFORM, and
# nd_platform.c treats a DISAGREEMENT between the two as a mis-assembled image
# -- it claims neither machine and says so on every screen that reports one.
#
# A runtime check that hard is only defensible if disagreement cannot be
# produced by an ordinary editing slip. Two copies of this table would make the
# common cause of a mismatch "somebody added a board to one of them", and
# nd_selftest.c's own stated bar is that a check which cries wolf stops being
# read -- after which the real failure goes unread too. Derived from one table,
# in one file, a disagreement is structurally impossible for any image this
# tree builds, and what remains for the runtime check to catch is a
# hand-assembled or tampered image, which is what it is for.
#
# So adding an image tag is one line here and every reader gets it at once.
#
# qemu-armv7 is what the emulator IS now -- neodct_qemu_defconfig builds armv7
# cortex-a7, so the two images share one ABI and `uname -m` can no longer tell
# them apart. This table is the discriminator that replaces it.
#
# qemu-aarch64 STAYS, and the reason is one build, not sentiment. It is the
# update-compatibility key stamped into every QEMU image ever flashed, and
# nd_manifest_check_compatible() runs in the RUNNING image's libneodct -- so
# the D1 alias that lets such an image take a qemu-armv7 package can only ever
# execute on an image that both carries the new code AND still calls itself
# qemu-aarch64. This row is the only thing that lets one be built:
#
#   BR2_ROOTFS_POST_BUILD_SCRIPT_ARGS="... qemu-aarch64"
#
# on the armv7 defconfig maps to word=qemu, board=qemu-virt, ND_PLATFORM_QEMU,
# so the image is self-consistent -- an armv7 rootfs under the retired key,
# which the old image's bare strcmp accepts and whose new libneodct then
# carries the alias. Delete the row and that hop cannot be built, which
# strands every flashed qemu-aarch64 image for good. nd_manifest.c's alias
# block spells the whole migration out.
#
# It is NOT kept for the C fixtures. Nothing under neodct/src reads this file:
# the only callers are buildroot/package/neodct/neodct.mk,
# post-build-system-metadata.sh, post-build-prune-tests.sh and
# neodct/tests/test_post_build_prune.py, and `make -C neodct/src test` is
# green with the row gone. A justification that a reader can disprove in one
# grep is worse than none, because it is the row itself that gets deleted.
#
# luckfox-armv7 is untouched, because a field phone that stops recognising its
# own image stops taking updates.
#
# ============ AND WHY --tags EXISTS ============
#
# So that the refusals can name what they WOULD have accepted without holding a
# second copy of the list. post-build-system-metadata.sh's error message used
# to spell "qemu-aarch64 or luckfox-armv7" out, which is the same table again
# in prose -- it would go stale the first time a tag was added, and a refusal
# that names the wrong set is worse than one that names none. That is also why
# the table below is DATA rather than a `case`: a `case` cannot list itself.
#
# ============ AND WHY cdefine IS NOT A COLUMN ============
#
# It was one, and three headers stake a hard runtime refusal on the claim that
# the file and the constant "cannot disagree for any image this tree builds".
# A third independent column does not support that claim: it removes two-FILE
# drift and leaves intra-ROW drift, which is the same ordinary editing slip
# wearing a different hat. `qemu-armv7 qemu qemu-virt ND_PLATFORM_HW` builds
# clean -- both readers get a non-empty answer, so both guards pass -- and
# ships an image whose /NeoDCT/platform says qemu and whose libneodct says HW.
# Every process on it then logs MIS-ASSEMBLED IMAGE, nd_platform() names no
# machine for the life of every process, and nd-selftest FAILs, with both test
# suites green throughout and the one build-log line that shows the pair
# unasserted by anything.
#
# Derived from the word, that row cannot be written. A test could have caught
# it instead; deriving makes the three headers TRUE rather than merely tested,
# and there is no second fact here to hold -- "qemu" and ND_PLATFORM_QEMU are
# one fact spelled for two languages.
#
# busybox ash: no arrays, no [[, no local. This runs on the BUILD HOST, but it
# is a NeoDCT shell script and AGENTS.md's rule is the same everywhere.
set -eu

# tag             word  board
TABLE="
qemu-aarch64      qemu  qemu-virt
qemu-armv7        qemu  qemu-virt
luckfox-armv7     hw    luckfox-pico-mini-b
"

if [ "${1:-}" = "--tags" ]; then
    printf '%s\n' "$TABLE" | while read -r _tag _rest; do
        case "$_tag" in
            "") continue ;;
        esac
        printf '%s\n' "$_tag"
    done
    exit 0
fi

TAG="${1:-}"
FIELD="${2:-}"

# A path is somebody else's argument, not an image tag; so is the empty string.
# Neither can match a row, so both fall out of the lookup as a refusal -- the
# same answer a typo gets, which is the right one for all three.
#
# The loop runs in a subshell (it is the right-hand side of a pipe), so it
# cannot assign to anything out here; it prints the row it found and emptiness
# is the miss.
ROW="$(printf '%s\n' "$TABLE" | while read -r _tag _word _board; do
    case "$_tag" in
        "") continue ;;
    esac
    [ "$_tag" = "$TAG" ] || continue
    printf '%s %s\n' "$_word" "$_board"
done)"
[ -n "$ROW" ] || exit 1

# Deliberately unquoted: the row is two fields this file wrote, and splitting
# them is the point.
# shellcheck disable=SC2086
set -- $ROW

case "$FIELD" in
    word)  printf '%s\n' "$1" ;;
    board) printf '%s\n' "$2" ;;
    cdefine)
        # The nd_platform_t enumerator for the word, and the `*` arm is not
        # dead code: it is what stops a word this mapping has never heard of
        # from reaching neodct.mk as an empty ND_BUILD_PLATFORM, which its
        # guard then reports as an unmappable image tag rather than as the
        # unmapped WORD it actually is.
        case "$1" in
            qemu) printf 'ND_PLATFORM_QEMU\n' ;;
            hw)   printf 'ND_PLATFORM_HW\n' ;;
            *)    exit 1 ;;
        esac
        ;;
    *) exit 1 ;;
esac

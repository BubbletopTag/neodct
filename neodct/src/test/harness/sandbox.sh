#!/bin/sh
# sandbox.sh -- run a command with this machine out of its reach.
#
#     test/harness/sandbox.sh CMD [ARGS...]
#
# The test suite spawns real programs. On 2026-08-31 and again on 2026-09-04
# one of them was poweroff(8), and the developer's workstation went down: a
# test process on a desktop has the same rights as the person at the
# keyboard, and logind lets that person switch the machine off. So `make
# test` no longer runs the binaries in the developer's session at all. It
# runs them in here, a bubblewrap container that keeps every way of reaching
# the machine out of the process's hands:
#
#   --unshare-user         no capabilities at all, whatever the uid
#   --tmpfs /run           no D-Bus, so systemctl/poweroff/loginctl have
#                          nobody to talk to ("Can't operate.")
#   --unshare-net          no network: no relay tunnel, no SNTP, no sshd
#                          listening on the developer's ports, and no
#                          default route -- which the simulated signal bars
#                          treat as offline, deterministically. Also no
#                          Bluetooth: AF_BLUETOOTH sockets exist only in the
#                          initial network namespace, so from in here the
#                          kernel looks as if CONFIG_BT were off, and
#                          test_bluetooth skips the cases that need it.
#   --dev /dev             a fresh minimal /dev: no /dev/snd (the speakers),
#                          no /dev/uinput (the keyboard), no /dev/fb0, no
#                          ttyUSB (the modem), no i2c, no input devices
#   --ro-bind / /          everything read-only except the checkout, so a
#                          test cannot touch $HOME or a real /NeoDCT
#   --bind <tmp> /tmp      a private /tmp on disk under build/, so the
#                          scratch roots neither fill the real tmpfs nor
#                          collide with another session's
#
# bwrap is unprivileged: it needs only user namespaces, which this kernel
# allows (kernel.unprivileged_userns_clone=1). If it is missing, this script
# REFUSES rather than falling back to a bare run -- a fallback nobody notices
# is exactly how the machine went down the first time. A disposable VM or
# container that genuinely has nothing to lose may opt out with
# NEODCT_TEST_SANDBOX=none, and says so in the run's output.
#
# What the sandbox does NOT replace: the halt interlock in nd_svc.c (a
# non-root, test-root or disarmed process never resolves a real halt) and
# the fake verbs test/harness/fakebin puts on $PATH. Those hold on any
# machine; this is the layer that holds even if they did not.

set -u

HARNESS=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HARNESS/../../../.." && pwd)

if [ "$#" -eq 0 ]; then
    echo "usage: sandbox.sh CMD [ARGS...]" >&2
    exit 2
fi

case "${NEODCT_TEST_SANDBOX:-bwrap}" in
none)
    echo "sandbox.sh: NEODCT_TEST_SANDBOX=none -- running WITHOUT the container." >&2
    echo "sandbox.sh: only do this on a machine that has nothing to lose." >&2
    NEODCT_TEST_SANDBOX=none
    export NEODCT_TEST_SANDBOX
    exec "$@"
    ;;
bwrap) ;;
*)
    echo "sandbox.sh: NEODCT_TEST_SANDBOX must be 'bwrap' (default) or 'none'" >&2
    exit 2
    ;;
esac

if ! command -v bwrap >/dev/null 2>&1; then
    echo "sandbox.sh: bwrap (bubblewrap) is not installed, so the tests cannot be" >&2
    echo "sandbox.sh: run safely on this machine. Install it (pacman -S bubblewrap," >&2
    echo "sandbox.sh: apt install bubblewrap), or run the suite in a disposable" >&2
    echo "sandbox.sh: VM with NEODCT_TEST_SANDBOX=none." >&2
    exit 2
fi
if ! bwrap --unshare-user --ro-bind / / -- /bin/true >/dev/null 2>&1; then
    echo "sandbox.sh: bwrap cannot create a user namespace here. Check" >&2
    echo "sandbox.sh: /proc/sys/kernel/unprivileged_userns_clone and" >&2
    echo "sandbox.sh: /proc/sys/user/max_user_namespaces. Refusing a bare run." >&2
    exit 2
fi

TMP=${NEODCT_SANDBOX_TMP:-$REPO/neodct/src/build/tmp}
mkdir -p "$TMP" || exit 2
TMP=$(cd "$TMP" && pwd) # bwrap wants it absolute; the Makefile and the gate may not pass it so

NEODCT_TEST_SANDBOX=bwrap
export NEODCT_TEST_SANDBOX

exec bwrap \
    --unshare-user --unshare-net --unshare-pid --unshare-ipc --unshare-uts \
    --die-with-parent \
    --ro-bind / / \
    --ro-bind /sys /sys \
    --dev /dev \
    --proc /proc \
    --tmpfs /run \
    --bind "$REPO" "$REPO" \
    --bind "$TMP" /tmp \
    -- "$@"

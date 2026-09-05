#!/bin/sh
# run-tests.sh -- the loop `make test` used to carry inline, run INSIDE
# sandbox.sh. Arguments are the test binaries. Needs NEODCT_GOLDEN and
# LD_LIBRARY_PATH from the Makefile.
#
# Two things this adds to the loop it replaced:
#
#   - $PATH starts with test/harness/fakebin, so poweroff, reboot, systemctl
#     and friends resolve to scripts that refuse and write a line to
#     $NEODCT_FAKEBIN_LOG. A non-empty log at the end FAILS the run: a test
#     reached a verb that should have been unreachable, and that is a bug
#     to look at, not noise.
#   - NEODCT_TEST_HARNESS=1, which is what test/harness/nd_testguard.c
#     (linked into every test binary) looks for before it lets main() run.
#     A binary started any other way stops with a message pointing here.
#
# NEODCT_ROOT is set per binary, as before, to a scratch root that is
# removed afterwards.

set -u

HARNESS=$(cd "$(dirname "$0")" && pwd)

if [ "$#" -eq 0 ]; then
    printf '  %-7s %s\n' 'SKIP' 'run-tests.sh: nothing to run'
    exit 0
fi

NEODCT_TEST_HARNESS=1
NEODCT_FAKEBIN="$HARNESS/fakebin"
PATH="$NEODCT_FAKEBIN:$PATH"
export NEODCT_TEST_HARNESS NEODCT_FAKEBIN PATH

log=$(mktemp) || exit 2
NEODCT_FAKEBIN_LOG="$log"
export NEODCT_FAKEBIN_LOG

# 0711 rather than mktemp's 0700: it is the parent of every per-case root,
# and a child that dropped to ndusr_ut has to be able to traverse it. The
# phone's own shape -- traversable by everyone, listable by nobody.
root=$(mktemp -d) || exit 2
chmod 0711 "$root"

# One binary may not stall the run. Ten minutes is several times the longest
# suite member under ASan; a test still going after that is waiting for a
# key, a device or a network that the sandbox does not have, and the useful
# output is "timed out", not a make that never returns. -k gives a test that
# ignores SIGTERM five seconds before SIGKILL.
per_test=${NEODCT_TEST_TIMEOUT_S:-600}

fail=0
for t in "$@"; do
    printf '  %-7s %s\n' 'TEST' "$t"
    NEODCT_ROOT="$root" NEODCT_COLOR=1 timeout -k 5 "$per_test" "$t"
    rc=$?
    if [ "$rc" -eq 124 ]; then
        echo "FAIL $t: timed out after ${per_test}s"
        fail=1
    elif [ "$rc" -ne 0 ]; then
        fail=1
    fi
done
rm -rf "$root"

if [ -s "$log" ]; then
    echo "SAFETY: a test reached a system verb the harness had to refuse:"
    cat "$log"
    fail=1
fi
rm -f "$log"

if [ "$fail" -ne 0 ]; then
    echo "FAILED"
    exit 1
fi
echo "all tests passed"

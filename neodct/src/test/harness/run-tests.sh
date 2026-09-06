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
#
# ============ AND ONE LINE THAT SAYS WHAT THIS RUN COULD NOT TEST ============
#
# Added after 0.5.8b, in which eight releases of a phone that made no sound
# at all shipped behind a green suite.
#
# The privilege drop is not a mode bit, it is a capability: setgroups(2)
# needs CAP_SETGID, so a process that is ALREADY ndusr cannot hand a child to
# ndusr -- it can only kill it at exit 122 before execve. That is what
# silenced the phone. The suite could not see it and STRUCTURALLY CANNOT,
# for a reason worth printing rather than remembering:
#
#   on a build host getpwnam("ndusr") misses, so nd_priv_lookup() leaves
#   run_as invalid and nd_priv_become() is a documented no-op -- the drop
#   never happens and the working branch is taken;
#
#   inside QEMU `make test` runs as root, so the drop genuinely succeeds.
#
# Neither environment is the one that breaks and neither ever will be. The
# only uid that breaks it is an unprivileged process that is already ndusr,
# and no test process ever has it.
#
# So this cannot be fixed by asserting harder. What it CAN do is stop the
# green from being read as coverage: the banner below names the configuration
# this run was in, and the closing line repeats the caveat beside "all tests
# passed". A person who knows the suite never exercised the drop will not
# conclude from a pass that the drop works. nd-selftest, run on the phone, is
# where that question is actually answered -- its `tone` section forks twice
# to reach the failing configuration on purpose.

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

# grep rather than getent or id: the sandbox binds /etc read-only off the
# host and busybox may have neither applet, and a missing applet inside a
# command substitution is an empty string rather than an error -- which would
# make this quietly claim the wrong configuration, which is the failure it
# exists to describe.
have_user() {
    grep -q "^$1:" /etc/passwd 2>/dev/null
}

drop_testable=0
if [ "$(id -u 2>/dev/null || echo 1)" = "0" ]; then
    drop_note="running as ROOT, so every drop SUCCEEDS and no test reaches the failure"
elif have_user ndusr && have_user ndusr_ut; then
    drop_testable=1
    drop_note="ndusr and ndusr_ut exist here, so the real drop IS exercised"
elif have_user ndusr || have_user ndusr_ut; then
    drop_note="only one of ndusr/ndusr_ut exists here, so half the drop is exercised"
else
    drop_note="no ndusr/ndusr_ut here, so nd_priv_become() is a no-op and NO test exercises a drop"
fi
printf '  %-7s %s\n' 'CONFIG' "$drop_note"

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
if [ "$drop_testable" -eq 0 ]; then
    echo "  ...but NOT the privilege drop: $drop_note."
    echo "  A green suite is not evidence that a spawn reaches execve on the phone."
    echo "  Run nd-selftest there; its 'tone' and 'browser' sections ask the kernel."
fi

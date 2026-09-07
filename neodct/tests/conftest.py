# Host-side unit tests for NeoDCT (never shipped to the target).
# Make /NeoDCT-style absolute imports (System.hw..., System.ui...) work
# by putting the overlay root on sys.path, same as the device runtime.
import os
import sys
import tempfile

import pytest

# The Python OS is the reference implementation and lives outside the
# overlay, so BR2_ROOTFS_OVERLAY cannot put it on a phone. Importing
# System.* is importing that reference.
OVERLAY_NEODCT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "python-reference",
)
if OVERLAY_NEODCT not in sys.path:
    sys.path.insert(0, OVERLAY_NEODCT)


# ============ THE HALT VERBS CANNOT BE REACHED FROM HERE ============
#
# The C suite has had this since 0.5.8b and the reason is on the record in
# AGENTS.md: on 2026-08-31 and 2026-09-04 a test reached poweroff(8) and
# switched off the workstation running it. test/harness/run-tests.sh answered
# that by putting a fakebin first on $PATH and failing the run if anything in
# it was ever called.
#
# This suite had no such thing. It sources the real busybox helpers out of the
# overlay -- neodct-sdcard, S00userdata, the initramfs applier -- and those
# call mount, umount, poweroff and reboot for real. Every one of them is
# stubbed per-test today, by a shell function or a monkeypatched Popen, and
# per-test containment has already failed once: running the suite as root
# (which is how it runs inside QEMU) left three bind mounts of the host's own
# root device behind under /tmp/pytest-of-root, because do_layout() ends in
# mount_untrusted_noexec() and that one path had no stub.
#
# So the verbs are taken away from the whole suite rather than from each test
# that remembers to. A shell function still wins where a test defines one --
# this is a floor, not a replacement for the stubs that assert what was asked.
#
#   the halt family   refuse AND fail the test. Nothing here has any business
#                     reaching them, so reaching one is a bug to look at.
#   mount/umount      refuse. That is what they do for the non-root user these
#                     tests were written for, so it changes no expectation --
#                     it only stops a root run from doing it for real.
#
# Not a fixture that yields and cleans up: $PATH has to be set before any test
# module runs a subprocess, including at collection time.

_FAKEBIN_HALT = ("poweroff", "reboot", "halt", "shutdown", "telinit",
                 "systemctl", "init")
_FAKEBIN_MOUNT = ("mount", "umount")

_FAKEBIN = tempfile.mkdtemp(prefix="neodct-fakebin-")
FAKEBIN_LOG = os.path.join(_FAKEBIN, "reached.log")


def _install_fakebin():
    for name in _FAKEBIN_HALT + _FAKEBIN_MOUNT:
        path = os.path.join(_FAKEBIN, name)
        with open(path, "w") as handle:
            handle.write(
                "#!/bin/sh\n"
                "printf '%%s %%s\\n' %s \"$*\" >> '%s'\n"
                "echo '%s: refused by the NeoDCT host test harness' >&2\n"
                "exit 1\n" % (name, FAKEBIN_LOG, name))
        os.chmod(path, 0o755)
    os.environ["PATH"] = _FAKEBIN + os.pathsep + os.environ.get("PATH", "")
    os.environ["NEODCT_FAKEBIN_LOG"] = FAKEBIN_LOG


_install_fakebin()


@pytest.fixture(autouse=True)
def _no_test_reaches_a_halt_verb():
    """Fail the test that called one, rather than the run that noticed later.

    mount and umount are deliberately NOT in here: refusing them is the point,
    and a helper attempting one is ordinary behaviour worth no complaint."""
    before = _reached()
    yield
    new = [line for line in _reached() if line not in before
           or _reached().count(line) > before.count(line)]
    halted = [line for line in new if line.split(" ", 1)[0] in _FAKEBIN_HALT]
    assert not halted, (
        "this test reached a verb that would have switched the machine off: %s"
        % halted)


def _reached():
    if not os.path.exists(FAKEBIN_LOG):
        return []
    with open(FAKEBIN_LOG) as handle:
        return handle.read().splitlines()

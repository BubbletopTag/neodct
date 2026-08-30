"""/NeoDCT/User/env.sh is a root shell from writable storage. It needs a gate.

SECURITY-AUDIT.md section 4 Q5 vector 2, rated Critical: run_neodct.sh sourced
it as uid 0 on every boot, by design, so a developer could set NEODCT_T9=1
without rebuilding a read-only image. Anything that could write one file got a
permanent root backdoor -- and one that survives an update, because an update
replaces the rootfs and never touches /NeoDCT/User.

The feature stays. What it cannot be is unconditional, and the gate has to be
something the writable partition cannot set: the kernel cmdline, or a file in
the verity-protected rootfs. These tests drive the real script with both, and
with neither.

Engineering mode is deliberately not a gate and there is a test for that too:
it lives in settings.prop, on the same writable partition, so an attacker who
can drop env.sh can turn it on in the same breath.
"""

import os
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN_SH = os.path.join(ROOT, "overlay", "bin", "run_neodct.sh")


def run_boot(tmp_path, cmdline="", marker=False, env_sh="export NEODCT_T9=1\n"):
    """Run the script's env.sh block against a fake root.

    The script is not sourceable as a whole -- it ends by exec'ing nd-core --
    so the block under test is extracted and run with the paths redirected.
    That keeps the assertion on the shipped text rather than on a copy of it:
    if the block moves or its condition changes, the extraction fails loudly.
    """
    fake = tmp_path / "root"
    (fake / "NeoDCT" / "User").mkdir(parents=True, exist_ok=True)
    (fake / "etc").mkdir(exist_ok=True)
    (fake / "proc").mkdir(exist_ok=True)
    (fake / "proc" / "cmdline").write_text(cmdline + "\n")
    if env_sh is not None:
        (fake / "NeoDCT" / "User" / "env.sh").write_text(env_sh)
    if marker:
        (fake / "etc" / "neodct-devenv").write_text("")

    source = open(RUN_SH).read()
    start = source.index("NEODCT_DEVENV=\"\"")
    end = source.index("# nd-core replaces")
    block = source[start:end]
    assert "env.sh" in block, "the env.sh block moved; update this test"

    # Redirect the three absolute paths at the fake root, and /dev/tty0 at
    # stdout so the decision is visible to the assertions.
    block = (block
             .replace("/etc/neodct-devenv", str(fake / "etc" / "neodct-devenv"))
             .replace("/proc/cmdline", str(fake / "proc" / "cmdline"))
             .replace("/NeoDCT/User/env.sh",
                      str(fake / "NeoDCT" / "User" / "env.sh"))
             .replace("> /dev/tty0", ""))
    script = block + '\necho "T9=${NEODCT_T9:-unset}"\n'
    return subprocess.run(["sh", "-c", script], capture_output=True, text=True)


def test_a_plain_boot_does_not_source_env_sh(tmp_path):
    """The shipped default: no marker, no cmdline flag, env.sh ignored."""
    result = run_boot(tmp_path)

    assert "T9=unset" in result.stdout
    assert "IGNORING" in result.stdout


def test_the_refusal_says_how_to_turn_it_on(tmp_path):
    """A developer whose NEODCT_T9=1 stopped working must not have to read
    the boot script to find out why."""
    result = run_boot(tmp_path)

    assert "neodct.devenv=1" in result.stdout


def test_the_kernel_cmdline_enables_it(tmp_path):
    result = run_boot(tmp_path, cmdline="console=ttyAMA0 neodct.devenv=1 quiet")

    assert "T9=1" in result.stdout
    assert "Sourcing" in result.stdout


def test_a_rootfs_marker_enables_it(tmp_path):
    """The other trust root: a file in the read-only, verity-covered image."""
    result = run_boot(tmp_path, marker=True)

    assert "T9=1" in result.stdout


def test_a_lookalike_cmdline_word_does_not_enable_it(tmp_path):
    """Substring matching here would be a hole: neodct.devenv=0 must not read
    as neodct.devenv=1, and neither must some other option that contains it."""
    for cmdline in ("neodct.devenv=0", "xneodct.devenv=1",
                    "other=neodct.devenv=1", "neodct.devenv=11"):
        result = run_boot(tmp_path, cmdline=cmdline)

        assert "T9=unset" in result.stdout, cmdline


def test_no_env_sh_at_all_is_silent(tmp_path):
    """A phone with nothing on the partition should say nothing about it."""
    result = run_boot(tmp_path, env_sh=None)

    assert "IGNORING" not in result.stdout
    assert "Sourcing" not in result.stdout


def test_engineering_mode_is_not_a_gate():
    """settings.prop is on the writable partition, which is the partition the
    attacker already wrote env.sh to. It must not appear in this decision."""
    source = open(RUN_SH).read()
    start = source.index("NEODCT_DEVENV=\"\"")
    end = source.index("# nd-core replaces")
    block = source[start:end]

    assert "settings.prop" not in block
    assert "engineering" not in block.lower().replace(
        "engineering mode is deliberately not accepted", "")


def test_the_emulator_still_passes_the_flag():
    """run_qemu.sh is the developer path this feature exists for; if it does
    not set the flag, the gate reads as "the feature was removed"."""
    qemu = open(os.path.join(ROOT, "tools", "run_qemu.sh")).read()

    assert "neodct.devenv=1" in qemu
    assert "NEODCT_DEVENV" in qemu

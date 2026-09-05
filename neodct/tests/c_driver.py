"""Run a C test binary as a driver -- `test_manifest --parse`, `test_package
--dump` -- the only way one may be started: inside test/harness/sandbox.sh.

Every test binary links test/harness/nd_testguard.c, which refuses to run
unless the harness started it, because the suite once reached poweroff(8)
and switched off the machine running it (AGENTS.md, Tests). A driver mode
reads a file and prints what it saw; it could not halt anything. But the
guard cannot know that, and should not have to: the rule is one rule, and
verify-c-build.sh goes through the sandbox for its glyph check the same way.

The sandbox hides the host's /tmp, so a file the test wants the driver to
read -- or a directory it wants it to extract into -- has to live under the
checkout, which the sandbox binds read-write at its own path. The tmp_path
fixture below puts a test's scratch directory there; a test module that
drives a binary imports it, and every path it builds from tmp_path is then
the same path on both sides of the container.
"""

import os
import pathlib
import shutil
import subprocess
import tempfile

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # neodct/
SRC = os.path.join(REPO, "src")
SANDBOX = os.path.join(SRC, "test", "harness", "sandbox.sh")
SANDBOX_TMP = os.path.join(SRC, "build", "tmp")
LIBDIR = os.path.join(SRC, "build", "default", "lib")


def run(driver, *args, check=True):
    """subprocess.run() of `driver args...` inside the sandbox, output captured."""
    if shutil.which("bwrap") is None:
        pytest.skip("bwrap (bubblewrap) is not installed; a C test binary "
                    "runs only inside the sandbox (AGENTS.md, Tests)")
    os.makedirs(SANDBOX_TMP, exist_ok=True)
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = LIBDIR
    env["NEODCT_TEST_HARNESS"] = "1"
    env["NEODCT_SANDBOX_TMP"] = SANDBOX_TMP
    env.pop("NEODCT_ROOT", None)
    return subprocess.run([SANDBOX, str(driver)] + [str(a) for a in args],
                          capture_output=True, check=check, env=env)


@pytest.fixture
def tmp_path():
    """pytest's tmp_path, relocated under the checkout so the driver can see it."""
    base = os.path.join(SANDBOX_TMP, "pytest")
    os.makedirs(base, exist_ok=True)
    d = tempfile.mkdtemp(prefix="case-", dir=base)
    try:
        yield pathlib.Path(d)
    finally:
        shutil.rmtree(d, ignore_errors=True)

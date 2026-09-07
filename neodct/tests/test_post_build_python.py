"""No Python reaches the image, and the pass that ensures it cannot fail open.

This file used to assert the opposite, and it is worth saying why rather than
just replacing it. The overlay carried the whole Python OS -- 127 files -- and
post-build-prune-tests.sh compiled it in place, because /NeoDCT is a read-only
squashfs and an interpreter that cannot cache bytecode beside its source pays
for the import every boot: System.ui.framework measured 4.0 MB from source
against 0.4 MB from a .pyc.

Nothing on the phone runs Python any more. /bin/run_neodct.sh execs nd-core,
nd-apprun dlopen()s an app's app.so, and the reference sources live in
neodct/python-reference/, outside BR2_ROOTFS_OVERLAY. So the compile step was
removed and a delete step put in its place -- and the four tests here went on
asserting that a __pycache__ appeared, which made them fail on every run from
the day the port finished. A permanently red test is not a safety net; it is
noise that hides the next real failure, so these now check what the script
actually promises.

The promise is a backstop, not a formality. The overlay is a directory anybody
can drop a file into, and a stray .py under /NeoDCT is not a small mistake:
`ls /NeoDCT/System/core` showing a service full of .py files and no sign of
the service running is exactly the wrong thing for the next person debugging
this phone over serial to find.
"""

import os
import subprocess

SCRIPT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "scripts", "post-build-prune-tests.sh",
)


def _target(tmp_path):
    """A stand-in TARGET_DIR with Python in every shape the pass removes."""
    core = tmp_path / "NeoDCT" / "System" / "core"
    (core / "ModemService").mkdir(parents=True)
    (core / "ModemService" / "__init__.py").write_text("VALUE = 1\n")
    (core / "ModemService" / "__pycache__").mkdir()
    (core / "ModemService" / "__pycache__" / "__init__.cpython-311.pyc").write_bytes(b"\x00")
    (core / "thing.py").write_text("VALUE = 1\n")
    (core / "thing.py.old").write_text("VALUE = 0\n")
    (core / "t9.dict").write_text("a\n")
    (tmp_path / "NeoDCT" / "System" / "apps").mkdir(parents=True, exist_ok=True)
    # Not under /NeoDCT: the pass is scoped, and a package's own Python is
    # none of its business.
    (tmp_path / "usr" / "lib").mkdir(parents=True)
    (tmp_path / "usr" / "lib" / "keep.py").write_text("VALUE = 2\n")
    return tmp_path


def _run(target, **env):
    return subprocess.run(
        [SCRIPT, str(target), "qemu-aarch64"],
        env={**os.environ, **env},
        capture_output=True, text=True, timeout=120)


def test_every_shape_of_python_is_removed(tmp_path):
    target = _target(tmp_path)

    result = _run(target)

    assert result.returncode == 0, result.stderr
    left = [os.path.join(root, name)
            for root, _dirs, files in os.walk(target / "NeoDCT")
            for name in files
            if name.endswith((".py", ".pyc", ".pyo", ".py.old"))]
    assert left == [], left
    caches = [root for root, dirs, _files in os.walk(target / "NeoDCT")
              for name in dirs if name == "__pycache__"]
    assert caches == [], caches


def test_it_says_how_many_it_dropped(tmp_path):
    """A silent pass cannot be told from a pass that found nothing, and on a
    clean tree finding nothing is the expected answer -- so the count is the
    only thing that distinguishes "the overlay is clean" from "the glob
    broke"."""
    target = _target(tmp_path)

    result = _run(target)

    # Three, not four: the __pycache__ directories are removed wholesale by an
    # earlier pass, so the .pyc inside one is already gone by the time the
    # count is taken. The number is about what is left to find.
    assert "dropped 3 Python files from the image" in result.stdout, result.stdout


def test_a_service_directory_left_empty_goes_with_them(tmp_path):
    """System/core is t9.dict and directories and nothing else, so a
    directory there with nothing in it is a leftover rather than something
    somebody meant."""
    target = _target(tmp_path)

    _run(target)

    assert not (target / "NeoDCT" / "System" / "core" / "ModemService").exists()
    # ...and a directory that still has something in it stays, along with the
    # file that kept it.
    assert (target / "NeoDCT" / "System" / "core" / "t9.dict").exists()


def test_nothing_outside_NeoDCT_is_touched(tmp_path):
    """Scoped deliberately: /usr is full of Python that packages installed and
    that the image is entitled to keep."""
    target = _target(tmp_path)

    _run(target)

    assert (target / "usr" / "lib" / "keep.py").exists()


def test_the_escape_hatch_keeps_it_all(tmp_path):
    """NEODCT_KEEP_PYTHON=1, for a build somebody is deliberately putting an
    interpreter back into."""
    target = _target(tmp_path)

    result = _run(target, NEODCT_KEEP_PYTHON="1")

    assert result.returncode == 0, result.stderr
    assert (target / "NeoDCT" / "System" / "core" / "thing.py").exists()
    assert (target / "NeoDCT" / "System" / "core" / "ModemService"
            / "__init__.py").exists()

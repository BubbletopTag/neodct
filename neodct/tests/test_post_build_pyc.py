"""The UI's bytecode belongs in the image, not on the user partition.

/NeoDCT is read-only squashfs, so python cannot drop .pyc files beside the
.py files at runtime. Compiling a module instead costs real memory that is
never given back: measured on the device, System.ui.framework costs 4.0 MB
to import from source and 0.4 MB to import from bytecode.

Shipping the bytecode in the image is better than caching it on the user
partition, which is what used to happen:

  * it is there on the very first boot, including the first boot after an
    update, which is exactly when the phone is slowest;
  * it survives a user-data reset;
  * it is inside the dm-verity tree, so it is covered by the same
    signature as the code it was compiled from. Bytecode on the writable
    partition is not, and python trusts a .pyc over its source.
"""

import os
import subprocess
import sys

SCRIPT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "scripts", "post-build-prune-tests.sh",
)


def _target(tmp_path):
    """A stand-in TARGET_DIR with one module in it."""
    pkg = tmp_path / "NeoDCT" / "System" / "core"
    pkg.mkdir(parents=True)
    (pkg / "thing.py").write_text("VALUE = 1\n")
    (tmp_path / "NeoDCT" / "System" / "apps").mkdir(parents=True, exist_ok=True)
    return tmp_path


def _run(target):
    host = target / "hostdir" / "bin"
    host.mkdir(parents=True, exist_ok=True)
    link = host / "python3"
    if not link.exists():
        os.symlink(sys.executable, link)
    return subprocess.run(
        [SCRIPT, str(target), "qemu-aarch64"],
        env={**os.environ, "HOST_DIR": str(target / "hostdir")},
        capture_output=True, text=True, timeout=120)


def test_bytecode_is_compiled_into_the_image(tmp_path):
    target = _target(tmp_path)
    result = _run(target)
    assert result.returncode == 0, result.stderr

    cache = target / "NeoDCT" / "System" / "core" / "__pycache__"
    assert cache.is_dir(), "no __pycache__ produced"
    assert list(cache.glob("thing.*.pyc")), sorted(os.listdir(cache))


def test_the_source_is_kept_beside_the_bytecode(tmp_path):
    # Tracebacks on a phone with no debugger are worth the few kB.
    target = _target(tmp_path)
    _run(target)
    assert (target / "NeoDCT" / "System" / "core" / "thing.py").exists()


def test_the_bytecode_records_runtime_paths(tmp_path):
    """A .pyc records the source path it was built from.

    Compiled naively that is the build directory, so every traceback on
    the phone would name a path that exists only on the build machine.
    """
    target = _target(tmp_path)
    _run(target)
    cache = target / "NeoDCT" / "System" / "core" / "__pycache__"
    blob = next(iter(cache.glob("thing.*.pyc"))).read_bytes()
    assert b"/NeoDCT/System/core/thing.py" in blob
    assert bytes(str(target), "utf-8") not in blob


def test_a_failed_compile_does_not_fail_the_image_build(tmp_path):
    """Bytecode is an optimisation. Losing it must cost the image nothing.

    The script runs under `set -e`, so an unguarded failure here would
    abort the whole build after everything else had already succeeded.
    """
    target = _target(tmp_path)
    host = target / "brokenhost" / "bin"
    host.mkdir(parents=True)
    broken = host / "python3"
    broken.write_text("#!/bin/sh\nexit 1\n")
    broken.chmod(0o755)

    result = subprocess.run(
        [SCRIPT, str(target), "qemu-aarch64"],
        env={**os.environ, "HOST_DIR": str(target / "brokenhost")},
        capture_output=True, text=True, timeout=120)

    assert result.returncode == 0, result.stderr
    assert "shipping source only" in result.stdout
    # and the image still has its source, so the phone still boots
    assert (target / "NeoDCT" / "System" / "core" / "thing.py").exists()

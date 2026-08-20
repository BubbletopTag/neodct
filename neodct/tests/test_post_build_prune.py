"""post-build-prune-tests.sh: what must not survive into the image.

BR2_ROOTFS_OVERLAY copies over the target tree and never deletes, and
buildroot does not rebuild target/ between builds. An app removed from the
overlay therefore stays installed in every image built in that output
directory until someone runs `make clean` -- and it is still scanned by the
launcher, so it is still in the app grid. That is not theoretical: a
deleted app shipped inside a signed update once already.
"""

import os
import subprocess

SCRIPT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "scripts", "post-build-prune-tests.sh",
)
APPS = os.path.join("NeoDCT", "System", "apps")
ENGINEERING_APPS = os.path.join("NeoDCT", "System", "engineering", "apps")


def _overlay_apps():
    overlay = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "overlay", APPS,
    )
    return sorted(name for name in os.listdir(overlay)
                  if os.path.isdir(os.path.join(overlay, name))
                  and name != "__pycache__")


def _make_target(tmp_path, app_names, engineering=()):
    target = tmp_path / "target"
    for name in app_names:
        directory = target / APPS / name
        directory.mkdir(parents=True)
        (directory / "main.py").write_text("def run(ui):\n    pass\n")
    for name in engineering:
        directory = target / ENGINEERING_APPS / name
        directory.mkdir(parents=True)
        (directory / "main.py").write_text("def run(ui):\n    pass\n")
    return target


def _run(target):
    return subprocess.run([SCRIPT, str(target), "qemu-aarch64"],
                          capture_output=True, text=True, check=True)


def test_an_app_deleted_from_the_overlay_is_dropped_from_the_image(tmp_path):
    real = _overlay_apps()[0]
    target = _make_target(tmp_path, [real, "Forwarding"])

    _run(target)

    assert (target / APPS / real).is_dir()
    assert not (target / APPS / "Forwarding").exists()


def test_apps_that_are_still_in_the_overlay_are_kept(tmp_path):
    names = _overlay_apps()
    target = _make_target(tmp_path, names)

    _run(target)

    for name in names:
        assert (target / APPS / name).is_dir(), name


def test_the_engineering_apps_are_pruned_the_same_way(tmp_path):
    target = _make_target(tmp_path, [], engineering=["NoSuchEngineeringApp"])

    _run(target)

    assert not (target / ENGINEERING_APPS / "NoSuchEngineeringApp").exists()


def test_pruning_says_what_it_removed(tmp_path):
    """Silence here is how a stale app rides along unnoticed."""
    target = _make_target(tmp_path, ["Forwarding"])

    result = _run(target)

    assert "Forwarding" in result.stdout


def test_a_target_without_an_apps_directory_is_not_an_error(tmp_path):
    """The script also runs on trees that have no overlay applied yet."""
    target = tmp_path / "target"
    (target / "usr" / "bin").mkdir(parents=True)

    _run(target)

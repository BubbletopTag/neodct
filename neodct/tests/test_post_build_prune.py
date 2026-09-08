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


def test_an_unrecognised_platform_id_stops_the_build(tmp_path):
    """It used to fall through as PLATFORM=unknown and keep going.

    This script is the reason its sibling refuses: reading $2 instead of the
    last argument made the luckfox branch silently false, and real hardware
    shipped the generic inittab for several releases with nothing in the build
    saying so. An unrecognised tag has the same shape -- the image is
    assembled, it looks finished, and the console config was chosen by
    accident."""
    target = tmp_path / "target"
    target.mkdir()

    result = subprocess.run([SCRIPT, str(target), "luckfox-armv8"],
                            capture_output=True, text=True)

    assert result.returncode == 1, result.stdout + result.stderr
    assert 'Unknown platform id "luckfox-armv8"' in result.stderr


def test_the_refusal_names_the_tags_it_would_have_accepted(tmp_path):
    """Asked of platform-id.sh rather than spelled out here, so it cannot name
    a stale set the day somebody adds a tag. A refusal that lists the wrong
    accepted values is worse than one that lists none."""
    target = tmp_path / "target"
    target.mkdir()

    result = subprocess.run([SCRIPT, str(target), "nonsense"],
                            capture_output=True, text=True)

    for tag in ("qemu-armv7", "luckfox-armv7"):
        assert tag in result.stderr, result.stderr


def _inittab_target(tmp_path):
    target = tmp_path / "target"
    (target / "etc").mkdir(parents=True)
    (target / "etc" / "inittab").write_text("generic\n")
    (target / "etc" / "inittab.luckfox").write_text("ttyFIQ0 getty\n")
    return target


def test_the_phone_gets_the_console_it_has(tmp_path):
    """inittab.luckfox puts a getty on ttyFIQ0, which exists on the Pico Mini
    and nowhere else. There has been no coverage of this until now, and it is
    the thing that shipped wrong for several releases."""
    target = _inittab_target(tmp_path)

    subprocess.run([SCRIPT, str(target), "luckfox-armv7"],
                   capture_output=True, text=True, check=True)

    assert (target / "etc" / "inittab").read_text() == "ttyFIQ0 getty\n"


def test_the_emulator_does_not_get_a_console_it_does_not_have(tmp_path):
    """The decision comes from platform-id.sh's board column, not from a
    string prefix on the tag. `board` and not `word` is deliberate: a future
    devkit-x is also platform=hw and also has no ttyFIQ0."""
    target = _inittab_target(tmp_path)

    subprocess.run([SCRIPT, str(target), "qemu-armv7"],
                   capture_output=True, text=True, check=True)

    assert (target / "etc" / "inittab").read_text() == "generic\n"


def test_the_flavour_specific_inittab_never_ships(tmp_path):
    """Either way it is build scaffolding, and an extra init config sitting in
    /etc of a read-only rootfs is a thing somebody will one day copy over the
    real one to see what happens."""
    for platform in ("luckfox-armv7", "qemu-armv7"):
        target = _inittab_target(tmp_path / platform)

        subprocess.run([SCRIPT, str(target), platform],
                       capture_output=True, text=True, check=True)

        assert not (target / "etc" / "inittab.luckfox").exists(), platform

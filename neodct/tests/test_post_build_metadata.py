"""post-build-system-metadata.sh: what the image says it is.

Buildroot calls a post-build script as

    script TARGET_DIR $BR2_ROOTFS_POST_SCRIPT_ARGS $BR2_ROOTFS_POST_BUILD_SCRIPT_ARGS

and the qemu board's post-image script needs the defconfig *path* in
POST_SCRIPT_ARGS, so that path lands in $2 and the platform is the last
argument. Reading $2 put a build-machine path into system.os.platform, and
that value is what every update's manifest is compared against -- so an
update built in a different directory would have been refused for the rest
of time as "WRONG UPDATE FOR THIS PHONE".
"""

import os
import subprocess

import pytest

SCRIPT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "scripts", "post-build-system-metadata.sh",
)

DEFCONFIG = "/home/someone/Projects/neodct/buildroot/configs/neodct_qemu_defconfig"


def target_tree(tmp_path, version="0.3.1a"):
    target = tmp_path / "target"
    (target / "etc").mkdir(parents=True)
    (target / "etc" / "os-release").write_text(
        "NAME=NeoDCT OS\nVERSION=v%s\nID=buildroot\nVERSION_ID=%s\n"
        "PRETTY_NAME=\"NeoDCT v%s\"\n" % (version, version, version))
    return target


def run(target, *args, epoch="1785160800"):
    return subprocess.run(
        ["sh", SCRIPT, str(target)] + [str(arg) for arg in args],
        capture_output=True, text=True,
        env=dict(os.environ, SOURCE_DATE_EPOCH=epoch))


def version_prop(target):
    values = {}
    path = target / "NeoDCT" / "System" / "version.prop"
    for line in path.read_text().splitlines():
        if line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip()
    return values


def test_the_platform_is_the_last_argument_not_the_second(tmp_path):
    """Exactly how buildroot calls it: TARGET_DIR, defconfig, platform."""
    target = target_tree(tmp_path)

    result = run(target, DEFCONFIG, "qemu-aarch64")

    assert result.returncode == 0, result.stderr
    assert version_prop(target)["system.os.platform"] == "qemu-aarch64"


def test_a_path_is_never_recorded_as_a_platform(tmp_path):
    """Nothing with a slash in it is a platform id, whatever position it
    arrives in -- the value has to mean the same thing on every machine."""
    target = target_tree(tmp_path)

    result = run(target, DEFCONFIG)

    assert result.returncode == 0, result.stderr
    assert version_prop(target)["system.os.platform"] == "unknown"


def test_the_version_comes_from_os_release(tmp_path):
    target = target_tree(tmp_path, version="0.3.1a")

    run(target, DEFCONFIG, "qemu-aarch64")

    values = version_prop(target)
    assert values["system.os.versionnumber"] == "0.3.1a"
    assert values["system.os.versionname"] == "NeoDCT System v0.3.1a"


def test_the_build_time_is_recorded_both_ways_round(tmp_path):
    """The About screen shows the string; mkupdate needs the epoch."""
    target = target_tree(tmp_path)

    run(target, DEFCONFIG, "luckfox-armv7", epoch="1785160800")

    values = version_prop(target)
    assert values["system.os.buildepoch"] == "1785160800"
    assert values["system.os.buildtime"].startswith("2026-")
    assert values["system.os.platform"] == "luckfox-armv7"


def test_the_user_mountpoint_exists_inside_the_read_only_image(tmp_path):
    """Nothing can create it at runtime: / is a read-only squashfs."""
    target = target_tree(tmp_path)

    run(target, DEFCONFIG, "qemu-aarch64")

    assert (target / "NeoDCT" / "User").is_dir()


def test_a_target_tree_with_no_version_is_an_error(tmp_path):
    target = tmp_path / "target"
    (target / "etc").mkdir(parents=True)
    (target / "etc" / "os-release").write_text("NAME=NeoDCT OS\n")

    result = run(target, DEFCONFIG, "qemu-aarch64")

    assert result.returncode != 0
    assert "VERSION_ID" in result.stderr

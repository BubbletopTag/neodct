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


def props(path):
    values = {}
    for line in path.read_text().splitlines():
        if line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip()
    return values


def version_prop(target):
    return props(target / "NeoDCT" / "System" / "version.prop")


def platform_flag(target):
    """/NeoDCT/platform -- the boot scripts' and libneodct's copy of the fact.

    A sibling of version.prop rather than a key in it: this one is read by
    busybox sh (/bin/nd-platform) and by nd_platform.c in paths where
    nd_settings_get()'s R-24 rewrite would be a flash write, so it is four
    lines with one lowercase token per value.
    """
    return props(target / "NeoDCT" / "platform")


def test_the_platform_is_the_last_argument_not_the_second(tmp_path):
    """Exactly how buildroot calls it: TARGET_DIR, defconfig, platform."""
    target = target_tree(tmp_path)

    result = run(target, DEFCONFIG, "qemu-aarch64")

    assert result.returncode == 0, result.stderr
    assert version_prop(target)["system.os.platform"] == "qemu-aarch64"


def test_a_path_is_never_recorded_as_a_platform(tmp_path):
    """Nothing with a slash in it is a platform id, whatever position it
    arrives in -- the value has to mean the same thing on every machine.

    It used to fall through and write system.os.platform=unknown. It now
    refuses the build; test_an_unknown_platform_stops_the_build below is where
    that half is asserted. What this case still pins is that the DEFCONFIG
    path never became the platform.
    """
    target = target_tree(tmp_path)

    result = run(target, DEFCONFIG)

    assert result.returncode != 0
    # The path was normalised to "unknown" before anything looked at it, so
    # that is what the refusal names -- the path itself never became an id.
    assert 'Unknown platform id "unknown"' in result.stderr
    assert DEFCONFIG not in result.stderr
    assert not (target / "NeoDCT" / "System" / "version.prop").exists()


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


# --- /NeoDCT/platform: the same fact, in the shape a boot script can read ---
#
# system.os.platform is the UPDATE system's compatibility key and its format is
# load-bearing for every .ndsw ever built, so it stays as it is. The flag is
# derived from the same argument in the same block, which is what stops the two
# from ever disagreeing -- and the board is split out from the platform word so
# that a future dev-kit can ship as platform=hw board=devkit-x without breaking
# every "is this hardware" branch at once.


def test_the_qemu_image_says_qemu(tmp_path):
    target = target_tree(tmp_path)

    result = run(target, DEFCONFIG, "qemu-aarch64")

    assert result.returncode == 0, result.stderr
    assert platform_flag(target) == {
        "platform": "qemu",
        "board": "qemu-virt",
        "image": "qemu-aarch64",
    }


def test_the_luckfox_image_says_hw(tmp_path):
    target = target_tree(tmp_path)

    result = run(target, DEFCONFIG, "luckfox-armv7")

    assert result.returncode == 0, result.stderr
    assert platform_flag(target) == {
        "platform": "hw",
        "board": "luckfox-pico-mini-b",
        "image": "luckfox-armv7",
    }


def test_the_flag_and_version_prop_can_never_disagree(tmp_path):
    """image= is system.os.platform, byte for byte, from the one argument."""
    target = target_tree(tmp_path)

    run(target, DEFCONFIG, "luckfox-armv7")

    assert platform_flag(target)["image"] == version_prop(target)["system.os.platform"]


def test_an_unknown_platform_stops_the_build(tmp_path):
    """A post-build script that falls through quietly ships an image that
    looks finished and is not.

    post-build-prune-tests.sh is the precedent: the same last-argument mistake
    made "${PLATFORM%%-*}" = "luckfox" silently false and real hardware
    shipped the generic inittab for several releases with nothing saying so.
    And an image whose system.os.platform is "unknown" is one no update can
    ever be built for, so there is nothing worth rescuing by continuing.
    """
    target = target_tree(tmp_path)

    result = run(target, DEFCONFIG, "luckfox-armv8")

    assert result.returncode != 0
    assert "luckfox-armv8" in result.stderr
    assert "qemu-aarch64" in result.stderr and "luckfox-armv7" in result.stderr
    # And it refuses BEFORE writing anything, so a failed build leaves no
    # half-labelled tree for the next one to pick up.
    assert not (target / "NeoDCT" / "platform").exists()
    assert not (target / "NeoDCT" / "System" / "version.prop").exists()


def test_the_flag_is_never_written_with_platform_unknown_in_it(tmp_path):
    """The whole point of refusing: "unknown" is a runtime answer, produced by
    a missing file, and never a value an image is allowed to assert about
    itself. nd_platform.h's third value exists for images that cannot say --
    not for images that were built without being told."""
    target = target_tree(tmp_path)

    run(target, DEFCONFIG, "unknown")

    assert not (target / "NeoDCT" / "platform").exists()


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


# --- os-release says one version, in three places ---------------------------

def test_os_release_agrees_with_itself():
    """VERSION_ID, VERSION and PRETTY_NAME are three copies of one fact.

    0.3.10a shipped with VERSION_ID bumped and the other two left at
    0.3.9a, because the bump was a sed for one line. Everything that
    matters reads VERSION_ID -- version.prop, the banner, /etc/issue, the
    release workflow's tag check -- so the image was correct and told the
    owner it was the previous version anyway.
    """
    import os
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(here, "overlay", "etc", "os-release")

    values = {}
    for line in open(path):
        if "=" in line:
            key, value = line.strip().split("=", 1)
            values[key] = value.strip('"')

    version_id = values["VERSION_ID"]
    assert values["VERSION"] == "v" + version_id, values
    assert values["PRETTY_NAME"] == "NeoDCT v" + version_id, values


def test_os_release_matches_the_changelog():
    """A release with no changelog section is a release with no notes: the
    workflow builds them from the section headed exactly this version."""
    import os
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    version = None
    for line in open(os.path.join(here, "overlay", "etc", "os-release")):
        if line.startswith("VERSION_ID="):
            version = line.split("=", 1)[1].strip().strip('"')
    assert version

    changelog = os.path.join(here, "overlay", "NeoDCT", "CHANGELOG.txt")
    headings = [l.strip() for l in open(changelog) if l.strip() == version]
    assert headings, "CHANGELOG.txt has no section headed exactly %s" % version

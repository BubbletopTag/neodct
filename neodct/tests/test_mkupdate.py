"""neodct/tools/mkupdate.py -- the producer side of the .ndsw format.

These tests close the loop: everything mkupdate writes is read back with
the same UpdateService code the phone runs, so producer and consumer can
never drift apart silently.
"""

import hashlib
import os
import shutil
import subprocess
import sys

import pytest

from System.core.UpdateService import BadSignature, package, verity
from System.core.UpdateService import manifest as manifest_mod

from update_fixtures import BS, pattern, png

TOOLS_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"
)
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import mkupdate

CHANGELOG = """# NeoDCT OS Changelog

0.3.2a
Added
- SystemUpdate app

Changed
- Music now lives on the SD card

0.3.0a
Added
- Something older
"""


def openssl_key(tmp_path):
    key = tmp_path / "release.key"
    pub = tmp_path / "release.pub"
    subprocess.run(["openssl", "genpkey", "-algorithm", "RSA",
                    "-pkeyopt", "rsa_keygen_bits:2048", "-out", str(key)],
                   check=True, capture_output=True)
    subprocess.run(["openssl", "rsa", "-in", str(key), "-pubout", "-out", str(pub)],
                   check=True, capture_output=True)
    return key, pub


def test_appends_the_hash_tree_after_the_squashfs():
    squashfs = pattern(8 * BS)

    image, tree = mkupdate.build_system_image(squashfs)

    assert image[:len(squashfs)] == squashfs
    assert tree.data_blocks == 8
    assert verity.parse_superblock(image[8 * BS:])["data_blocks"] == 8


def test_pads_a_squashfs_that_is_not_block_aligned():
    """The tree has to start on a block boundary or dm-verity's maths is off."""
    squashfs = pattern(8 * BS + 100)

    image, tree = mkupdate.build_system_image(squashfs)

    assert tree.data_blocks == 9
    assert image[:len(squashfs)] == squashfs
    assert image[len(squashfs):9 * BS] == b"\x00" * (9 * BS - len(squashfs))
    assert verity.parse_superblock(image[9 * BS:])["data_blocks"] == 9


def test_refuses_an_empty_image():
    with pytest.raises(ValueError):
        mkupdate.build_system_image(b"")


def test_the_manifest_points_at_where_the_tree_actually_is():
    """manifest.hash_offset is what the initramfs feeds dmsetup -- if it
    disagreed with the real layout, every boot would fail verity."""
    squashfs = pattern(9 * BS + 7)
    image, tree = mkupdate.build_system_image(squashfs)

    body = mkupdate.build_manifest(tree, image, version="0.3.2a",
                                   buildtime=1785160800, changelog="hi",
                                   platform="qemu-aarch64")
    parsed = manifest_mod.parse(mkupdate.encode_manifest(body))

    assert parsed.hash_offset == 10 * BS
    assert image[parsed.hash_offset:parsed.hash_offset + 8] == b"verity\x00\x00"
    assert parsed.sha256 == hashlib.sha256(image).hexdigest()
    assert parsed.verity["root_hash"] == tree.root_hash


def test_writes_a_package_the_phone_accepts(tmp_path):
    image, tree = mkupdate.build_system_image(pattern(8 * BS))
    body = mkupdate.build_manifest(tree, image, version="0.3.2a",
                                   buildtime=1785160800,
                                   changelog="Fixed things.",
                                   platform="qemu-aarch64")
    out = tmp_path / "UPDATE.ndsw"

    mkupdate.write_ndsw(out, image, body)

    with package.open_package(out) as pkg:
        assert pkg.manifest.version == "0.3.2a"
        assert pkg.manifest.changelog == "Fixed things."
        extracted = tmp_path / "out.img"
        pkg.extract_image(extracted)
        assert extracted.read_bytes() == image


@pytest.mark.skipif(shutil.which("openssl") is None, reason="openssl not installed")
def test_a_signed_package_verifies_against_the_matching_public_key(tmp_path):
    key, pub = openssl_key(tmp_path)
    image, tree = mkupdate.build_system_image(pattern(8 * BS))
    body = mkupdate.build_manifest(tree, image, version="0.3.2a",
                                   buildtime=1785160800, changelog="x",
                                   platform="qemu-aarch64")
    out = tmp_path / "UPDATE.ndsw"

    mkupdate.write_ndsw(out, image, body, key_path=key)

    with package.open_package(out) as pkg:
        pkg.verify_signature(pub)
        assert pkg.signed is True


@pytest.mark.skipif(shutil.which("openssl") is None, reason="openssl not installed")
def test_a_signature_from_the_wrong_key_is_rejected(tmp_path):
    key, _ = openssl_key(tmp_path)
    other = tmp_path / "other"
    other.mkdir()
    _, other_pub = openssl_key(other)
    image, tree = mkupdate.build_system_image(pattern(8 * BS))
    body = mkupdate.build_manifest(tree, image, version="0.3.2a",
                                   buildtime=1785160800, changelog="x",
                                   platform="qemu-aarch64")
    out = tmp_path / "UPDATE.ndsw"
    mkupdate.write_ndsw(out, image, body, key_path=key)

    with package.open_package(out) as pkg:
        with pytest.raises(BadSignature):
            pkg.verify_signature(other_pub)


def test_an_unsigned_package_is_reported_as_unsigned(tmp_path):
    """`make update` with no key must still build, so this path is normal."""
    image, tree = mkupdate.build_system_image(pattern(8 * BS))
    body = mkupdate.build_manifest(tree, image, version="0.3.2a",
                                   buildtime=1785160800, changelog="x",
                                   platform="qemu-aarch64")
    out = tmp_path / "UPDATE.ndsw"

    mkupdate.write_ndsw(out, image, body, key_path=None)

    with package.open_package(out) as pkg:
        with pytest.raises(BadSignature, match="not signed"):
            pkg.verify_signature(tmp_path / "any.pub")


def test_takes_the_changelog_section_for_the_version_being_built():
    section = mkupdate.changelog_section(CHANGELOG, "0.3.2a")

    assert "SystemUpdate app" in section
    assert "Music now lives on the SD card" in section
    assert "Something older" not in section
    assert "0.3.2a" not in section


def test_changelog_falls_back_when_the_version_has_no_section():
    assert mkupdate.changelog_section(CHANGELOG, "9.9.9z") == ""


def test_reads_the_version_from_the_target_tree(tmp_path):
    """The build is the source of truth for what version an image is."""
    system = tmp_path / "NeoDCT" / "System"
    system.mkdir(parents=True)
    (system / "version.prop").write_text(
        "system.os.versionnumber=0.3.2a\n"
        "system.os.platform=luckfox-armv7\n"
        "system.os.buildtime=2026-07-26 19:28 UTC\n"
        "system.os.buildepoch=1785160800\n"
    )

    info = mkupdate.read_target_version(tmp_path)

    assert info["version"] == "0.3.2a"
    assert info["platform"] == "luckfox-armv7"
    assert int(info["buildtime"]) == 1785160800


def test_missing_version_prop_is_an_error(tmp_path):
    with pytest.raises(SystemExit, match="version.prop"):
        mkupdate.read_target_version(tmp_path)


def build_args(images_dir, tmp_path, *extra):
    """The minimum command line for a package, plus whatever is being tested."""
    (images_dir / "rootfs.squashfs").write_bytes(pattern(8 * BS))
    return ["--images-dir", str(images_dir),
            "--version", "0.3.2a", "--platform", "qemu-aarch64",
            "--buildtime", "1785160800",
            "--changelog-file", os.devnull,
            "--output", str(tmp_path / "UPDATE.ndsw")] + list(extra)


def test_a_thumbnail_travels_in_the_package(tmp_path):
    """`make update THUMBNAIL=art.png` is how a release gets its picture."""
    images = tmp_path / "images"
    images.mkdir()
    art = tmp_path / "art.png"
    art.write_bytes(png())

    mkupdate.main(build_args(images, tmp_path, "--thumbnail", str(art)))

    with package.open_package(tmp_path / "UPDATE.ndsw") as pkg:
        assert pkg.read_thumbnail() == art.read_bytes()


def test_the_thumbnail_hash_is_signed_along_with_everything_else(tmp_path):
    """It rides in manifest.json, so the one signature covers it too."""
    image, tree = mkupdate.build_system_image(pattern(8 * BS))
    body = mkupdate.build_manifest(tree, image, version="0.3.2a",
                                   buildtime=1785160800, changelog="x",
                                   platform="qemu-aarch64")
    art = png()
    out = tmp_path / "UPDATE.ndsw"

    mkupdate.write_ndsw(out, image, body, thumbnail=art)

    with package.open_package(out) as pkg:
        assert pkg.manifest.thumbnail_sha256 == hashlib.sha256(art).hexdigest()
        assert hashlib.sha256(art).hexdigest() in pkg.manifest.raw.decode()


def test_a_package_built_without_a_thumbnail_says_nothing_about_one(tmp_path):
    images = tmp_path / "images"
    images.mkdir()

    mkupdate.main(build_args(images, tmp_path))

    with package.open_package(tmp_path / "UPDATE.ndsw") as pkg:
        assert pkg.manifest.thumbnail_sha256 == ""
        assert pkg.read_thumbnail() is None


def test_a_thumbnail_that_is_not_a_png_is_refused_at_build_time(tmp_path):
    """Better to fail the build than to ship art the phone cannot draw."""
    images = tmp_path / "images"
    images.mkdir()
    art = tmp_path / "art.png"
    art.write_bytes(b"GIF89a and not a png at all")

    with pytest.raises(SystemExit, match="PNG"):
        mkupdate.main(build_args(images, tmp_path, "--thumbnail", str(art)))


def test_an_oversized_thumbnail_is_refused_at_build_time(tmp_path):
    images = tmp_path / "images"
    images.mkdir()
    art = tmp_path / "art.png"
    art.write_bytes(png() + b"\x00" * package.MAX_THUMBNAIL_BYTES)

    with pytest.raises(SystemExit, match="thumbnail"):
        mkupdate.main(build_args(images, tmp_path, "--thumbnail", str(art)))

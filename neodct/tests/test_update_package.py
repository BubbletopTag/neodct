"""Opening, verifying and unpacking .ndsw files."""

import hashlib
import zipfile

import pytest

from System.core.UpdateService import (BadSignature, IncompatibleUpdate,
                                       InvalidUpdate, package)

from update_fixtures import (BS, PLATFORM, build_image, make_ndsw, png, sign,
                             write_public_key)


def test_reads_the_manifest_out_of_a_package(tmp_path):
    path = tmp_path / "UPDATE.ndsw"
    make_ndsw(path)

    with package.open_package(path) as pkg:
        assert pkg.manifest.version == "0.3.2a"
        assert pkg.manifest.platform == PLATFORM


def test_a_file_that_is_not_a_zip_is_invalid(tmp_path):
    path = tmp_path / "UPDATE.ndsw"
    path.write_bytes(b"I am not a zip file")

    with pytest.raises(InvalidUpdate, match="zip"):
        package.open_package(path)


def test_a_package_with_no_manifest_is_invalid(tmp_path):
    path = tmp_path / "UPDATE.ndsw"
    make_ndsw(path, members=("rootfs.squashfs", "manifest.sig"))

    with pytest.raises(InvalidUpdate, match="manifest.json"):
        package.open_package(path)


def test_a_package_with_no_image_is_invalid(tmp_path):
    path = tmp_path / "UPDATE.ndsw"
    make_ndsw(path, members=("manifest.json", "manifest.sig"))

    with pytest.raises(InvalidUpdate, match="rootfs.squashfs"):
        package.open_package(path)


def test_a_missing_file_is_invalid(tmp_path):
    with pytest.raises(InvalidUpdate):
        package.open_package(tmp_path / "nope.ndsw")


def test_accepts_a_correctly_signed_package(tmp_path):
    path = tmp_path / "UPDATE.ndsw"
    make_ndsw(path)
    key = write_public_key(tmp_path)

    with package.open_package(path) as pkg:
        pkg.verify_signature(key)  # must not raise
        assert pkg.signed is True


def test_rejects_a_package_signed_over_different_bytes(tmp_path):
    """Re-serialising the manifest must not be able to rescue a bad signature."""
    path = tmp_path / "UPDATE.ndsw"
    make_ndsw(path, signature=sign(b'{"version": "9.9.9"}'))
    key = write_public_key(tmp_path)

    with package.open_package(path) as pkg:
        with pytest.raises(BadSignature):
            pkg.verify_signature(key)
        assert pkg.signed is False


def test_an_unsigned_package_is_a_bad_signature_not_an_invalid_one(tmp_path):
    """Engineering mode can override this, so it must be distinguishable."""
    path = tmp_path / "UPDATE.ndsw"
    make_ndsw(path, members=("rootfs.squashfs", "manifest.json"))
    key = write_public_key(tmp_path)

    with package.open_package(path) as pkg:
        with pytest.raises(BadSignature, match="not signed"):
            pkg.verify_signature(key)


def test_a_missing_public_key_is_a_bad_signature(tmp_path):
    path = tmp_path / "UPDATE.ndsw"
    make_ndsw(path)

    with package.open_package(path) as pkg:
        with pytest.raises(BadSignature):
            pkg.verify_signature(tmp_path / "no-such.pub")


def test_extracts_the_image_and_checks_its_hash(tmp_path):
    path = tmp_path / "UPDATE.ndsw"
    make_ndsw(path)
    dest = tmp_path / "pending.img"

    with package.open_package(path) as pkg:
        pkg.extract_image(dest)
        assert hashlib.sha256(dest.read_bytes()).hexdigest() == pkg.manifest.sha256


def test_extraction_reports_progress(tmp_path):
    path = tmp_path / "UPDATE.ndsw"
    make_ndsw(path)
    seen = []

    with package.open_package(path) as pkg:
        pkg.extract_image(tmp_path / "pending.img",
                          progress=lambda done, total: seen.append((done, total)))

    assert seen[-1][0] == seen[-1][1] > 0
    assert seen == sorted(seen)


def test_a_corrupted_image_fails_extraction_and_leaves_nothing_behind(tmp_path):
    """A truncated or tampered image must never survive as a pending update."""
    path = tmp_path / "UPDATE.ndsw"
    image, tree = build_image()
    body = make_ndsw(path, image=image, tree=tree)
    # Rewrite the zip with a flipped byte in the image, manifest untouched.
    tampered = bytearray(image)
    tampered[BS] ^= 0xFF
    with zipfile.ZipFile(path, "r") as original:
        manifest_raw = original.read("manifest.json")
        signature = original.read("manifest.sig")
    with zipfile.ZipFile(path, "w") as rewritten:
        rewritten.writestr("rootfs.squashfs", bytes(tampered))
        rewritten.writestr("manifest.json", manifest_raw)
        rewritten.writestr("manifest.sig", signature)
    dest = tmp_path / "pending.img"

    with package.open_package(path) as pkg:
        with pytest.raises(InvalidUpdate, match="sha256"):
            pkg.extract_image(dest)

    assert not dest.exists()
    assert body["sha256"] == hashlib.sha256(image).hexdigest()


def test_refuses_to_extract_without_room_on_the_partition(tmp_path):
    path = tmp_path / "UPDATE.ndsw"
    make_ndsw(path)

    with package.open_package(path) as pkg:
        with pytest.raises(package.NotEnoughSpace):
            pkg.extract_image(tmp_path / "pending.img", free_bytes=1024)


def test_platform_mismatch_is_reported_from_the_package(tmp_path):
    path = tmp_path / "UPDATE.ndsw"
    make_ndsw(path, platform="luckfox-armv7")

    with package.open_package(path) as pkg:
        with pytest.raises(IncompatibleUpdate):
            pkg.manifest.check_compatible(platform=PLATFORM)


def test_reads_a_thumbnail_out_of_a_package(tmp_path):
    """Updates can carry a small square picture the phone shows on the
    "update available" page."""
    art = png()
    path = tmp_path / "UPDATE.ndsw"
    make_ndsw(path, thumbnail=art)

    with package.open_package(path) as pkg:
        assert pkg.read_thumbnail() == art


def test_a_package_without_a_thumbnail_has_none(tmp_path):
    path = tmp_path / "UPDATE.ndsw"
    make_ndsw(path)

    with package.open_package(path) as pkg:
        assert pkg.read_thumbnail() is None


def test_an_undeclared_thumbnail_is_ignored(tmp_path):
    """Art nobody signed for is not art we display."""
    path = tmp_path / "UPDATE.ndsw"
    make_ndsw(path, thumbnail=png(), thumbnail_hash=False)

    with package.open_package(path) as pkg:
        assert pkg.read_thumbnail() is None


def test_a_thumbnail_that_does_not_match_its_hash_is_invalid(tmp_path):
    """Swapping the picture after signing changes nothing the signature
    covers, so the manifest's hash is what has to catch it."""
    path = tmp_path / "UPDATE.ndsw"
    make_ndsw(path, thumbnail=png(), thumbnail_hash=hashlib.sha256(b"other").hexdigest())

    with package.open_package(path) as pkg:
        with pytest.raises(InvalidUpdate, match="thumbnail"):
            pkg.read_thumbnail()


def test_an_oversized_thumbnail_is_refused_before_it_is_read(tmp_path):
    """64MB of RAM: a "thumbnail" the size of a photo album never gets
    decompressed, whatever the manifest claims about it."""
    huge = png(size=64) + b"\x00" * (package.MAX_THUMBNAIL_BYTES + 1)
    path = tmp_path / "UPDATE.ndsw"
    make_ndsw(path, thumbnail=huge)

    with package.open_package(path) as pkg:
        with pytest.raises(InvalidUpdate, match="thumbnail"):
            pkg.read_thumbnail()

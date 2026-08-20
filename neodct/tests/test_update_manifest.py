"""manifest.json parsing and compatibility rules."""

import json

import pytest

from System.core.UpdateService import IncompatibleUpdate, InvalidUpdate, manifest

GOOD = {
    "version": "0.3.2a",
    "buildtime": 1785160800,
    "changelog": "Fixed SMS database sorting bug and updated stock wallpapers.",
    "platform": "qemu-aarch64",
    "sha256": "a" * 64,
    "verity": {
        "root_hash": "b" * 64,
        "block_size": 4096,
        "image_blocks": 30720,
        "salt": "f8e7d6c5b4a3",
    },
}


def encode(**overrides):
    body = dict(GOOD)
    body.update(overrides)
    return json.dumps(body).encode()


def test_parses_a_well_formed_manifest():
    parsed = manifest.parse(encode())

    assert parsed.version == "0.3.2a"
    assert parsed.buildtime == 1785160800
    assert parsed.platform == "qemu-aarch64"
    assert parsed.sha256 == "a" * 64
    assert parsed.changelog.startswith("Fixed SMS")


def test_keeps_the_exact_bytes_it_was_given():
    """The signature covers the file verbatim, so it must never be re-encoded."""
    raw = encode()

    assert manifest.parse(raw).raw == raw


def test_derives_the_hash_offset_from_the_image_size():
    """The tree is appended straight after the squashfs on one device."""
    parsed = manifest.parse(encode())

    assert parsed.hash_offset == 30720 * 4096


def test_changelog_is_optional():
    body = dict(GOOD)
    del body["changelog"]

    assert manifest.parse(json.dumps(body).encode()).changelog == ""


@pytest.mark.parametrize("field", ["version", "buildtime", "platform",
                                   "sha256", "verity"])
def test_missing_required_field_is_an_invalid_update(field):
    body = dict(GOOD)
    del body[field]

    with pytest.raises(InvalidUpdate, match=field):
        manifest.parse(json.dumps(body).encode())


@pytest.mark.parametrize("field", ["root_hash", "block_size", "image_blocks"])
def test_missing_verity_field_is_an_invalid_update(field):
    verity = dict(GOOD["verity"])
    del verity[field]

    with pytest.raises(InvalidUpdate, match=field):
        manifest.parse(encode(verity=verity))


def test_verity_salt_may_be_absent():
    verity = dict(GOOD["verity"])
    del verity["salt"]

    assert manifest.parse(encode(verity=verity)).verity["salt"] == ""


def test_rejects_a_non_hex_image_hash():
    with pytest.raises(InvalidUpdate, match="sha256"):
        manifest.parse(encode(sha256="nope"))


def test_rejects_a_non_hex_root_hash():
    verity = dict(GOOD["verity"], root_hash="zz")

    with pytest.raises(InvalidUpdate, match="root_hash"):
        manifest.parse(encode(verity=verity))


def test_rejects_a_non_numeric_buildtime():
    with pytest.raises(InvalidUpdate, match="buildtime"):
        manifest.parse(encode(buildtime="tuesday"))


def test_rejects_a_block_size_that_is_not_a_power_of_two():
    verity = dict(GOOD["verity"], block_size=3000)

    with pytest.raises(InvalidUpdate, match="block_size"):
        manifest.parse(encode(verity=verity))


def test_rejects_malformed_json():
    with pytest.raises(InvalidUpdate, match="JSON"):
        manifest.parse(b"{not json")


def test_rejects_a_json_document_that_is_not_an_object():
    with pytest.raises(InvalidUpdate):
        manifest.parse(b"[1, 2, 3]")


def test_accepts_a_matching_platform():
    parsed = manifest.parse(encode())

    parsed.check_compatible(platform="qemu-aarch64")  # must not raise


def test_refuses_an_update_built_for_another_platform():
    """The Luckfox and QEMU images share a filename; installing the wrong
    one on real hardware is unrecoverable without a reflash."""
    parsed = manifest.parse(encode())

    with pytest.raises(IncompatibleUpdate, match="luckfox-armv7"):
        parsed.check_compatible(platform="luckfox-armv7")


def test_refuses_an_update_that_needs_a_newer_kernel():
    parsed = manifest.parse(encode(min_kernel="6.20.0"))

    with pytest.raises(IncompatibleUpdate, match="6.20.0"):
        parsed.check_compatible(platform="qemu-aarch64", kernel="6.12.47")


def test_accepts_an_update_whose_kernel_requirement_is_met():
    parsed = manifest.parse(encode(min_kernel="6.12.0"))

    parsed.check_compatible(platform="qemu-aarch64", kernel="6.12.47")


def test_kernel_requirement_is_ignored_when_absent():
    parsed = manifest.parse(encode())

    parsed.check_compatible(platform="qemu-aarch64", kernel="5.10.160")


def test_a_thumbnail_hash_is_carried_through():
    """The manifest is what gets signed, so the picture is authenticated by
    putting its hash in here rather than by signing a second member."""
    parsed = manifest.parse(encode(thumbnail_sha256="c" * 64))

    assert parsed.thumbnail_sha256 == "c" * 64


def test_a_manifest_without_a_thumbnail_reports_no_hash():
    assert manifest.parse(encode()).thumbnail_sha256 == ""


def test_rejects_a_thumbnail_hash_that_is_not_a_sha256():
    with pytest.raises(InvalidUpdate, match="thumbnail_sha256"):
        manifest.parse(encode(thumbnail_sha256="deadbeef"))


def test_rejects_a_thumbnail_hash_that_is_not_hex():
    with pytest.raises(InvalidUpdate, match="thumbnail_sha256"):
        manifest.parse(encode(thumbnail_sha256="z" * 64))

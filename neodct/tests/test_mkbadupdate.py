"""Deliberately broken update packages, and what each one must trigger.

This is the manual test matrix from docs/TESTING_UPDATES.md, machine
checked: every variant mkbadupdate produces is fed through the same
UpdateService code the phone runs, and asserted to raise the specific
refusal the tester should see on screen. If a variant stops reproducing its
failure mode, this fails rather than quietly wasting someone's afternoon.
"""

import hashlib
import os
import shutil
import subprocess
import sys
import zipfile

import pytest

from System.core.UpdateService import (BadSignature, IncompatibleUpdate,
                                       InvalidUpdate, package)

TOOLS_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"
)
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import mkbadupdate
import mkupdate

from update_fixtures import BS, pattern

pytestmark = pytest.mark.skipif(shutil.which("openssl") is None,
                                reason="openssl not installed")

PLATFORM = "qemu-aarch64"


@pytest.fixture
def good(tmp_path):
    """A correctly built, correctly signed package plus its keys."""
    key = tmp_path / "release.key"
    pub = tmp_path / "release.pub"
    subprocess.run(["openssl", "genpkey", "-algorithm", "RSA",
                    "-pkeyopt", "rsa_keygen_bits:2048", "-out", str(key)],
                   check=True, capture_output=True)
    subprocess.run(["openssl", "rsa", "-in", str(key), "-pubout", "-out", str(pub)],
                   check=True, capture_output=True)

    image, tree = mkupdate.build_system_image(pattern(8 * BS))
    body = mkupdate.build_manifest(tree, image, version="0.3.2a",
                                   buildtime=1785160800,
                                   changelog="A test build.",
                                   platform=PLATFORM)
    path = tmp_path / "UPDATE.ndsw"
    mkupdate.write_ndsw(path, image, body, key_path=key)

    class Good:
        pass

    Good.path = path
    Good.key = key
    Good.pub = pub
    Good.image = image
    Good.dir = tmp_path
    return Good


def variant(good, name):
    out = good.dir / "bad"
    out.mkdir(exist_ok=True)
    return mkbadupdate.make_variant(name, good.path, out, key_path=good.key)


def test_the_starting_package_is_actually_good(good):
    """If this fails, every other test here is meaningless."""
    with package.open_package(good.path) as pkg:
        pkg.verify_signature(good.pub)
        pkg.manifest.check_compatible(platform=PLATFORM, kernel="6.12.47")
        pkg.extract_image(good.dir / "out.img")
    assert (good.dir / "out.img").read_bytes() == good.image


def test_unsigned_is_a_bad_signature(good):
    path = variant(good, "unsigned")

    with package.open_package(path) as pkg:
        with pytest.raises(BadSignature, match="not signed"):
            pkg.verify_signature(good.pub)


def test_signed_by_the_wrong_key_is_a_bad_signature(good):
    path = variant(good, "wrong-key")

    with package.open_package(path) as pkg:
        with pytest.raises(BadSignature, match="does not match"):
            pkg.verify_signature(good.pub)


def test_a_tampered_manifest_breaks_its_signature(good):
    """Editing the manifest after signing must not go unnoticed."""
    path = variant(good, "tampered-manifest")

    with package.open_package(path) as pkg:
        assert pkg.manifest.version != "0.3.2a"
        with pytest.raises(BadSignature):
            pkg.verify_signature(good.pub)


def test_a_corrupt_image_passes_signing_but_fails_on_copy(good):
    """The signature covers the manifest; the manifest's sha256 covers the
    image. So a flipped byte gets through verification and is caught during
    the copy -- on the phone, after the progress bar has appeared."""
    path = variant(good, "corrupt-image")

    with package.open_package(path) as pkg:
        pkg.verify_signature(good.pub)          # signature is still valid
        with pytest.raises(InvalidUpdate, match="sha256"):
            pkg.extract_image(good.dir / "corrupt.img")
    assert not (good.dir / "corrupt.img").exists()


def test_a_truncated_image_fails_on_copy(good):
    path = variant(good, "truncated-image")

    with package.open_package(path) as pkg:
        pkg.verify_signature(good.pub)
        with pytest.raises(InvalidUpdate, match="sha256"):
            pkg.extract_image(good.dir / "short.img")


def test_a_package_with_no_manifest_is_invalid(good):
    path = variant(good, "no-manifest")

    with pytest.raises(InvalidUpdate, match="manifest.json"):
        package.open_package(path)


def test_a_package_with_no_image_is_invalid(good):
    path = variant(good, "no-image")

    with pytest.raises(InvalidUpdate, match="rootfs.squashfs"):
        package.open_package(path)


def test_something_that_is_not_a_zip_is_invalid(good):
    path = variant(good, "not-a-zip")

    with pytest.raises(InvalidUpdate, match="zip"):
        package.open_package(path)


def test_a_wrong_platform_package_is_incompatible(good):
    path = variant(good, "wrong-platform")

    with package.open_package(path) as pkg:
        # Re-signed, so the signature is fine -- it is the platform that is not.
        pkg.verify_signature(good.pub)
        with pytest.raises(IncompatibleUpdate, match="luckfox"):
            pkg.manifest.check_compatible(platform=PLATFORM, kernel="6.12.47")


def test_a_future_kernel_requirement_is_incompatible(good):
    path = variant(good, "future-kernel")

    with package.open_package(path) as pkg:
        pkg.verify_signature(good.pub)
        with pytest.raises(IncompatibleUpdate, match="99"):
            pkg.manifest.check_compatible(platform=PLATFORM, kernel="6.12.47")


def test_a_bad_root_hash_package_installs_cleanly(good):
    """The dangerous one: it passes every check the phone makes before
    rebooting, and only dm-verity catches it. That is exactly why it is the
    test that proves verity is switched on."""
    path = variant(good, "bad-root-hash")

    with package.open_package(path) as pkg:
        pkg.verify_signature(good.pub)
        pkg.manifest.check_compatible(platform=PLATFORM, kernel="6.12.47")
        pkg.extract_image(good.dir / "badhash.img")     # no complaint
        assert pkg.manifest.verity["root_hash"] != _real_root_hash(good)


def _real_root_hash(good):
    with zipfile.ZipFile(good.path) as archive:
        import json
        return json.loads(archive.read("manifest.json"))["verity"]["root_hash"]


def test_every_variant_is_produced_by_make_all(good):
    out = good.dir / "all"
    out.mkdir()

    produced = mkbadupdate.make_all(good.path, out, key_path=good.key)

    assert set(produced) == set(mkbadupdate.VARIANTS)
    for path in produced.values():
        assert os.path.getsize(path) > 0


def test_variants_are_named_so_they_can_be_told_apart_on_the_card(good):
    out = good.dir / "named"
    out.mkdir()

    produced = mkbadupdate.make_all(good.path, out, key_path=good.key)

    for name, path in produced.items():
        assert name.replace("-", "_") in os.path.basename(path)
        assert os.path.basename(path).endswith(".ndsw")


def test_re_signing_needs_a_key(good):
    """Without a key the platform variant cannot be re-signed, and saying so
    is better than handing back a package that fails for two reasons."""
    out = good.dir / "nokey"
    out.mkdir()

    with pytest.raises(SystemExit, match="key"):
        mkbadupdate.make_variant("wrong-platform", good.path, out, key_path=None)


def test_variants_that_need_no_key_work_without_one(good):
    out = good.dir / "nokey2"
    out.mkdir()

    path = mkbadupdate.make_variant("corrupt-image", good.path, out,
                                    key_path=None)

    assert os.path.exists(path)


def test_an_unknown_variant_is_rejected(good):
    with pytest.raises(SystemExit, match="unknown variant"):
        mkbadupdate.make_variant("banana", good.path, good.dir, key_path=good.key)

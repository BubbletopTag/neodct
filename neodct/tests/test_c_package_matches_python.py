"""The C .ndsw reader must agree with the Python one, over the same bytes.

`neodct/src/lib/nd_package.c` and `nd_manifest.c` are a port of
`System/core/UpdateService/package.py` and `manifest.py`. The C has its own
unit tests, but a reader that only ever agrees with its own test file is
worth nothing: the packages the phone will actually meet are written by
CPython's `zipfile` (via `mkupdate.py`) and broken by `mkbadupdate.py`, and
the question that matters is whether the two implementations see the same
thing in the same file.

So this feeds both sides the same packages and compares what comes out:

    build/default/test/test_package --dump FILE.ndsw

against `package.open_package(FILE)`. It is skipped when the C has not been
built, so a Python-only checkout is unaffected:

    make -C neodct/src test && python3 -m pytest neodct/tests/test_c_package_matches_python.py

Where the two are allowed to differ is written down at each assertion. The
short version: the C splits Python's single `InvalidUpdate` into three
taxonomy values and its refusal wording is its own, so only the
accept/reject verdict is compared -- but every VALUE that comes out of an
accepted package must match exactly.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import zipfile

import pytest

from System.core.UpdateService import IncompatibleUpdate, InvalidUpdate
from System.core.UpdateService import package

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.path.join(REPO, "tools")
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import mkbadupdate  # noqa: E402
import mkupdate  # noqa: E402

from update_fixtures import BS, make_ndsw, pattern, png  # noqa: E402
import c_driver  # noqa: E402
from c_driver import tmp_path  # noqa: E402,F401 -- a scratch dir the sandbox can see

BUILD = os.path.join(REPO, "src", "build", "default")
DUMPER = os.path.join(BUILD, "test", "test_package")

PLATFORM = "qemu-aarch64"

pytestmark = [
    pytest.mark.skipif(not os.path.exists(DUMPER),
                       reason="the C build is not present (make -C neodct/src test)"),
    pytest.mark.skipif(shutil.which("openssl") is None, reason="openssl not installed"),
]


# ------------------------------------------------------------------ #
# The two views
# ------------------------------------------------------------------ #

def unescape(text):
    """Reverses put_field() in test_package.c's --dump mode."""
    out = []
    i = 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text):
            out.append({"n": "\n", "r": "\r", "t": "\t", "\\": "\\"}[text[i + 1]])
            i += 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def c_view(path):
    """What nd_package.c sees. Returns a dict, or {"open": "ERR", ...}."""
    out = c_driver.run(DUMPER, "--dump", path)
    view = {"members": []}
    for line in out.stdout.decode().splitlines():
        parts = line.split("\t")
        if parts[0] == "open":
            view["open"] = parts[1]
            if parts[1] == "ERR":
                view["why"] = parts[3]
        elif parts[0] == "member":
            view["members"].append((parts[1], int(parts[2]), int(parts[3])))
        elif parts[0] == "image_size":
            view["image_size"] = int(parts[1])
        elif parts[0] == "mf":
            key, value = parts[1], unescape(parts[2])
            if key in ("buildtime", "block_size", "image_blocks", "image_bytes"):
                value = int(value)
            view.setdefault("manifest", {})[key] = value
        elif parts[0] == "manifest_raw_len":
            view["raw_len"] = int(parts[1])
        elif parts[0] == "manifest_raw_sha256":
            view["raw_sha256"] = parts[1]
        elif parts[0] == "signature":
            view["signature"] = "ERR" if parts[1] == "ERR" else (int(parts[1]), parts[2])
        elif parts[0] == "thumbnail":
            if parts[1] == "ERR":
                view["thumbnail"] = "ERR"
            elif parts[1] == "NONE":
                view["thumbnail"] = None
            else:
                view["thumbnail"] = (int(parts[1]), parts[2])
    return view


def c_run(*args):
    out = c_driver.run(DUMPER, *args)
    fields = {}
    for line in out.stdout.decode().splitlines():
        parts = line.split("\t")
        fields[parts[0]] = parts[1:]
    return fields


def py_view(path):
    """What package.py sees, in the same shape."""
    try:
        pkg = package.open_package(path)
    except InvalidUpdate as exc:
        return {"open": "ERR", "why": str(exc), "members": []}

    with pkg:
        m = pkg.manifest
        archive = zipfile.ZipFile(str(path), "r")
        with archive:
            members = [(name, archive.getinfo(name).file_size,
                        archive.getinfo(name).compress_type)
                       for name in archive.namelist()]
            try:
                signature = archive.read("manifest.sig")
            except KeyError:
                signature = None

        view = {
            "open": "OK",
            "members": members,
            "image_size": pkg.image_size,
            "manifest": {
                "version": m.version, "buildtime": m.buildtime,
                "platform": m.platform, "sha256": m.sha256,
                "changelog": m.changelog, "min_kernel": m.min_kernel,
                "thumbnail_sha256": m.thumbnail_sha256,
                "root_hash": m.verity["root_hash"],
                "block_size": m.verity["block_size"],
                "image_blocks": m.verity["image_blocks"],
                "salt": m.verity["salt"],
                "image_bytes": m.image_bytes,
            },
            "raw_len": len(m.raw),
            "raw_sha256": hashlib.sha256(m.raw).hexdigest(),
            "signature": ("ERR" if signature is None
                          else (len(signature), hashlib.sha256(signature).hexdigest())),
        }
        try:
            art = pkg.read_thumbnail()
            view["thumbnail"] = (None if art is None
                                 else (len(art), hashlib.sha256(art).hexdigest()))
        except InvalidUpdate:
            view["thumbnail"] = "ERR"
        return view


def assert_agree(path):
    """Both sides, same file. Returns the shared verdict for the caller."""
    c = c_view(path)
    p = py_view(path)

    # Only the VERDICT is compared, never the wording: Python raises one
    # InvalidUpdate for every structural failure, the C splits it into
    # ND_UPD_ERR_UNREADABLE / _BAD_ZIP / _BAD_MANIFEST so the serial log says
    # which, and neither message is a promise to the other.
    assert c["open"] == p["open"], (
        "verdict differs for %s: python=%s(%s) c=%s(%s)"
        % (path, p["open"], p.get("why"), c["open"], c.get("why")))
    if c["open"] == "ERR":
        return "ERR"

    # Member order is central-directory order after duplicate resolution, and
    # the C must resolve duplicates the way CPython does (last wins) or the
    # two open different bytes under the same name.
    assert c["members"] == p["members"], (
        "members differ for %s:\n  python=%r\n  c=%r" % (path, p["members"], c["members"]))
    assert c["image_size"] == p["image_size"]
    assert c["manifest"] == p["manifest"], (
        "manifest differs for %s:\n  python=%r\n  c=%r" % (path, p["manifest"], c["manifest"]))

    # The signature is over the manifest bytes EXACTLY AS STORED. If these
    # two ever differ, one of the readers is verifying a re-encoding.
    assert c["raw_len"] == p["raw_len"]
    assert c["raw_sha256"] == p["raw_sha256"]

    assert c["signature"] == p["signature"]
    assert c["thumbnail"] == p["thumbnail"]
    return "OK"


# ------------------------------------------------------------------ #
# Packages
# ------------------------------------------------------------------ #

@pytest.fixture(scope="module")
def keys(tmp_path_factory):
    d = tmp_path_factory.mktemp("keys")
    key = d / "release.key"
    pub = d / "release.pub"
    subprocess.run(["openssl", "genpkey", "-algorithm", "RSA",
                    "-pkeyopt", "rsa_keygen_bits:2048", "-out", str(key)],
                   check=True, capture_output=True)
    subprocess.run(["openssl", "rsa", "-in", str(key), "-pubout", "-out", str(pub)],
                   check=True, capture_output=True)
    return key, pub


def build_real_package(tmp_path, key=None, thumbnail=None, **manifest_over):
    """A package built the way `make update` builds one."""
    image, tree = mkupdate.build_system_image(pattern(8 * BS))
    body = mkupdate.build_manifest(tree, image, version="0.3.2a",
                                   buildtime=1785160800,
                                   changelog="A test build.\nSecond line.",
                                   platform=PLATFORM)
    body.update(manifest_over)
    path = tmp_path / "UPDATE.ndsw"
    mkupdate.write_ndsw(path, image, body, key_path=key, thumbnail=thumbnail)
    return path


def test_a_package_built_by_mkupdate_reads_the_same(tmp_path, keys):
    """The real thing: the image STORED, everything else DEFLATED."""
    assert assert_agree(build_real_package(tmp_path, key=keys[0])) == "OK"


def test_an_unsigned_package_reads_the_same(tmp_path):
    assert assert_agree(build_real_package(tmp_path)) == "OK"


def test_a_package_with_a_thumbnail_reads_the_same(tmp_path, keys):
    path = build_real_package(tmp_path, key=keys[0], thumbnail=png())
    assert assert_agree(path) == "OK"


def test_a_package_with_a_min_kernel_reads_the_same(tmp_path, keys):
    path = build_real_package(tmp_path, key=keys[0], min_kernel="6.12.0")
    assert assert_agree(path) == "OK"


def test_the_test_fixtures_read_the_same(tmp_path):
    """update_fixtures.make_ndsw deflates EVERY member, including the image.
    Both compression methods have to work or half the suite is untested."""
    path = tmp_path / "fixture.ndsw"
    make_ndsw(path)
    assert assert_agree(path) == "OK"


def test_a_fixture_with_a_thumbnail_reads_the_same(tmp_path):
    path = tmp_path / "fixture.ndsw"
    make_ndsw(path, thumbnail=png())
    assert assert_agree(path) == "OK"


def test_an_undeclared_thumbnail_is_ignored_by_both(tmp_path):
    """Art nobody signed for is not art we display -- on both sides."""
    path = tmp_path / "fixture.ndsw"
    make_ndsw(path, thumbnail=png(), thumbnail_hash=False)
    assert assert_agree(path) == "OK"
    assert c_view(path)["thumbnail"] is None


def test_a_thumbnail_that_does_not_match_its_hash_is_refused_by_both(tmp_path):
    path = tmp_path / "fixture.ndsw"
    make_ndsw(path, thumbnail=png(),
              thumbnail_hash=hashlib.sha256(b"other").hexdigest())
    assert assert_agree(path) == "OK"
    assert c_view(path)["thumbnail"] == "ERR"


def test_an_oversized_thumbnail_is_refused_by_both(tmp_path):
    path = tmp_path / "fixture.ndsw"
    huge = png() + b"\x00" * (package.MAX_THUMBNAIL_BYTES + 1)
    make_ndsw(path, thumbnail=huge)
    assert assert_agree(path) == "OK"
    assert c_view(path)["thumbnail"] == "ERR"


@pytest.mark.parametrize("variant", sorted(mkbadupdate.VARIANTS))
def test_every_mkbadupdate_variant_reads_the_same(tmp_path, keys, variant):
    """mkbadupdate exists to produce exactly the shapes of broken package a
    tester should see refused. Whatever each one does to package.py, the C
    must do the same -- including the ones that OPEN cleanly and fail later,
    which is most of them: only no-manifest, no-image and not-a-zip are
    refused at open."""
    good = build_real_package(tmp_path, key=keys[0], thumbnail=png())
    out_dir = tmp_path / "bad"
    out_dir.mkdir()
    path = mkbadupdate.make_variant(variant, good, out_dir, key_path=keys[0])
    assert_agree(path)


def test_a_file_that_is_not_a_zip_is_refused_by_both(tmp_path):
    path = tmp_path / "UPDATE.ndsw"
    path.write_bytes(b"I am not a zip file")
    assert assert_agree(path) == "ERR"


def test_a_missing_file_is_refused_by_both(tmp_path):
    assert assert_agree(tmp_path / "nope.ndsw") == "ERR"


def test_a_truncated_package_is_refused_by_both(tmp_path, keys):
    """Every prefix of a good package. Python has to refuse each one and so
    does the C -- and neither may ever return a manifest out of a file that
    stops half way through it."""
    good = build_real_package(tmp_path, key=keys[0])
    whole = good.read_bytes()
    cut = tmp_path / "cut.ndsw"
    for fraction in (1, 2, 4, 8, 16, 32, 64, 99):
        cut.write_bytes(whole[:len(whole) * fraction // 100])
        assert assert_agree(cut) == "ERR"
    # One byte short of the whole file: the central directory is intact but
    # its last record is not.
    cut.write_bytes(whole[:-1])
    assert assert_agree(cut) == "ERR"


def test_a_package_with_no_manifest_or_no_image_is_refused_by_both(tmp_path):
    for members in (("rootfs.squashfs", "manifest.sig"),
                    ("manifest.json", "manifest.sig")):
        path = tmp_path / "partial.ndsw"
        make_ndsw(path, members=members)
        assert assert_agree(path) == "ERR"


def test_a_manifest_broken_in_every_way_is_refused_by_both(tmp_path):
    """The manifest validator, reached through the zip rather than directly:
    what a package with each kind of broken manifest does at open()."""
    good = json.loads(make_ndsw(tmp_path / "seed.ndsw") and
                      zipfile.ZipFile(str(tmp_path / "seed.ndsw")).read("manifest.json"))
    broken = [
        ("not-json", b"{not json"),
        ("not-object", b"[1, 2, 3]"),
        ("empty", b""),
    ]
    for field in ("version", "buildtime", "platform", "sha256", "verity"):
        body = dict(good)
        del body[field]
        broken.append(("missing-" + field, json.dumps(body).encode()))
    for name, value in (("sha256", "nope"), ("version", ""), ("platform", ""),
                        ("buildtime", "tuesday")):
        body = dict(good)
        body[name] = value
        broken.append(("bad-" + name, json.dumps(body).encode()))
    for field, value in (("root_hash", "zz"), ("block_size", 3000),
                         ("image_blocks", 0)):
        body = dict(good)
        body["verity"] = dict(good["verity"])
        body["verity"][field] = value
        broken.append(("bad-verity-" + field, json.dumps(body).encode()))

    for label, raw in broken:
        path = tmp_path / ("%s.ndsw" % label)
        with zipfile.ZipFile(str(path), "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("rootfs.squashfs", b"x" * 64)
            zf.writestr("manifest.json", raw)
        assert assert_agree(path) == "ERR", label


# ------------------------------------------------------------------ #
# The two halves that only run AFTER a package has opened cleanly
# ------------------------------------------------------------------ #
#
# corrupt-image, truncated-image, wrong-platform and future-kernel all open
# without complaint -- a cross-check that stops at open() never reaches the
# code that refuses them, which is the code that decides whether a phone is
# bricked.

def py_extract(path, dest):
    try:
        package.open_package(path).extract_image(dest)
        return "OK", ""
    except (InvalidUpdate, package.NotEnoughSpace) as exc:
        return "ERR", str(exc)


@pytest.mark.parametrize("variant", ["corrupt-image", "truncated-image"])
def test_a_broken_image_is_refused_by_both_at_extraction(tmp_path, keys, variant):
    good = build_real_package(tmp_path, key=keys[0])
    out_dir = tmp_path / "bad"
    out_dir.mkdir()
    path = mkbadupdate.make_variant(variant, good, out_dir, key_path=keys[0])

    py_dest = tmp_path / "py.img"
    c_dest = tmp_path / "c.img"
    py_verdict, py_why = py_extract(path, py_dest)
    c = c_run("--extract", path, c_dest)

    assert c["extract"][0] == py_verdict == "ERR", (py_why, c["extract"])
    # "A half-written image must never be left behind where the applier could
    # find it." Both sides unlink it.
    assert not py_dest.exists()
    assert c["left_behind"] == ["0"]


def test_a_good_image_extracts_to_the_same_bytes(tmp_path, keys):
    path = build_real_package(tmp_path, key=keys[0])
    py_dest = tmp_path / "py.img"
    c_dest = tmp_path / "c.img"

    assert py_extract(path, py_dest) == ("OK", "")
    assert c_run("--extract", path, c_dest)["extract"] == ["OK"]
    assert py_dest.read_bytes() == c_dest.read_bytes()
    with package.open_package(path) as pkg:
        assert hashlib.sha256(c_dest.read_bytes()).hexdigest() == pkg.manifest.sha256


def py_compat(path, platform, kernel):
    with package.open_package(path) as pkg:
        try:
            pkg.manifest.check_compatible(platform=platform, kernel=kernel)
            return "OK", ""
        except IncompatibleUpdate as exc:
            return "ERR", str(exc)


@pytest.mark.parametrize("platform,kernel", [
    (PLATFORM, None),
    (PLATFORM, "6.12.47"),
    ("luckfox-armv7", None),
    ("luckfox-armv7", "6.12.47"),
    (PLATFORM, "5.10.160"),
    (PLATFORM, "6.12"),
    (PLATFORM, "6.12.47-rt"),
    (PLATFORM, "6.x.3"),
    (PLATFORM, ""),
])
def test_check_compatible_agrees_including_its_wording(tmp_path, keys, platform, kernel):
    """THE BRICK CASE. This one is compared down to the message, because it is
    what the app pastes after "WRONG UPDATE FOR THIS PHONE!" and a Python test
    matches on the platform name inside it."""
    path = build_real_package(tmp_path, key=keys[0], min_kernel="6.12.30")
    p_verdict, p_why = py_compat(path, platform, kernel)
    c = c_run("--compat", path, platform, *( [kernel] if kernel is not None else [] ))
    assert c["compat"][0] == p_verdict, (platform, kernel, p_why, c["compat"])
    if p_verdict == "ERR":
        assert c["compat"][1] == p_why


@pytest.mark.parametrize("variant", ["wrong-platform", "future-kernel"])
def test_the_brick_variants_are_refused_by_both(tmp_path, keys, variant):
    good = build_real_package(tmp_path, key=keys[0])
    out_dir = tmp_path / "bad"
    out_dir.mkdir()
    path = mkbadupdate.make_variant(variant, good, out_dir, key_path=keys[0])

    p_verdict, p_why = py_compat(path, PLATFORM, "6.12.47")
    c = c_run("--compat", path, PLATFORM, "6.12.47")
    assert p_verdict == "ERR"
    assert c["compat"] == ["ERR", p_why]

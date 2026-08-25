"""The C manifest validator must agree with the Python one, word for word.

`neodct/src/lib/nd_manifest.c` is a port of
`System/core/UpdateService/manifest.py`. Every refusal it produces is a
sentence somebody reads on the phone, and three Python suites match on the
field name inside it -- so this compares not just the verdict but the message.

    make -C neodct/src test && python3 -m pytest neodct/tests/test_c_manifest_matches_python.py

`DIVERGENT` below is the complete list of inputs where the two deliberately
disagree, each with the reason. Every one of them is a manifest Python
ACCEPTS and the C REFUSES: the C reads an SD card that arrived from
who-knows-where and has no arbitrary-precision integers or unbounded strings
to fall back on. If a case ever moves the other way -- the C accepting
something Python refuses -- that is a bug, and this test says so out loud
rather than letting it hide in a list of expected differences.
"""

import copy
import json
import os
import subprocess

import pytest

from System.core.UpdateService import IncompatibleUpdate, InvalidUpdate
from System.core.UpdateService import manifest as manifest_mod

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(REPO, "src", "build", "default")
DRIVER = os.path.join(BUILD, "test", "test_manifest")
LIBDIR = os.path.join(BUILD, "lib")

pytestmark = pytest.mark.skipif(
    not os.path.exists(DRIVER),
    reason="the C build is not present (make -C neodct/src test)")

GOOD = {
    "version": "0.3.2a",
    "buildtime": 1785160800,
    "changelog": "Fixed SMS database sorting bug.\nAnd a second line.",
    "platform": "qemu-aarch64",
    "sha256": "a" * 64,
    "verity": {
        "root_hash": "b" * 64,
        "block_size": 4096,
        "image_blocks": 30720,
        "salt": "f8e7d6c5b4a3",
    },
}

# Inputs where the C deliberately refuses a manifest Python accepts. Nothing
# may be added here without a reason that survives being read out loud.
DIVERGENT = {
    "image-blocks-true":
        'bool is a subclass of int in Python, so "image_blocks": true is 1 '
        "there. nd_json.h keeps ND_JSON_BOOL distinct from ND_JSON_INT so the "
        "C can refuse it, and a block count that arrived as a boolean is not "
        "one anybody meant to sign.",
    "changelog-not-a-string":
        "Python keeps the object and str()s it on the way to the screen.",
    "min-kernel-not-a-string":
        "Python str()s it, and for min_kernel that decides whether an update "
        "installs -- so the C refuses rather than guess a formatter.",
    "buildtime-past-int64":
        "nd_json refuses an integer outside int64 for the whole document, "
        "before nd_manifest sees it. Python has bignums.",
    "sha256-padded-with-spaces":
        "bytes.fromhex skips whitespace between pairs, so Python accepts a "
        "64-digit hash spread over 95 characters and stores all 95. The C "
        "field is 64 digits plus a NUL and refuses rather than truncate a "
        "hash.",
    "version-too-long":
        "No bound in Python. A truncated version reaches pending.prop.",
    "salt-too-long":
        "verity.py caps the salt at 256 bytes; a truncated salt builds the "
        "wrong dm-verity table and the phone does not boot.",
    "utf8-bom":
        "json.loads() on bytes strips a UTF-8 BOM; nd_json does not. Nothing "
        "the project writes emits one.",
}


# One input where the two differ in a VALUE rather than a verdict: Python
# multiplies two bignums for image_bytes and C saturates. Everything else
# about the manifest still has to match.
SATURATES = {
    "huge-image-blocks":
        "image_blocks * block_size is bignum arithmetic in Python. C "
        "saturates at UINT64_MAX rather than wrapping a hostile block count "
        "into a small, plausible-looking image size.",
}


def unescape(text):
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


NUMERIC = ("buildtime", "block_size", "image_blocks", "image_bytes", "raw_len")


def c_run(tmp_path, raw, *args):
    path = tmp_path / "manifest.json"
    path.write_bytes(raw)
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = LIBDIR
    env.pop("NEODCT_ROOT", None)
    out = subprocess.run([DRIVER] + list(args[:1]) + [str(path)] + [str(a) for a in args[1:]],
                         capture_output=True, check=True, env=env)
    verdict = None
    fields = {}
    for line in out.stdout.decode().splitlines():
        parts = line.split("\t")
        if parts[0] in ("parse", "compat"):
            verdict = parts[1]
        elif parts[0] == "mf":
            value = unescape(parts[2]) if len(parts) > 2 else ""
            fields[parts[1]] = int(value) if parts[1] in NUMERIC else value
    return verdict, fields


def c_parse(tmp_path, raw):
    verdict, fields = c_run(tmp_path, raw, "--parse")
    if verdict == "ERR":
        return "ERR", fields.get("why", "")
    return "OK", fields


def py_parse(raw):
    try:
        m = manifest_mod.parse(raw)
    except InvalidUpdate as exc:
        return "ERR", str(exc)
    return "OK", {
        "version": m.version, "buildtime": m.buildtime, "platform": m.platform,
        "sha256": m.sha256, "changelog": m.changelog, "min_kernel": m.min_kernel,
        "thumbnail_sha256": m.thumbnail_sha256,
        "root_hash": m.verity["root_hash"], "block_size": m.verity["block_size"],
        "image_blocks": m.verity["image_blocks"], "salt": m.verity["salt"],
        "image_bytes": m.image_bytes, "raw_len": len(m.raw),
    }


def body(**over):
    b = copy.deepcopy(GOOD)
    for key, value in over.items():
        if key.startswith("v_"):
            b["verity"][key[2:]] = value
        elif value is _ABSENT:
            b.pop(key, None)
        else:
            b[key] = value
    return json.dumps(b).encode()


class _Absent:
    pass


_ABSENT = _Absent()

NULL = json.loads("null")

# (label, manifest bytes). Anything not in DIVERGENT must produce the same
# verdict AND the same message on both sides.
CASES = [
    ("good", body()),
    ("good-as-mkupdate-writes-it",
     (json.dumps(GOOD, indent=2, sort_keys=True) + "\n").encode()),

    ("missing-version", body(version=_ABSENT)),
    ("missing-buildtime", body(buildtime=_ABSENT)),
    ("missing-platform", body(platform=_ABSENT)),
    ("missing-sha256", body(sha256=_ABSENT)),
    ("missing-verity", body(verity=_ABSENT)),
    ("missing-root-hash", body(verity={"block_size": 4096, "image_blocks": 1})),
    ("missing-block-size", body(verity={"root_hash": "b" * 64, "image_blocks": 1})),
    ("missing-image-blocks", body(verity={"root_hash": "b" * 64, "block_size": 4096})),

    ("no-salt", body(verity={"root_hash": "b" * 64, "block_size": 4096,
                             "image_blocks": 7})),
    ("no-changelog", body(changelog=_ABSENT)),

    ("sha256-not-hex", body(sha256="nope")),
    ("sha256-too-short", body(sha256="aabb")),
    ("sha256-odd-digits", body(sha256="a" * 63)),
    ("sha256-uppercase", body(sha256="A" * 64)),
    ("sha256-space-inside-a-pair", body(sha256="a b" + "a" * 61)),
    ("sha256-not-a-string", body(sha256=12345)),
    ("sha256-null", body(sha256=NULL)),
    ("root-hash-not-hex", body(v_root_hash="zz")),
    ("root-hash-uppercase", body(v_root_hash="B" * 64)),

    ("buildtime-string", body(buildtime="tuesday")),
    ("buildtime-float", body(buildtime=1785160800.0)),
    ("buildtime-true", body(buildtime=True)),
    ("buildtime-negative", body(buildtime=-1)),
    ("buildtime-zero", body(buildtime=0)),

    ("block-size-3000", body(v_block_size=3000)),
    ("block-size-511", body(v_block_size=511)),
    ("block-size-512", body(v_block_size=512)),
    ("block-size-zero", body(v_block_size=0)),
    ("block-size-negative", body(v_block_size=-4096)),
    ("block-size-true", body(v_block_size=True)),
    ("block-size-float", body(v_block_size=4096.0)),
    ("block-size-string", body(v_block_size="4096")),

    ("image-blocks-zero", body(v_image_blocks=0)),
    ("image-blocks-negative", body(v_image_blocks=-1)),
    ("image-blocks-one", body(v_image_blocks=1)),
    ("image-blocks-float", body(v_image_blocks=1.0)),
    ("image-blocks-string", body(v_image_blocks="10")),
    ("image-blocks-true", body(v_image_blocks=True)),

    ("verity-string", body(verity="nope")),
    ("verity-null", body(verity=NULL)),
    ("verity-list", body(verity=[1, 2])),

    ("version-empty", body(version="")),
    ("version-int", body(version=12)),
    ("platform-empty", body(platform="")),
    ("platform-null", body(platform=NULL)),

    ("thumbnail-good", body(thumbnail_sha256="c" * 64)),
    ("thumbnail-too-short", body(thumbnail_sha256="deadbeef")),
    ("thumbnail-not-hex", body(thumbnail_sha256="z" * 64)),
    ("thumbnail-empty", body(thumbnail_sha256="")),
    ("thumbnail-null", body(thumbnail_sha256=NULL)),
    ("thumbnail-false", body(thumbnail_sha256=False)),
    ("thumbnail-zero", body(thumbnail_sha256=0)),
    ("thumbnail-not-a-string", body(thumbnail_sha256=99)),

    ("changelog-null", body(changelog=NULL)),
    ("changelog-empty", body(changelog="")),
    ("changelog-false", body(changelog=False)),
    ("changelog-not-a-string", body(changelog=[1, 2])),

    ("min-kernel", body(min_kernel="6.20.0")),
    ("min-kernel-empty", body(min_kernel="")),
    ("min-kernel-null", body(min_kernel=NULL)),
    ("min-kernel-not-a-string", body(min_kernel=99)),

    ("salt-empty", body(v_salt="")),
    ("salt-null", body(v_salt=NULL)),
    ("salt-odd-digits", body(v_salt="abc")),
    ("salt-not-hex", body(v_salt="zz")),
    ("salt-spaced", body(v_salt="f8 e7 d6")),
    ("salt-not-a-string", body(v_salt=42)),

    ("duplicate-keys",
     b'{"version":"x","version":"0.3.2a","buildtime":1,"platform":"p",'
     b'"sha256":"' + b"a" * 64 + b'","verity":{"root_hash":"' + b"b" * 64 +
     b'","block_size":4096,"image_blocks":2}}'),
    ("utf8-in-version", body(version="0.3.2å")),
    ("huge-image-blocks", body(v_image_blocks=4611686018427387904)),

    ("sha256-padded-with-spaces", body(sha256=" ".join(["aa"] * 32))),
    ("buildtime-past-int64", body(buildtime=10 ** 30)),
    ("version-too-long", body(version="x" * 200)),
    ("salt-too-long", body(v_salt="a" * 1024)),
    ("utf8-bom", b"\xef\xbb\xbf" + body()),
]


@pytest.mark.parametrize("label,raw", CASES, ids=[c[0] for c in CASES])
def test_the_two_validators_agree(tmp_path, label, raw):
    c_verdict, c_value = c_parse(tmp_path, raw)
    p_verdict, p_value = py_parse(raw)

    if label in DIVERGENT:
        assert p_verdict == "OK" and c_verdict == "ERR", (
            "%s is listed as a deliberate divergence, but python=%s c=%s. A "
            "divergence is only ever the C refusing what Python accepts; "
            "either the code changed or the list is stale.\n  %s"
            % (label, p_verdict, c_verdict, DIVERGENT[label]))
        return

    assert c_verdict == p_verdict, (
        "%s: python=%s(%r) c=%s(%r)" % (label, p_verdict, p_value, c_verdict, c_value))
    if c_verdict == "ERR":
        assert c_value == p_value, "%s: refusal differs" % label
        return

    if label in SATURATES:
        assert c_value.pop("image_bytes") == 2 ** 64 - 1, SATURATES[label]
        assert p_value.pop("image_bytes") > 2 ** 64 - 1
    assert c_value == p_value, "%s: fields differ" % label


def test_json_syntax_errors_agree_on_the_verdict_and_the_prefix(tmp_path):
    """The parser detail after the colon is CPython's json module talking and
    cannot be reproduced -- but "manifest is not valid JSON: " is the part the
    screen shows and the part a test matches on."""
    for raw in (b"{not json", b"", b"null", b"42", b'"hello"', b"[1,2,3]",
                json.dumps(GOOD).encode() + b" trailing"):
        c_verdict, c_why = c_parse(tmp_path, raw)
        p_verdict, p_why = py_parse(raw)
        assert c_verdict == p_verdict == "ERR", raw
        if p_why.startswith("manifest is not valid JSON:"):
            assert c_why.startswith("manifest is not valid JSON:"), (raw, c_why)
        else:
            assert c_why == p_why, raw


def test_the_derived_size_saturates_where_python_grows(tmp_path):
    """Python multiplies two bignums; C saturates at UINT64_MAX rather than
    wrapping a hostile block count into a small, plausible-looking size."""
    raw = body(v_image_blocks=4611686018427387904)
    _, c_value = c_parse(tmp_path, raw)
    _, p_value = py_parse(raw)
    assert p_value["image_bytes"] == 4611686018427387904 * 4096
    assert c_value["image_bytes"] == 2 ** 64 - 1, c_value


COMPAT = [
    ("qemu-aarch64", None, None),
    ("luckfox-armv7", None, None),
    ("qemu-aarch64", "6.12.47", "6.20.0"),
    ("qemu-aarch64", "6.12.47", "6.12.0"),
    ("qemu-aarch64", "6.12.47", "6.12.47"),
    ("qemu-aarch64", "6.12", "6.12.47"),
    ("qemu-aarch64", "6.12.47-rt", "6.12.47"),
    ("qemu-aarch64", "6.x.3", "6.12"),
    ("qemu-aarch64", "", "6.20.0"),
    ("qemu-aarch64", None, "99.0.0"),
    ("qemu-aarch64", "7", "6.99.99"),
    ("qemu-aarch64", "6.99.99", "7"),
]


@pytest.mark.parametrize("platform,kernel,min_kernel", COMPAT)
def test_check_compatible_agrees_including_its_wording(tmp_path, platform, kernel, min_kernel):
    """THE BRICK CASE, and the only refusal in the subsystem that is never
    overridable. Compared down to the sentence, because the app pastes it
    straight after "WRONG UPDATE FOR THIS PHONE!"."""
    raw = body() if min_kernel is None else body(min_kernel=min_kernel)
    args = ["--compat", platform] if kernel is None else ["--compat", platform, kernel]
    c_verdict, c_fields = c_run(tmp_path, raw, *args)

    m = manifest_mod.parse(raw)
    try:
        m.check_compatible(platform=platform, kernel=kernel)
        p_verdict, p_why = "OK", ""
    except IncompatibleUpdate as exc:
        p_verdict, p_why = "ERR", str(exc)

    assert c_verdict == p_verdict, (platform, kernel, min_kernel, p_why)
    assert c_fields.get("why", "") == p_why

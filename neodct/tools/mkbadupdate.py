#!/usr/bin/env python3
"""Make deliberately broken .ndsw packages, for testing the refusals by hand.

Takes a good package (from `make update`) and writes variants that each fail
in one specific way, so you can watch the phone react to each. What every
variant should produce on screen is in docs/TESTING_UPDATES.md, and
neodct/tests/test_mkbadupdate.py asserts each one still fails the way it is
supposed to.

    neodct/tools/mkbadupdate.py buildroot/output/images/UPDATE.ndsw \
        --output-dir /tmp/bad --key neodct/tools/devkey/neodct-dev.key

Variants that alter the manifest are re-signed, so they fail for exactly one
reason rather than tripping the signature check on the way past. That needs
the private key; without --key those variants are refused instead of being
written misleadingly.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

# The Python OS is the reference implementation, not something that ships.
# It lives outside the overlay so BR2_ROOTFS_OVERLAY cannot put it on a
# phone; see neodct/python-reference/README.md.
REPO_NEODCT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "python-reference",
)
if REPO_NEODCT not in sys.path:
    sys.path.insert(0, REPO_NEODCT)

IMAGE_MEMBER = "rootfs.squashfs"
MANIFEST_MEMBER = "manifest.json"
SIGNATURE_MEMBER = "manifest.sig"

# variant -> (what it breaks, needs the private key to re-sign)
VARIANTS = {
    "unsigned":           ("manifest.sig removed", False),
    "wrong-key":          ("signed by a different key", False),
    "tampered-manifest":  ("manifest edited after signing", False),
    "corrupt-image":      ("one byte flipped in the image", False),
    "truncated-image":    ("last 4K of the image cut off", False),
    "no-manifest":        ("manifest.json removed", False),
    "no-image":           ("rootfs.squashfs removed", False),
    "not-a-zip":          ("not an archive at all", False),
    "wrong-platform":     ("built for luckfox-armv7", True),
    "future-kernel":      ("needs kernel 99.0.0", True),
    "bad-root-hash":      ("verity root hash does not match the image", True),
}


def _read_members(path):
    with zipfile.ZipFile(str(path), "r") as archive:
        names = archive.namelist()
        return {name: archive.read(name) for name in names}


def _write_members(path, members):
    with zipfile.ZipFile(str(path), "w") as archive:
        for name, data in members.items():
            compress = (zipfile.ZIP_STORED if name == IMAGE_MEMBER
                        else zipfile.ZIP_DEFLATED)
            archive.writestr(zipfile.ZipInfo(name), data, compress_type=compress)
    return path


def _sign(raw, key_path):
    result = subprocess.run(
        ["openssl", "dgst", "-sha256", "-sign", str(key_path)],
        input=raw, capture_output=True)
    if result.returncode != 0:
        sys.exit("mkbadupdate: signing failed: %s"
                 % result.stderr.decode(errors="replace").strip())
    return result.stdout


def _encode(body):
    return (json.dumps(body, indent=2, sort_keys=True) + "\n").encode()


def _resign(members, body, key_path):
    """Replace the manifest and its signature so only `body` is wrong."""
    raw = _encode(body)
    members[MANIFEST_MEMBER] = raw
    members[SIGNATURE_MEMBER] = _sign(raw, key_path)
    return members


def make_variant(name, source, output_dir, key_path=None):
    """Write one broken package. Returns its path."""
    if name not in VARIANTS:
        sys.exit("mkbadupdate: unknown variant %r (have: %s)"
                 % (name, ", ".join(sorted(VARIANTS))))
    _, needs_key = VARIANTS[name]
    if needs_key and not key_path:
        sys.exit("mkbadupdate: variant %s edits the manifest and must be "
                 "re-signed, so it needs a private key (--key). Without one it "
                 "would also fail the signature check and you could not tell "
                 "which refusal you were looking at." % name)

    output = os.path.join(str(output_dir), "%s.ndsw" % name.replace("-", "_"))

    if name == "not-a-zip":
        with open(output, "wb") as handle:
            handle.write(b"NeoDCT: this is deliberately not a zip archive.\n")
        return output

    members = _read_members(source)

    if name == "unsigned":
        members.pop(SIGNATURE_MEMBER, None)

    elif name == "wrong-key":
        # A throwaway key, so the signature is well-formed but from a
        # keyholder the phone has never heard of.
        temp_dir = tempfile.mkdtemp(prefix="mkbadupdate-")
        other = os.path.join(temp_dir, "other.key")
        subprocess.run(["openssl", "genpkey", "-algorithm", "RSA",
                        "-pkeyopt", "rsa_keygen_bits:2048", "-out", other],
                       check=True, capture_output=True)
        members[SIGNATURE_MEMBER] = _sign(members[MANIFEST_MEMBER], other)
        shutil.rmtree(temp_dir, ignore_errors=True)

    elif name == "tampered-manifest":
        # Edit after signing: the classic "someone changed the version".
        body = json.loads(members[MANIFEST_MEMBER])
        body["version"] = body.get("version", "0.0.0") + "-tampered"
        members[MANIFEST_MEMBER] = _encode(body)   # signature left alone

    elif name == "corrupt-image":
        image = bytearray(members[IMAGE_MEMBER])
        # Middle of the squashfs, well inside the data verity covers.
        offset = len(image) // 2
        image[offset] ^= 0xFF
        members[IMAGE_MEMBER] = bytes(image)

    elif name == "truncated-image":
        members[IMAGE_MEMBER] = members[IMAGE_MEMBER][:-4096]

    elif name == "no-manifest":
        members.pop(MANIFEST_MEMBER, None)

    elif name == "no-image":
        members.pop(IMAGE_MEMBER, None)

    elif name == "wrong-platform":
        body = json.loads(members[MANIFEST_MEMBER])
        body["platform"] = ("qemu-aarch64" if body.get("platform") != "qemu-aarch64"
                            else "luckfox-armv7")
        members = _resign(members, body, key_path)

    elif name == "future-kernel":
        body = json.loads(members[MANIFEST_MEMBER])
        body["min_kernel"] = "99.0.0"
        members = _resign(members, body, key_path)

    elif name == "bad-root-hash":
        body = json.loads(members[MANIFEST_MEMBER])
        root = body["verity"]["root_hash"]
        # Flip the first nibble: still valid hex, still the right length, and
        # nothing before dm-verity will notice.
        body["verity"]["root_hash"] = ("f" if root[0] != "f" else "0") + root[1:]
        members = _resign(members, body, key_path)

    return _write_members(output, members)


def make_all(source, output_dir, key_path=None):
    """Write every variant the available key allows. Returns {name: path}."""
    produced = {}
    for name, (_, needs_key) in sorted(VARIANTS.items()):
        if needs_key and not key_path:
            print("mkbadupdate: skipping %s (needs --key)" % name,
                  file=sys.stderr)
            continue
        produced[name] = make_variant(name, source, output_dir,
                                      key_path=key_path)
    return produced


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", nargs="?",
                        help="a good UPDATE.ndsw to break")
    parser.add_argument("--output-dir", default=".")
    parser.add_argument("--key", default=os.environ.get("NEODCT_SIGN_KEY"),
                        help="private key, for variants that must be re-signed")
    parser.add_argument("--variant", action="append",
                        help="only this variant (repeatable); default is all")
    parser.add_argument("--list", action="store_true", help="list the variants")
    args = parser.parse_args(argv)

    if args.list:
        width = max(len(name) for name in VARIANTS)
        for name, (what, needs_key) in sorted(VARIANTS.items()):
            print("  %-*s  %s%s" % (width, name, what,
                                    "  (needs --key)" if needs_key else ""))
        return 0

    if not args.source:
        parser.error("a source UPDATE.ndsw is required (or use --list)")
    if not os.path.exists(args.source):
        sys.exit("mkbadupdate: no such file: %s" % args.source)
    os.makedirs(args.output_dir, exist_ok=True)

    if args.variant:
        produced = {name: make_variant(name, args.source, args.output_dir,
                                       key_path=args.key)
                    for name in args.variant}
    else:
        produced = make_all(args.source, args.output_dir, key_path=args.key)

    for name, path in sorted(produced.items()):
        print("%-20s %s  (%s)" % (name, path, VARIANTS[name][0]))
    if not args.key:
        print("\nNote: no key given, so nothing was re-signed and the "
              "manifest-editing variants were skipped.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

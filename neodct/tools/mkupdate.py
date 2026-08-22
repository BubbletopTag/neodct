#!/usr/bin/env python3
"""Build a NeoDCT system image and/or an UPDATE.ndsw from a buildroot tree.

Called by `make update` inside buildroot (see the `update` target) and by
neodct/scripts/post-image-neodct.sh, which needs the same verity-appended
image for the factory build.

The image is the padded squashfs with the dm-verity hash area appended, so
one file carries both the filesystem and the tree that authenticates it:

    [ squashfs, 4K aligned ][ verity superblock ][ hash tree ]
                            ^ manifest.verity.image_blocks * block_size

Signing shells out to openssl on the build host -- the phone only ever
verifies, and it does that with System/core/UpdateService/signing.py.

    NEODCT_SIGN_KEY=~/.config/neodct/release.key make update

With no key the package is still written, unsigned; the phone then shows
the BAD SIGNATURE warning, which engineering mode can override.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import zipfile

REPO_NEODCT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "overlay", "NeoDCT",
)
if REPO_NEODCT not in sys.path:
    sys.path.insert(0, REPO_NEODCT)

from System.core.UpdateService import package  # noqa: E402  (needs sys.path above)
from System.core.UpdateService import verity  # noqa: E402

BLOCK_SIZE = 4096
SALT_BYTES = 32
IMAGE_MEMBER = "rootfs.squashfs"
MANIFEST_MEMBER = "manifest.json"
SIGNATURE_MEMBER = "manifest.sig"
THUMBNAIL_MEMBER = "thumbnail.png"
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def build_system_image(squashfs, salt=None, block_size=BLOCK_SIZE):
    """Pad the squashfs to a block boundary and append its verity hash area."""
    if not squashfs:
        raise ValueError("squashfs image is empty")
    if salt is None:
        salt = os.urandom(SALT_BYTES)
    padded = squashfs + b"\x00" * (-len(squashfs) % block_size)
    tree = verity.build_hash_tree(padded, salt=salt, data_block_size=block_size,
                                  hash_block_size=block_size)
    return padded + verity.format_hash_area(tree), tree


def build_manifest(tree, image, version, buildtime, changelog, platform,
                   min_kernel=None):
    body = {
        "version": version,
        "buildtime": int(buildtime),
        "changelog": changelog or "",
        "platform": platform,
        "sha256": hashlib.sha256(image).hexdigest(),
        "verity": {
            "root_hash": tree.root_hash,
            "block_size": tree.data_block_size,
            "image_blocks": tree.data_blocks,
            "salt": tree.salt.hex(),
        },
    }
    if min_kernel:
        body["min_kernel"] = min_kernel
    return body


def encode_manifest(body):
    """Serialise once, sign and ship those exact bytes."""
    return (json.dumps(body, indent=2, sort_keys=True) + "\n").encode()


def sign_manifest(raw, key_path):
    """Detached RSA/SHA-256 signature over the manifest bytes, via openssl."""
    if shutil.which("openssl") is None:
        sys.exit("mkupdate: openssl is needed to sign (or unset NEODCT_SIGN_KEY)")
    result = subprocess.run(
        ["openssl", "dgst", "-sha256", "-sign", str(key_path)],
        input=raw, capture_output=True)
    if result.returncode != 0:
        sys.exit("mkupdate: signing failed: %s"
                 % result.stderr.decode(errors="replace").strip())
    return result.stdout


def write_ndsw(path, image, manifest_body, key_path=None, thumbnail=None):
    """Write the .ndsw zip. Unsigned when key_path is None.

    The thumbnail's hash goes into the manifest here rather than in
    build_manifest, so the bytes that get written and the bytes that get
    signed cannot disagree: there is one place that knows about both.
    """
    if thumbnail is not None:
        manifest_body = dict(manifest_body)
        manifest_body["thumbnail_sha256"] = hashlib.sha256(thumbnail).hexdigest()
    raw = encode_manifest(manifest_body)
    with zipfile.ZipFile(str(path), "w", zipfile.ZIP_DEFLATED) as archive:
        # Stored, not deflated: a squashfs is already compressed, and
        # deflating 50MB for nothing costs a minute of build time.
        archive.writestr(zipfile.ZipInfo(IMAGE_MEMBER), image,
                         compress_type=zipfile.ZIP_STORED)
        archive.writestr(MANIFEST_MEMBER, raw)
        if thumbnail is not None:
            archive.writestr(THUMBNAIL_MEMBER, thumbnail)
        if key_path:
            archive.writestr(SIGNATURE_MEMBER, sign_manifest(raw, key_path))
    return path


def read_thumbnail(path):
    """Load the release picture, refusing anything the phone would not draw."""
    try:
        with open(str(path), "rb") as handle:
            data = handle.read()
    except OSError as exc:
        sys.exit("mkupdate: cannot read the thumbnail: %s" % exc)
    if not data.startswith(PNG_MAGIC):
        sys.exit("mkupdate: %s is not a PNG -- thumbnails must be a square PNG "
                 "(64x64 is the size the phone draws)" % path)
    if len(data) > package.MAX_THUMBNAIL_BYTES:
        sys.exit("mkupdate: thumbnail is %d bytes, the phone refuses anything "
                 "over %d" % (len(data), package.MAX_THUMBNAIL_BYTES))
    return data


def changelog_section(text, version):
    """The block of CHANGELOG.txt under a bare `version` heading."""
    lines = (text or "").splitlines()
    collected = []
    inside = False
    for line in lines:
        stripped = line.strip()
        if stripped == version:
            inside = True
            continue
        if inside:
            # A bare version-looking heading starts the next section.
            if stripped and stripped[0].isdigit() and " " not in stripped \
                    and stripped != version:
                break
            collected.append(line.rstrip())
    return "\n".join(collected).strip()


def read_target_version(target_dir):
    """Read version.prop out of a built target tree."""
    path = os.path.join(str(target_dir), "NeoDCT", "System", "version.prop")
    if not os.path.exists(path):
        sys.exit("mkupdate: no version.prop in %s -- is the target built?" % path)
    values = {}
    with open(path) as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            values[key.strip()] = value.strip()
    return {
        "version": values.get("system.os.versionnumber", ""),
        "platform": values.get("system.os.platform", ""),
        # buildepoch is the machine-readable one; system.os.buildtime is the
        # human string the About screen shows.
        "buildtime": values.get("system.os.buildepoch", ""),
        "min_kernel": values.get("system.os.min_kernel", ""),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--images-dir", required=True,
                        help="buildroot BINARIES_DIR (holds rootfs.squashfs)")
    parser.add_argument("--target-dir",
                        help="buildroot TARGET_DIR, for version.prop")
    parser.add_argument("--version")
    parser.add_argument("--platform")
    parser.add_argument("--buildtime", type=int)
    parser.add_argument("--changelog-file")
    parser.add_argument("--min-kernel")
    parser.add_argument("--thumbnail", metavar="PNG",
                        help="square PNG shown on the phone's update page")
    parser.add_argument("--key", default=os.environ.get("NEODCT_SIGN_KEY"),
                        help="private key to sign with (default $NEODCT_SIGN_KEY)")
    parser.add_argument("--image-only", action="store_true",
                        help="write system.img + system.manifest.json, no zip")
    parser.add_argument("--installed-prop", metavar="STATE_DIR",
                        help="also seed installed.prop for a factory image")
    parser.add_argument("--output")
    args = parser.parse_args(argv)

    images_dir = args.images_dir
    squashfs_path = os.path.join(images_dir, "rootfs.squashfs")
    if not os.path.exists(squashfs_path):
        sys.exit("mkupdate: %s not found -- enable BR2_TARGET_ROOTFS_SQUASHFS"
                 % squashfs_path)

    info = {"version": "", "platform": "", "buildtime": "", "min_kernel": ""}
    if args.target_dir:
        info = read_target_version(args.target_dir)
    version = args.version or info["version"]
    platform = args.platform or info["platform"]
    if not version or not platform:
        sys.exit("mkupdate: need --version and --platform (or --target-dir)")
    buildtime = args.buildtime or int(info["buildtime"] or 0) or int(time.time())
    min_kernel = args.min_kernel or info["min_kernel"]

    changelog = ""
    changelog_path = args.changelog_file or os.path.join(
        REPO_NEODCT, "CHANGELOG.txt")
    if os.path.exists(changelog_path):
        with open(changelog_path, errors="replace") as handle:
            changelog = changelog_section(handle.read(), version)

    with open(squashfs_path, "rb") as handle:
        squashfs = handle.read()
    image, tree = build_system_image(squashfs)
    body = build_manifest(tree, image, version=version, buildtime=buildtime,
                          changelog=changelog, platform=platform,
                          min_kernel=min_kernel)

    if args.image_only:
        image_path = args.output or os.path.join(images_dir, "system.img")
        with open(image_path, "wb") as handle:
            handle.write(image)
        with open(os.path.join(images_dir, "system.manifest.json"), "wb") as handle:
            handle.write(encode_manifest(body))
        if args.installed_prop:
            # A factory image is already "installed": seed the record the
            # initramfs reads to build its dm-verity table on first boot.
            from System.core.UpdateService import manifest as manifest_mod
            from System.core.UpdateService import staging
            staging.record_installed(manifest_mod.parse(encode_manifest(body)),
                                     args.installed_prop,
                                     image_bytes=len(image))
        print("mkupdate: %s (%.1f MiB, root %s)"
              % (image_path, len(image) / 1048576.0, tree.root_hash[:16]))
        return 0

    output = args.output or os.path.join(images_dir, "UPDATE.ndsw")
    thumbnail = read_thumbnail(args.thumbnail) if args.thumbnail else None
    write_ndsw(output, image, body, key_path=args.key, thumbnail=thumbnail)
    print("mkupdate: %s (%s %s, %.1f MiB, %s)"
          % (output, version, platform, os.path.getsize(output) / 1048576.0,
             "signed" if args.key else "UNSIGNED -- set NEODCT_SIGN_KEY"))

    # Keep a copy under its own name. UPDATE.ndsw is a fixed path, so every
    # build silently destroys the last one -- which is how several releases'
    # packages were lost before anyone thought to archive them. The archived
    # name is also the name a release asset wants, so uploading is a copy
    # rather than a rebuild.
    if not args.output:
        archive_dir = os.path.join(images_dir, "packages")
        archive = os.path.join(archive_dir,
                               "UPDATE-%s-%s.ndsw" % (platform, version))
        try:
            os.makedirs(archive_dir, exist_ok=True)
            shutil.copy2(output, archive)
            print("mkupdate: archived %s" % archive)
        except OSError as exc:
            # Not fatal: the package itself is built and the archive is a
            # convenience. Say so rather than failing the build over it.
            print("mkupdate: could not archive (%s)" % exc, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

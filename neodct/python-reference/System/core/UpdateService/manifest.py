"""manifest.json: what an update claims to be, and whether we'll take it."""

import json

from . import IncompatibleUpdate, InvalidUpdate

REQUIRED = ("version", "buildtime", "platform", "sha256", "verity")
REQUIRED_VERITY = ("root_hash", "block_size", "image_blocks")


def _hex(value, field, length=None):
    if not isinstance(value, str):
        raise InvalidUpdate("%s must be a hex string" % field)
    try:
        raw = bytes.fromhex(value)
    except ValueError:
        raise InvalidUpdate("%s is not valid hex" % field)
    if length is not None and len(raw) != length:
        raise InvalidUpdate("%s must be %d hex digits" % (field, length * 2))
    return value


def _version_tuple(text):
    """"6.12.47" -> (6, 12, 47). Trailing junk ("6.12.47-rt") is ignored."""
    parts = []
    for chunk in str(text).split("."):
        digits = ""
        for char in chunk:
            if not char.isdigit():
                break
            digits += char
        if not digits:
            break
        parts.append(int(digits))
    return tuple(parts)


class Manifest:
    def __init__(self, body, raw):
        self.raw = raw
        self.body = body
        self.version = body["version"]
        self.buildtime = body["buildtime"]
        self.platform = body["platform"]
        self.sha256 = body["sha256"]
        self.changelog = body.get("changelog") or ""
        self.min_kernel = body.get("min_kernel") or ""
        self.thumbnail_sha256 = body.get("thumbnail_sha256") or ""
        self.verity = body["verity"]

    @property
    def hash_offset(self):
        """Byte offset of the verity hash area: straight after the squashfs."""
        return self.verity["image_blocks"] * self.verity["block_size"]

    @property
    def image_bytes(self):
        return self.verity["image_blocks"] * self.verity["block_size"]

    def check_compatible(self, platform, kernel=None):
        """Raise IncompatibleUpdate unless this package fits this device."""
        if self.platform != platform:
            raise IncompatibleUpdate(
                "update is for %s, this is %s" % (self.platform, platform))
        if self.min_kernel and kernel:
            if _version_tuple(kernel) < _version_tuple(self.min_kernel):
                raise IncompatibleUpdate(
                    "update needs kernel %s, running %s"
                    % (self.min_kernel, kernel))

    def __repr__(self):
        return "<Manifest %s %s>" % (self.version, self.platform)


def parse(raw):
    """Parse manifest bytes. Keeps `raw` verbatim for signature checking."""
    try:
        body = json.loads(raw)
    except (ValueError, UnicodeDecodeError) as exc:
        raise InvalidUpdate("manifest is not valid JSON: %s" % exc)
    if not isinstance(body, dict):
        raise InvalidUpdate("manifest must be a JSON object")

    for field in REQUIRED:
        if field not in body:
            raise InvalidUpdate("manifest is missing %s" % field)

    if not isinstance(body["version"], str) or not body["version"]:
        raise InvalidUpdate("version must be a non-empty string")
    if not isinstance(body["platform"], str) or not body["platform"]:
        raise InvalidUpdate("platform must be a non-empty string")
    if isinstance(body["buildtime"], bool) or not isinstance(body["buildtime"], int):
        raise InvalidUpdate("buildtime must be a unix timestamp")
    _hex(body["sha256"], "sha256", 32)
    if body.get("thumbnail_sha256"):
        _hex(body["thumbnail_sha256"], "thumbnail_sha256", 32)

    verity = body["verity"]
    if not isinstance(verity, dict):
        raise InvalidUpdate("verity must be a JSON object")
    for field in REQUIRED_VERITY:
        if field not in verity:
            raise InvalidUpdate("verity is missing %s" % field)
    _hex(verity["root_hash"], "root_hash", 32)
    verity["salt"] = _hex(verity.get("salt") or "", "salt")

    block_size = verity["block_size"]
    if not isinstance(block_size, int) or block_size < 512 \
            or block_size & (block_size - 1):
        raise InvalidUpdate("block_size must be a power of two >= 512")
    if not isinstance(verity["image_blocks"], int) or verity["image_blocks"] < 1:
        raise InvalidUpdate("image_blocks must be a positive integer")

    return Manifest(body, raw)

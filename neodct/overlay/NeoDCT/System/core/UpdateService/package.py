"""Reading .ndsw packages off an SD card.

Everything streams: a system image is tens of megabytes and the phone has
64MB of RAM, so the image is never held in memory -- it is copied out in
chunks while its sha256 is computed on the way past.
"""

import hashlib
import os
import zipfile

from . import BadSignature, InvalidUpdate, UpdateError
from . import manifest as manifest_mod
from . import signing

IMAGE_MEMBER = "rootfs.squashfs"
MANIFEST_MEMBER = "manifest.json"
SIGNATURE_MEMBER = "manifest.sig"
THUMBNAIL_MEMBER = "thumbnail.png"

# A 64x64 PNG is a couple of kilobytes; the cap only exists so a package
# cannot ask a 64MB phone to decompress a photo album.
MAX_THUMBNAIL_BYTES = 256 * 1024

CHUNK = 256 * 1024
# Leave room so staging an update can never be what fills the partition.
SPACE_MARGIN = 4 * 1024 * 1024


class NotEnoughSpace(UpdateError):
    """The staging partition cannot hold this image."""


class Package:
    def __init__(self, zipfile_handle, path):
        self._zip = zipfile_handle
        self.path = str(path)
        self.signed = False

        names = self._zip.namelist()
        for required in (MANIFEST_MEMBER, IMAGE_MEMBER):
            if required not in names:
                raise InvalidUpdate("package has no %s" % required)
        self.manifest = manifest_mod.parse(self._zip.read(MANIFEST_MEMBER))

    @property
    def image_size(self):
        return self._zip.getinfo(IMAGE_MEMBER).file_size

    def verify_signature(self, key_path):
        """Raise BadSignature unless manifest.json is signed by `key_path`.

        Verification is over the manifest bytes exactly as stored, never a
        re-encoding of the parsed object.
        """
        try:
            signature = self._zip.read(SIGNATURE_MEMBER)
        except KeyError:
            raise BadSignature("update is not signed")
        try:
            with open(key_path, "rb") as handle:
                key = signing.load_public_key(handle.read())
        except (OSError, ValueError) as exc:
            raise BadSignature("cannot read the release key: %s" % exc)

        if not signing.verify(self.manifest.raw, signature, key):
            raise BadSignature("signature does not match manifest.json")
        self.signed = True

    def read_thumbnail(self):
        """The package's picture, or None if it hasn't got one.

        Only art the manifest vouches for comes back: the signature covers
        manifest.json alone, so a thumbnail with no hash recorded there is
        an unsigned attachment and is treated as absent.
        """
        wanted = self.manifest.thumbnail_sha256
        if not wanted:
            return None
        try:
            info = self._zip.getinfo(THUMBNAIL_MEMBER)
        except KeyError:
            return None
        if info.file_size > MAX_THUMBNAIL_BYTES:
            raise InvalidUpdate(
                "thumbnail is %d bytes, over the %d byte limit"
                % (info.file_size, MAX_THUMBNAIL_BYTES))

        data = self._zip.read(THUMBNAIL_MEMBER)
        if hashlib.sha256(data).hexdigest() != wanted:
            raise InvalidUpdate("thumbnail does not match the manifest")
        return data

    def extract_image(self, dest, progress=None, free_bytes=None):
        """Copy the system image to `dest`, verifying its sha256 as it goes.

        On any mismatch the partial file is removed: a half-written image
        must never be left behind where the applier could find it.
        """
        dest = str(dest)
        total = self.image_size
        if free_bytes is None:
            free_bytes = _free_space(os.path.dirname(dest) or ".")
        if free_bytes is not None and free_bytes < total + SPACE_MARGIN:
            raise NotEnoughSpace(
                "image needs %d bytes, only %d free" % (total, free_bytes))

        digest = hashlib.sha256()
        done = 0
        try:
            with self._zip.open(IMAGE_MEMBER) as source, \
                    open(dest, "wb") as target:
                while True:
                    chunk = source.read(CHUNK)
                    if not chunk:
                        break
                    target.write(chunk)
                    digest.update(chunk)
                    done += len(chunk)
                    if progress:
                        progress(done, total)
                target.flush()
                os.fsync(target.fileno())

            if digest.hexdigest() != self.manifest.sha256:
                raise InvalidUpdate(
                    "image sha256 does not match the manifest "
                    "(%s != %s)" % (digest.hexdigest(), self.manifest.sha256))
        except BaseException:
            _unlink(dest)
            raise
        return dest

    def close(self):
        self._zip.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
        return False


def open_package(path):
    """Open an .ndsw. Raises InvalidUpdate if it isn't a usable package."""
    try:
        handle = zipfile.ZipFile(str(path), "r")
    except FileNotFoundError:
        raise InvalidUpdate("no such update file: %s" % path)
    except (zipfile.BadZipFile, OSError) as exc:
        raise InvalidUpdate("not a readable zip archive: %s" % exc)
    try:
        return Package(handle, path)
    except BaseException:
        handle.close()
        raise


def _free_space(directory):
    try:
        stats = os.statvfs(directory)
    except (OSError, AttributeError):
        return None
    return stats.f_bavail * stats.f_frsize


def _unlink(path):
    try:
        os.unlink(path)
    except OSError:
        pass

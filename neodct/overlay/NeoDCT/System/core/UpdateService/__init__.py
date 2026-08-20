"""NeoDCT system update handling (.ndsw packages).

An .ndsw is a zip holding a system image and a signed description of it:

    UPDATE.ndsw/
      rootfs.squashfs      squashfs, with the dm-verity hash tree appended
      manifest.json        version, buildtime, changelog, platform, verity
      manifest.sig         detached RSA/SHA-256 signature over manifest.json
      thumbnail.png        optional square picture for the update page

manifest.json carries the sha256 of the whole image plus the verity root
hash, so signing the manifest is enough to cover the image itself. The
thumbnail's hash lives there too, for the same reason: one signature over
manifest.json authenticates every other member.

The failure taxonomy drives what the UI is allowed to offer:

  InvalidUpdate       structurally broken (no manifest, no image, bad JSON).
                      Dead end -- there is nothing to install.
  BadSignature        intact but unsigned or wrongly signed. Engineering
                      mode may override this after a warning.
  IncompatibleUpdate  built for another platform or a newer kernel. Never
                      overridable: installing it would brick the phone.
"""


class UpdateError(Exception):
    """Base class for every refusal to install."""


class InvalidUpdate(UpdateError):
    """The package is malformed or incomplete."""


class BadSignature(UpdateError):
    """The signature is missing or does not verify."""


class IncompatibleUpdate(UpdateError):
    """The package targets different hardware or a newer kernel."""


__all__ = [
    "UpdateError",
    "InvalidUpdate",
    "BadSignature",
    "IncompatibleUpdate",
]

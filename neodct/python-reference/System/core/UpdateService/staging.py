"""Pending-update state, shared between the UI and the boot-time applier.

SystemUpdate writes; neodct/initramfs/init reads. That reader is a busybox
shell script, so records are flat KEY=value lines (same shape as
settings.prop) instead of JSON -- and the init script parses them with
`while IFS='=' read`, never by sourcing them, so a value can never execute.

Ordering is the whole point of this module. The image lands on disk first
and the record file is written last with an atomic rename, so the applier
can only ever see a pending update whose image is already complete. A
power cut mid-stage leaves an orphan image and no record, which reads as
"nothing pending" and is cleaned up on the next stage.
"""

import os

# Everything lives on the writable user partition: / and /NeoDCT/System are
# read-only squashfs at runtime.
STATE_DIR = "/NeoDCT/User/.ndsys"

PENDING_RECORD = "pending.prop"
PENDING_IMAGE = "pending.img"
INSTALLED_RECORD = "installed.prop"
RESULT_RECORD = "last_result.prop"

MAX_ATTEMPTS = 3

# Common to both kinds of pending record. What names the bytes to install
# differs: "image" for a copy on the user partition, "package" for a .ndsw
# left on the card. See stage() and stage_package().
_PENDING_REQUIRED = ("image_bytes", "sha256", "version", "platform",
                     "verity_root_hash", "verity_block_size",
                     "verity_image_blocks")


class Record:
    """A parsed KEY=value record with typed access to the fields we need.

    Knows the directory it was read from, because the recorded image path is
    only meaningful where it was written: the running system has the user
    partition at /NeoDCT/User, the initramfs has the same partition at
    /mnt/user. Paths are therefore resolved against `state_dir`, never
    trusted as written.
    """

    def __init__(self, values, state_dir=None):
        self.values = values
        self.state_dir = state_dir

    def __getattr__(self, name):
        try:
            return self.values[name]
        except KeyError:
            raise AttributeError(name)

    @property
    def verity_block_size(self):
        return int(self.values["verity_block_size"])

    @property
    def verity_image_blocks(self):
        return int(self.values["verity_image_blocks"])

    @property
    def image(self):
        """The staged image, resolved where this record actually lives."""
        recorded = self.values.get("image", "")
        if not self.state_dir:
            return recorded
        return os.path.join(str(self.state_dir),
                            os.path.basename(recorded) or PENDING_IMAGE)

    @property
    def package(self):
        """The .ndsw this record installs from, or "" for a staged image.

        A basename, never a path: the card is mounted somewhere different
        in the initramfs than it was when the record was written, and the
        applier searches for the file by name for the same reason it
        resolves `image` against state_dir.
        """
        return os.path.basename(self.values.get("package", ""))

    @property
    def from_package(self):
        return bool(self.package)

    @property
    def image_bytes(self):
        return int(self.values["image_bytes"])

    @property
    def attempts(self):
        return int(self.values.get("attempts", 0))

    @property
    def exhausted(self):
        return self.attempts >= MAX_ATTEMPTS

    @property
    def hash_offset(self):
        return self.verity_image_blocks * self.verity_block_size

    def __repr__(self):
        return "<Record %s>" % self.values.get("version", "?")


def _write_record(path, values):
    """Write KEY=value lines atomically (temp file, fsync, rename, fsync dir)."""
    for key, value in values.items():
        if "\n" in str(value) or "\r" in str(value):
            raise ValueError("%s contains a newline: records are one line per key"
                             % key)
    directory = os.path.dirname(path)
    os.makedirs(directory, exist_ok=True)
    temp = path + ".new"
    with open(temp, "w") as handle:
        for key in sorted(values):
            handle.write("%s=%s\n" % (key, values[key]))
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temp, path)
    _sync_dir(directory)


def _read_record(path):
    """Parse KEY=value lines. Returns None if the file is missing/unreadable."""
    try:
        with open(path, "r") as handle:
            lines = handle.readlines()
    except OSError:
        return None
    values = {}
    for line in lines:
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = value
    return values


def _sync_dir(directory):
    """Make a rename durable -- otherwise a power cut can lose the record."""
    try:
        fd = os.open(directory, os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(fd)
    except OSError:
        pass
    finally:
        os.close(fd)


def _unlink(path):
    try:
        os.unlink(path)
    except OSError:
        pass


def stage(parsed_manifest, image_path, state_dir=None):
    """Make `image_path` the pending update described by `parsed_manifest`.

    The image is *moved* into the state directory -- a system image is tens
    of megabytes and the user partition cannot be asked to hold two copies.
    """
    state_dir = str(state_dir or STATE_DIR)
    os.makedirs(state_dir, exist_ok=True)
    image = os.path.join(state_dir, PENDING_IMAGE)

    # Drop any earlier attempt before its record, so a crash here reads as
    # "nothing pending" rather than "pending, image missing".
    _unlink(os.path.join(state_dir, PENDING_RECORD))
    os.replace(str(image_path), image)
    _sync_dir(state_dir)

    verity = parsed_manifest.verity
    _write_record(os.path.join(state_dir, PENDING_RECORD), {
        "image": image,
        "image_bytes": os.path.getsize(image),
        "sha256": parsed_manifest.sha256,
        "version": parsed_manifest.version,
        "buildtime": parsed_manifest.buildtime,
        "platform": parsed_manifest.platform,
        "verity_root_hash": verity["root_hash"],
        "verity_block_size": verity["block_size"],
        "verity_image_blocks": verity["image_blocks"],
        "verity_salt": verity.get("salt", ""),
        "attempts": 0,
    })
    return read_pending(state_dir)


def stage_package(parsed_manifest, package_path, image_bytes, state_dir=None):
    """Make the .ndsw at `package_path` the pending update.

    Nothing is copied. The record names the package by basename and the
    applier streams the image out of it at boot, straight to the system
    partition.

    This exists because stage() cannot work on the hardware. It writes the
    whole system image to the user partition first, and on the Luckfox that
    partition is 8 MiB against a 51 MiB image -- there is no card large
    enough to help, because the card is not where the copy goes. QEMU
    builds userdata at 512 MiB, which is why the fault only ever appeared
    on a real phone.

    `image_bytes` is the size of the rootfs.squashfs member inside the
    package, which the caller reads from the zip.

    The signature is checked here, by the running system, before this is
    called. The initramfs has no crypto and cannot repeat that, so the
    record carries the sha256 the verified manifest gave, and the applier
    checks the bytes it is about to write against it. Swapping the card
    between staging and reboot therefore fails the hash rather than
    installing something nobody signed.
    """
    state_dir = str(state_dir or STATE_DIR)
    os.makedirs(state_dir, exist_ok=True)

    # Drop any earlier attempt, record first, so a crash between the two
    # cannot leave a record pointing at an image that has been deleted.
    _unlink(os.path.join(state_dir, PENDING_RECORD))
    _unlink(os.path.join(state_dir, PENDING_IMAGE))

    verity = parsed_manifest.verity
    _write_record(os.path.join(state_dir, PENDING_RECORD), {
        "package": os.path.basename(str(package_path)),
        # The whole rootfs.squashfs member: the squashfs and the verity
        # tree appended to it. Not manifest.image_bytes, which is the
        # squashfs alone -- the sha256 recorded below covers both, and the
        # applier hashes that many bytes back off the device afterwards.
        "image_bytes": int(image_bytes),
        "sha256": parsed_manifest.sha256,
        "version": parsed_manifest.version,
        "buildtime": parsed_manifest.buildtime,
        "platform": parsed_manifest.platform,
        "verity_root_hash": verity["root_hash"],
        "verity_block_size": verity["block_size"],
        "verity_image_blocks": verity["image_blocks"],
        "verity_salt": verity.get("salt", ""),
        "attempts": 0,
    })
    _sync_dir(state_dir)
    return read_pending(state_dir)


def read_pending(state_dir=None):
    """The staged update, or None if there isn't a usable one."""
    state_dir = str(state_dir or STATE_DIR)
    values = _read_record(os.path.join(state_dir, PENDING_RECORD))
    if not values:
        return None
    if any(field not in values for field in _PENDING_REQUIRED):
        return None
    if not values.get("image") and not values.get("package"):
        return None
    record = Record(values, state_dir)
    if record.from_package:
        # The package lives on the card, which may not be mounted here and
        # may not even be in the phone. Whether it is still there is the
        # applier's question at boot; the record itself is usable.
        return record
    if not os.path.exists(record.image):
        return None
    try:
        int(values["verity_block_size"])
        int(values["verity_image_blocks"])
        expected = int(values["image_bytes"])
    except (TypeError, ValueError):
        return None
    # A short image means the copy was interrupted before the record was
    # replaced; refuse it rather than dd a truncated system.
    try:
        if os.path.getsize(record.image) != expected:
            return None
    except OSError:
        return None
    return record


def clear_pending(state_dir=None):
    """Forget the staged update and reclaim its space."""
    state_dir = str(state_dir or STATE_DIR)
    _unlink(os.path.join(state_dir, PENDING_RECORD))
    _unlink(os.path.join(state_dir, PENDING_IMAGE))
    _sync_dir(state_dir)


def note_attempt(state_dir=None):
    """Count one apply attempt and return the new total."""
    state_dir = str(state_dir or STATE_DIR)
    values = _read_record(os.path.join(state_dir, PENDING_RECORD)) or {}
    attempts = int(values.get("attempts", 0)) + 1
    values["attempts"] = attempts
    _write_record(os.path.join(state_dir, PENDING_RECORD), values)
    return attempts


def record_installed(parsed_manifest, state_dir=None, image_bytes=None):
    """Describe the system image now on the system partition.

    The applier writes this after a successful install and the initramfs
    reads it on every later boot to build the dm-verity table.
    """
    verity = parsed_manifest.verity
    _write_record(os.path.join(str(state_dir or STATE_DIR), INSTALLED_RECORD), {
        "sha256": parsed_manifest.sha256,
        "image_bytes": image_bytes if image_bytes is not None else "",
        "version": parsed_manifest.version,
        "buildtime": parsed_manifest.buildtime,
        "platform": parsed_manifest.platform,
        "verity_root_hash": verity["root_hash"],
        "verity_block_size": verity["block_size"],
        "verity_image_blocks": verity["image_blocks"],
        "verity_salt": verity.get("salt", ""),
    })


def read_installed(state_dir=None):
    state_dir = str(state_dir or STATE_DIR)
    values = _read_record(os.path.join(state_dir, INSTALLED_RECORD))
    return Record(values, state_dir) if values else None


def record_result(state_dir=None, result="ok", **fields):
    """Leave a note about the last apply for the UI to show after reboot."""
    values = {"result": result}
    values.update({key: value for key, value in fields.items()
                   if value is not None})
    _write_record(os.path.join(str(state_dir or STATE_DIR), RESULT_RECORD),
                  values)


def read_result(state_dir=None):
    return _read_record(os.path.join(str(state_dir or STATE_DIR),
                                     RESULT_RECORD)) or None


def clear_result(state_dir=None):
    _unlink(os.path.join(str(state_dir or STATE_DIR), RESULT_RECORD))

"""Finding and fetching an update over the network.

The phone asks GitHub for the latest release of its own repository, looks
for the asset built for its own platform, and downloads it onto the SD
card. Everything after that is the path a card-installed update already
takes: the same signature check, the same staging, the same applier. This
module deliberately stops at "there is a file on the card" -- it is the
only part that touches the network, and it has no business also deciding
whether a package is trustworthy.

Two things it will not do:

  * Download into /NeoDCT/User. On the Luckfox that partition is 8MB and a
    package is nearly 60MB. The card is the only place with room, and
    Storage.folder("update") is where a card-installed package would live
    anyway, so an interrupted download leaves a file the existing flow can
    still find.

  * Trust what it downloaded. The release assets are signed with the
    project's own development key, and the public half is baked into the
    image. A package built by anyone else fails verify_signature() and the
    phone says BAD SIGNATURE, which is the correct outcome rather than a
    bug -- see docs/TESTING_UPDATES.md.

Asset naming matters: the qemu and luckfox images are different builds and
their manifests carry different platform strings, so a release has one
asset per platform and the phone picks by name. Installing the wrong one
is refused by manifest.check_compatible() anyway, but downloading 60MB to
find that out would be a poor way to learn it.
"""

import json
import os
import re
import ssl
import urllib.error
import urllib.request

from . import UpdateError

# The project's own repository. Overridable so a fork, or a laptop serving
# a build over the LAN, can be pointed at without rebuilding the image.
DEFAULT_REPO = "BubbletopTag/neodct"
REPO_ENV = "NEODCT_UPDATE_REPO"
API_ROOT = "https://api.github.com/repos/%s/releases/latest"
API_ALL = "https://api.github.com/repos/%s/releases?per_page=%d"

# GitHub rejects requests with no User-Agent.
USER_AGENT = "NeoDCT-Update/1.0 (+https://github.com/BubbletopTag/neodct)"

CONNECT_TIMEOUT = 20
CHUNK = 64 * 1024

# Leave the card room to breathe rather than filling it exactly.
SPACE_MARGIN = 8 * 1024 * 1024


class NetworkError(UpdateError):
    """The phone could not reach the release, whatever the reason."""


class NoRelease(UpdateError):
    """Reached GitHub, but there is nothing here for this phone."""


def asset_name(platform):
    """The asset a phone on this platform should download."""
    return "UPDATE-%s.ndsw" % platform


def repo():
    return os.environ.get(REPO_ENV) or DEFAULT_REPO


def _open(url, timeout=CONNECT_TIMEOUT):
    request = urllib.request.Request(url, headers={
        "User-Agent": USER_AGENT,
        "Accept": "application/vnd.github+json",
    })
    # Default context: verified against the ca-certificates bundle the
    # image ships. An unverified fetch would make the signature check the
    # only thing standing between the phone and a hostile package, and one
    # line of defence is not enough for something that replaces the rootfs.
    context = ssl.create_default_context()
    try:
        return urllib.request.urlopen(request, timeout=timeout,
                                      context=context)
    except urllib.error.HTTPError as exc:
        if exc.code == 404:
            raise NoRelease("no published release for %s" % repo())
        raise NetworkError("GitHub said %s %s" % (exc.code, exc.reason))
    except urllib.error.URLError as exc:
        raise NetworkError("cannot reach GitHub: %s" % exc.reason)
    except (OSError, ssl.SSLError) as exc:
        raise NetworkError("network error: %s" % exc)


def latest(platform):
    """What is published for this platform.

    Returns a dict with version, tag, url, size and notes. Raises
    NoRelease when the newest release carries nothing for this platform,
    which is normal while a release is being uploaded one asset at a time.
    """
    try:
        with _open(API_ROOT % repo()) as response:
            body = json.loads(response.read().decode("utf-8", "replace"))
    except ValueError:
        raise NetworkError("GitHub sent something that is not JSON")

    tag = body.get("tag_name") or ""
    wanted = asset_name(platform)
    for asset in body.get("assets") or ():
        if asset.get("name") == wanted:
            return {
                "version": tag.lstrip("v"),
                "tag": tag,
                "url": asset.get("browser_download_url"),
                "size": int(asset.get("size") or 0),
                "notes": body.get("body") or "",
            }
    raise NoRelease("release %s has no %s" % (tag or "?", wanted))


def all_releases(platform, limit=30):
    """Every published release that has a package for this platform.

    Newest first, as GitHub returns them. Used by the Downgrade tool, which
    needs the whole list rather than just the newest -- the point of it is
    going backwards.

    A release with no asset for this platform is skipped rather than shown
    and then refused: an entry you cannot pick is worse than one that is
    not there.
    """
    try:
        with _open(API_ALL % (repo(), limit)) as response:
            body = json.loads(response.read().decode("utf-8", "replace"))
    except ValueError:
        raise NetworkError("GitHub sent something that is not JSON")
    if not isinstance(body, list):
        raise NetworkError("GitHub did not send a list of releases")

    wanted = asset_name(platform)
    out = []
    for entry in body:
        tag = entry.get("tag_name") or ""
        for asset in entry.get("assets") or ():
            if asset.get("name") == wanted:
                out.append({
                    "version": tag.lstrip("v"),
                    "tag": tag,
                    "url": asset.get("browser_download_url"),
                    "size": int(asset.get("size") or 0),
                    "notes": entry.get("body") or "",
                    "prerelease": bool(entry.get("prerelease")),
                })
                break
    if not out:
        raise NoRelease("no release carries %s" % wanted)
    return out


def is_newer(candidate, installed):
    """True when candidate should be offered over installed.

    Versions look like 0.3.7a: numbers with an optional letter suffix.
    Compared piecewise so 0.3.10a sorts above 0.3.9a, which a plain string
    comparison gets backwards.
    """
    def parts(text):
        out = []
        for chunk in re.split(r"[.\-_]", (text or "").strip()):
            match = re.match(r"^(\d+)([a-zA-Z]*)$", chunk)
            if match:
                out.append((int(match.group(1)), match.group(2)))
            elif chunk:
                out.append((-1, chunk))
        return out
    if not installed:
        return True
    return parts(candidate) > parts(installed)


def enough_space(directory, size):
    """Room for size bytes in directory, with a margin."""
    try:
        stat = os.statvfs(directory)
    except OSError:
        return True          # cannot tell; let the write fail honestly
    return stat.f_bavail * stat.f_frsize >= size + SPACE_MARGIN


def download(url, destination, size=0, progress=None):
    """Stream a package to destination. Returns the bytes written.

    Written to a .part file and renamed only once complete, so a download
    cut off by a dropped carrier -- which on this phone is the expected
    case, not the exception -- never leaves something that looks like an
    installable package.
    """
    directory = os.path.dirname(destination) or "."
    if size and not enough_space(directory, size):
        raise UpdateError("not enough room on the card for %d bytes" % size)

    partial = destination + ".part"
    done = 0
    try:
        with _open(url) as response, open(partial, "wb") as handle:
            while True:
                chunk = response.read(CHUNK)
                if not chunk:
                    break
                handle.write(chunk)
                done += len(chunk)
                if progress is not None:
                    progress(done, size)
            handle.flush()
            os.fsync(handle.fileno())
    except NetworkError:
        _discard(partial)
        raise
    except OSError as exc:
        _discard(partial)
        raise UpdateError("could not write the download: %s" % exc)

    if size and done != size:
        _discard(partial)
        raise NetworkError("download stopped early (%d of %d bytes)"
                           % (done, size))
    os.replace(partial, destination)
    return done


def _discard(path):
    try:
        os.remove(path)
    except OSError:
        pass

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
import time
import urllib.error
import urllib.request

from . import UpdateError

# The project's own repository. Overridable so a fork, or a laptop serving
# a build over the LAN, can be pointed at without rebuilding the image.
DEFAULT_REPO = "BubbletopTag/neodct"
REPO_ENV = "NEODCT_UPDATE_REPO"
API_ALL = "https://api.github.com/repos/%s/releases?per_page=%d"

# GitHub rejects requests with no User-Agent.
USER_AGENT = "NeoDCT-Update/1.0 (+https://github.com/BubbletopTag/neodct)"

CONNECT_TIMEOUT = 20
# Reading the package is not the same job as reaching GitHub. This phone
# is on a carrier, through an antenna glued inside a plastic back cover,
# fetching 53MB. A gap between packets is normal there; treating it as a
# failure after 20 seconds is what made a slow download an impossible one.
DOWNLOAD_TIMEOUT = 120
# How many times to pick the download back up before giving up on it. Each
# attempt resumes, so these are not restarts -- five is a lot of rope.
DOWNLOAD_ATTEMPTS = 5
# Wait between attempts, doubling. Retrying instantly is worse than not
# retrying: when the bearer drops the whole thing goes -- DNS included,
# hence "Temporary failure in name resolution" -- and five immediate
# attempts are five failures in under a second, after which the phone
# gives up on a connection that would have come back on its own.
RETRY_BACKOFF = 5
RETRY_BACKOFF_MAX = 60
# Overridable so the tests do not actually sleep.
_sleep = time.sleep
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


def _open(url, timeout=CONNECT_TIMEOUT, headers=None):
    fields = {
        "User-Agent": USER_AGENT,
        "Accept": "application/vnd.github+json",
    }
    if headers:
        fields.update(headers)
    request = urllib.request.Request(url, headers=fields)
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
    """The newest published package for this platform.

    Returns a dict with version, tag, url, size and notes. Raises
    NoRelease when nothing published carries a package for this platform.

    Built on all_releases rather than GitHub's /releases/latest, which
    looks like the obvious endpoint and is the wrong one here: it ignores
    prereleases, and every NeoDCT release is a prerelease because the
    software is alpha and saying otherwise would be a lie. With no stable
    release ever published, that endpoint answers 404 for this repository
    and the phone concluded there was nothing to install -- not "no newer
    version", but no releases at all, for every release ever made.

    Ordered by version rather than by publication date, so re-publishing
    an old tag cannot make it the newest thing on offer.
    """
    published = all_releases(platform)
    return max(published, key=lambda entry: version_key(entry["version"]))


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


def version_key(text):
    """Sort key for a version like 0.3.10a: numbers first, then any
    letter suffix. Compared piecewise so 0.3.10a sorts above 0.3.9a,
    which a plain string comparison gets backwards."""
    out = []
    for chunk in re.split(r"[.\-_]", (text or "").strip()):
        match = re.match(r"^(\d+)([a-zA-Z]*)$", chunk)
        if match:
            out.append((int(match.group(1)), match.group(2)))
        elif chunk:
            out.append((-1, chunk))
    return out


def is_newer(candidate, installed):
    """True when candidate should be offered over installed.

    Versions look like 0.3.7a: numbers with an optional letter suffix.
    Compared piecewise so 0.3.10a sorts above 0.3.9a, which a plain string
    comparison gets backwards.
    """
    if not installed:
        return True
    return version_key(candidate) > version_key(installed)


def enough_space(directory, size):
    """Room for size bytes in directory, with a margin."""
    try:
        stat = os.statvfs(directory)
    except OSError:
        return True          # cannot tell; let the write fail honestly
    return stat.f_bavail * stat.f_frsize >= size + SPACE_MARGIN


def _fetch_into(url, partial, have, size, progress):
    """One attempt. Appends to `partial` from byte `have`. Returns the total.

    Raises NetworkError on anything that leaves the file short -- the
    caller decides whether to try again, and the bytes already on the card
    stay where they are.
    """
    headers = {}
    if have:
        # Ask for the rest. A server that honours this answers 206 and we
        # append; one that ignores it answers 200 with the whole file, and
        # appending *that* would produce a package with the first N bytes
        # written twice -- which fails its hash later with nothing to say
        # why. So check the status and start over rather than guess.
        headers["Range"] = "bytes=%d-" % have

    with _open(url, timeout=DOWNLOAD_TIMEOUT, headers=headers) as response:
        resuming = have and getattr(response, "status", None) == 206
        if have and not resuming:
            have = 0            # server sent the lot; take it from the top
        mode = "ab" if resuming else "wb"
        done = have
        with open(partial, mode) as handle:
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
    return done


def download(url, destination, size=0, progress=None,
             attempts=DOWNLOAD_ATTEMPTS):
    """Stream a package to destination. Returns the bytes written.

    Resumes. This phone downloads 53MB over a carrier, through an antenna
    glued inside a plastic back cover, and the connection does not always
    survive that. The old version deleted its partial file on any network
    error, so every attempt began at zero and a link that could not carry
    the whole package in one run could never carry it at all -- it did not
    matter how many times you pressed the button.

    Now the partial file survives, each attempt asks for the rest with a
    Range header, and progress accumulates across dropped connections. It
    is thrown away only when it turns out to be wrong: the wrong length at
    the end, or a server that ignored the Range.

    Written as `.part` and renamed only once complete, so a half-finished
    download is never mistaken for an installable package.
    """
    directory = os.path.dirname(destination) or "."
    partial = destination + ".part"

    have = 0
    try:
        have = os.path.getsize(partial)
    except OSError:
        have = 0
    if size and have > size:
        _discard(partial)       # longer than the package: not ours
        have = 0

    # Only the part still to come needs room.
    if size and not enough_space(directory, max(0, size - have)):
        raise UpdateError("not enough room on the card for %d bytes" % size)

    last = None
    total = max(1, attempts)
    for attempt in range(total):
        if attempt:
            # Give the bearer a chance to come back before asking again.
            _sleep(min(RETRY_BACKOFF * (2 ** (attempt - 1)),
                       RETRY_BACKOFF_MAX))
        try:
            done = _fetch_into(url, partial, have, size, progress)
        except NetworkError as exc:
            last = exc
            try:
                have = os.path.getsize(partial)
            except OSError:
                have = 0
            continue
        except OSError as exc:
            _discard(partial)
            raise UpdateError("could not write the download: %s" % exc)

        if size and done != size:
            # Short, but the bytes are good as far as they go. Pick it up.
            last = NetworkError("download stopped early (%d of %d bytes)"
                                % (done, size))
            have = done
            continue

        os.replace(partial, destination)
        return done

    raise last or NetworkError("download did not finish")


def _discard(path):
    try:
        os.remove(path)
    except OSError:
        pass

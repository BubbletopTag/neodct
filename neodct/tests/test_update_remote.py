"""Finding an update over the network.

No test here touches the network. urlopen is replaced, because a test that
needs GitHub to be reachable is a test that fails on a train.
"""

import io
import json
import os

import pytest

from System.core.UpdateService import remote


@pytest.fixture(autouse=True)
def no_waiting(monkeypatch):
    """Backoff is real on the phone and pointless in a test suite."""
    monkeypatch.setattr(remote, "_sleep", lambda seconds: None)
from System.core.UpdateService import UpdateError


class _Response(io.BytesIO):
    """Minimal stand-in for what urlopen returns.

    `status` matters: download() only appends to a partial file when the
    server answers 206, because a 200 means it ignored the Range and is
    sending the whole package again."""

    status = 200

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def _entry(tag="0.3.7a", assets=(), body="notes here", prerelease=True):
    return {
        "tag_name": tag,
        "body": body,
        "prerelease": prerelease,
        "assets": [{"name": n, "size": s,
                    "browser_download_url": "https://example/%s" % n}
                   for n, s in assets],
    }


def _release(tag="0.3.7a", assets=(), body="notes here", prerelease=True):
    """One release, as GitHub's list endpoint returns it -- in a list.

    A list, not a bare object: latest() reads /releases, not
    /releases/latest. See the endpoint tests at the bottom of this file
    for why that distinction cost every release its updates."""
    return json.dumps([_entry(tag, assets, body, prerelease)]).encode()


def _release_list(*entries):
    """Several releases, newest-published first, as GitHub lists them."""
    return json.dumps(list(entries)).encode()


@pytest.fixture
def fake_open(monkeypatch):
    """Replace remote._open with something scriptable."""
    calls = []

    def install(payload):
        def _open(url, timeout=remote.CONNECT_TIMEOUT, headers=None):
            calls.append(url)
            if isinstance(payload, Exception):
                raise payload
            return _Response(payload)
        monkeypatch.setattr(remote, "_open", _open)
        return calls
    return install


# --- picking the right asset ---

def test_the_asset_name_carries_the_platform():
    """qemu and luckfox are different builds; one release holds both."""
    assert remote.asset_name("luckfox-armv7") == "UPDATE-luckfox-armv7.ndsw"
    assert remote.asset_name("qemu-aarch64") == "UPDATE-qemu-aarch64.ndsw"


def test_latest_returns_the_asset_for_this_platform(fake_open):
    fake_open(_release(assets=[("UPDATE-qemu-aarch64.ndsw", 11),
                               ("UPDATE-luckfox-armv7.ndsw", 22)]))

    found = remote.latest("luckfox-armv7")

    assert found["version"] == "0.3.7a"
    assert found["size"] == 22
    assert found["url"].endswith("UPDATE-luckfox-armv7.ndsw")


def test_a_release_without_this_platform_is_not_an_error_worth_crashing_on(fake_open):
    """Normal while a release is uploaded one asset at a time."""
    fake_open(_release(assets=[("UPDATE-qemu-aarch64.ndsw", 11)]))

    with pytest.raises(remote.NoRelease, match="luckfox"):
        remote.latest("luckfox-armv7")


def test_a_leading_v_on_the_tag_is_stripped(fake_open):
    """Old tags were v0.1.5a; os-release never carries the v."""
    fake_open(_release(tag="v0.3.7a", assets=[("UPDATE-x.ndsw", 1)]))

    assert remote.latest("x")["version"] == "0.3.7a"


def test_unreachable_github_is_a_network_error_not_a_traceback(fake_open):
    fake_open(remote.NetworkError("cannot reach GitHub: timed out"))

    with pytest.raises(remote.NetworkError):
        remote.latest("luckfox-armv7")


def test_garbage_instead_of_json_is_reported_as_such(fake_open):
    fake_open(b"<html>404 not found</html>")

    with pytest.raises(remote.NetworkError, match="not JSON"):
        remote.latest("luckfox-armv7")


# --- deciding whether to offer it ---

def test_a_newer_version_is_offered():
    assert remote.is_newer("0.3.7a", "0.3.6a")


def test_the_same_version_is_not_offered():
    assert not remote.is_newer("0.3.7a", "0.3.7a")


def test_an_older_version_is_not_offered():
    assert not remote.is_newer("0.3.5a", "0.3.6a")


def test_double_digit_versions_sort_by_number_not_by_string():
    """0.3.10a is newer than 0.3.9a. Compared as text it is not, and the
    phone would sit on 0.3.9a forever telling the owner it is up to date."""
    assert remote.is_newer("0.3.10a", "0.3.9a")
    assert not remote.is_newer("0.3.9a", "0.3.10a")


def test_anything_is_newer_than_nothing():
    """A phone with no recorded version should still be offered one."""
    assert remote.is_newer("0.3.7a", "")
    assert remote.is_newer("0.3.7a", None)


# --- downloading ---

def test_a_download_lands_at_its_final_name_only_when_complete(tmp_path, fake_open):
    fake_open(b"x" * 100)
    dest = tmp_path / "UPDATE.ndsw"

    written = remote.download("https://example/pkg", str(dest), size=100)

    assert written == 100
    assert dest.read_bytes() == b"x" * 100
    assert not (tmp_path / "UPDATE.ndsw.part").exists()


def test_a_truncated_download_never_looks_installable(tmp_path, fake_open):
    """A dropped carrier mid-download is the expected case on this phone.
    What must never happen is a short file sitting under the real name,
    where the update flow would pick it up and try to install it."""
    fake_open(b"x" * 40)
    dest = tmp_path / "UPDATE.ndsw"

    with pytest.raises(remote.NetworkError, match="stopped early"):
        remote.download("https://example/pkg", str(dest), size=100, attempts=1)

    assert not dest.exists()


def test_a_truncated_download_keeps_what_it_got(tmp_path, fake_open):
    """The bytes on the card are good as far as they go, and this phone
    fetches 53MB over a carrier. Deleting them meant every attempt began
    at zero, so a link that could not carry the whole package in one run
    could never carry it at all -- pressing the button again did nothing
    but waste the same bandwidth."""
    fake_open(b"x" * 40)
    dest = tmp_path / "UPDATE.ndsw"

    with pytest.raises(remote.NetworkError):
        remote.download("https://example/pkg", str(dest), size=100, attempts=1)

    partial = tmp_path / "UPDATE.ndsw.part"
    assert partial.exists()
    assert partial.stat().st_size == 40


def test_progress_is_reported_as_it_goes(tmp_path, fake_open):
    fake_open(b"y" * (remote.CHUNK * 2))
    seen = []
    dest = tmp_path / "UPDATE.ndsw"

    remote.download("https://example/pkg", str(dest),
                    size=remote.CHUNK * 2,
                    progress=lambda done, total: seen.append(done))

    assert seen and seen[-1] == remote.CHUNK * 2
    assert seen == sorted(seen)


def test_a_download_too_big_for_the_card_is_refused_before_it_starts(tmp_path, fake_open, monkeypatch):
    """Better to say so than to fill the card and fail at the last byte."""
    fake_open(b"x")
    monkeypatch.setattr(remote, "enough_space", lambda directory, size: False)

    with pytest.raises(UpdateError, match="not enough room"):
        remote.download("https://example/pkg",
                        str(tmp_path / "UPDATE.ndsw"), size=99999)


def test_the_repository_can_be_pointed_elsewhere(monkeypatch):
    """So a fork, or a laptop serving a build over the LAN, does not need
    a rebuilt image."""
    monkeypatch.setenv(remote.REPO_ENV, "someone/else")
    assert remote.repo() == "someone/else"


# --- listing every release, for the Downgrade tool ---

def _releases(entries):
    """entries: [(tag, [(asset_name, size)], prerelease)]"""
    return json.dumps([
        {"tag_name": tag, "body": "notes", "prerelease": pre,
         "assets": [{"name": n, "size": s,
                     "browser_download_url": "https://example/%s/%s" % (tag, n)}
                    for n, s in assets]}
        for tag, assets, pre in entries
    ]).encode()


def test_all_releases_returns_every_one_carrying_this_platform(fake_open):
    fake_open(_releases([
        ("0.3.7a", [("UPDATE-luckfox-armv7.ndsw", 3)], True),
        ("0.3.6a", [("UPDATE-luckfox-armv7.ndsw", 2)], True),
    ]))

    found = remote.all_releases("luckfox-armv7")

    assert [r["version"] for r in found] == ["0.3.7a", "0.3.6a"]


def test_releases_without_a_package_for_this_phone_are_left_out(fake_open):
    """An entry you cannot pick is worse than one that is not there."""
    fake_open(_releases([
        ("0.3.7a", [("UPDATE-luckfox-armv7.ndsw", 3)], True),
        ("0.3.6a", [("UPDATE-qemu-aarch64.ndsw", 2)], True),   # qemu only
    ]))

    found = remote.all_releases("luckfox-armv7")

    assert [r["version"] for r in found] == ["0.3.7a"]


def test_no_release_at_all_for_this_platform_is_reported(fake_open):
    fake_open(_releases([("0.3.7a", [("UPDATE-qemu-aarch64.ndsw", 1)], True)]))

    with pytest.raises(remote.NoRelease):
        remote.all_releases("luckfox-armv7")


def test_a_non_list_reply_is_a_network_error(fake_open):
    """The single-release endpoint returns an object; asking for the list
    and getting one back means something is wrong."""
    fake_open(b'{"tag_name": "0.3.7a"}')

    with pytest.raises(remote.NetworkError, match="list of releases"):
        remote.all_releases("luckfox-armv7")


# --- which endpoint the phone asks ---
#
# These exist because the old tests all passed while the feature was
# completely broken on every phone. latest() used GitHub's
# /releases/latest, which quietly ignores prereleases -- and every NeoDCT
# release is a prerelease, because the software is alpha. So the endpoint
# answered 404, the phone reported "no published release", and it did that
# for every release ever made. A fixture handing latest() a single release
# object could never notice: it never asked what URL was fetched.

def test_the_phone_does_not_ask_for_the_latest_release(fake_open):
    """/releases/latest ignores prereleases. Every release here is one, so
    that endpoint answers 404 and the phone concludes nothing exists."""
    calls = fake_open(_release(assets=[("UPDATE-x.ndsw", 1)]))

    remote.latest("x")

    assert calls, "latest() fetched nothing"
    for url in calls:
        assert not url.rstrip("/").endswith("/releases/latest"), url


def test_a_prerelease_is_still_an_update(fake_open):
    """The one that matters: alpha software ships as prereleases, and a
    phone that skips them never updates at all."""
    fake_open(_release(tag="0.3.9a", prerelease=True,
                       assets=[("UPDATE-luckfox-armv7.ndsw", 99)]))

    found = remote.latest("luckfox-armv7")

    assert found["version"] == "0.3.9a"


def test_the_newest_version_wins_not_the_newest_publication(fake_open):
    """GitHub lists by publication date. Re-publishing an old tag would
    put it first, and the phone would offer a downgrade as an update."""
    fake_open(_release_list(
        _entry(tag="0.3.2a", assets=[("UPDATE-x.ndsw", 1)]),
        _entry(tag="0.3.10a", assets=[("UPDATE-x.ndsw", 2)]),
        _entry(tag="0.3.9a", assets=[("UPDATE-x.ndsw", 3)]),
    ))

    assert remote.latest("x")["version"] == "0.3.10a"


def test_a_release_carrying_another_platform_is_skipped_not_chosen(fake_open):
    """The newest release may not have this phone's package yet -- an
    upload in progress. Offer the newest one that does."""
    fake_open(_release_list(
        _entry(tag="0.3.9a", assets=[("UPDATE-qemu-aarch64.ndsw", 1)]),
        _entry(tag="0.3.8a", assets=[("UPDATE-luckfox-armv7.ndsw", 2)]),
    ))

    assert remote.latest("luckfox-armv7")["version"] == "0.3.8a"


def test_version_key_orders_the_way_the_phone_needs():
    assert remote.version_key("0.3.10a") > remote.version_key("0.3.9a")
    assert remote.version_key("0.4.0a") > remote.version_key("0.3.99a")


# --- resuming a download over a carrier that keeps dropping -----------------
#
# The phone reported "download timed out" over and over on a 53MB package,
# with an antenna glued inside a plastic back cover. The link was not the
# whole problem: every failure deleted the partial file, so no amount of
# retrying could ever finish. These pin the resume that fixes it.

class _RangeServer:
    """A server that honours Range, and can be told to drop the connection
    after so many bytes -- which is what a carrier does."""

    def __init__(self, body, cut=None):
        self.body = body
        self.cut = cut
        self.ranges = []

    def open(self, url, timeout=None, headers=None):
        start = 0
        headers = headers or {}
        if "Range" in headers:
            start = int(headers["Range"].split("=")[1].split("-")[0])
            self.ranges.append(start)
        piece = self.body[start:]
        if self.cut is not None:
            piece = piece[:self.cut]
        response = _Response(piece)
        response.status = 206 if start else 200
        return response


def test_a_download_picks_up_where_it_stopped(tmp_path, monkeypatch):
    body = b"".join(bytes([i % 251]) for i in range(1000))
    server = _RangeServer(body, cut=300)          # drops after 300 bytes
    monkeypatch.setattr(remote, "_open", server.open)
    dest = tmp_path / "UPDATE.ndsw"

    written = remote.download("https://example/pkg", str(dest),
                              size=len(body), attempts=10)

    assert written == len(body)
    assert dest.read_bytes() == body, "the reassembled package is wrong"
    assert server.ranges == [300, 600, 900], server.ranges


def test_resuming_asks_only_for_the_rest(tmp_path, monkeypatch):
    """Not the whole file again. The point is to stop re-sending bytes the
    phone already paid for."""
    body = b"y" * 500
    server = _RangeServer(body)
    monkeypatch.setattr(remote, "_open", server.open)
    dest = tmp_path / "UPDATE.ndsw"
    (tmp_path / "UPDATE.ndsw.part").write_bytes(body[:200])

    remote.download("https://example/pkg", str(dest), size=len(body))

    assert server.ranges == [200]
    assert dest.read_bytes() == body


def test_a_server_that_ignores_range_is_started_over(tmp_path, monkeypatch):
    """Answering 200 to a Range request means the whole file is coming.
    Appending it would write the first bytes twice and produce a package
    that fails its hash with nothing to explain why."""
    body = b"z" * 400

    def ignores_range(url, timeout=None, headers=None):
        response = _Response(body)
        response.status = 200            # "I am sending you everything"
        return response

    monkeypatch.setattr(remote, "_open", ignores_range)
    dest = tmp_path / "UPDATE.ndsw"
    (tmp_path / "UPDATE.ndsw.part").write_bytes(b"z" * 150)

    remote.download("https://example/pkg", str(dest), size=len(body))

    assert dest.read_bytes() == body
    assert len(dest.read_bytes()) == 400, "the partial was appended to"


def test_a_partial_longer_than_the_package_is_thrown_away(tmp_path, monkeypatch):
    """Left over from a different, larger package. Resuming from past the
    end would ask for a range the server cannot serve."""
    body = b"w" * 300
    server = _RangeServer(body)
    monkeypatch.setattr(remote, "_open", server.open)
    dest = tmp_path / "UPDATE.ndsw"
    (tmp_path / "UPDATE.ndsw.part").write_bytes(b"old" * 500)

    remote.download("https://example/pkg", str(dest), size=len(body))

    assert dest.read_bytes() == body
    assert server.ranges == []           # started clean, no Range asked


def test_giving_up_still_leaves_the_progress(tmp_path, monkeypatch):
    """Out of attempts is not the same as out of luck: the next time the
    owner presses the button it carries on from here."""
    body = b"q" * 1000
    server = _RangeServer(body, cut=100)
    monkeypatch.setattr(remote, "_open", server.open)
    dest = tmp_path / "UPDATE.ndsw"

    with pytest.raises(remote.NetworkError):
        remote.download("https://example/pkg", str(dest),
                        size=len(body), attempts=3)

    assert not dest.exists()
    assert (tmp_path / "UPDATE.ndsw.part").stat().st_size == 300


def test_the_read_timeout_is_not_the_connect_timeout(tmp_path):
    """Reaching GitHub and hauling 53MB off it are different jobs. Twenty
    seconds is right for the first and absurd for the second on a phone
    behind an antenna glued inside a plastic cover."""
    assert remote.DOWNLOAD_TIMEOUT > remote.CONNECT_TIMEOUT


# --- a bearer that drops entirely -------------------------------------------
#
# The owner's report: "It'll say it timed out then lose internet completely.
# Temporary failure in name resolution. But it's still able to make calls."
# The data session goes while voice stays up, and DNS goes with it. Retrying
# instantly through that is five failures in under a second.

def test_attempts_wait_longer_each_time(tmp_path, monkeypatch):
    waits = []
    monkeypatch.setattr(remote, "_sleep", lambda s: waits.append(s))
    monkeypatch.setattr(remote, "_open", lambda *a, **k: (_ for _ in ()).throw(
        remote.NetworkError("cannot reach GitHub: [Errno -3] Temporary "
                            "failure in name resolution")))

    with pytest.raises(remote.NetworkError, match="name resolution"):
        remote.download("https://example/pkg", str(tmp_path / "U.ndsw"),
                        size=100, attempts=4)

    assert waits == [5, 10, 20], waits


def test_the_wait_is_capped(tmp_path, monkeypatch):
    """Doubling forever means a phone that looks hung rather than patient."""
    waits = []
    monkeypatch.setattr(remote, "_sleep", lambda s: waits.append(s))
    monkeypatch.setattr(remote, "_open", lambda *a, **k: (_ for _ in ()).throw(
        remote.NetworkError("down")))

    with pytest.raises(remote.NetworkError):
        remote.download("https://example/pkg", str(tmp_path / "U.ndsw"),
                        size=100, attempts=8)

    assert max(waits) <= remote.RETRY_BACKOFF_MAX
    assert waits[-1] == remote.RETRY_BACKOFF_MAX


def test_the_first_attempt_does_not_wait(tmp_path, fake_open, monkeypatch):
    """Nothing has failed yet. Making the owner wait five seconds before
    the first byte would be punishing them for the carrier."""
    waits = []
    monkeypatch.setattr(remote, "_sleep", lambda s: waits.append(s))
    fake_open(b"k" * 50)

    remote.download("https://example/pkg", str(tmp_path / "U.ndsw"), size=50)

    assert waits == []


def test_a_dns_failure_is_retried_not_fatal(tmp_path, monkeypatch):
    """The bearer comes back. The phone should still be trying when it
    does, rather than having given up in the first second."""
    body = b"m" * 200
    attempts = {"n": 0}

    def flaky(url, timeout=None, headers=None):
        attempts["n"] += 1
        if attempts["n"] < 3:
            raise remote.NetworkError(
                "cannot reach GitHub: Temporary failure in name resolution")
        return _Response(body)

    monkeypatch.setattr(remote, "_open", flaky)
    dest = tmp_path / "U.ndsw"

    written = remote.download("https://example/pkg", str(dest), size=len(body))

    assert written == len(body)
    assert dest.read_bytes() == body

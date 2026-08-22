"""Keeping the phone's clock honest.

A phone with no battery-backed RTC boots at the Unix epoch, and every TLS
certificate on the internet has a "not valid before" date -- so a clock
reading 1970 fails validation on every HTTPS site at once. These tests pin
the two behaviours that stop that: the offline floor, and a sync that
refuses an implausible answer.

Nothing here opens a socket. set_clock is patched everywhere, because a
test that sets the machine's clock is a test that ruins someone's day.
"""

import socket
import struct
import time

import pytest

from System.core import ClockService as clock


@pytest.fixture
def phone(tmp_path, monkeypatch):
    """A phone whose clock and state file we can inspect."""
    state = tmp_path / ".clock"
    version = tmp_path / "version.prop"
    version.write_text("system.os.buildepoch=1787359945\n")
    monkeypatch.setattr(clock, "STATE_FILE", str(state))
    monkeypatch.setattr(clock, "VERSION_PROP", str(version))
    applied = []
    monkeypatch.setattr(clock, "set_clock", lambda when: applied.append(int(when)) or True)
    return type("Phone", (), {"applied": applied, "state": state,
                              "build": 1787359945})()


# --- the offline floor ---

def test_a_clock_at_the_epoch_is_pushed_up_to_the_build_date(phone, monkeypatch):
    """The date will be wrong, but wrong in the direction that keeps
    certificates valid, which is the whole point."""
    monkeypatch.setattr(time, "time", lambda: 300.0)      # 1970

    settled = clock.apply_floor()

    assert settled == phone.build
    assert phone.applied == [phone.build]


def test_a_clock_already_ahead_is_left_alone(phone, monkeypatch):
    """Never move a good clock backwards to the build date."""
    monkeypatch.setattr(time, "time", lambda: phone.build + 86400)

    assert clock.apply_floor() is None
    assert phone.applied == []


def test_the_last_synced_time_beats_the_build_date(phone, monkeypatch):
    """After one good sync, later boots start from that rather than from
    whenever the image happened to be built."""
    later = phone.build + 999999
    clock.remember(later)
    monkeypatch.setattr(time, "time", lambda: 300.0)

    assert clock.apply_floor() == later


def test_a_missing_or_corrupt_state_file_is_not_fatal(phone, monkeypatch):
    phone.state.write_text("not a number")
    monkeypatch.setattr(time, "time", lambda: 300.0)

    assert clock.apply_floor() == phone.build     # falls back to build date


# --- the SNTP reply ---

def _reply(epoch):
    """48 bytes with the transmit timestamp at offset 40."""
    seconds = int(epoch) + clock.NTP_EPOCH_OFFSET
    return b"\x1b" + 39 * b"\0" + struct.pack("!I", seconds) + 4 * b"\0"


@pytest.fixture
def fake_udp(monkeypatch):
    def install(payload):
        class _Sock:
            def settimeout(self, _): pass
            def sendto(self, *a): pass
            def recvfrom(self, n):
                if isinstance(payload, Exception):
                    raise payload
                return payload, ("1.2.3.4", 123)
            def close(self): pass
        monkeypatch.setattr(socket, "socket", lambda *a, **k: _Sock())
    return install


def test_a_good_reply_is_read_as_the_time(fake_udp):
    fake_udp(_reply(1800000000))

    assert clock.query("example") == 1800000000


def test_a_reply_from_before_2020_is_refused(fake_udp):
    """An unset server answers with something near its own epoch. Trusting
    it would push the phone back to exactly the broken state we are fixing."""
    fake_udp(_reply(946684800))            # 2000-01-01

    with pytest.raises(OSError, match="implausible"):
        clock.query("example")


def test_a_zero_timestamp_is_refused(fake_udp):
    fake_udp(b"\x1b" + 47 * b"\0")

    with pytest.raises(OSError, match="zero timestamp"):
        clock.query("example")


def test_a_truncated_reply_is_refused(fake_udp):
    fake_udp(b"\x1b" + 10 * b"\0")

    with pytest.raises(OSError, match="short"):
        clock.query("example")


def test_sync_moves_to_the_next_server_when_one_is_silent(phone, monkeypatch):
    """Some carriers hijack or drop the NTP pool, so one dead server must
    not be the end of it."""
    tried = []

    def flaky(server, timeout=None):
        tried.append(server)
        if len(tried) < 3:
            raise OSError("timed out")
        return 1800000000

    monkeypatch.setattr(clock, "query", flaky)

    assert clock.sync(("a", "b", "c")) == 1800000000
    assert tried == ["a", "b", "c"]


def test_sync_returns_nothing_when_every_server_is_silent(phone, monkeypatch):
    monkeypatch.setattr(clock, "query",
                        lambda s, timeout=None: (_ for _ in ()).throw(OSError("no")))

    assert clock.sync(("a", "b")) is None
    assert phone.applied == []


def test_a_successful_sync_is_remembered_for_next_boot(phone, monkeypatch):
    monkeypatch.setattr(clock, "query", lambda s, timeout=None: 1800000000)

    clock.sync(("a",))

    assert clock.last_known() == 1800000000

"""System/core/MediaWidget.play(): mpv, the keypad bridge and the app.

These run a stand-in for mpv that speaks the real IPC protocol over a real
unix socket, so the bridge is exercised end to end. Nothing about the
bridge is mocked -- the part most likely to be wrong is the handshake with
a process that may not have created its socket yet, and a mock would prove
nothing about it.
"""

import os
import signal
import subprocess
import sys
import textwrap
import time

import pytest

from System.core import MediaWidget

# A fake mpv: creates the IPC socket, records every command it is sent, and
# exits when told to quit or when its own timeout expires.
FAKE_MPV = textwrap.dedent('''
    import json, os, socket, sys, time
    argv = sys.argv[1:]
    sock_path = None
    for arg in argv:
        if arg.startswith("--input-ipc-server="):
            sock_path = arg.split("=", 1)[1]
    log = open(os.environ["FAKE_MPV_LOG"], "a", buffering=1)
    log.write("ARGV " + json.dumps(argv) + "\\n")
    delay = float(os.environ.get("FAKE_MPV_SOCKET_DELAY", "0"))
    time.sleep(delay)
    if sock_path is None:
        time.sleep(float(os.environ.get("FAKE_MPV_LIFETIME", "0.2")))
        sys.exit(int(os.environ.get("FAKE_MPV_RC", "0")))
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(sock_path)
    server.listen(1)
    server.settimeout(float(os.environ.get("FAKE_MPV_LIFETIME", "5")))
    try:
        conn, _ = server.accept()
    except socket.timeout:
        sys.exit(int(os.environ.get("FAKE_MPV_RC", "0")))
    conn.settimeout(float(os.environ.get("FAKE_MPV_LIFETIME", "5")))
    buf = b""
    while True:
        try:
            data = conn.recv(4096)
        except socket.timeout:
            break
        if not data:
            break
        buf += data
        while b"\\n" in buf:
            line, buf = buf.split(b"\\n", 1)
            if not line.strip():
                continue
            command = json.loads(line)["command"]
            log.write("CMD " + json.dumps(command) + "\\n")
            if command and command[0] == "quit":
                sys.exit(int(os.environ.get("FAKE_MPV_RC", "0")))
    sys.exit(int(os.environ.get("FAKE_MPV_RC", "0")))
''')


@pytest.fixture
def fake_mpv(tmp_path, monkeypatch):
    """(path-to-fake-mpv, read-the-log) with the log wired up via the env."""
    script = tmp_path / "fake-mpv"
    script.write_text("#!" + sys.executable + "\n" + FAKE_MPV)
    script.chmod(0o755)
    log = tmp_path / "mpv.log"
    log.write_text("")
    monkeypatch.setenv("FAKE_MPV_LOG", str(log))

    def read():
        return [line for line in log.read_text().splitlines() if line]

    return str(script), read


class Keys:
    """A keysource with the same shape as ui.read_keypress(timeout)."""

    def __init__(self, codes):
        self.codes = list(codes)
        self.calls = 0

    def __call__(self, timeout=0.1):
        self.calls += 1
        if self.codes:
            return self.codes.pop(0)
        time.sleep(min(timeout, 0.01))
        return None


def _proc_state(pid):
    with open("/proc/%d/stat" % pid, "rb") as handle:
        return handle.read().rsplit(b")", 1)[1].split()[0].decode()


# --- running mpv at all ---------------------------------------------------

def test_play_runs_mpv_with_the_url(fake_mpv, tmp_path):
    mpv, read = fake_mpv
    MediaWidget.play("http://host/watch.avi", mpv=mpv,
                     ipc_socket=str(tmp_path / "s.sock"))
    argv_line = next(line for line in read() if line.startswith("ARGV "))
    assert "http://host/watch.avi" in argv_line
    assert "--vo=fbdev" in argv_line


def test_play_returns_mpvs_exit_status(fake_mpv, tmp_path, monkeypatch):
    mpv, _ = fake_mpv
    monkeypatch.setenv("FAKE_MPV_RC", "3")
    monkeypatch.setenv("FAKE_MPV_LIFETIME", "0.2")
    rc = MediaWidget.play("x.avi", mpv=mpv, ipc_socket=None)
    assert rc == 3


def test_a_missing_mpv_is_reported_not_raised(tmp_path):
    rc = MediaWidget.play("x.avi", mpv=str(tmp_path / "nope"),
                          ipc_socket=None)
    assert rc != 0


# --- the key bridge -------------------------------------------------------

def test_keypresses_reach_mpv_as_ipc_commands(fake_mpv, tmp_path):
    mpv, read = fake_mpv
    keys = Keys([106, 103, 14])          # right, up, C
    MediaWidget.play("x.avi", mpv=mpv, keysource=keys,
                     ipc_socket=str(tmp_path / "s.sock"))
    sent = [line[4:] for line in read() if line.startswith("CMD ")]
    assert '["seek", "10", "relative"]' in sent
    assert '["add", "volume", "5"]' in sent
    assert '["quit"]' in sent


def test_unbound_keys_are_not_forwarded(fake_mpv, tmp_path):
    mpv, read = fake_mpv
    keys = Keys([50, 50, 14])            # menu twice, then C
    MediaWidget.play("x.avi", mpv=mpv, keysource=keys,
                     ipc_socket=str(tmp_path / "s.sock"))
    sent = [line[4:] for line in read() if line.startswith("CMD ")]
    assert sent == ['["quit"]']


def test_the_bridge_waits_for_a_socket_that_appears_late(fake_mpv, tmp_path,
                                                         monkeypatch):
    # mpv binds its IPC socket some way into startup; connecting too early
    # would silently drop every key the user pressed first.
    mpv, read = fake_mpv
    monkeypatch.setenv("FAKE_MPV_SOCKET_DELAY", "0.6")
    keys = Keys([14])
    MediaWidget.play("x.avi", mpv=mpv, keysource=keys,
                     ipc_socket=str(tmp_path / "s.sock"))
    sent = [line[4:] for line in read() if line.startswith("CMD ")]
    assert sent == ['["quit"]']


def test_play_returns_when_mpv_exits_on_its_own(fake_mpv, tmp_path,
                                                monkeypatch):
    # End of file: nobody pressed anything, mpv leaves, so must we.
    mpv, _ = fake_mpv
    monkeypatch.setenv("FAKE_MPV_LIFETIME", "0.4")
    keys = Keys([])
    started = time.monotonic()
    MediaWidget.play("x.avi", mpv=mpv, keysource=keys,
                     ipc_socket=str(tmp_path / "s.sock"))
    assert time.monotonic() - started < 5


def test_the_socket_is_cleaned_up_afterwards(fake_mpv, tmp_path):
    mpv, _ = fake_mpv
    sock = tmp_path / "s.sock"
    MediaWidget.play("x.avi", mpv=mpv, keysource=Keys([14]),
                     ipc_socket=str(sock))
    assert not sock.exists()


def test_a_stale_socket_from_a_crash_does_not_block_the_next_play(fake_mpv,
                                                                  tmp_path):
    mpv, read = fake_mpv
    sock = tmp_path / "s.sock"
    sock.write_text("")                  # left behind by a killed mpv
    MediaWidget.play("x.avi", mpv=mpv, keysource=Keys([14]),
                     ipc_socket=str(sock))
    assert any(line.startswith("CMD ") for line in read())


# --- the application underneath -------------------------------------------

@pytest.fixture
def sleeper():
    proc = subprocess.Popen(["sleep", "30"])
    try:
        yield proc
    finally:
        proc.send_signal(signal.SIGCONT)
        proc.kill()
        proc.wait()


def test_the_application_is_stopped_while_mpv_plays(fake_mpv, tmp_path,
                                                    sleeper, monkeypatch):
    mpv, _ = fake_mpv
    monkeypatch.setenv("FAKE_MPV_LIFETIME", "0.5")
    states = []

    class Watcher(Keys):
        def __call__(self, timeout=0.1):
            states.append(_proc_state(sleeper.pid))
            return super().__call__(timeout)

    MediaWidget.play("x.avi", mpv=mpv, keysource=Watcher([]),
                     suspend_pid=sleeper.pid,
                     ipc_socket=str(tmp_path / "s.sock"))
    assert "T" in states
    assert _proc_state(sleeper.pid) != "T"


def test_the_application_is_resumed_when_mpv_is_missing(tmp_path, sleeper):
    MediaWidget.play("x.avi", mpv=str(tmp_path / "nope"),
                     suspend_pid=sleeper.pid, ipc_socket=None)
    assert _proc_state(sleeper.pid) != "T"

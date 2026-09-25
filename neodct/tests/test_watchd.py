"""nd-watchd and its tap, end to end, against the host build.

The daemon is run for real on loopback with a plain file standing in for fb0
(its --fb accepts one exactly for this), a unix socket standing in for the
devkey channel, and an abstract audio name of its own so a real nd-watchd on
the machine is never touched. The viewer half is exercised through the same
wire: what ndlink-watch reads is what these read.

The tap tests matter most. The tap sits in the write path of every sound the
phone makes, so the property held here is the one in nd-watchd.c's header: it
consumes every byte it is given and exits 0, whatever it is given and whether
or not anyone is listening. A tap that exits early is a ringtone that fails.
"""

import importlib.machinery
import importlib.util
import os
import socket
import struct
import subprocess
import time

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WATCHD = os.path.join(REPO, "src", "build", "default", "bin", "nd-watchd")
VIEWER = os.path.join(REPO, "tools", "ndlink-watch")

pytestmark = pytest.mark.skipif(
    not os.access(WATCHD, os.X_OK),
    reason="nd-watchd not built (cd neodct/src && make)")

W, H = 240, 175
HDR = struct.Struct("<B3xI")
ND_KEY_UP = 103


def wav_header(rate=48000, channels=2, bits=16):
    """The 44 bytes alsa-lib's pcm_file writes before the data."""
    frame = channels * bits // 8
    return (b"RIFF" + struct.pack("<I", 0x24) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * frame, frame, bits) +
            b"data" + struct.pack("<I", 0))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def recv_exact(s, n):
    out = b""
    while len(out) < n:
        chunk = s.recv(n - len(out))
        assert chunk, "nd-watchd closed the connection"
        out += chunk
    return out


def recv_msg(s):
    t, n = HDR.unpack(recv_exact(s, HDR.size))
    return chr(t), recv_exact(s, n)


def recv_until(s, kind, timeout=3.0):
    s.settimeout(timeout)
    while True:
        t, body = recv_msg(s)
        if t == kind:
            return body


@pytest.fixture
def watchd(tmp_path):
    fb = tmp_path / "fb"
    fb.write_bytes(bytes(W * H * 4))
    (tmp_path / "input").mkdir(mode=0o700)
    devkey_path = str(tmp_path / "devkey")
    devkey = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    devkey.bind(devkey_path)
    devkey.settimeout(2)
    audio = "nd-watchd-test-%d" % os.getpid()
    port = free_port()
    proc = subprocess.Popen(
        [WATCHD, "--bind", "127.0.0.1", "--port", str(port), "--fps", "30",
         "--fb", str(fb), "--devkey", devkey_path, "--audio", audio,
         # never this machine's own sound card: a test that muted the
         # developer's speakers would be found the hard way
         "--mute-card", "none", "--mute-state", str(tmp_path / "muted"),
         "--keyecho", str(tmp_path / "input" / "keyecho")],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    deadline = time.monotonic() + 5
    while True:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            assert proc.poll() is None, proc.stderr.read().decode()
            assert time.monotonic() < deadline, "nd-watchd never listened"
            time.sleep(0.05)
    env = {"fb": fb, "devkey": devkey, "audio": audio, "port": port, "proc": proc,
           "keyecho": str(tmp_path / "input" / "keyecho")}
    yield env
    proc.terminate()
    proc.wait(timeout=5)
    devkey.close()


def connect(env, identify=True):
    """Connect as ndlink-watch does: read the hello, then send options. Until
    that options message a connection is only pending, not the viewer."""
    s = socket.create_connection(("127.0.0.1", env["port"]), timeout=3)
    t, body = recv_msg(s)
    assert t == "H"
    if identify:
        s.sendall(HDR.pack(ord("O"), 4) + struct.pack("<B3x", 0))
    return s, body


def run_tap(env_or_name, data):
    name = env_or_name if isinstance(env_or_name, str) else env_or_name["audio"]
    return subprocess.run([WATCHD, "--tap", "--audio", name], input=data,
                          capture_output=True, timeout=10)


def test_hello_then_a_whole_first_frame(watchd):
    s, hello = connect(watchd)
    assert hello[:4] == b"NDW1"
    w, h, pixfmt, keys, fps = struct.unpack_from("<HHBBH", hello, 4)
    assert (w, h, pixfmt, keys, fps) == (W, H, 1, 1, 30)

    body = recv_until(s, "V")
    assert struct.unpack_from("<HHHH", body) == (0, 0, W, H)
    assert body[8:] == bytes(W * H * 2)


def test_a_change_sends_only_its_rectangle_in_rgb565(watchd):
    s, _ = connect(watchd)
    recv_until(s, "V")

    # vfb is red first: R G B X. Red, green, blue from (10, 20).
    with open(watchd["fb"], "r+b") as f:
        f.seek((20 * W + 10) * 4)
        f.write(bytes([255, 0, 0, 0, 0, 255, 0, 0, 0, 0, 255, 0]))

    body = recv_until(s, "V")
    assert struct.unpack_from("<HHHH", body) == (10, 20, 3, 1)
    assert struct.unpack_from("<3H", body, 8) == (0xF800, 0x07E0, 0x001F)


def test_a_reconnecting_viewer_gets_a_whole_frame_again(watchd):
    s, _ = connect(watchd)
    recv_until(s, "V")
    s.close()
    s2, _ = connect(watchd)
    body = recv_until(s2, "V")
    assert struct.unpack_from("<HHHH", body) == (0, 0, W, H)


def test_keys_reach_the_devkey_channel(watchd):
    s, _ = connect(watchd)
    s.sendall(HDR.pack(ord("K"), 8) + struct.pack("<iB3x", ND_KEY_UP, 1))
    assert watchd["devkey"].recv(64) == b"103 1"
    s.sendall(HDR.pack(ord("K"), 8) + struct.pack("<iB3x", ND_KEY_UP, 0))
    assert watchd["devkey"].recv(64) == b"103 0"


def test_a_key_the_phone_does_not_have_is_dropped(watchd):
    s, _ = connect(watchd)
    s.sendall(HDR.pack(ord("K"), 8) + struct.pack("<iB3x", 30, 1))    # KEY_A
    s.sendall(HDR.pack(ord("K"), 8) + struct.pack("<iB3x", ND_KEY_UP, 1))
    assert watchd["devkey"].recv(64) == b"103 1"


def test_a_viewer_that_vanishes_mid_press_releases_the_key(watchd):
    """Otherwise the phone sees UP held forever, and widgets read held state."""
    s, _ = connect(watchd)
    s.sendall(HDR.pack(ord("K"), 8) + struct.pack("<iB3x", ND_KEY_UP, 1))
    assert watchd["devkey"].recv(64) == b"103 1"
    s.close()
    assert watchd["devkey"].recv(64) == b"103 0"


def test_played_audio_reaches_the_viewer(watchd):
    s, _ = connect(watchd)
    recv_until(s, "V")
    pcm = bytes(range(256)) * 30            # 7680 bytes = 1920 frames
    r = run_tap(watchd, wav_header() + pcm)
    assert r.returncode == 0

    got = b""
    while len(got) < len(pcm):
        body = recv_until(s, "A")
        assert struct.unpack_from("<IHH", body) == (48000, 2, 16)
        got += body[8:]
    assert got == pcm


def test_the_tap_splits_only_on_whole_frames(watchd):
    """A sample split across two datagrams would be noise at the viewer."""
    s, _ = connect(watchd)
    recv_until(s, "V")
    pcm = bytes(6 * 5000)                   # 24-bit stereo: 6-byte frames
    run_tap(watchd, wav_header(bits=24) + pcm)
    got = 0
    while got < len(pcm):
        body = recv_until(s, "A")
        assert (len(body) - 8) % 6 == 0
        got += len(body) - 8


def test_the_tap_with_no_daemon_still_drinks_everything():
    r = run_tap("nd-watchd-nobody-%d" % os.getpid(), wav_header() + bytes(1 << 20))
    assert r.returncode == 0


@pytest.mark.parametrize("junk", [
    b"",                                    # opened and closed at once
    b"RIFF",                                # short header
    b"not a wav file at all" * 1000,        # not a header
    wav_header(channels=0) + bytes(4096),   # a header that makes no sense
])
def test_the_tap_never_fails_its_pipe(junk):
    r = run_tap("nd-watchd-nobody-%d" % os.getpid(), junk)
    assert r.returncode == 0


def test_the_tap_ignores_a_bad_argument_rather_than_failing():
    r = subprocess.run([WATCHD, "--tap", "--nonsense"], input=wav_header() + bytes(4096),
                       capture_output=True, timeout=10)
    assert r.returncode == 0


# ------------------------------------------------------------------ #
# the viewer's half of the same wire
# ------------------------------------------------------------------ #

def load_viewer():
    pytest.importorskip("pygame")
    loader = importlib.machinery.SourceFileLoader("ndlink_watch", VIEWER)
    spec = importlib.util.spec_from_loader("ndlink_watch", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def test_the_viewer_reads_keycodes_from_the_c_header():
    v = load_viewer()
    nd = v.load_keycodes()
    assert nd["UP"] == ND_KEY_UP and nd["STAR"] == 42 and nd["0"] == 11
    by_char, by_key = v.build_keymap(nd)
    assert by_char["#"] == 43 and by_char["1"] == 2


def test_the_viewer_decodes_rgb565():
    v = load_viewer()
    import pygame
    canvas = pygame.Surface((W, H))
    # an odd width, so the 16-bit surface's pitch is padded past w*2
    data = struct.pack("<3H", 0xF800, 0x07E0, 0x001F)
    v.blit_565(canvas, 5, 6, 3, 1, data)
    r, g, b = canvas.get_at((5, 6))[:3], canvas.get_at((6, 6))[:3], canvas.get_at((7, 6))[:3]
    assert r[0] > 240 and r[1] == 0 and r[2] == 0
    assert g[1] > 240 and g[0] == 0 and g[2] == 0
    assert b[2] > 240 and b[0] == 0 and b[1] == 0


def feed(a, chunk_ms, total_ms, rate=48000, t0=100.0, bursts=1):
    """Push audio as the tap delivers it: `bursts` datagrams at once, every
    chunk_ms. Returns the bytes pushed."""
    bpms = rate * 4 // 1000
    out = b""
    t = t0
    k = 0
    while k * chunk_ms < total_ms:
        for _ in range(bursts):
            pcm = bytes([(k * 7 + i) % 251 for i in range(chunk_ms * bpms // bursts)])
            a.push(rate, 2, 16, pcm, now=t)
            out += pcm
        t += chunk_ms / 1000.0
        k += 1
    return out


def test_the_viewer_pipes_audio_to_a_player_in_order(tmp_path):
    v = load_viewer()
    out = tmp_path / "played"
    a = v.AudioOut(60, argv_for=lambda *fmt: ["sh", "-c", "cat > '%s'" % out])
    chunk = 20 * 192                    # 20 ms at 48 kHz stereo S16
    sent = feed(a, 20, 400)
    a.proc.stdin.close()
    a.proc.wait(timeout=5)
    played = out.read_bytes()
    # Pushed faster than real time, so the latency cap may drop a chunk --
    # which is its job. What must hold: what plays is whole chunks of what
    # was sent, in the order sent, and nothing else.
    chunks = [sent[i:i + chunk] for i in range(0, len(sent), chunk)]
    assert played and len(played) % chunk == 0
    k = 0
    for i in range(0, len(played), chunk):
        while chunks[k] != played[i:i + chunk]:
            k += 1                      # IndexError here = out of order
        k += 1
    assert a.error is None


def test_steady_audio_keeps_the_buffer_small():
    """mpv hands over 20 ms at a time: nothing to absorb, so no delay added."""
    v = load_viewer()
    a = v.AudioOut(60, argv_for=lambda *fmt: ["sleep", "30"])
    feed(a, 20, 1000)
    assert a.target_ms() == 60
    a.close()


def test_bursty_audio_gets_a_buffer_that_covers_the_burst():
    """aplay's default is 125 ms periods, and the tap can only pass on what
    the app has written. A buffer smaller than the burst ran dry between
    every burst -- the choppy ringtones."""
    v = load_viewer()
    a = v.AudioOut(60, argv_for=lambda *fmt: ["sleep", "30"])
    feed(a, 125, 1000, bursts=3)
    assert 125 < a.target_ms() <= 250
    a.close()


def test_a_player_that_stops_reading_never_blocks_the_viewer():
    """The network thread writes into the player. If that could block, one
    stalled sound server would freeze the picture too."""
    v = load_viewer()
    a = v.AudioOut(50, argv_for=lambda *fmt: ["sleep", "30"])
    t = time.monotonic()
    feed(a, 20, 5000)
    assert time.monotonic() - t < 3
    assert a.dropped >= 1
    assert len(a.pending) < 4          # never more than the rest of one frame
    a.close()


def test_the_phone_mute_request_is_harmless_without_a_card(watchd):
    """--mute-card none: the request is answered with a log line, not a
    dropped connection."""
    s, _ = connect(watchd)
    s.sendall(HDR.pack(ord("O"), 4) + struct.pack("<B3x", 1))
    s.sendall(HDR.pack(ord("K"), 8) + struct.pack("<iB3x", ND_KEY_UP, 1))
    assert watchd["devkey"].recv(64) == b"103 1"


def test_no_player_is_said_rather_than_silent():
    v = load_viewer()
    a = v.AudioOut(80, argv_for=lambda *fmt: None)
    a.push(48000, 2, 16, bytes(64))
    assert a.error and "no audio" in a.error


def test_the_player_follows_the_desktop_default_output():
    """No device name: pw-play/paplay/aplay pick the default like any app."""
    v = load_viewer()
    argv = v.player_argv(44100, 2, 16, 80)
    if argv is None:
        pytest.skip("no pw-play, paplay or aplay here")
    assert "--target" not in " ".join(argv) and "-D" not in argv
    assert "44100" in " ".join(argv)


# ------------------------------------------------------------------ #
# --verbose: the key echo and the stats
# ------------------------------------------------------------------ #

def test_the_phones_own_keys_reach_the_viewer(watchd):
    """nd-core echoes every key it queues; nd-watchd passes each on as 'k'."""
    s, _ = connect(watchd)
    recv_until(s, "V")
    assert os.stat(watchd["keyecho"]).st_mode & 0o777 == 0o600
    tx = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    tx.sendto(b"6 1", watchd["keyecho"])           # the keypad's 5, pressed
    tx.sendto(b"6 0", watchd["keyecho"])
    got = [struct.unpack_from("<iB", recv_until(s, "k")) for _ in range(2)]
    assert got == [(6, 1), (6, 0)]


def test_a_symlink_in_the_echo_path_is_replaced_not_followed(tmp_path, watchd):
    """nd-watchd binds as root in a directory ndusr owns. Whatever is already
    at the name must be removed, never written through."""
    target = tmp_path / "precious"
    target.write_text("keep")
    os.unlink(watchd["keyecho"]) if os.path.lexists(watchd["keyecho"]) else None
    os.symlink(target, watchd["keyecho"])
    s, _ = connect(watchd)                      # a connect re-binds
    recv_until(s, "V")
    assert not os.path.islink(watchd["keyecho"])
    assert target.read_text() == "keep"


def test_cpu_and_ram_arrive(watchd):
    s, _ = connect(watchd)
    first = recv_until(s, "S")
    cpu, _, total, avail, _, _ = struct.unpack_from("<HHIIII", first)
    assert cpu == 0xFFFF                        # no interval to measure yet
    assert total > 0 and 0 < avail <= total
    second = recv_until(s, "S", timeout=4)      # a second after connecting
    assert struct.unpack_from("<H", second)[0] <= 1000


def test_the_keys_window_draws(monkeypatch):
    monkeypatch.setenv("SDL_VIDEODRIVER", "dummy")
    v = load_viewer()
    import pygame
    pygame.display.init()
    pygame.font.init()
    pygame.display.set_mode((100, 100))
    try:
        nd = v.load_keycodes()
        kw = v.KeysWindow(nd)

        class FakeLink:
            stats = {"cpu": 42.0, "total": 55668, "avail": 30000,
                     "stotal": 27832, "sfree": 26000, "at": 0.0}
        kw.feed([(nd["UP"], True, 1.0), (nd["5"], True, 1.0), (nd["5"], False, 1.25)])
        assert nd["UP"] in kw.down and nd["5"] not in kw.down
        assert kw.log[-1][0] == "5" and abs(kw.log[-1][1] - 0.25) < 1e-9
        kw.draw(FakeLink(), 1.5)
        # the UP key is lit in the drawn surface
        lit = [kw.surf.get_at((x, y))[:3] for x in range(0, kw.W, 4) for y in range(30, 90, 4)]
        assert kw.LIT in lit
        kw.close()
    finally:
        pygame.quit()


def test_a_port_probe_does_not_take_the_phone_from_the_viewer(watchd):
    """`ndlink watch` checks the port before launching. That check used to
    replace the open viewer, which then showed "reconnecting" and took the
    phone back a second later -- over and over."""
    s, _ = connect(watchd)
    recv_until(s, "V")
    for _ in range(3):
        socket.create_connection(("127.0.0.1", watchd["port"]), timeout=1).close()
    silent = socket.create_connection(("127.0.0.1", watchd["port"]), timeout=1)
    time.sleep(0.3)

    # still the viewer: its keys still land, and the phone did not hang up
    s.sendall(HDR.pack(ord("K"), 8) + struct.pack("<iB3x", ND_KEY_UP, 1))
    assert watchd["devkey"].recv(64) == b"103 1"
    s.settimeout(0.3)
    try:
        assert s.recv(1) != b"", "the viewer was disconnected"
    except socket.timeout:
        pass
    silent.close()


def test_a_second_real_viewer_still_wins(watchd):
    s, _ = connect(watchd)
    recv_until(s, "V")
    s2, _ = connect(watchd)
    body = recv_until(s2, "V")
    assert struct.unpack_from("<HHHH", body) == (0, 0, W, H)
    s.settimeout(2)
    while True:                          # the first is closed after its backlog
        if not s.recv(65536):
            break

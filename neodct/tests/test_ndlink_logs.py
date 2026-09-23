"""ndlink-logs against a fake telnetd.

The fake speaks just enough of what busybox telnetd + ash do after
ndlink-telnet's handshake: it ignores the stty line, reads the one framed
command, and answers it the way the phone's shell would -- the log file for a
read, the history + live marker + new lines for a follow. It then closes,
which for a follow is "the phone went away" and must exit 3, not hang.
"""

import json
import os
import re
import socket
import subprocess
import sys
import threading

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HELPER = os.path.join(ROOT, "neodct", "tools", "ndlink-logs")
ND_LOG_C = os.path.join(ROOT, "neodct", "src", "lib", "nd_log.c")

OLD = [
    "Sep 23 10:00:00 neodct user.info nd-core: [MODEM] Network: registered",
    "Sep 23 10:00:01 neodct kern.info kernel: usb 1-1: new device",
]
CUR = [
    "Sep 23 10:01:00 neodct user.info nd-core: [CORE] started",
    "Sep 23 10:01:02 neodct user.info nd-core: [MODEM] Requesting Dial: 5551234",
    "Sep 23 10:01:03 neodct user.info nd-core: [AUDIO] capture on",
    "Sep 23 10:01:04 neodct daemon.info bluetoothd[412]: adapter up",
]
LIVE = [
    "Sep 23 10:02:00 neodct user.info nd-core: [MODEM] Call connected (outgoing) after 4.1 s.",
    "Sep 23 10:02:01 neodct user.info nd-core: [UI] redraw",
    "Sep 23 10:02:30 neodct user.err nd-core: [MODEM] Call ended: ended by the network",
]


class FakePhone:
    def __init__(self):
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(4)
        self.port = self.srv.getsockname()[1]
        self.commands = []
        self.cleared = False
        self.th = threading.Thread(target=self._serve, daemon=True)
        self.th.start()

    def _serve(self):
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self._one, args=(c,), daemon=True).start()

    def _one(self, c):
        buf = b""
        with c:
            while buf.count(b"\n") < 2:
                d = c.recv(4096)
                if not d:
                    return
                buf += d
            cmd = buf.split(b"\n")[1].decode()
            self.commands.append(cmd)
            nonce = re.search(r"'(<<ND\d+>>)'", cmd).group(1)
            out = [nonce]
            if "tail -n +" in cmd:
                live = re.search(r"'(<<NDLIVE\d+>>)'", cmd).group(1)
                out += OLD + CUR + [live] + LIVE
            elif ": >" in cmd:
                self.cleared = True
                out += ["<<NDE=0>>"]
            else:
                out += ([] if self.cleared else OLD + CUR) + ["<<NDE=0>>"]
            # The pty turns \n into \r\n; the helper must not care.
            c.sendall(("\r\n".join(out) + "\r\n").encode())

    def close(self):
        self.srv.close()


@pytest.fixture
def phone():
    p = FakePhone()
    yield p
    p.close()


def run(phone_port, *args):
    r = subprocess.run(
        [sys.executable, HELPER, "--host", "127.0.0.1", "--port", str(phone_port),
         "--timeout", "5", *args],
        capture_output=True, text=True, timeout=30)
    return r.returncode, r.stdout.splitlines(), r.stderr


def test_a_read_includes_the_rotated_file_oldest_first(phone):
    rc, out, _ = run(phone.port)
    assert rc == 0
    assert out == OLD + CUR
    assert "/var/log/messages.0 /var/log/messages" in phone.commands[0]


def test_modem_keeps_the_modem_and_call_audio_only(phone):
    rc, out, _ = run(phone.port, "--modem")
    assert rc == 0
    assert out == [OLD[0], CUR[1], CUR[2]]


def test_n_counts_matching_lines_not_raw_ones(phone):
    rc, out, _ = run(phone.port, "--modem", "-n", "1")
    assert out == [CUR[2]]


def test_a_tag_also_matches_a_daemon_name(phone):
    rc, out, _ = run(phone.port, "--tag", "bluetoothd")
    assert out == [CUR[3]]


def test_os_drops_the_kernel(phone):
    rc, out, _ = run(phone.port, "--os")
    assert OLD[1] not in out and len(out) == 5


def test_grep_is_case_insensitive(phone):
    rc, out, _ = run(phone.port, "--grep", "requesting dial")
    assert out == [CUR[1]]


def test_json_read_is_one_object(phone):
    rc, out, _ = run(phone.port, "--json", "hw", "--modem")
    assert rc == 0 and len(out) == 1
    obj = json.loads(out[0])
    assert obj["ok"] is True and obj["verb"] == "logs" and obj["target"] == "hw"
    assert obj["lines"].split("\n") == [OLD[0], CUR[1], CUR[2]]


def test_follow_shows_the_backlog_then_live_lines(phone):
    rc, out, err = run(phone.port, "-f", "--modem", "-n", "2")
    # The last two MATCHING history lines, then only the live modem lines.
    assert out == [CUR[1], CUR[2], LIVE[0], LIVE[2]]
    # The fake hangs up after the live lines: a lost phone, said out loud.
    assert rc == 3
    assert "closed the connection" in err


def test_follow_counts_before_it_tails_so_nothing_falls_between(phone):
    run(phone.port, "-f")
    cmd = phone.commands[0]
    assert cmd.index("wc -l") < cmd.index("head -n") < cmd.index("tail -n +")
    assert "-F" in cmd  # survives syslogd's rename-rotation


def test_follow_in_json_is_one_object_per_line(phone):
    rc, out, _ = run(phone.port, "-f", "-n", "0", "--json", "hw", "--modem")
    objs = [json.loads(x) for x in out]
    assert [o.get("line") for o in objs[:2]] == [LIVE[0], LIVE[2]]
    assert objs[-1]["ok"] is False and objs[-1]["code"] == 3


def test_colour_paints_the_tag_like_the_serial_console(phone):
    rc, out, _ = run(phone.port, "--tag", "MODEM", "-n", "1", "--color", "always")
    assert "\033[1m\033[38;5;39m[MODEM]\033[0m" in out[0]


def test_clear_empties_the_log(phone):
    rc, _, err = run(phone.port, "--clear")
    assert rc == 0 and "cleared" in err
    assert ": > /var/log/messages" in phone.commands[0]
    rc, out, _ = run(phone.port)
    assert out == []


def test_an_absent_phone_is_exit_3():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    rc, _, err = run(port)
    assert rc == 3 and "unreachable" in err


def test_bad_usage_is_exit_5():
    rc, _, err = run(1, "-n", "-3")
    assert rc == 5
    rc, _, err = run(1, "--grep", "(")
    assert rc == 5


def test_the_palette_matches_nd_log_c():
    """The colours are copied from nd_log.c; this is what keeps them copied."""
    src = open(ND_LOG_C).read()
    named = dict((t, int(c)) for t, c in re.findall(
        r'\{"([^"]+)",\s*(\d+)\}', src.split("NAMED[] = {", 1)[1].split("};", 1)[0]))
    apps = set(re.findall(r'"([^"]+)"', src.split("APP_TAGS[] = {", 1)[1].split("};", 1)[0]))
    import importlib.machinery
    import importlib.util
    loader = importlib.machinery.SourceFileLoader("ndlink_logs", HELPER)
    spec = importlib.util.spec_from_loader("ndlink_logs", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    assert mod.NAMED == named
    assert mod.APP_TAGS == apps

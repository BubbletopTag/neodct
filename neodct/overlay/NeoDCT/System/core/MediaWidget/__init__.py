"""Play media without letting mpv take the phone down with it.

mpv is by far the largest thing NeoDCT will ever start: decoding a video
costs more memory than the whole Python UI, and the browser it is usually
launched from is the second largest. Both alive at once does not fit in
64 MB, so this module owns three decisions and every caller inherits them.

  * The framebuffer, never DRM. mpv 0.35 ships no fbdev output at all --
    the NeoDCT build adds one (`--vo=fbdev`), because the RV1103's display
    is an ST7789 driven through a plain fbdev node and there is no DRM
    driver behind it. Nothing here may ever fall back to `--vo=drm`: on
    this board that fails, and a failed video output means mpv holds the
    screen showing nothing.

  * The calling application is stopped, not merely ignored. `suspended()`
    SIGSTOPs it for exactly as long as mpv runs. This is about the CPU
    more than the memory: one 1.2 GHz core decodes 240x175 MJPEG with very
    little to spare, and NetSurf's redraw loop happily eats what is left.

  * Keys arrive over mpv's IPC socket, not through mpv. The phone's keypad
    is an i2c port expander that only the Python side knows how to read,
    and in QEMU the same code reads an evdev keyboard instead. Forwarding
    from there means one input path serves both, and mpv needs no idea
    which machine it is on. The C key is bound to `quit` in mpv's own
    built-in defaults as well, so it works even if this bridge never
    starts -- being unable to leave a video is being unable to use the
    phone.

`kind_for()` exists so that images and MMS parts can be shown by the same
path later: mpv already decodes them, and an image is a video that does not
move. Only the argv changes.
"""

import json
import os
import select
import signal
import socket
import struct
import subprocess
import time
from contextlib import contextmanager

MPV_BIN = "/usr/bin/mpv"
FB_DEVICE = "/dev/fb0"

# /run is the tmpfs the rest of the system already publishes state into.
IPC_SOCKET = "/run/neodct/mpv.sock"

# Baked into the read-only rootfs beside this file.
INPUT_CONF = "/NeoDCT/System/core/MediaWidget/input.conf"

_VIDEO_EXT = (".avi", ".mp4", ".mkv", ".webm", ".mov", ".ndv", ".3gp", ".ogv")
_IMAGE_EXT = (".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp")
_AUDIO_EXT = (".mp3", ".wav", ".m4a", ".ogg", ".flac", ".aac", ".amr")


def kind_for(url):
    """"video", "image" or "audio" for a url, guessing video when unsure."""
    path = url.split("?", 1)[0].split("#", 1)[0]
    ext = os.path.splitext(path)[1].lower()
    if ext in _IMAGE_EXT:
        return "image"
    if ext in _AUDIO_EXT:
        return "audio"
    return "video"


def build_argv(url, kind=None, fbdev=FB_DEVICE, ipc_socket=None,
               mpv=MPV_BIN, input_conf=INPUT_CONF):
    """The full mpv command line for `url`.

    Everything is spelled out rather than left to mpv's defaults. An
    appliance that behaves differently depending on what happens to be
    installed is one that cannot be debugged from a serial log.
    """
    if kind is None:
        kind = kind_for(url)

    argv = [
        mpv,
        "--no-config",
        "--input-conf=" + input_conf,

        # The whole point: software decode straight into /dev/fb0.
        "--vo=fbdev",
        "--fbdev-device=" + fbdev,
        "--hwdec=no",
        "--vd-lavc-threads=1",

        "--ao=alsa",

        # Subtitle auto-loading stats the whole directory for every file
        # played, off a card mounted over SPI.
        "--sub-auto=no",

        # Nothing here turns off the OSC, scripts or ytdl. They are all
        # parts of mpv's Lua layer, this build has no Lua in it, and the
        # options that control them therefore do not exist -- mpv exits on
        # an unrecognised option rather than warning about it.

        # Return to the caller at the end of the file instead of parking on
        # a black screen waiting for input.
        "--keep-open=no",
        "--idle=no",

        # The demuxer cache, kept deliberately tiny. Measured on the
        # device: at 4MiB mpv is 19.4 MB RSS with 7.8 MB dirty; at 512KiB
        # it is 16.3 MB with 5.4 MB dirty, and the same clip plays at the
        # same speed. Dirty pages are the ones that matter, because they
        # can only go to zram -- and compressing them costs the single
        # core the decoder is already using. A bigger cache buys nothing
        # here: the phone either has the bandwidth to keep up or it does
        # not, and no amount of buffering fixes the second case.
        "--cache=yes",
        "--demuxer-max-bytes=512KiB",
        "--demuxer-readahead-secs=1",
    ]

    if kind == "image":
        argv += ["--image-display-duration=inf", "--audio=no"]

    if ipc_socket:
        argv.append("--input-ipc-server=" + ipc_socket)

    # "--" first: a src attribute is attacker-controlled text, and mpv would
    # read one starting with a dash as an option.
    argv += ["--", url]
    return argv


# NeoDCT keycode -> mpv IPC command. Matches the bindings compiled into the
# NeoDCT mpv build, so the phone behaves the same whether keys arrive over
# the socket or through mpv's own input.
_KEYMAP = {
    14: ["quit"],                            # C: back to the application
    28: ["cycle", "pause"],                  # navikey
    105: ["seek", "-10", "relative"],
    106: ["seek", "10", "relative"],
    103: ["add", "volume", "5"],
    108: ["add", "volume", "-5"],
    42: ["seek", "-60", "relative"],         # *
    43: ["seek", "60", "relative"],          # #
}

# 1..9 jump to 10%..90%, 0 to the start -- the way every DVD player did it.
for _code, _percent in zip(range(2, 12), list(range(10, 100, 10)) + [0]):
    _KEYMAP[_code] = ["seek", str(_percent), "absolute-percent"]


# What each NeoDCT keycode is called in mpv's own input layer. The names
# are mpv's, not Linux's: SHARP because '#' starts a comment in input.conf,
# BS because that is what mpv calls the key NeoDCT prints "C" on.
MPV_KEY_NAMES = {
    14: "BS",
    28: "ENTER",
    103: "UP",
    108: "DOWN",
    105: "LEFT",
    106: "RIGHT",
    42: "*",
    43: "SHARP",
}
for _code, _digit in zip(range(2, 12), "1234567890"):
    MPV_KEY_NAMES[_code] = _digit


def ipc_command(keycode):
    """The mpv command a NeoDCT keypress means, or None if it means nothing."""
    command = _KEYMAP.get(keycode)
    return list(command) if command is not None else None


def encode_command(command):
    """One mpv IPC request: compact JSON, newline terminated."""
    return (json.dumps({"command": list(command)},
                       separators=(",", ":")) + "\n").encode()


@contextmanager
def suspended(pid):
    """Stop `pid` for the duration of the block, whatever happens in it.

    A SIGSTOP that is never undone leaves a phone that looks powered on and
    answers nothing, so the SIGCONT is in a finally and a process that
    exited in the meantime is not treated as a failure.
    """
    if pid is None:
        yield
        return

    pid = int(pid)
    # Stopping the process that is meant to resume us, or init, produces a
    # phone that cannot be recovered without pulling the battery.
    if pid == os.getpid():
        raise ValueError("refusing to suspend the calling process")
    if pid <= 1:
        raise ValueError("refusing to suspend pid %d" % pid)

    try:
        os.kill(pid, signal.SIGSTOP)
    except ProcessLookupError:
        yield
        return
    except PermissionError:
        yield
        return

    try:
        yield
    finally:
        try:
            os.kill(pid, signal.SIGCONT)
        except OSError:
            pass


# How long to wait for mpv to bind its IPC socket. It does so during
# startup, after loading its config but before the first frame, so this is
# generous: dropping the user's first keypress is worse than a slow start.
_SOCKET_TIMEOUT = 5.0

# The bridge's poll interval. Long enough not to spin a core the decoder
# needs, short enough that a keypress does not feel dropped.
_KEY_POLL = 0.1


def _connect(proc, sock_path, timeout=_SOCKET_TIMEOUT):
    """Wait for mpv's IPC socket to appear and connect to it.

    Returns None if mpv exited or never got that far; the caller then just
    waits it out, and mpv's own built-in bindings are the way back.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return None
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.connect(sock_path)
            return sock
        except OSError:
            sock.close()
            time.sleep(0.05)
    return None


def _bridge(proc, sock_path, keysource):
    """Forward keypresses to mpv until it exits."""
    conn = _connect(proc, sock_path)
    if conn is None:
        proc.wait()
        return

    try:
        while proc.poll() is None:
            code = keysource(_KEY_POLL)
            if code is None:
                continue
            command = ipc_command(code)
            if command is None:
                continue
            try:
                conn.sendall(encode_command(command))
            except OSError:
                # mpv closed the socket -- it is on its way out anyway.
                break
    finally:
        try:
            conn.close()
        except OSError:
            pass
    proc.wait()


def play(url, kind=None, suspend_pid=None, keysource=None, mpv=MPV_BIN,
         fbdev=FB_DEVICE, ipc_socket=IPC_SOCKET, input_conf=INPUT_CONF,
         env=None):
    """Play `url` with mpv, returning its exit status.

    `suspend_pid` is stopped for as long as mpv runs -- normally the
    application that asked for playback. `keysource` is called as
    `keysource(timeout)` and should return a NeoDCT keycode or None; both
    the i2c keypad and the QEMU keyboard already have that shape.

    Without a keysource no IPC socket is created: there would be nothing
    on the other end of it, and mpv's built-in bindings still work.
    """
    if keysource is None:
        ipc_socket = None

    if ipc_socket:
        # A killed mpv leaves its socket behind, and binding onto an
        # existing path fails -- so one crash would break playback until
        # the next reboot.
        try:
            os.unlink(ipc_socket)
        except OSError:
            pass
        try:
            os.makedirs(os.path.dirname(ipc_socket), exist_ok=True)
        except OSError:
            pass

    argv = build_argv(url, kind=kind, fbdev=fbdev, ipc_socket=ipc_socket,
                      mpv=mpv, input_conf=input_conf)

    try:
        with suspended(suspend_pid):
            try:
                proc = subprocess.Popen(argv, env=env)
            except OSError:
                # No mpv on this image: the caller gets its screen back and
                # a status to report, rather than a traceback.
                return 127

            if ipc_socket:
                _bridge(proc, ipc_socket, keysource)
            else:
                proc.wait()
            return proc.returncode
    finally:
        if ipc_socket:
            try:
                os.unlink(ipc_socket)
            except OSError:
                pass


# struct input_event. Native sizes on purpose: the timeval is two longs, so
# this is 16 bytes on 32-bit ARM and 24 on the host the tests run on, and
# hardcoding either would break the other.
_INPUT_EVENT = struct.Struct("llHHi")
_EV_KEY = 0x01

# The uinput keyboard the Browser app bridges i2c presses onto. By the time
# mpv starts, that bridge owns the i2c bus, so this is where the keypad is.
KEYPAD_UINPUT_NAME = "neodct-t9-keypad"
KEYPAD_DEVICE_ENV = "NEODCT_KEYPAD_DEVICE"


class EvdevKeys:
    """A keysource over a Linux evdev device.

    Same shape as `ui.read_keypress(timeout)`, so mpv is driven by the same
    call whether the keys come from the phone's keypad bridge or from a
    real keyboard under QEMU.
    """

    def __init__(self, fd):
        self.fd = fd
        # One read can carry a whole burst; hold the rest so no press in it
        # is lost.
        self._pending = []

    @classmethod
    def open(cls, path):
        """A reader for `path`, or None if it cannot be opened."""
        if not path:
            return None
        try:
            return cls(fd=os.open(path, os.O_RDONLY | os.O_NONBLOCK))
        except OSError:
            return None

    @classmethod
    def discover(cls, env=None, sysfs="/sys/class/input", dev="/dev/input"):
        """Which evdev device the keypad reaches us on, or None.

        Prefers the NeoDCT keypad bridge, then anything calling itself a
        keyboard, so the same binary works on the phone and in QEMU.
        """
        if env is None:
            env = os.environ
        override = (env.get(KEYPAD_DEVICE_ENV) or "").strip()
        if override and os.path.exists(override):
            return override

        keypad, keyboard = None, None
        try:
            events = sorted(name for name in os.listdir(sysfs)
                            if name.startswith("event"))
        except OSError:
            return None

        for event in events:
            node = os.path.join(dev, event)
            if not os.path.exists(node):
                continue
            try:
                with open(os.path.join(sysfs, event, "device", "name")) as f:
                    name = f.read().strip()
            except OSError:
                continue
            if name == KEYPAD_UINPUT_NAME:
                return node
            if keyboard is None and "keyboard" in name.lower():
                keyboard = node
            if keypad is None and "keypad" in name.lower():
                keypad = node

        return keypad or keyboard

    def __call__(self, timeout=0.1):
        """The next key-down code, or None if none arrived in `timeout`."""
        if self._pending:
            return self._pending.pop(0)

        try:
            readable, _, _ = select.select([self.fd], [], [], timeout)
        except (OSError, ValueError):
            return None
        if not readable:
            return None

        try:
            data = os.read(self.fd, _INPUT_EVENT.size * 64)
        except OSError:
            return None
        if not data:
            return None

        size = _INPUT_EVENT.size
        for offset in range(0, len(data) - size + 1, size):
            _, _, etype, code, value = _INPUT_EVENT.unpack_from(data, offset)
            # value 1 is a press; 0 is a release and 2 is autorepeat, and
            # neither should count as the user asking for something again.
            if etype == _EV_KEY and value == 1:
                self._pending.append(code)

        return self._pending.pop(0) if self._pending else None

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass

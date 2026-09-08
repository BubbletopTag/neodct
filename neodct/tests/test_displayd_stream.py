"""The ST7789 transcript neodct_displayd writes, asserted as bytes.

`neodct_displayd --panel stream:<path>` writes every DC-framed call it would
have put on the SPI bus, and `--fb-at WxH@BPP:STRIDE <file>` lets an ordinary
file stand in for /dev/fb0 with its geometry supplied rather than asked for --
nd_bootfb_open_at()'s argument verbatim, and never used on a device. Between
them the daemon's composing half runs on a build host for the first time.

============ WHY THE ORACLE IS A BYTE STREAM AND NOT A PICTURE ============

A golden PNG of the composed 240x240 panel was the obvious alternative and it
is the wrong artefact. Every pixel of that image is either a pixel of an
existing golden frame or black, so it fails identically for two causes a
reviewer needs to tell apart: the UI screen changed on purpose, and the
letterbox arithmetic broke. CODING-STANDARDS section 7 already rules that a
redesigned screen does not get a new picture of itself, and there are 52
frames in neodct/tests/golden/ that a panel composite would double for no new
information.

The transcript fails for exactly one reason, so it is a gate rather than a
net. The picture is what a human looks at when the diff says "these 400 bytes
moved", and neodct/tools/st7789_replay.py renders it on demand -- decoding
with a second implementation written from the datasheet, which is what lets it
disagree with a wrong encoder rather than agreeing by construction.

No QEMU and no hardware.

    make -C neodct/src && python3 -m pytest neodct/tests/test_displayd_stream.py
"""

import os
import signal
import struct
import subprocess
import sys
import time

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(REPO, "src", "build", "default")
DISPLAYD = os.path.join(BUILD, "bin", "neodct_displayd")
TOOLS = os.path.join(REPO, "tools")
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

import st7789_replay as replay  # noqa: E402

pytestmark = pytest.mark.skipif(
    not os.path.exists(DISPLAYD),
    reason="the C build is not present (make -C neodct/src)")

PANEL_W = PANEL_H = 240
BAND_W, BAND_H = 240, 175
BAND_Y = PANEL_H - BAND_H          # 65, the Nokia faceplate letterbox
STRIDE = BAND_W * 4                # what vfb grants at 32 bpp
FB_BYTES = BAND_H * STRIDE         # 168,000

TAG_CMD = ord("C")
TAG_DATA = ord("D")
TAG_RESET = ord("R")


# --------------------------------------------------------------------- #
# The transcript, read straight out of nd_panel.h's description
# --------------------------------------------------------------------- #


def records(blob):
    """[(tag, payload)] after the 12-byte header. Refuses a torn tail."""
    assert blob[:4] == b"ND79", f"not a transcript: {blob[:8]!r}"
    version, _flags, w, h, _res = struct.unpack("<BBHHH", blob[4:12])
    assert version == 1
    assert (w, h) == (PANEL_W, PANEL_H)
    out = []
    off = 12
    while off < len(blob):
        assert off + 5 <= len(blob), f"torn record header at {off}"
        tag = blob[off]
        (length,) = struct.unpack("<I", blob[off + 1:off + 5])
        off += 5
        assert off + length <= len(blob), f"torn record body at {off}"
        out.append((tag, blob[off:off + length]))
        off += length
    return out


def cmd(op):
    return (TAG_CMD, bytes([op]))


def window(lo, hi):
    return (TAG_DATA, bytes([lo >> 8, lo & 0xFF, hi >> 8, hi & 0xFF]))


def pack565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def be565(r, g, b):
    v = pack565(r, g, b)
    return bytes([v >> 8, v & 0xFF])


# --------------------------------------------------------------------- #
# A framebuffer, and a daemon watching it
# --------------------------------------------------------------------- #


def gradient():
    """A band with no 0xA5 byte in column 0.

    prev_fb is memset to 0xA5 so the first frame is a full send, and the dirty
    COLUMN range is derived from the first and last differing BYTES of each
    row. A red channel that happened to be 0xA5 in column 0 of every row would
    shrink the first window by one pixel and make this test's expected CASET
    wrong for a reason that has nothing to do with the daemon.
    """
    fb = bytearray(FB_BYTES)
    for y in range(BAND_H):
        for x in range(BAND_W):
            o = y * STRIDE + x * 4
            fb[o + 0] = (x * 7 + 1) % 0xA5      # red   -- never 0xA5
            fb[o + 1] = (y * 3) % 0xA5          # green
            fb[o + 2] = 0x40                    # blue
            fb[o + 3] = 0x00                    # x
    # Three named colours the assertions below can name back.
    put(fb, 0, 0, 0xFF, 0x00, 0x00)
    put(fb, 1, 0, 0x80, 0x80, 0x80)
    put(fb, BAND_W - 1, BAND_H - 1, 0x12, 0x7E, 0xC3)
    return fb


def put(fb, x, y, r, g, b):
    o = y * STRIDE + x * 4
    fb[o + 0], fb[o + 1], fb[o + 2], fb[o + 3] = r, g, b, 0x00


class Rig:
    """A framebuffer file, a transcript file, and a daemon between them."""

    def __init__(self, tmp_path):
        self.fb_path = tmp_path / "fb.raw"
        self.tr_path = tmp_path / "panel.nd79"
        self.log_path = tmp_path / "displayd.log"
        self.proc = None
        self.log = None

    def write_fb(self, fb):
        self.fb_path.write_bytes(bytes(fb))

    def poke(self, x, y, r, g, b):
        """Change ONE pixel in place, with one four-byte write.

        The daemon has the file mmap'd MAP_SHARED, so a whole-frame rewrite
        could be read half-done and produce a dirty rectangle wider than the
        change. Four aligned bytes cannot tear that way, which is also exactly
        the case the dirty-rect optimisation is for.
        """
        with open(self.fb_path, "r+b") as fh:
            fh.seek(y * STRIDE + x * 4)
            fh.write(bytes([r, g, b, 0x00]))
            fh.flush()
            os.fsync(fh.fileno())

    def touch_transcript(self):
        """The sink deliberately does not O_CREAT -- see nd_panel_stream.c.

        A boot found what O_CREAT cost: a guest with no virtio-serial port
        attached opened /dev/vport0p1 as a new regular file in devtmpfs and
        reported success, which is a panel stream written to nowhere.
        """
        self.tr_path.write_bytes(b"")

    def start(self, *extra):
        self.touch_transcript()
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            [DISPLAYD,
             "--fb-at", f"{BAND_W}x{BAND_H}@32:{STRIDE}", str(self.fb_path),
             "--panel", f"stream:{self.tr_path}",
             "--fps", "60", *extra],
            stdout=self.log, stderr=subprocess.STDOUT)

    def run_once(self):
        self.touch_transcript()
        subprocess.run(
            [DISPLAYD,
             "--fb-at", f"{BAND_W}x{BAND_H}@32:{STRIDE}", str(self.fb_path),
             "--panel", f"stream:{self.tr_path}",
             "--once"],
            check=True, capture_output=True, timeout=60)

    def size(self):
        return self.tr_path.stat().st_size if self.tr_path.exists() else 0

    def settle(self, quiet=0.35, timeout=30.0):
        """Wait until the transcript stops growing, and return its size.

        A skipped frame writes literally nothing, so "no growth for a third of
        a second at 60 fps" is roughly twenty polls of silence rather than a
        guess about timing.
        """
        deadline = time.time() + timeout
        last, since = self.size(), time.time()
        while time.time() < deadline:
            time.sleep(0.02)
            now = self.size()
            if now != last:
                last, since = now, time.time()
            elif last > 0 and time.time() - since >= quiet:
                return last
        raise AssertionError("the transcript never stopped growing")

    def wait_grow(self, past, timeout=30.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.size() > past:
                return self.settle()
            time.sleep(0.01)
        raise AssertionError(f"the transcript never grew past {past} bytes"
                             f" (log: {self.log_path.read_text()})")

    def records(self):
        return records(self.tr_path.read_bytes())

    def stop(self):
        if self.proc is not None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=30)
            except subprocess.TimeoutExpired:      # pragma: no cover
                self.proc.kill()
                self.proc.wait(timeout=10)
            self.proc = None
        if self.log is not None:
            self.log.close()
            self.log = None


@pytest.fixture
def rig(tmp_path):
    r = Rig(tmp_path)
    try:
        yield r
    finally:
        r.stop()


# --------------------------------------------------------------------- #
# The single-frame transcript
# --------------------------------------------------------------------- #

INIT = [
    (TAG_RESET, b""),
    cmd(0x01),                      # SWRESET
    cmd(0x11),                      # SLPOUT
    cmd(0x3A), (TAG_DATA, b"\x55"),  # COLMOD, RGB565
    cmd(0x36), (TAG_DATA, b"\x00"),  # MADCTL
    cmd(0x21),                      # INVON, required on this IPS panel
    cmd(0x13),                      # NORON
    cmd(0x29),                      # DISPON
]


def test_the_init_transcript_is_the_st7789_bring_up(rig):
    """The bring-up sequence, byte for byte, on a machine with no panel."""
    rig.write_fb(gradient())
    rig.run_once()
    recs = rig.records()
    assert recs[:len(INIT)] == INIT


def test_the_blanking_fill_is_the_whole_panel_and_it_is_the_letterbox(rig):
    """fill_color(0,0,0) covers 240x240 once, and nothing ever repaints it.

    This is the letterboxing. There is no 240x240 compose buffer in the
    daemon and there must not become one: the rows above the band are black
    because the panel was blanked at start-up and the band is written into
    rows 65..239 for ever after.
    """
    rig.write_fb(gradient())
    rig.run_once()
    recs = rig.records()[len(INIT):]
    assert recs[0] == cmd(0x2A)
    assert recs[1] == window(0, PANEL_W - 1)
    assert recs[2] == cmd(0x2B)
    assert recs[3] == window(0, PANEL_H - 1)
    assert recs[4] == cmd(0x2C)
    assert recs[5][0] == TAG_DATA
    assert len(recs[5][1]) == PANEL_W * PANEL_H * 2   # 115,200
    assert recs[5][1] == bytes(PANEL_W * PANEL_H * 2)


def test_the_full_frame_window_carries_the_letterbox_offset_as_bytes(rig):
    """CASET 00 00 00 EF / RASET 00 41 00 EF.

    0x41 is 65 -- the offset arriving as BYTES rather than as an integer,
    which is why the seam is at the DC-framed byte and not at a rectangle. A
    rect seam would leave these six bytes produced by code no test on either
    machine ever reads back.
    """
    rig.write_fb(gradient())
    rig.run_once()
    recs = rig.records()[len(INIT) + 6:]
    assert recs[0] == cmd(0x2A)
    assert recs[1] == (TAG_DATA, bytes([0x00, 0x00, 0x00, 0xEF]))
    assert recs[2] == cmd(0x2B)
    assert recs[3] == (TAG_DATA, bytes([0x00, 0x41, 0x00, 0xEF]))
    assert recs[4] == cmd(0x2C)
    assert len(recs[5][1]) == BAND_W * BAND_H * 2     # 84,000
    assert len(recs) == 6, "one frame and no more"


def test_a_known_rgb_packs_to_big_endian_565_with_red_first(rig):
    """The 8888 -> 565 pack, and which end red is on.

    fb_swap_rb comes from fb_var_screeninfo rather than from an assumption,
    which is the bug convert_rect()'s own comment block is about: the phone's
    vfb declares red.offset 0 (bytes R G B x), and --fb-at supplies the same
    layout because vfb is the driver on both machines now.
    """
    fb = gradient()
    rig.write_fb(fb)
    rig.run_once()
    payload = rig.records()[-1][1]

    assert payload[0:2] == be565(0xFF, 0x00, 0x00) == b"\xf8\x00"
    assert payload[2:4] == be565(0x80, 0x80, 0x80)
    last = ((BAND_H - 1) * BAND_W + (BAND_W - 1)) * 2
    assert payload[last:last + 2] == be565(0x12, 0x7E, 0xC3)

    # And every other pixel, so this is the pack and not three lucky ones.
    expected = bytearray()
    for y in range(BAND_H):
        for x in range(BAND_W):
            o = y * STRIDE + x * 4
            expected += be565(fb[o], fb[o + 1], fb[o + 2])
    assert payload == bytes(expected)


# --------------------------------------------------------------------- #
# What happens on the second frame, which is the whole v2 optimisation
# --------------------------------------------------------------------- #


def test_a_single_changed_pixel_sends_a_one_pixel_window(rig):
    """The dirty rectangle, checked on the wire rather than in an integer."""
    rig.write_fb(gradient())
    rig.start()
    first = rig.settle()
    n_before = len(rig.records())

    rig.poke(100, 40, 0x00, 0xFF, 0x00)
    rig.wait_grow(first)
    new = rig.records()[n_before:]

    assert new == [
        cmd(0x2A), window(100, 100),
        cmd(0x2B), window(40 + BAND_Y, 40 + BAND_Y),   # 105 = 0x69
        cmd(0x2C), (TAG_DATA, be565(0x00, 0xFF, 0x00)),
    ], "one 1x1 window at (100, 105) and exactly two payload bytes"


def test_an_unchanged_frame_sends_nothing_at_all(rig):
    """The frame skip. An idle UI costs no SPI and no transcript."""
    rig.write_fb(gradient())
    rig.start()
    settled = rig.settle()
    time.sleep(0.6)                      # ~36 polls at --fps 60
    assert rig.size() == settled, "an unchanged framebuffer produced records"


def test_the_first_frame_after_start_is_the_whole_band(rig):
    """prev_fb is 0xA5 everywhere, so poll one is a full send by construction.

    Worth pinning: it is what makes the daemon's first frame correct after a
    restart, and it is the reason the polling path and --once agree on the
    window even though one goes through render_dirty() and the other through
    render_full().
    """
    rig.write_fb(gradient())
    rig.start()
    rig.settle()
    recs = rig.records()[len(INIT) + 6:]
    assert recs[0] == cmd(0x2A)
    assert recs[1] == (TAG_DATA, bytes([0x00, 0x00, 0x00, 0xEF]))
    assert recs[3] == (TAG_DATA, bytes([0x00, 0x41, 0x00, 0xEF]))
    assert len(recs[5][1]) == BAND_W * BAND_H * 2


# --------------------------------------------------------------------- #
# The decoder, driven by the daemon and then by hand
# --------------------------------------------------------------------- #


def test_the_replay_puts_the_band_at_y_65_under_65_black_rows(rig, tmp_path):
    """st7789_replay decodes the transcript to the composed 240x240 panel.

    The PNG is a debugging artefact rendered here so a failure has something
    to look at. It is not committed and it is not compared -- see the module
    docstring for why the picture cannot be the oracle.
    """
    fb = gradient()
    rig.write_fb(fb)
    rig.run_once()
    panel = replay.replay(rig.tr_path.read_bytes())

    assert (panel.width, panel.height) == (PANEL_W, PANEL_H)
    assert panel.resets == 1
    assert panel.colmod == 0x55
    assert panel.madctl == 0x00
    assert panel.windows == [(0, 0, 239, 239), (0, 65, 239, 239)]

    for y in range(BAND_Y):
        for x in range(PANEL_W):
            assert panel.gram[y * PANEL_W + x] == 0, f"({x},{y}) is not black"
    for y in range(BAND_H):
        for x in range(BAND_W):
            o = y * STRIDE + x * 4
            want = pack565(fb[o], fb[o + 1], fb[o + 2])
            got = panel.gram[(y + BAND_Y) * PANEL_W + x]
            if got != want:
                panel.png(str(tmp_path / "panel.png"))
                raise AssertionError(
                    f"({x},{y + BAND_Y}) is {got:#06x}, want {want:#06x}; "
                    f"decoded frame written to {tmp_path / 'panel.png'}")


def hand_written(*recs, magic=b"ND79", version=1, w=240, h=240):
    """A transcript nobody's encoder produced. The decoder is a second
    implementation of the panel contract, and a decoder nobody has tested
    cannot contradict an encoder."""
    blob = magic + struct.pack("<BBHHH", version, 0, w, h, 0)
    for tag, payload in recs:
        blob += bytes([tag]) + struct.pack("<I", len(payload)) + payload
    return blob


def test_the_decoder_refuses_a_transcript_it_does_not_understand():
    with pytest.raises(replay.ReplayError, match="not an ND79"):
        replay.replay(hand_written(magic=b"ND78"))
    with pytest.raises(replay.ReplayError, match="version 2"):
        replay.replay(hand_written(version=2))
    with pytest.raises(replay.ReplayError, match="shorter than"):
        replay.replay(b"ND79\x01")
    torn = hand_written(cmd(0x2A))[:-1]
    with pytest.raises(replay.ReplayError, match="claims 1 bytes"):
        replay.replay(torn)


def test_the_decoder_refuses_a_window_off_the_panel():
    off = hand_written(cmd(0x2A), (TAG_DATA, bytes([0x00, 0x00, 0x01, 0x00])))
    with pytest.raises(replay.ReplayError, match="CASET end 256"):
        replay.replay(off)
    backwards = hand_written(cmd(0x2B), (TAG_DATA, bytes([0x00, 0x64, 0x00, 0x0A])))
    with pytest.raises(replay.ReplayError, match="RASET start 100 is past"):
        replay.replay(backwards)


def test_the_decoder_refuses_a_ramwr_with_no_window():
    with pytest.raises(replay.ReplayError, match="RAMWR with no address window"):
        replay.replay(hand_written(cmd(0x2C)))


def test_the_decoder_refuses_a_colmod_that_is_not_565():
    for bad in (0x53, 0x66, 0x00):
        with pytest.raises(replay.ReplayError, match="RGB565"):
            replay.replay(hand_written(cmd(0x3A), (TAG_DATA, bytes([bad]))))


def window_of(x0, y0, x1, y1, *payloads):
    """CASET, RASET, RAMWR and however many D records follow it."""
    recs = [cmd(0x2A), window(x0, x1), cmd(0x2B), window(y0, y1), cmd(0x2C)]
    recs += [(TAG_DATA, p) for p in payloads]
    return recs


def test_the_decoder_refuses_a_payload_that_does_not_fill_its_window():
    """The third refusal the module docstring promises, and the one that was
    not implemented.

    `_window_pair` catches a window off the panel and the big-endian unpack
    catches a byte order; a rectangle whose payload is the wrong SIZE used to
    decode in silence -- an over-long one WRAPPED to the top of the window
    (with an inline comment saying that was "worth recording" and nothing
    recording it) and a short one left the rest of the window holding its
    previous value. Both produce a picture that looks almost right, which is
    the one outcome a second implementation exists to prevent.
    """
    px = b"\x00\x01"
    exact = hand_written(*window_of(0, 0, 1, 1, px * 4))
    assert replay.replay(exact).pixels_written == 4

    with pytest.raises(replay.ReplayError, match="more than the 4 pixels"):
        replay.replay(hand_written(*window_of(0, 0, 1, 1, px * 5)))

    with pytest.raises(replay.ReplayError, match="carried 3 pixels for a 4-pixel"):
        replay.replay(hand_written(*window_of(0, 0, 1, 1, px * 3)))

    # ...and it is the frame that caused it that fails, not the last one.
    with pytest.raises(replay.ReplayError, match="the next CASET arrived"):
        replay.replay(hand_written(*(window_of(0, 0, 1, 1, px * 3)
                                     + window_of(0, 0, 1, 1, px * 4))))

    # A payload split across several D records is legal and stays legal: the
    # window is only checked when something ends it.
    split = hand_written(*window_of(0, 0, 1, 1, px + px[:1], px[1:] + px * 2))
    assert replay.replay(split).pixels_written == 4

    with pytest.raises(replay.ReplayError, match="half a pixel"):
        replay.replay(hand_written(*window_of(0, 0, 1, 1, px * 4 + b"\x99")))


def test_a_dropped_record_decodes_as_a_plausible_wrong_picture(rig):
    """Why the sink may not leave a hole, measured rather than argued.

    nd_panel_stream.c's `emit()` used to drop a record it could not afford
    and CARRY ON, on the grounds that the RECORD framing stayed intact. It
    does. The layer above it does not: an ST7789 command and its parameters
    are paired by position and by nothing else, so deleting the band's
    `C 0x2B` makes the four RASET bytes read as CASET's.

    And the decoder cannot save you here, which is the point of pinning it.
    Its exact-fill check catches most of these -- but the band is 240x175 and
    the window this desync produces is 175x240, the SAME AREA, so the payload
    fits it exactly and the transcript decodes clean. What comes out is the
    frame transposed into the wrong corner: window (65,0)-(239,239) where the
    daemon meant (0,65)-(239,239). A picture that looks almost right, with no
    error anywhere, which is precisely the outcome this whole seam exists to
    prevent -- so the sink now ends the transcript at the first drop instead.
    """
    rig.write_fb(gradient())
    rig.run_once()
    recs = rig.records()

    intact = replay.replay(rig.tr_path.read_bytes())
    assert intact.windows == [(0, 0, 239, 239), (0, BAND_Y, 239, 239)]

    # One dropped record, from the host's side: the band's RASET command.
    band_raset = [i for i, (tag, payload) in enumerate(recs)
                  if tag == TAG_CMD and payload == b"\x2b"][-1]
    dropped = [r for i, r in enumerate(recs) if i != band_raset]

    holed = replay.replay(hand_written(*dropped))
    assert holed.windows == [(0, 0, 239, 239), (BAND_Y, 0, 239, 239)], (
        "a dropped record is supposed to be undetectable to a reader of this "
        "format -- if this ever starts raising, the format grew something "
        "that notices, and nd_panel_stream.c's sink_die on drop can be "
        "reconsidered")

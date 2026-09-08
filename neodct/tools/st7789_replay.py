#!/usr/bin/env python3
"""st7789_replay.py -- decode a neodct_displayd panel transcript into a picture.

`neodct_displayd --panel stream:<path>` writes every DC-framed call it would
have put on the SPI bus as the ND79 record stream pinned in
neodct/src/displayd/nd_panel.h. This reads one and rebuilds what the panel's
GRAM would hold.

============ IT IS A SECOND IMPLEMENTATION, DELIBERATELY ============

This decoder is written from the ST7789 datasheet and NOT from
neodctDisplay.c. That is the whole of its value: a decoder derived from the
encoder agrees with it by construction, including where the encoder is wrong.
This one has its own idea of what CASET, RASET, RAMWR and COLMOD mean, so a
window off by the letterbox offset, a byte order flipped, or a rectangle whose
width does not match its payload comes out as a disagreement rather than as a
picture that looks plausible.

So: no importing anything from the daemon, no copying its arithmetic, and a
transcript that does not make sense is an ERROR rather than a best effort. The
refusals below are the point.

============ AND THE PICTURE IS NOT THE ORACLE ============

The committed oracle is the byte stream: neodct/tests/test_displayd_stream.py
asserts records, and a byte stream fails for exactly one reason. A PNG of the
composed 240x240 panel is a picture of a picture -- every pixel of it is
either a pixel of an existing golden frame or black -- so it would fail
identically for "the screen was redesigned on purpose" and "the letterbox
arithmetic broke". CODING-STANDARDS section 7 is explicit that a new screen
does not get a new frame, and this would be 52 more of them.

What the PNG is for is a human looking at a failure whose diff says "these 400
bytes moved". It is rendered on demand and never committed.

    neodct/tools/st7789_replay.py panel.nd79 --out /tmp/panel.png
    neodct/tools/st7789_replay.py panel.nd79 --frames /tmp/f --summary
"""

import argparse
import struct
import sys

MAGIC = b"ND79"
VERSION = 1
HDR_LEN = 12
REC_HDR_LEN = 5

TAG_CMD = ord("C")
TAG_DATA = ord("D")
TAG_RESET = ord("R")

# The ST7789 commands this decoder claims to understand. Anything else is
# recorded and ignored -- a panel init sequence is allowed to grow.
CMD_SWRESET = 0x01
CMD_SLPOUT = 0x11
CMD_NORON = 0x13
CMD_INVON = 0x21
CMD_CASET = 0x2A
CMD_RASET = 0x2B
CMD_RAMWR = 0x2C
CMD_MADCTL = 0x36
CMD_COLMOD = 0x3A
CMD_DISPON = 0x29

COLMOD_RGB565 = 0x55

# Only for the refusal messages -- a decoder that named a command by its hex
# byte would make the reader look it up in the datasheet to read its own error.
NAMES = {
    CMD_CASET: "CASET", CMD_RASET: "RASET", CMD_RAMWR: "RAMWR",
    CMD_MADCTL: "MADCTL", CMD_COLMOD: "COLMOD",
}


class ReplayError(Exception):
    """The transcript does not describe something a panel could have done."""


def parse_header(data):
    """(version, flags, panel_w, panel_h) or a refusal.

    The version byte is checked rather than assumed. A decoder that half
    understands a format it does not know is worse than one that stops: the
    transcript is a committed artefact and the next change to it must not
    quietly produce a plausible wrong picture.
    """
    if len(data) < HDR_LEN:
        raise ReplayError(f"transcript is {len(data)} bytes, shorter than the {HDR_LEN}-byte header")
    if data[:4] != MAGIC:
        raise ReplayError(f"not an ND79 transcript (magic {data[:4]!r})")
    version, flags, panel_w, panel_h, _reserved = struct.unpack("<BBHHH", data[4:HDR_LEN])
    if version != VERSION:
        raise ReplayError(
            f"transcript version {version}, this decoder understands {VERSION} only")
    if panel_w == 0 or panel_h == 0:
        raise ReplayError(f"transcript declares a {panel_w}x{panel_h} panel")
    return version, flags, panel_w, panel_h


def parse_records(data):
    """Yield (tag, payload) from the record stream after the header.

    A truncated final record is an error and not a shrug: the sink disables
    itself on a partial write precisely so that a transcript is either well
    formed to its end or says where it stopped.
    """
    off = HDR_LEN
    while off < len(data):
        if off + REC_HDR_LEN > len(data):
            raise ReplayError(f"truncated record header at byte {off}")
        tag = data[off]
        (length,) = struct.unpack("<I", data[off + 1:off + REC_HDR_LEN])
        off += REC_HDR_LEN
        if off + length > len(data):
            raise ReplayError(
                f"record at byte {off - REC_HDR_LEN} claims {length} bytes, "
                f"only {len(data) - off} remain")
        yield tag, data[off:off + length]
        off += length


class Panel:
    """A 240x240 RGB565 GRAM and the address window that writes into it."""

    def __init__(self, width, height):
        self.width = width
        self.height = height
        # 16-bit values, one per pixel. Undefined until written, which is what
        # 0 means here: the daemon blanks the whole panel before it draws.
        self.gram = [0] * (width * height)
        self.written = [False] * (width * height)
        self.colmod = None
        self.madctl = None
        self.resets = 0
        self.commands = []          # every command byte, in order
        self.windows = []           # every (x0, y0, x1, y1) a RAMWR used
        self.frames = 0             # RAMWR payloads seen
        self.pixels_written = 0
        self._x0 = self._x1 = None
        self._y0 = self._y1 = None
        self._cur = None            # (x, y) cursor inside the window
        self._pending = None        # the command whose parameters come next
        self._odd_byte = None       # a 565 value split across two D records
        # The window a RAMWR is filling, and how much of it has arrived. The
        # module docstring promises that "a rectangle whose width does not
        # match its payload comes out as a disagreement"; these two are what
        # makes that true rather than aspirational. See _ramwr().
        self._win_area = 0
        self._win_px = 0

    # ---- decoding ----------------------------------------------------

    def feed(self, tag, payload):
        if tag == TAG_RESET:
            if payload:
                raise ReplayError(f"reset record carries {len(payload)} bytes")
            self.resets += 1
            return
        if tag == TAG_CMD:
            if len(payload) != 1:
                raise ReplayError(f"command record carries {len(payload)} bytes, expected 1")
            self._command(payload[0])
            return
        if tag == TAG_DATA:
            if not payload:
                raise ReplayError("data record carries no bytes")
            self._data(payload)
            return
        raise ReplayError(f"unknown record tag {tag:#04x}")

    def _command(self, op):
        # A window is finished the moment anything starts setting up the next
        # one. Checking here rather than only at end of stream is what makes
        # an under-filled rectangle fail on the frame that caused it instead
        # of on the last frame of the transcript.
        if op in (CMD_CASET, CMD_RASET, CMD_RAMWR):
            self._end_window("the next %s arrived" % NAMES.get(op, "0x%02X" % op))
        self.commands.append(op)
        self._pending = op
        if op == CMD_RAMWR:
            if self._x0 is None or self._y0 is None:
                raise ReplayError("RAMWR with no address window set")
            self._cur = (self._x0, self._y0)
            self._odd_byte = None
            self._win_area = (self._x1 - self._x0 + 1) * (self._y1 - self._y0 + 1)
            self._win_px = 0

    def _end_window(self, because):
        """Refuse a RAMWR whose payload did not fill its address window.

        The third of the docstring's three refusals, and the one that was
        missing: `_window_pair` catches a window off the panel and the
        big-endian unpack catches a byte order, but a rectangle whose payload
        is the wrong SIZE used to decode in silence -- an over-long one wrapped
        to the top of the window and a short one simply left the rest of the
        window holding its previous value. Either produces a picture that
        looks almost right, which is the one outcome a second implementation
        exists to prevent.

        A payload split across several D records is short until the last one,
        so this is called at window boundaries and at end of stream and never
        per record.
        """
        if self._cur is None:
            return
        if self._odd_byte is not None:
            raise ReplayError(
                "the RAMWR into window (%d,%d)-(%d,%d) ends on half a pixel -- "
                "a 565 value is two bytes and one is left over -- and then %s" %
                (self._x0, self._y0, self._x1, self._y1, because))
        if self._win_px != self._win_area:
            raise ReplayError(
                "the RAMWR into window (%d,%d)-(%d,%d) carried %d pixels for "
                "a %d-pixel window, and then %s. A window and its payload that "
                "disagree are the arithmetic this decoder exists to check." %
                (self._x0, self._y0, self._x1, self._y1,
                 self._win_px, self._win_area, because))
        self._cur = None

    def end_of_stream(self):
        """The last window has to be complete too. replay() calls this."""
        self._end_window("the transcript ended")

    def _data(self, payload):
        op = self._pending
        if op == CMD_CASET:
            self._x0, self._x1 = self._window_pair(payload, "CASET", self.width)
        elif op == CMD_RASET:
            self._y0, self._y1 = self._window_pair(payload, "RASET", self.height)
        elif op == CMD_COLMOD:
            if len(payload) != 1:
                raise ReplayError(f"COLMOD takes 1 byte, got {len(payload)}")
            if payload[0] != COLMOD_RGB565:
                raise ReplayError(
                    f"COLMOD {payload[0]:#04x}: this decoder handles RGB565 "
                    f"({COLMOD_RGB565:#04x}) only, which is the only format "
                    f"neodctDisplay.c packs")
            self.colmod = payload[0]
        elif op == CMD_MADCTL:
            if len(payload) != 1:
                raise ReplayError(f"MADCTL takes 1 byte, got {len(payload)}")
            self.madctl = payload[0]
        elif op == CMD_RAMWR:
            self._ramwr(payload)
        # Anything else takes its parameters and is ignored on purpose.

    def _window_pair(self, payload, name, limit):
        if len(payload) != 4:
            raise ReplayError(f"{name} takes 4 bytes, got {len(payload)}")
        lo = (payload[0] << 8) | payload[1]
        hi = (payload[2] << 8) | payload[3]
        if lo > hi:
            raise ReplayError(f"{name} start {lo} is past its end {hi}")
        if hi >= limit:
            raise ReplayError(f"{name} end {hi} is off a {limit}-pixel panel")
        return lo, hi

    def _ramwr(self, payload):
        if self._cur is None:
            raise ReplayError("pixel data with no RAMWR")
        data = payload
        if self._odd_byte is not None:
            data = bytes([self._odd_byte]) + bytes(payload)
            self._odd_byte = None
        x, y = self._cur
        for i in range(0, len(data) - 1, 2):
            # An over-long payload used to WRAP to the top of the window here,
            # with a comment saying that was "worth recording" and nothing
            # recording it. The panel really does wrap, but the daemon never
            # relies on it, so a wrap in a transcript means the window and the
            # rectangle disagree -- which is exactly the failure the seam was
            # put at the DC-framed byte to catch.
            if self._win_px >= self._win_area:
                raise ReplayError(
                    "the RAMWR into window (%d,%d)-(%d,%d) carries more than "
                    "the %d pixels that window holds; the panel would wrap to "
                    "its top-left and draw a plausible wrong picture" %
                    (self._x0, self._y0, self._x1, self._y1, self._win_area))
            # Big-endian on the wire: the panel takes the high byte first.
            value = (data[i] << 8) | data[i + 1]
            self.gram[y * self.width + x] = value
            self.written[y * self.width + x] = True
            self.pixels_written += 1
            self._win_px += 1
            x += 1
            if x > self._x1:
                x = self._x0
                y += 1
        if len(data) % 2:
            self._odd_byte = data[-1]
        self._cur = (x, y)
        self.frames += 1
        self.windows.append((self._x0, self._y0, self._x1, self._y1))

    # ---- output ------------------------------------------------------

    def rgb(self):
        """The GRAM as RGB888 bytes, 565 expanded the usual way."""
        out = bytearray(self.width * self.height * 3)
        for i, value in enumerate(self.gram):
            r5 = (value >> 11) & 0x1F
            g6 = (value >> 5) & 0x3F
            b5 = value & 0x1F
            out[i * 3 + 0] = (r5 << 3) | (r5 >> 2)
            out[i * 3 + 1] = (g6 << 2) | (g6 >> 4)
            out[i * 3 + 2] = (b5 << 3) | (b5 >> 2)
        return bytes(out)

    def png(self, path):
        from PIL import Image
        Image.frombytes("RGB", (self.width, self.height), self.rgb()).save(path)


def replay(data, on_frame=None):
    """Decode a whole transcript. on_frame(panel, n) fires after each RAMWR."""
    _version, _flags, width, height = parse_header(data)
    panel = Panel(width, height)
    seen = 0
    for tag, payload in parse_records(data):
        before = panel.frames
        panel.feed(tag, payload)
        if panel.frames != before and on_frame is not None:
            seen += 1
            on_frame(panel, seen)
    panel.end_of_stream()
    return panel


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("transcript")
    ap.add_argument("--out", help="write the final GRAM as a PNG")
    ap.add_argument("--frames", metavar="DIR",
                    help="write DIR/frame-NNN.png after every RAMWR payload")
    ap.add_argument("--summary", action="store_true",
                    help="print the commands, the windows and the pixel count")
    args = ap.parse_args(argv)

    with open(args.transcript, "rb") as fh:
        data = fh.read()

    frames_dir = args.frames
    if frames_dir:
        import os
        os.makedirs(frames_dir, exist_ok=True)

    def on_frame(panel, n):
        import os
        panel.png(os.path.join(frames_dir, f"frame-{n:03d}.png"))

    try:
        panel = replay(data, on_frame=on_frame if frames_dir else None)
    except ReplayError as exc:
        print(f"st7789_replay: {exc}", file=sys.stderr)
        return 1

    if args.out:
        panel.png(args.out)
    if args.summary:
        print(f"panel        {panel.width}x{panel.height}")
        print(f"resets       {panel.resets}")
        print(f"commands     {' '.join(f'{c:02X}' for c in panel.commands)}")
        print(f"colmod       {panel.colmod:#04x}" if panel.colmod is not None else "colmod       unset")
        print(f"madctl       {panel.madctl:#04x}" if panel.madctl is not None else "madctl       unset")
        print(f"ramwr        {panel.frames} payload(s), {panel.pixels_written} pixels")
        for x0, y0, x1, y1 in panel.windows:
            print(f"window       ({x0},{y0})-({x1},{y1})  "
                  f"{x1 - x0 + 1}x{y1 - y0 + 1}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

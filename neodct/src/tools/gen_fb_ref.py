"""Generate the framebuffer reference corpus for test_nd_fb.c.

The oracle here is the SHIPPED PYTHON ITSELF, not a transcription of it. The
`Framebuffer` class is lifted verbatim out of System/core/main.py and executed
against a fake mmap, so what the C is checked against is the real update()
with its real crop, centring and packing -- including the quirks (the slow
path's alpha-255 black, the data[:size] truncation) that a hand-written
expectation would quietly drop.

`__init__` is bypassed: it does two ioctls on /dev/fb0 and nothing else that
update() depends on. The four derived values it computes are recomputed here
with the same four lines, and that duplication is deliberate -- it is the only
part of the driver this generator restates rather than runs.

Source pixels come from synth(), the same integer hash tools/gen_image_ref.py
uses, so the C test needs no reference images.

Regenerate with:  python3 tools/gen_fb_ref.py > test/unit/fb_ref.inc
"""
import os
import re
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN_PY = os.path.join(HERE, "..", "..", "overlay", "NeoDCT", "System", "core", "main.py")

M32 = 0xFFFFFFFF


def synth(x, y, c):
    v = ((x * 2654435761) ^ (y * 40503) ^ (c * 97)) & M32
    v ^= v >> 13
    v = (v * 0x5BD1E995) & M32
    v ^= v >> 15
    return v & 0xFF


def make(mode, w, h):
    im = Image.new(mode, (w, h))
    bands = len(im.getbands())
    data = bytes(synth(x, y, c) for y in range(h) for x in range(w) for c in range(bands))
    return Image.frombytes(mode, (w, h), data)


def fnv(b):
    h = 0xCBF29CE484222325
    for x in b:
        h ^= x
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def load_framebuffer_class():
    """exec the Framebuffer class body out of main.py.

    Importing main.py outright drags in sqlite3, the modem stack and half the
    overlay; the class needs five stdlib names and Image.
    """
    with open(MAIN_PY) as fh:
        src = fh.read()
    m = re.search(r"^class Framebuffer:.*?(?=^\S)", src, re.S | re.M)
    if not m:
        raise SystemExit("could not find class Framebuffer in " + MAIN_PY)
    ns = {"Image": Image}
    exec(compile(m.group(0), MAIN_PY, "exec"), ns)  # noqa: S102 -- that is the point
    return ns["Framebuffer"]


class FakeMap:
    """Just enough of mmap for Framebuffer: seek() and write()."""

    def __init__(self, size):
        self.buf = bytearray(size)
        self.pos = 0

    def seek(self, off):
        self.pos = off

    def write(self, data):
        end = self.pos + len(data)
        if end > len(self.buf):
            raise ValueError("write past the end of the mapping")
        self.buf[self.pos:end] = data
        self.pos = end


def make_fb(cls, xres, yres, bpp, line_length=0):
    fb = cls.__new__(cls)
    fb.fd = -1
    fb.xres, fb.yres, fb.bpp = xres, yres, bpp

    # The four derived lines from __init__, restated. See the module docstring.
    fb.line_length = line_length or xres * (bpp // 8)
    fb.size = fb.line_length * yres
    fb.bytes_per_pixel = max(1, bpp // 8)
    fb.stride_pixels = fb.line_length // fb.bytes_per_pixel

    fb.mm = FakeMap(fb.size)
    fb.native_img = None
    fb._black = (0, 0, 0)

    # False on the shipped image: Pillow >= 11 dropped the "BGR;16" packer, so
    # the reference for 16bpp is the pure-Python packer, which is exactly the
    # byte order the C packer has to produce.
    fb._has_bgr16 = False
    fb._r565 = [(i & 0xF8) << 8 for i in range(256)]
    fb._g565 = [(i & 0xFC) << 3 for i in range(256)]
    fb._b565 = [i >> 3 for i in range(256)]
    fb._rgb565_out = bytearray(fb.size)
    fb._rgb565_band_out = bytearray(fb.xres * fb.yres * 2)
    return fb


CASES = []


def case(name, xres, yres, bpp, sw, sh, line_length=0, mode="RGB"):
    cls = load_framebuffer_class.cls
    fb = make_fb(cls, xres, yres, bpp, line_length)
    fb.update(make(mode, sw, sh))
    CASES.append((name, xres, yres, bpp, fb.line_length, sw, sh, mode,
                  len(fb.mm.buf), fnv(bytes(fb.mm.buf))))


def main():
    load_framebuffer_class.cls = load_framebuffer_class()

    # The live configuration: displayd has already switched the panel to
    # 240x175 @ 32bpp, so this is one contiguous 168,000-byte write.
    case("live_240x175_32", 240, 175, 32, 240, 175)

    # A genuine 240x240 panel: the band lands at row 32, and the letterbox
    # rows keep the zeros the initial clear put there.
    case("panel_240x240_32", 240, 240, 32, 240, 175)

    # Side padding as well as top padding: dst_x is non-zero, so the write
    # goes row by row and the margins stay black.
    case("wide_320x240_32", 320, 240, 32, 240, 175)

    # Row padding with no side padding: row_bytes != line_length, which is the
    # other way into the row-by-row branch.
    case("stride_pad_240x175_32", 240, 175, 32, 240, 175, line_length=240 * 4 + 16)

    # Source larger than the framebuffer in both axes: centre-cropped, and the
    # crop pushes it onto the slow path, where black is alpha-255.
    case("crop_240x175_from_320x240_32", 240, 175, 32, 320, 240)

    # Taller only: copy_h < src.height, so still the slow path.
    case("crop_240x175_from_240x240_32", 240, 175, 32, 240, 240)

    # An RGBA source: update() converts to RGB first, so the alpha channel is
    # dropped rather than composited.
    case("rgba_source_240x175_32", 240, 175, 32, 240, 175, mode="RGBA")

    # 16bpp, both paths.
    case("live_240x175_16", 240, 175, 16, 240, 175)
    case("panel_240x240_16", 240, 240, 16, 240, 175)
    case("crop_240x175_from_240x240_16", 240, 175, 16, 240, 240)

    # 24bpp: neither packer applies, so update() writes raw RGB through the
    # slow path. Nothing ships like this; it is here because the code has a
    # branch for it and an untested branch is a rumour.
    case("raw_240x175_24", 240, 175, 24, 240, 175)
    case("raw_panel_240x240_24", 240, 240, 24, 240, 175)

    # Odd sizes, to catch an off-by-one in the centring that a 240-wide case
    # cannot see: (7-5)//2 = 1, (9-4)//2 = 2.
    case("odd_7x9_32", 7, 9, 32, 5, 4)
    case("odd_7x9_16", 7, 9, 16, 5, 4)

    out = sys.stdout
    out.write("/* GENERATED -- do not edit by hand.\n"
              " *\n"
              " * Produced by running the real Framebuffer class out of\n"
              " * System/core/main.py against a fake mmap; see tools/gen_fb_ref.py.\n"
              " * Each entry is the FNV-1a 64 hash of the WHOLE mapping after one\n"
              " * update(), so a difference anywhere -- band, letterbox or row padding\n"
              " * -- fails the case.\n"
              " *\n"
              " * Regenerate with:  python3 tools/gen_fb_ref.py > test/unit/fb_ref.inc\n"
              " */\n\n")
    out.write("typedef struct {\n"
              "    const char *name;\n"
              "    int32_t xres;\n"
              "    int32_t yres;\n"
              "    int32_t bpp;\n"
              "    size_t line_length;\n"
              "    int32_t sw;\n"
              "    int32_t sh;\n"
              "    bool src_rgba;\n"
              "    size_t map_size;\n"
              "    uint64_t hash;\n"
              "} nd_fb_ref;\n\n")
    out.write("static const nd_fb_ref ND_FB_REF[] = {\n")
    for (name, xres, yres, bpp, ll, sw, sh, mode, size, h) in CASES:
        out.write('    {"%s", %d, %d, %d, %du, %d, %d, %s, %du, 0x%016xULL},\n'
                  % (name, xres, yres, bpp, ll, sw, sh,
                     "true" if mode == "RGBA" else "false", size, h))
    out.write("};\n")

    # A handful of single-pixel packings, quoted straight out of nd_fb.h's
    # header comment, so a packer that is wrong in an obvious way fails with an
    # obvious message rather than as a hash mismatch.
    out.write("\n/* (r,g,b) -> the four BGRA bytes and the two RGB565 bytes. */\n")
    out.write("typedef struct {\n"
              "    uint8_t rgb[3];\n"
              "    uint8_t bgra[4];\n"
              "    uint8_t rgb565[2];\n"
              "} nd_fb_px_ref;\n\n")
    out.write("static const nd_fb_px_ref ND_FB_PX_REF[] = {\n")
    for rgb in [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255),
                (0, 0, 0), (18, 52, 86), (1, 2, 3), (127, 128, 129)]:
        one = Image.new("RGB", (1, 1), rgb)
        bgra = one.convert("RGBA").tobytes("raw", "BGRA")
        r, g, b = rgb
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out.write("    {{%du, %du, %du}, {%du, %du, %du, %du}, {%du, %du}},\n"
                  % (r, g, b, bgra[0], bgra[1], bgra[2], bgra[3],
                     v & 0xFF, (v >> 8) & 0xFF))
    out.write("};\n")


if __name__ == "__main__":
    main()

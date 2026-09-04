"""neodct/src/recovery/nd_recfont.h is generated. Keep it honest.

The recovery UI draws in the phone's own typeface without freetype and
without the squashfs font.ttf lives on, by carrying a 1-bit ASCII bitmap of
it as a committed C header. That is the same arrangement fontref.py and
mkicons.py already have -- a developer-run PIL tool whose output is checked
in -- and it exists for the reason mkinitramfs.py's hand-rolled BMP reader
exists: nothing guarantees PIL is installed on a build host, and buildroot
invokes neodct/src/Makefile directly on whatever machine somebody has.

The failure mode a committed generated file has is drift: somebody changes
font.ttf, or the generator, and the header keeps saying what it said before.
Nothing else in the tree would notice -- the C compiles, the unit tests pass,
and the phone draws the old typeface. So this regenerates and byte-compares.

It skips without PIL rather than failing, which is the same latitude
test_fontref.py takes, and for the same reason: a build host is allowed not
to have it.
"""

import os
import subprocess
import sys

import pytest

pytest.importorskip("PIL")

NEODCT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(NEODCT, "tools")
GENERATOR = os.path.join(TOOLS, "mkrecfont.py")
HEADER = os.path.join(NEODCT, "src", "recovery", "nd_recfont.h")

if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)


def test_the_committed_header_is_what_the_generator_produces(tmp_path):
    regenerated = tmp_path / "nd_recfont.h"

    subprocess.run([sys.executable, GENERATOR, "--out", str(regenerated)],
                   check=True, capture_output=True)

    want = open(HEADER, "rb").read()
    got = regenerated.read_bytes()
    assert got == want, (
        "nd_recfont.h disagrees with font.ttf. Regenerate it:\n"
        "    python3 neodct/tools/mkrecfont.py"
    )


def test_it_carries_the_two_sizes_recovery_ships_and_no_others():
    """18 px for the title and the menu items, 14 px for the heading, the
    percentage, the byte reading and package filenames. Not 24: "NeoDCT
    recovery" is 258 px at 24 and the panel is 240 wide, so the framework's
    own title size is unavailable here regardless of budget."""
    text = open(HEADER).read()

    assert "nd_recfont_18" in text
    assert "nd_recfont_14" in text
    assert "nd_recfont_24" not in text
    assert "nd_recfont_20" not in text


def test_the_table_stays_inside_its_byte_budget():
    """spec-recovery-ui.md section 12: the whole binary is budgeted at 40 KiB
    stripped, of which this table is the incompressible part. One pre-rendered
    240x175 screen would be 168,000 bytes -- thirty-three times this -- and
    could not draw a filename read off an SD card anyway."""
    import mkrecfont

    total = 0
    for size in mkrecfont.SIZES:
        _, records, bits = mkrecfont.table(size)
        total += len(bits) + len(records) * 6

    assert total < 8 * 1024, total


def test_every_advance_is_a_whole_number_of_pixels():
    """The record stores an advance as a uint8_t. A fractional one would be
    silently truncated and every string past the first glyph would drift --
    which is exactly what test_recui.c's comparison against fontref.json's
    sum_of_advances would catch, one release too late."""
    import mkrecfont

    for size in mkrecfont.SIZES:
        font = mkrecfont.load(size)
        for code in range(mkrecfont.FIRST, mkrecfont.LAST + 1):
            advance = font.getlength(chr(code))
            assert advance == int(advance), (size, chr(code), advance)
            assert 0 <= advance <= 255, (size, chr(code), advance)


def test_the_ink_offset_is_zero_at_both_sizes():
    """ink_dx is not stored, because it is identically zero at every size this
    font is used at. If a font change broke that, the generator refuses rather
    than shifting one glyph inside its own cell -- this is the test that says
    so out loud."""
    from PIL import Image, ImageDraw

    import mkrecfont

    for size in mkrecfont.SIZES:
        font = mkrecfont.load(size)
        pad = size * 2
        for code in range(mkrecfont.FIRST, mkrecfont.LAST + 1):
            canvas = Image.new("L", (pad * 3, pad * 3), 0)
            ImageDraw.Draw(canvas).text((pad, pad), chr(code), font=font, fill=255)
            ink = canvas.getbbox()
            if ink is None:
                continue
            assert ink[0] - pad == 0, (size, chr(code), ink[0] - pad)

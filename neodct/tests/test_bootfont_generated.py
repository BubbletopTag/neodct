"""neodct/src/tools/nd_bootfont.c is generated, and has to stay generated.

The initramfs has no FreeType and no font.ttf -- roughly a megabyte of RAM on
a 64 MB device, unpacked from a cpio that on the Luckfox is built into the
kernel image -- so the boot screen draws from 1-bit tables rendered at build
time by tools/gen_bootfont.c, which links libneodct and therefore uses the
phone's own nd_font. The tables are committed because the initramfs build must
not depend on a host FreeType.

Committed generated code drifts. This re-runs the generator against the same
font.ttf and compares, exactly as neodct/tests/golden/font/fontref.json pins
the font renderer. A hand edit, a font change that nobody regenerated for, or
a generator change whose output was never committed all fail here.

What the tables MEAN -- that a string measures the same as nd_text_size()
reports, at all three sizes, and that the ladder picks the same rung -- is
neodct/src/test/unit/test_bootbar.c's business. This file only pins that the
file on disk is the file the generator produces.
"""

import os
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src")
GENERATOR = os.path.join(SRC, "build", "default", "bin", "gen-bootfont")
LIBDIR = os.path.join(SRC, "build", "default", "lib")
COMMITTED = os.path.join(SRC, "tools", "nd_bootfont.c")
FONT = os.path.join(REPO, "overlay", "NeoDCT", "System", "ui", "resources",
                    "fonts", "font.ttf")


def test_the_committed_tables_are_the_ones_the_generator_produces():
    if not os.path.exists(GENERATOR):
        pytest.skip("the C build is not present (make -C neodct/src)")

    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = LIBDIR + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    result = subprocess.run([GENERATOR, FONT], capture_output=True, env=env)

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout.decode() == open(COMMITTED).read(), (
        "neodct/src/tools/nd_bootfont.c is not what gen-bootfont produces from "
        "font.ttf. Regenerate it:\n"
        "  cd neodct/src && LD_LIBRARY_PATH=build/default/lib "
        "./build/default/bin/gen-bootfont "
        "../overlay/NeoDCT/System/ui/resources/fonts/font.ttf "
        "> tools/nd_bootfont.c")


def test_the_generated_file_says_it_is_generated():
    """So that the next person to open it does not edit it by hand."""
    head = open(COMMITTED).read(600)

    assert "GENERATED" in head
    assert "DO NOT EDIT" in head
    assert "gen_bootfont.c" in head


def test_the_generator_is_kept_out_of_the_format_target():
    """clang-format would fight the generator every time, and the file would
    then never match its own output again.

    Asserted as the PROPERTY -- the format target names this file as something
    it skips -- and not as one spelling of it. There are now two generated
    sources to exclude (nd_bootfont.c here, nd_recfont.h from the recovery UI)
    and find has more than one way to say it; `! -name X` and `-name X -prune`
    both work, and pinning the exact bytes of one of them made this test fail
    when the two exclusions were merged into a single find, for no reason a
    reader could act on."""
    makefile = open(os.path.join(SRC, "Makefile")).read()

    fmt = [l for l in makefile.splitlines() if "clang-format" in l or "nd_bootfont.c" in l]
    joined = "\n".join(fmt)
    assert "nd_bootfont.c" in joined, (
        "the format target does not mention nd_bootfont.c at all:\n" + joined)
    assert ("! -name 'nd_bootfont.c'" in joined
            or "-name nd_bootfont.c -prune" in joined
            or "-name 'nd_bootfont.c' -prune" in joined), (
        "nd_bootfont.c is named near clang-format but not as an exclusion:\n" + joined)


def test_the_generator_is_never_installed():
    """gen-bootfont links libneodct and runs on the build host. Nothing on
    the phone has any use for it, and the initramfs least of all."""
    makefile = open(os.path.join(SRC, "Makefile")).read()

    for line in makefile.splitlines():
        if "INSTALL" in line and "gen-bootfont" in line:
            pytest.fail("gen-bootfont is installed by: %s" % line.strip())

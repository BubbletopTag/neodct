"""neodct/tools/mknap.py -- the producer side of the .nap format.

The phone's reader is `neodct/src/lib/nd_nap.c` and its rules are in
`nd_nap.h`; this tool exists so a package follows them exactly rather than
approximately. What is pinned here is the half of that contract a C test
cannot see: which arch tags the tool will BUILD for, and which of the two
layouts it produces for one --so and for several.

The tag set is the reason this file exists at all. DECISIONS.md D1 gave the
emulator the phone's armv7 ABI, so one app.so now serves both machines and
`qemu-aarch64` names no machine either of them has. A warning there was what
let the two aarch64 Bible packages be built in the first place -- mknap said
something, the build carried on, and the result was an archive whose app.so
nothing could dlopen. It is a refusal now, and a refusal nothing exercises is
a refusal that comes back as a warning.
"""

import io
import json
import os
import subprocess
import sys
import tarfile

import pytest

TOOLS_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"
)
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import mknap

MKNAP = os.path.join(TOOLS_DIR, "mknap.py")

# nd_nap.h's ND_NAP_ARCH_RETIRED_QEMU_AARCH64. Spelled out rather than read
# out of mknap, so that emptying RETIRED_ARCH_TAGS fails here instead of
# quietly making every assertion below vacuous.
RETIRED = "qemu-aarch64"


def app_dir(tmp_path):
    d = tmp_path / "App"
    d.mkdir()
    (d / "manifest.json").write_text(
        json.dumps({"name": "Demo App", "id": 130, "icon": "icon.png"}) + "\n"
    )
    (d / "icon.png").write_bytes(b"PNG")
    return d


# (e_machine, 64-bit, little-endian, e_flags). Real values, because mknap now
# reads all four and a stand-in that carried only the magic would exercise the
# check it replaced rather than the one that is there.
ARMV7_HARD = (0x28, False, True, 0x5000400)     # EABI5 + EF_ARM_ABI_FLOAT_HARD
ARMV7_SOFT = (0x28, False, True, 0x5000200)     # ...FLOAT_SOFT: the near miss
AARCH64 = (0xb7, True, True, 0)
X86_64 = (0x3e, True, True, 0)


def elf(tmp_path, name, abi=ARMV7_HARD):
    """A file with a genuine ELF header for one ABI.

    The tag says what a package CLAIMS to be; this is what it IS, and since
    one armv7 tag now serves both machines the header is the only thing left
    that can tell them apart.
    """
    machine, is64, little, flags = abi
    order = "little" if little else "big"
    head = bytearray(64)
    head[0:4] = b"\x7fELF"
    head[4] = 2 if is64 else 1                  # EI_CLASS
    head[5] = 1 if little else 2                # EI_DATA
    head[6] = 1                                 # EI_VERSION
    head[16:18] = (3).to_bytes(2, order)        # ET_DYN
    head[18:20] = machine.to_bytes(2, order)
    off = 48 if is64 else 36                    # e_flags moves with the class
    head[off:off + 4] = flags.to_bytes(4, order)
    p = tmp_path / name
    p.write_bytes(bytes(head))
    return p


def run(tmp_path, *args):
    return subprocess.run(
        [sys.executable, MKNAP] + list(args),
        cwd=str(tmp_path),
        capture_output=True,
        text=True,
    )


def test_the_retired_tag_is_not_one_the_tool_builds_for():
    assert RETIRED not in mknap.ARCH_TAGS
    assert RETIRED in mknap.RETIRED_ARCH_TAGS
    # luckfox-armv7 now names an ABI shared by both machines and must not be
    # renamed: it is the "arch" key in every manifest ever written and the
    # lib/<tag>/ path in every universal package.
    assert "luckfox-armv7" in mknap.ARCH_TAGS
    assert "host-x86_64" in mknap.ARCH_TAGS


def test_a_retired_tag_is_refused_and_the_message_says_why(tmp_path):
    d = app_dir(tmp_path)
    so = elf(tmp_path, "old.so")

    r = run(tmp_path, "--app-dir", str(d), "--so", "%s=%s" % (RETIRED, so),
            "-o", "old.nap")

    assert r.returncode != 0
    # Named, and with the remedy in the same line: a hard failure with no
    # explanation is worse than the old silence, because the author's next
    # move is to delete the tag rather than to rebuild once for both.
    assert RETIRED in r.stderr
    assert "no longer built" in r.stderr
    assert "serves both" in r.stderr
    # Refused means nothing was written, the same rule nd_nap_install() obeys.
    assert not (tmp_path / "old.nap").exists()


def test_a_retired_tag_is_refused_even_beside_a_live_one(tmp_path):
    """The shape a third-party build script actually has: it was producing a
    universal package and one of its targets has gone. Half a package is not
    an improvement on none."""
    d = app_dir(tmp_path)
    good = elf(tmp_path, "a.so")
    old = elf(tmp_path, "b.so")

    r = run(tmp_path, "--app-dir", str(d),
            "--so", "luckfox-armv7=%s" % good,
            "--so", "%s=%s" % (RETIRED, old),
            "-o", "two.nap")

    assert r.returncode != 0
    assert not (tmp_path / "two.nap").exists()


def test_one_so_writes_app_so_at_the_root_and_names_the_arch(tmp_path):
    d = app_dir(tmp_path)
    so = elf(tmp_path, "a.so")

    r = run(tmp_path, "--app-dir", str(d), "--so", "luckfox-armv7=%s" % so,
            "-o", "one.nap")
    assert r.returncode == 0, r.stderr

    with tarfile.open(str(tmp_path / "one.nap")) as tar:
        names = tar.getnames()
        doc = json.loads(tar.extractfile("manifest.json").read().decode())
    assert "app.so" in names
    assert not any(n.startswith("lib/") for n in names)
    # app.so at the root is meaningless without a tag saying which machine it
    # is for; nd_nap.c refuses a package that has one and not the other.
    assert doc["arch"] == "luckfox-armv7"


def test_two_sos_write_lib_tag_app_so_and_no_arch_key(tmp_path):
    d = app_dir(tmp_path)
    a = elf(tmp_path, "a.so")
    b = elf(tmp_path, "b.so", X86_64)

    r = run(tmp_path, "--app-dir", str(d),
            "--so", "luckfox-armv7=%s" % a,
            "--so", "host-x86_64=%s" % b,
            "-o", "two.nap")
    assert r.returncode == 0, r.stderr

    with tarfile.open(str(tmp_path / "two.nap")) as tar:
        names = tar.getnames()
        doc = json.loads(tar.extractfile("manifest.json").read().decode())
    assert "lib/luckfox-armv7/app.so" in names
    assert "lib/host-x86_64/app.so" in names
    assert "app.so" not in names
    # The phone picks its own at install time and never installs lib/, so an
    # "arch" key here would be a second, contradicting answer.
    assert "arch" not in doc


def test_the_tag_cannot_be_used_to_rename_an_aarch64_app_so(tmp_path):
    """The refusal above guards the TAG. This guards the FILE, and it has to,
    because one armv7 tag now serves both machines -- so the retirement
    message reads as an instruction to pass `luckfox-armv7`, and the tree
    still carries an aarch64 app.so next to its armv7 sibling. Renaming used
    to produce a package the phone installed and could not dlopen."""
    d = app_dir(tmp_path)
    so = elf(tmp_path, "wrong.so", AARCH64)

    r = run(tmp_path, "--app-dir", str(d), "--so", "luckfox-armv7=%s" % so,
            "-o", "renamed.nap")

    assert r.returncode != 0
    assert "aarch64" in r.stderr
    assert "dlopen" in r.stderr
    assert not (tmp_path / "renamed.nap").exists()


def test_a_soft_float_armv7_app_so_is_refused_too(tmp_path):
    """Same machine, same class, same endianness. EF_ARM_ABI_FLOAT_HARD is
    the only thing in the header separating this ABI from one whose objects
    the phone's loader cannot use."""
    d = app_dir(tmp_path)
    so = elf(tmp_path, "soft.so", ARMV7_SOFT)

    r = run(tmp_path, "--app-dir", str(d), "--so", "luckfox-armv7=%s" % so,
            "-o", "soft.nap")

    assert r.returncode != 0
    assert "hard-float" in r.stderr
    assert not (tmp_path / "soft.nap").exists()


def test_a_host_so_under_the_phone_tag_is_refused(tmp_path):
    """The everyday version of the same mistake: the build script picked up
    the host build sitting next to the cross one."""
    d = app_dir(tmp_path)
    so = elf(tmp_path, "host.so", X86_64)

    r = run(tmp_path, "--app-dir", str(d), "--so", "luckfox-armv7=%s" % so,
            "-o", "host.nap")

    assert r.returncode != 0
    assert "x86-64" in r.stderr


def test_list_reports_a_retired_package_as_a_problem(tmp_path):
    """--list is documented as saying whether the phone would accept it, and
    it is the check made BEFORE the card trip. It used to print the retired
    tag and exit 0, so the tool said a dead package was fine and the phone
    said it was not."""
    nap = tmp_path / "old.nap"
    doc = {"name": "Demo App", "id": 130, "icon": "icon.png", "arch": RETIRED}
    with tarfile.open(str(nap), "w", format=tarfile.USTAR_FORMAT) as tar:
        for name, data in (("manifest.json",
                            (json.dumps(doc) + "\n").encode()),
                           ("app.so", elf(tmp_path, "x.so").read_bytes()),
                           ("icon.png", b"PNG")):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o644
            info.mtime = 0
            tar.addfile(info, io.BytesIO(data))

    r = run(tmp_path, "--list", str(nap))

    assert r.returncode != 0, r.stdout
    assert "PROBLEM" in r.stdout
    assert RETIRED in r.stdout


def test_list_is_happy_with_a_live_package(tmp_path):
    """The other half: the retired-tag report must not make every --list
    non-zero, or it stops being read."""
    d = app_dir(tmp_path)
    so = elf(tmp_path, "a.so")
    assert run(tmp_path, "--app-dir", str(d), "--so", "luckfox-armv7=%s" % so,
               "-o", "one.nap").returncode == 0

    r = run(tmp_path, "--list", "one.nap")

    assert r.returncode == 0, r.stdout
    assert "PROBLEM" not in r.stdout

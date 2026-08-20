"""Building the boot-time applier's initramfs.

The initramfs has to hold busybox and dmsetup from the *target* tree plus
exactly the shared libraries they need -- copying all of target/lib would
be 14MB of cpio on a phone with 64MB of RAM. So the builder resolves
DT_NEEDED itself rather than shelling out to a cross-ldd that buildroot
does not ship.

The ELF parsing is arch-agnostic, so these tests read host binaries.
"""

import os
import shutil
import subprocess
import sys

import pytest

TOOLS_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"
)
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import mkinitramfs

HOST_BINARY = "/usr/bin/ls" if os.path.exists("/usr/bin/ls") else "/bin/ls"
HOST_LIB_DIRS = [d for d in ("/usr/lib", "/lib", "/lib64", "/usr/lib64")
                 if os.path.isdir(d)]


def test_reads_the_libraries_a_binary_asks_for():
    needed, interpreter = mkinitramfs.elf_needed(HOST_BINARY)

    assert any(name.startswith("libc.so") for name in needed), needed
    assert interpreter and "ld-" in os.path.basename(interpreter)


def test_a_static_or_non_elf_file_has_no_dependencies(tmp_path):
    script = tmp_path / "init"
    script.write_text("#!/bin/sh\nexit 0\n")

    with pytest.raises(ValueError, match="ELF"):
        mkinitramfs.elf_needed(script)


def test_resolves_the_whole_dependency_closure():
    """libc pulls in its own dependencies; all of them have to come along."""
    resolved = mkinitramfs.resolve_libs([HOST_BINARY], HOST_LIB_DIRS)

    names = {os.path.basename(p) for p in resolved.values()}
    assert any(n.startswith("libc.so") for n in names), names
    for source in resolved.values():
        assert os.path.exists(source)


def test_an_unresolvable_library_is_an_error_not_a_silent_omission(tmp_path):
    """A missing .so means an initramfs that panics at boot instead."""
    with pytest.raises(mkinitramfs.MissingLibrary, match="libc"):
        mkinitramfs.resolve_libs([HOST_BINARY], [str(tmp_path)])


def test_packs_a_cpio_containing_init_and_the_binaries(tmp_path):
    """End to end: a cpio.gz whose table of contents has what boot needs."""
    fake_target = tmp_path / "target"
    (fake_target / "bin").mkdir(parents=True)
    (fake_target / "usr" / "lib").mkdir(parents=True)
    shutil.copy(HOST_BINARY, fake_target / "bin" / "busybox")
    for source in mkinitramfs.resolve_libs([HOST_BINARY], HOST_LIB_DIRS).values():
        shutil.copy(source, fake_target / "usr" / "lib" / os.path.basename(source))
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\nexit 0\n")
    out = tmp_path / "initramfs.cpio.gz"

    mkinitramfs.build(fake_target, init, out, extra_binaries=[])

    raw = subprocess.run(["gzip", "-dc", str(out)], capture_output=True,
                         check=True).stdout
    listing = subprocess.run(["cpio", "-it", "--quiet"], input=raw,
                             capture_output=True, check=True)
    names = listing.stdout.decode().split()

    assert "init" in names
    assert "bin/busybox" in names
    assert any(n.startswith("lib") and "libc.so" in n for n in names), names
    assert os.path.getsize(out) > 0


def test_the_init_script_is_executable_in_the_image(tmp_path):
    fake_target = tmp_path / "target"
    (fake_target / "bin").mkdir(parents=True)
    shutil.copy(HOST_BINARY, fake_target / "bin" / "busybox")
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\nexit 0\n")
    out = tmp_path / "initramfs.cpio.gz"

    mkinitramfs.build(fake_target, init, out, extra_binaries=[],
                      lib_dirs=HOST_LIB_DIRS)

    staged = mkinitramfs.LAST_STAGING
    assert os.access(os.path.join(staged, "init"), os.X_OK)


def test_missing_busybox_is_reported_clearly(tmp_path):
    fake_target = tmp_path / "target"
    fake_target.mkdir()
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")

    with pytest.raises(SystemExit, match="busybox"):
        mkinitramfs.build(fake_target, init, tmp_path / "out.gz",
                          extra_binaries=[])


def test_dmsetup_is_found_where_buildroot_actually_installs_it(tmp_path):
    """lvm2 configures with --exec-prefix=/usr, so dmsetup lands in
    /usr/sbin. Missing it would build an image that panics at boot under
    verity=enforce, so it has to be looked for in every plausible place and
    normalised to one path inside the initramfs."""
    fake_target = tmp_path / "target"
    (fake_target / "bin").mkdir(parents=True)
    (fake_target / "usr" / "sbin").mkdir(parents=True)
    (fake_target / "usr" / "lib").mkdir(parents=True)
    shutil.copy(HOST_BINARY, fake_target / "bin" / "busybox")
    shutil.copy(HOST_BINARY, fake_target / "usr" / "sbin" / "dmsetup")
    for source in mkinitramfs.resolve_libs([HOST_BINARY], HOST_LIB_DIRS).values():
        shutil.copy(source, fake_target / "usr" / "lib" / os.path.basename(source))
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")
    out = tmp_path / "initramfs.cpio.gz"

    mkinitramfs.build(fake_target, init, out)

    raw = subprocess.run(["gzip", "-dc", str(out)], capture_output=True,
                         check=True).stdout
    names = subprocess.run(["cpio", "-it", "--quiet"], input=raw,
                           capture_output=True, check=True).stdout.decode().split()
    assert "sbin/dmsetup" in names


def test_a_missing_dmsetup_fails_the_build(tmp_path):
    """Better a build error than an image that drops to a rescue shell."""
    fake_target = tmp_path / "target"
    (fake_target / "bin").mkdir(parents=True)
    (fake_target / "usr" / "lib").mkdir(parents=True)
    shutil.copy(HOST_BINARY, fake_target / "bin" / "busybox")
    for source in mkinitramfs.resolve_libs([HOST_BINARY], HOST_LIB_DIRS).values():
        shutil.copy(source, fake_target / "usr" / "lib" / os.path.basename(source))
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")

    with pytest.raises(SystemExit, match="dmsetup"):
        mkinitramfs.build(fake_target, init, tmp_path / "out.gz")


def test_the_library_search_aliases_the_loader_needs_are_present(tmp_path):
    """glibc's built-in search path here is /lib64 and /usr/lib64, and an
    initramfs has no /etc/ld.so.cache -- so libraries sitting in /lib are
    invisible to the loader without these aliases. Without them boot dies at
    `/bin/sh: error while loading shared libraries`, which is exactly what
    happened on the first real boot.

    The target itself solves this with lib64 -> lib symlinks; mirror them so
    every directory the loader might try lands on the one real lib dir.
    """
    fake_target = tmp_path / "target"
    (fake_target / "bin").mkdir(parents=True)
    (fake_target / "usr" / "lib").mkdir(parents=True)
    (fake_target / "usr" / "sbin").mkdir(parents=True)
    shutil.copy(HOST_BINARY, fake_target / "bin" / "busybox")
    shutil.copy(HOST_BINARY, fake_target / "usr" / "sbin" / "dmsetup")
    for source in mkinitramfs.resolve_libs([HOST_BINARY], HOST_LIB_DIRS).values():
        shutil.copy(source, fake_target / "usr" / "lib" / os.path.basename(source))
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")

    mkinitramfs.build(fake_target, init, tmp_path / "initramfs.cpio.gz")

    staged = mkinitramfs.LAST_STAGING
    real_lib = os.path.realpath(os.path.join(staged, "lib"))
    for alias in ("lib64", "usr/lib", "usr/lib64"):
        path = os.path.join(staged, alias)
        assert os.path.islink(path), "%s should be a symlink" % alias
        assert os.path.realpath(path) == real_lib, alias


def test_the_aliases_survive_into_the_archive(tmp_path):
    """cpio has to carry them as symlinks, not skip them."""
    fake_target = tmp_path / "target"
    (fake_target / "bin").mkdir(parents=True)
    (fake_target / "usr" / "lib").mkdir(parents=True)
    (fake_target / "usr" / "sbin").mkdir(parents=True)
    shutil.copy(HOST_BINARY, fake_target / "bin" / "busybox")
    shutil.copy(HOST_BINARY, fake_target / "usr" / "sbin" / "dmsetup")
    for source in mkinitramfs.resolve_libs([HOST_BINARY], HOST_LIB_DIRS).values():
        shutil.copy(source, fake_target / "usr" / "lib" / os.path.basename(source))
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")
    out = tmp_path / "initramfs.cpio.gz"

    mkinitramfs.build(fake_target, init, out)

    raw = subprocess.run(["gzip", "-dc", str(out)], capture_output=True,
                         check=True).stdout
    names = subprocess.run(["cpio", "-it", "--quiet"], input=raw,
                           capture_output=True, check=True).stdout.decode().split()
    assert "lib64" in names
    assert "usr/lib64" in names


# --- recovery panel: the ST7789 daemon and the splash ---

def _bmp24(width, height, pixels, bottom_up=True):
    """Smallest legal 24-bit BMP. pixels is [(r,g,b), ...] in top-down order."""
    stride = (width * 3 + 3) & ~3
    rows = []
    for y in range(height):
        source_y = (height - 1 - y) if bottom_up else y
        row = bytearray()
        for x in range(width):
            r, g, b = pixels[source_y * width + x]
            row += bytes((b, g, r))
        row += b"\x00" * (stride - width * 3)
        rows.append(bytes(row))
    body = b"".join(rows)
    offset = 54
    header = b"BM" + (offset + len(body)).to_bytes(4, "little") + b"\x00" * 4
    header += offset.to_bytes(4, "little")
    header += (40).to_bytes(4, "little")
    header += width.to_bytes(4, "little", signed=True)
    header += (height if bottom_up else -height).to_bytes(4, "little", signed=True)
    header += (1).to_bytes(2, "little") + (24).to_bytes(2, "little")
    header += b"\x00" * 24
    return header + body


def test_the_splash_is_converted_to_the_byte_order_the_daemon_reads(tmp_path):
    """neodctDisplay.c reads 32bpp as XRGB8888 -- bytes B,G,R,X. Getting
    this backwards is a red/blue swap nobody notices until hardware."""
    bmp = tmp_path / "sad.bmp"
    bmp.write_bytes(_bmp24(2, 1, [(255, 0, 0), (0, 0, 255)]))

    raw = mkinitramfs.bmp_to_xrgb8888(bmp, 2, 1)

    assert raw[0:4] == bytes((0, 0, 255, 0))    # red  -> B=0 G=0 R=255
    assert raw[4:8] == bytes((255, 0, 0, 0))    # blue -> B=255 G=0 R=0


def test_the_splash_is_emitted_top_down_whichever_way_the_bmp_stores_it(tmp_path):
    """BMP is bottom-up by default; a framebuffer is not. A flipped sad
    face is the kind of thing that only shows up on the phone."""
    top, bottom = (255, 255, 255), (0, 0, 0)
    pixels = [top, bottom]

    up_path = tmp_path / "up.bmp"
    down_path = tmp_path / "down.bmp"
    up_path.write_bytes(_bmp24(1, 2, pixels, bottom_up=True))
    down_path.write_bytes(_bmp24(1, 2, pixels, bottom_up=False))

    up = mkinitramfs.bmp_to_xrgb8888(up_path, 1, 2)
    down = mkinitramfs.bmp_to_xrgb8888(down_path, 1, 2)

    assert up == down
    assert up[0:3] == bytes((255, 255, 255))


def test_a_splash_of_the_wrong_size_fails_the_build(tmp_path):
    bmp = tmp_path / "sad.bmp"
    bmp.write_bytes(_bmp24(2, 2, [(0, 0, 0)] * 4))

    with pytest.raises(ValueError, match="expected"):
        mkinitramfs.bmp_to_xrgb8888(bmp, 240, 175)


def _panel_tree(tmp_path, daemon_source):
    fake_target = tmp_path / "target"
    (fake_target / "bin").mkdir(parents=True)
    (fake_target / "usr" / "sbin").mkdir(parents=True)
    (fake_target / "usr" / "lib").mkdir(parents=True)
    shutil.copy(HOST_BINARY, fake_target / "bin" / "busybox")
    shutil.copy(HOST_BINARY, fake_target / "usr" / "sbin" / "dmsetup")
    panel = fake_target / mkinitramfs.PANEL_DAEMON
    panel.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(daemon_source, panel)
    for source in mkinitramfs.resolve_libs([HOST_BINARY], HOST_LIB_DIRS).values():
        shutil.copy(source, fake_target / "usr" / "lib" / os.path.basename(source))
    return fake_target


def _names_in(out):
    raw = subprocess.run(["gzip", "-dc", str(out)], capture_output=True,
                         check=True).stdout
    listing = subprocess.run(["cpio", "-it", "--quiet"], input=raw,
                             capture_output=True, check=True)
    return listing.stdout.decode().split()


def test_the_panel_daemon_ships_when_it_matches_the_target(tmp_path):
    fake_target = _panel_tree(tmp_path, HOST_BINARY)
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")
    out = tmp_path / "out.gz"

    mkinitramfs.build(fake_target, init, out)

    assert "bin/neodct_displayd" in _names_in(out)


def test_a_daemon_of_another_architecture_is_left_out(tmp_path):
    """The daemon is a prebuilt ARM binary carried in the overlay, so it is
    present in the aarch64 QEMU tree too -- where shipping it would put an
    unrunnable binary in the image."""
    alien = tmp_path / "alien"
    data = bytearray(open(HOST_BINARY, "rb").read(64))
    data[18:20] = (0x28).to_bytes(2, "little")      # EM_ARM
    alien.write_bytes(bytes(data))
    fake_target = _panel_tree(tmp_path, alien)
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")
    out = tmp_path / "out.gz"

    mkinitramfs.build(fake_target, init, out)

    assert "bin/neodct_displayd" not in _names_in(out)


def test_the_splash_lands_in_the_image_but_the_bitmap_does_not(tmp_path):
    fake_target = _panel_tree(tmp_path, HOST_BINARY)
    init_dir = tmp_path / "initdir"
    (init_dir / "splash").mkdir(parents=True)
    (init_dir / "init").write_text("#!/bin/sh\n")
    (init_dir / "splash" / "sadface.bmp").write_bytes(
        _bmp24(mkinitramfs.SPLASH_W, mkinitramfs.SPLASH_H,
               [(0, 0, 0)] * (mkinitramfs.SPLASH_W * mkinitramfs.SPLASH_H)))
    out = tmp_path / "out.gz"

    mkinitramfs.build(fake_target, init_dir, out)

    names = _names_in(out)
    assert "splash.raw" in names
    assert not any(n.endswith(".bmp") for n in names), names

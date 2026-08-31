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


def _host_lib_dirs():
    """Where this machine keeps its shared libraries.

    resolve_libs() searches a flat list, which is exactly right for the
    target tree -- buildroot puts everything in lib/ and usr/lib/. A Debian
    or Ubuntu build host does not: libc lives in /usr/lib/<triplet>/, so the
    four fixed directories below find nothing and every test that resolves a
    host binary fails for a reason that has nothing to do with the code.
    Ask the machine instead of guessing.
    """
    dirs = [d for d in ("/usr/lib", "/lib", "/lib64", "/usr/lib64")
            if os.path.isdir(d)]
    for base in ("/usr/lib", "/lib"):
        if not os.path.isdir(base):
            continue
        for entry in sorted(os.listdir(base)):
            path = os.path.join(base, entry)
            if "-linux-" in entry and os.path.isdir(path):
                dirs.append(path)
    return dirs


HOST_LIB_DIRS = _host_lib_dirs()


def boot_requirements(fake_target):
    """Drop in the two files a real build path now insists on.

    nd-verify and the release public key: without them the initramfs cannot
    check an update signature and would install anything staged for it, so
    mkinitramfs refuses to build one, exactly as it refuses without dmsetup.
    Tests that are about something else still have to satisfy it.
    """
    verifier = fake_target / "usr" / "bin" / "nd-verify"
    verifier.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(HOST_BINARY, verifier)
    key = fake_target / "NeoDCT" / "System" / "keys" / "neodct-release.pub"
    key.parent.mkdir(parents=True, exist_ok=True)
    key.write_text("-----BEGIN PUBLIC KEY-----\nnot a real key\n"
                   "-----END PUBLIC KEY-----\n")
    return verifier, key


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
    """A missing .so means an initramfs that panics at boot instead.

    Which library is named depends on the host binary's DT_NEEDED order, so
    the assertion is that it names one of them -- not that it names libc.
    """
    needed, _ = mkinitramfs.elf_needed(HOST_BINARY)

    with pytest.raises(mkinitramfs.MissingLibrary) as caught:
        mkinitramfs.resolve_libs([HOST_BINARY], [str(tmp_path)])

    assert any(name in str(caught.value) for name in needed), caught.value


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
    boot_requirements(fake_target)
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
    boot_requirements(fake_target)
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
    boot_requirements(fake_target)
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
    boot_requirements(fake_target)
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


# --- the update signature check ------------------------------------------
#
# SECURITY-AUDIT.md section 3: the applier writes a staged image to the
# system partition, and before this it checked no signature. The check lives
# in the initramfs because that is the one link in the chain a running system
# cannot rewrite -- it is built into the kernel image. Which means the
# verifier and the key have to actually be IN the cpio, and an initramfs
# missing either of them is not a slightly worse initramfs, it is one that
# installs whatever it is handed.

def minimal_target(tmp_path, dmsetup=True):
    fake_target = tmp_path / "target"
    (fake_target / "bin").mkdir(parents=True)
    (fake_target / "usr" / "lib").mkdir(parents=True)
    shutil.copy(HOST_BINARY, fake_target / "bin" / "busybox")
    if dmsetup:
        (fake_target / "usr" / "sbin").mkdir(parents=True)
        shutil.copy(HOST_BINARY, fake_target / "usr" / "sbin" / "dmsetup")
    for source in mkinitramfs.resolve_libs([HOST_BINARY], HOST_LIB_DIRS).values():
        shutil.copy(source, fake_target / "usr" / "lib" / os.path.basename(source))
    return fake_target


def archive_names(out):
    raw = subprocess.run(["gzip", "-dc", str(out)], capture_output=True,
                         check=True).stdout
    return subprocess.run(["cpio", "-it", "--quiet"], input=raw,
                          capture_output=True, check=True).stdout.decode().split()


def test_the_verifier_and_the_key_are_both_in_the_image(tmp_path):
    fake_target = minimal_target(tmp_path)
    boot_requirements(fake_target)
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")
    out = tmp_path / "initramfs.cpio.gz"

    mkinitramfs.build(fake_target, init, out)

    names = archive_names(out)
    assert "bin/nd-verify" in names, names
    assert "neodct-release.pub" in names, names


def test_the_key_in_the_image_is_the_key_the_phone_verifies_against(tmp_path):
    """One file, taken from the target tree rather than committed twice. An
    initramfs whose key had drifted from the rootfs's would refuse every
    genuine update, and nothing would say why."""
    fake_target = minimal_target(tmp_path)
    _, key = boot_requirements(fake_target)
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")

    mkinitramfs.build(fake_target, init, tmp_path / "out.gz")

    staged = os.path.join(mkinitramfs.LAST_STAGING, "neodct-release.pub")
    assert open(staged, "rb").read() == key.read_bytes()


def test_the_key_is_not_made_executable(tmp_path):
    """It goes in as a plain file: it is not a binary and nothing resolves
    DT_NEEDED against it."""
    fake_target = minimal_target(tmp_path)
    boot_requirements(fake_target)
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")

    mkinitramfs.build(fake_target, init, tmp_path / "out.gz")

    staged = os.path.join(mkinitramfs.LAST_STAGING, "neodct-release.pub")
    assert not os.access(staged, os.X_OK)


def test_a_missing_verifier_fails_the_build(tmp_path):
    """Not a warning. An initramfs without nd-verify cannot tell a release
    image from one somebody staged, and it would apply either."""
    fake_target = minimal_target(tmp_path)
    key = fake_target / "NeoDCT" / "System" / "keys" / "neodct-release.pub"
    key.parent.mkdir(parents=True)
    key.write_text("x\n")
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")

    with pytest.raises(SystemExit, match="nd-verify"):
        mkinitramfs.build(fake_target, init, tmp_path / "out.gz")


def test_a_missing_release_key_fails_the_build(tmp_path):
    fake_target = minimal_target(tmp_path)
    verifier = fake_target / "usr" / "bin" / "nd-verify"
    verifier.parent.mkdir(parents=True)
    shutil.copy(HOST_BINARY, verifier)
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")

    with pytest.raises(SystemExit, match="neodct-release.pub"):
        mkinitramfs.build(fake_target, init, tmp_path / "out.gz")


def test_the_verifier_can_come_from_outside_the_target_tree(tmp_path):
    """It does: neodct.mk installs it into BINARIES_DIR, not into the rootfs,
    because it is 4 MB of statically linked OpenSSL that nothing in the
    running system calls. post-image-neodct.sh passes the path."""
    fake_target = minimal_target(tmp_path)
    key = fake_target / "NeoDCT" / "System" / "keys" / "neodct-release.pub"
    key.parent.mkdir(parents=True)
    key.write_text("x\n")
    elsewhere = tmp_path / "images" / "nd-verify"
    elsewhere.parent.mkdir()
    shutil.copy(HOST_BINARY, elsewhere)
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")
    out = tmp_path / "out.gz"

    mkinitramfs.build(fake_target, init, out, verifier=str(elsewhere))

    assert "bin/nd-verify" in archive_names(out)


def test_a_verifier_path_that_does_not_exist_is_an_error(tmp_path):
    """Silently falling back to the target tree would produce an image whose
    verifier is whatever happened to be lying around."""
    fake_target = minimal_target(tmp_path)
    key = fake_target / "NeoDCT" / "System" / "keys" / "neodct-release.pub"
    key.parent.mkdir(parents=True)
    key.write_text("x\n")
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")

    with pytest.raises(SystemExit, match="nd-verify"):
        mkinitramfs.build(fake_target, init, tmp_path / "out.gz",
                          verifier=str(tmp_path / "not-here"))


def test_the_post_image_hook_passes_the_verifier():
    """The build wiring, not the script: if post-image stops passing it, the
    build fails rather than shipping an unchecked initramfs -- but it should
    not fail, so pin the argument."""
    hook = open(os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "scripts", "post-image-neodct.sh")).read()

    assert "--verifier" in hook
    assert "nd-verify" in hook


# --- the on-screen recovery UI -------------------------------------------
#
# nd-recui is what makes the sixteen keys reach the recovery menu: they are on
# a PCF8575 that no kernel driver binds, so without it a phone shows a menu
# nobody can move. It is nevertheless OPTIONAL here, on the panel-daemon
# precedent rather than the nd-verify one -- ndsys-recovery.sh falls back to
# the text menu, which is today's behaviour, so its absence is a regression to
# the status quo rather than a broken image.

def _recui_tree(tmp_path, recui_source):
    fake_target = minimal_target(tmp_path)
    boot_requirements(fake_target)
    recui = fake_target / "usr" / "bin" / "nd-recui"
    recui.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(recui_source, recui)
    return fake_target


def test_the_recovery_ui_ships_when_it_matches_the_target(tmp_path):
    fake_target = _recui_tree(tmp_path, HOST_BINARY)
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")
    out = tmp_path / "out.gz"

    mkinitramfs.build(fake_target, init, out)

    assert "bin/nd-recui" in archive_names(out)


def test_a_recovery_ui_of_another_architecture_is_left_out(tmp_path):
    """install-boot writes into BINARIES_DIR, which is not architecture-tagged,
    so a stale cross build left over from another board is a real way to ship a
    binary the kernel cannot exec. This one is reached from the screen a person
    is standing in front of, where "nothing happened" is the whole failure
    report."""
    alien = tmp_path / "alien"
    data = bytearray(open(HOST_BINARY, "rb").read(64))
    data[18:20] = (0x28).to_bytes(2, "little")      # EM_ARM
    alien.write_bytes(bytes(data))
    fake_target = _recui_tree(tmp_path, alien)
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")
    out = tmp_path / "out.gz"

    mkinitramfs.build(fake_target, init, out)

    assert "bin/nd-recui" not in archive_names(out)


def test_a_missing_recovery_ui_does_not_fail_the_build(tmp_path, capsys):
    """The whole difference from nd-verify. An initramfs without a signature
    check installs whatever it is handed; an initramfs without a nicer menu
    still rescues a phone."""
    fake_target = minimal_target(tmp_path)
    boot_requirements(fake_target)
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")
    out = tmp_path / "out.gz"

    mkinitramfs.build(fake_target, init, out)

    assert "bin/nd-recui" not in archive_names(out)
    assert "nd-recui" in capsys.readouterr().err


def test_a_recovery_ui_path_that_does_not_exist_is_a_warning_not_an_error(tmp_path, capsys):
    """post-image passes --recui unconditionally, and a tree built before this
    existed has no nd-recui in BINARIES_DIR. That must not stop the build."""
    fake_target = minimal_target(tmp_path)
    boot_requirements(fake_target)
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")

    mkinitramfs.build(fake_target, init, tmp_path / "out.gz",
                      recui=str(tmp_path / "not-here"))

    assert "nd-recui" in capsys.readouterr().err


def test_the_recovery_ui_can_come_from_outside_the_target_tree(tmp_path):
    """It comes from BINARIES_DIR, beside nd-verify and for the same reason:
    nothing in the running system calls it, so a copy in the verity-covered
    squashfs would be bytes nobody executes."""
    fake_target = minimal_target(tmp_path)
    boot_requirements(fake_target)
    outside = tmp_path / "binaries" / "nd-recui"
    outside.parent.mkdir(parents=True)
    shutil.copy(HOST_BINARY, outside)
    init = tmp_path / "init"
    init.write_text("#!/bin/sh\n")
    out = tmp_path / "out.gz"

    mkinitramfs.build(fake_target, init, out, recui=str(outside))

    assert "bin/nd-recui" in archive_names(out)


def test_the_post_image_hook_passes_the_recovery_ui():
    hook = open(os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "scripts", "post-image-neodct.sh")).read()

    assert "--recui" in hook
    assert "nd-recui" in hook

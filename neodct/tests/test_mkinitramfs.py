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

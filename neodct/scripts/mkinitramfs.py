#!/usr/bin/env python3
"""Pack the boot-time update applier into an initramfs cpio.gz.

The applier (neodct/initramfs/init) runs before the root filesystem is
mounted, which is the only moment the system partition can safely be
rewritten. It needs busybox (mount/dd/sha256sum/switch_root) and dmsetup
(to load the dm-verity table), both taken from the built target tree so
they are the right architecture and libc.

Only the shared libraries those two binaries actually need get copied --
target/lib is 14MB and the whole cpio is unpacked into RAM on a 64MB
device. DT_NEEDED is read straight out of the ELF headers here because
buildroot ships no cross-ldd, and running the target's dynamic loader on
the build host is not an option.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

# Set by build() so tests (and post-image debugging) can inspect the tree
# that was packed.
LAST_STAGING = None

DEFAULT_LIB_DIRS = ("lib", "usr/lib", "lib64", "usr/lib64")

# Applets the init script relies on. busybox --install would create every
# applet; this is the documented set, and symlinking only these keeps the
# image honest about what it uses.
# Kept honest by neodct/tests/test_initramfs_applets.py, which scans the
# scripts for command names and fails if any of them is missing here. A
# missing applet does not always announce itself: `wc` failing inside a
# command substitution just yields an empty size, which reads as a truncated
# image and silently discards a perfectly good update.
APPLETS = (
    "sh", "mount", "umount", "mkdir", "cat", "echo", "sleep", "dd", "sync",
    "sha256sum", "switch_root", "blkid", "ls", "rm", "mv", "cp", "grep",
    "sed", "awk", "printf", "test", "true", "false", "dmesg", "mknod",
    "modprobe", "losetup", "head", "cut", "date", "expr",
    "wc", "tr", "dirname", "basename", "mountpoint", "chmod", "ln",
    "unzip", "stty", "reboot", "clear", "mkfifo", "kill",
    # The system partition on real hardware is a static UBI volume behind a
    # READ-ONLY ubiblock disk, so dd cannot write it and this is the only
    # thing that can. Reached through $NDSYS_UBIUPDATEVOL in ndsys-apply.sh,
    # which is why the scanner in test_initramfs_applets.py cannot see it and
    # a test names it directly instead.
    "ubiupdatevol",
    # And growing that volume when a new image is bigger than the one the
    # phone was flashed with -- mknand.sh sizes the volume to exactly the
    # image, so this is the ordinary case for any release that grew.
    "ubirsvol",
)


class MissingLibrary(Exception):
    """A needed shared library was not found in the target tree."""


def _read_exact(handle, offset, size):
    handle.seek(offset)
    data = handle.read(size)
    if len(data) != size:
        raise ValueError("truncated ELF")
    return data


def elf_machine(path):
    """(e_machine, is64, endian) for an ELF file.

    The panel daemon is a prebuilt binary carried in the overlay, so it is
    present in every target tree -- including ones it cannot run on. Ship it
    only when it matches the architecture of the rest of the initramfs.
    """
    with open(str(path), "rb") as handle:
        ident = handle.read(16)
        if len(ident) < 16 or ident[:4] != b"\x7fELF":
            raise ValueError("%s is not an ELF file" % path)
        endian = "<" if ident[5] == 1 else ">"
        machine = struct.unpack(endian + "H", _read_exact(handle, 18, 2))[0]
        return machine, ident[4] == 2, endian


def bmp_to_xrgb8888(path, width, height):
    """Read an uncompressed 24-bit BMP into framebuffer bytes.

    Bytes B,G,R,X, which is what a 32bpp DRM framebuffer -- QEMU's -- wants.
    Rows are emitted top-down; BMP stores them bottom-up.

    ON THE PHONE THIS IS THE WRONG WAY ROUND AND IT DOES NOT MATTER YET.
    fb0 there is vfb, which declares red in the low byte, and neodct_displayd
    now believes that declaration rather than assuming B,G,R,X, so a blob
    cat'ed to /dev/fb0 reaches the panel with red and blue exchanged. Both
    splashes are pure black and white, so nothing is visible either way, and
    one blob still serves both targets. A boot logo with any colour in it
    would need the order chosen per target instead -- there is no single
    answer, because the two framebuffers genuinely disagree.

    Deliberately hand-rolled rather than using Pillow: this runs on the build
    host during `make`, where nothing guarantees PIL is installed.
    """
    with open(str(path), "rb") as handle:
        data = handle.read()
    if data[:2] != b"BM":
        raise ValueError("%s is not a BMP" % path)
    offset = struct.unpack_from("<I", data, 10)[0]
    bmp_w, bmp_h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    if bpp != 24 or compression != 0:
        raise ValueError("%s: need an uncompressed 24-bit BMP (got %d bpp, "
                         "compression %d)" % (path, bpp, compression))
    flip = bmp_h > 0                      # positive height means bottom-up
    bmp_h = abs(bmp_h)
    if (bmp_w, bmp_h) != (width, height):
        raise ValueError("%s is %dx%d, expected %dx%d"
                         % (path, bmp_w, bmp_h, width, height))

    stride = (bmp_w * 3 + 3) & ~3         # BMP rows pad to 4 bytes
    out = bytearray(width * height * 4)
    for y in range(height):
        row = (height - 1 - y) if flip else y
        start = offset + row * stride
        dst = y * width * 4
        for x in range(width):
            b, g, r = data[start + x * 3: start + x * 3 + 3]
            out[dst] = b
            out[dst + 1] = g
            out[dst + 2] = r
            out[dst + 3] = 0
            dst += 4
    return bytes(out)


def elf_needed(path):
    """(DT_NEEDED names, PT_INTERP) for an ELF file.

    Minimal 32/64-bit, little/big-endian ELF reader: parse the program
    headers to find PT_DYNAMIC and PT_INTERP, walk the dynamic table for
    DT_NEEDED (1) and DT_STRTAB (5), then read the names out of .dynstr.
    """
    with open(str(path), "rb") as handle:
        ident = handle.read(16)
        if len(ident) < 16 or ident[:4] != b"\x7fELF":
            raise ValueError("%s is not an ELF file" % path)
        is64 = ident[4] == 2
        endian = "<" if ident[5] == 1 else ">"

        if is64:
            header = _read_exact(handle, 16, 48)
            (_, _, _, _, phoff, _, _, _, phentsize, phnum, _, _, _) = \
                struct.unpack(endian + "HHIQQQIHHHHHH", header)
        else:
            header = _read_exact(handle, 16, 36)
            (_, _, _, _, phoff, _, _, _, phentsize, phnum, _, _, _) = \
                struct.unpack(endian + "HHIIIIIHHHHHH", header)

        dynamic = None
        interpreter = None
        for index in range(phnum):
            entry = _read_exact(handle, phoff + index * phentsize, phentsize)
            if is64:
                p_type, _, p_offset, _, _, p_filesz = \
                    struct.unpack(endian + "IIQQQQ", entry[:40])
            else:
                p_type, p_offset, _, _, p_filesz = \
                    struct.unpack(endian + "IIIII", entry[:20])
            if p_type == 2:      # PT_DYNAMIC
                dynamic = (p_offset, p_filesz)
            elif p_type == 3:    # PT_INTERP
                interpreter = _read_exact(handle, p_offset, p_filesz)
                interpreter = interpreter.split(b"\x00")[0].decode()

        if dynamic is None:
            return [], interpreter

        offset, size = dynamic
        entry_size = 16 if is64 else 8
        fmt = endian + ("qQ" if is64 else "iI")
        needed_offsets = []
        strtab = None
        raw = _read_exact(handle, offset, size)
        for position in range(0, size - entry_size + 1, entry_size):
            tag, value = struct.unpack(fmt, raw[position:position + entry_size])
            if tag == 0:         # DT_NULL
                break
            if tag == 1:         # DT_NEEDED
                needed_offsets.append(value)
            elif tag == 5:       # DT_STRTAB (a virtual address)
                strtab = value

        if strtab is None or not needed_offsets:
            return [], interpreter

        # DT_STRTAB is a vaddr; map it back to a file offset via the segment
        # that contains it.
        file_offset = None
        for index in range(phnum):
            entry = _read_exact(handle, phoff + index * phentsize, phentsize)
            if is64:
                p_type, _, p_offset, p_vaddr, _, p_filesz = \
                    struct.unpack(endian + "IIQQQQ", entry[:40])
            else:
                p_type, p_offset, p_vaddr, _, p_filesz = \
                    struct.unpack(endian + "IIIII", entry[:20])
            if p_type == 1 and p_vaddr <= strtab < p_vaddr + p_filesz:  # PT_LOAD
                file_offset = p_offset + (strtab - p_vaddr)
                break
        if file_offset is None:
            return [], interpreter

        names = []
        for string_offset in needed_offsets:
            handle.seek(file_offset + string_offset)
            chunk = handle.read(256).split(b"\x00")[0]
            if chunk:
                names.append(chunk.decode())
        return names, interpreter


def resolve_libs(binaries, lib_dirs):
    """Transitive closure of shared libraries, as {archive path: source path}."""
    lib_dirs = [str(d) for d in lib_dirs]
    resolved = {}
    pending = []
    for binary in binaries:
        names, interpreter = elf_needed(binary)
        pending.extend(names)
        if interpreter:
            pending.append(os.path.basename(interpreter))

    seen = set()
    while pending:
        # FIFO, so a missing dependency is always reported as the first one
        # the binary itself asked for rather than whichever happened to be
        # last on the stack.
        name = pending.pop(0)
        if name in seen:
            continue
        seen.add(name)
        for directory in lib_dirs:
            candidate = os.path.join(directory, name)
            if os.path.exists(candidate):
                # Flatten into /lib: the loader is told where to look by the
                # interpreter path, and a flat /lib keeps the cpio simple.
                resolved["lib/" + name] = candidate
                try:
                    more, _ = elf_needed(candidate)
                except ValueError:
                    more = []
                pending.extend(more)
                break
        else:
            raise MissingLibrary(
                "%s not found in %s" % (name, ", ".join(lib_dirs)))
    return resolved


# dmsetup is required, and buildroot's lvm2 configures with
# --exec-prefix=/usr so it lands in /usr/sbin -- but a hand-built or
# differently configured tree may put it elsewhere. Whichever is found is
# installed at sbin/dmsetup, which is on the PATH the kernel gives init.
DMSETUP_CANDIDATES = ("usr/sbin/dmsetup", "sbin/dmsetup",
                      "usr/bin/dmsetup", "bin/dmsetup")

# The ST7789 panel daemon. Built from source by the neodct buildroot
# package (neodct/src/displayd/) and installed into the target tree; it
# used to be a committed binary in the overlay, which could not work once
# the toolchain moved to musl. The path is unchanged either way, and this
# reads the built target tree rather than the overlay, so nothing here had
# to change when it stopped being a blob.
PANEL_DAEMON = "NeoDCT/System/hw/neodct_displayd"

# The recovery splash. Kept in a subdirectory of the init dir because
# everything that is a *file* there is copied verbatim into the cpio, and
# the 126KB bitmap has no business being in the image -- only the converted
# blob does.
# (bitmap in the init dir, blob in the cpio). The boot logo is optional in
# the strong sense: with no bootlogo.raw the init script never starts the
# panel daemon at boot, so an image without one costs nothing.
SPLASH_IMAGES = (
    (os.path.join("splash", "sadface.bmp"), "splash.raw"),
    (os.path.join("splash", "bootlogo.bmp"), "bootlogo.raw"),
)

# The update signature check (SECURITY-AUDIT.md section 3, the critical
# finding). Two files, and the initramfs is useless for that purpose without
# either of them, so both are required exactly as dmsetup is.
#
# nd-verify is a statically linked 4 MB binary and it comes from BINARIES_DIR
# rather than from the target tree, on purpose: nothing in the running system
# calls it -- the Update app checks the same signature through libneodct,
# which is already mapped -- so putting it in target/ would add those 4 MB to
# the read-only squashfs for nothing. See neodct/src/Makefile's install-boot.
#
# The public key DOES come from the target tree, and that is the point: it is
# the same file the running system verifies against, so the initramfs and the
# UI can never disagree about which key is the release key.
VERIFIER_CANDIDATES = ("NeoDCT/System/bin/nd-verify", "usr/bin/nd-verify",
                       "bin/nd-verify")

# The on-screen recovery UI. Like nd-verify it comes from BINARIES_DIR rather
# than the target tree -- neodct/src/Makefile's install-boot puts it there,
# and nothing in the running system calls it, so a copy in the verity-covered
# squashfs would be bytes nobody executes.
#
# OPTIONAL, and that is the panel-daemon precedent rather than the
# dmsetup/nd-verify one: an initramfs without it still recovers a phone,
# because ndsys-recovery.sh falls back to the tty menu it has always had.
# Failing the build over a nicer menu would be wrong. It is a close call --
# on hardware, without it, recovery has no working input at all -- but that
# is *already* true today, so its absence is a regression to the status quo
# rather than a broken image.
RECUI_CANDIDATES = ("NeoDCT/System/bin/nd-recui", "usr/bin/nd-recui",
                    "bin/nd-recui")
RELEASE_KEY = "NeoDCT/System/keys/neodct-release.pub"
RELEASE_KEY_TARGET = "neodct-release.pub"
SPLASH_SOURCE, SPLASH_TARGET = SPLASH_IMAGES[0]
# Matches UI_W/UI_H and neodctDisplay.c FB_W/FB_H.
SPLASH_W, SPLASH_H = 240, 175


def find_verifier(target_dir, verifier=None):
    """Where nd-verify is, or None. `verifier` is a path from the caller."""
    if verifier:
        return str(verifier) if os.path.exists(str(verifier)) else None
    for candidate in VERIFIER_CANDIDATES:
        source = os.path.join(target_dir, candidate)
        if os.path.exists(source):
            return source
    return None


def find_recui(target_dir, recui=None):
    """Where nd-recui is, or None. `recui` is a path from the caller."""
    if recui:
        return str(recui) if os.path.exists(str(recui)) else None
    for candidate in RECUI_CANDIDATES:
        source = os.path.join(target_dir, candidate)
        if os.path.exists(source):
            return source
    return None


def build(target_dir, init_script, output, extra_binaries=None, lib_dirs=None,
          verifier=None, recui=None):
    """Stage and pack the initramfs. Returns the output path."""
    global LAST_STAGING
    target_dir = str(target_dir)
    busybox = os.path.join(target_dir, "bin", "busybox")
    if not os.path.exists(busybox):
        sys.exit("mkinitramfs: no busybox in %s -- build the target first"
                 % target_dir)

    binaries = {"bin/busybox": busybox}
    # Plain files: copied verbatim, not chmod +x and not searched for
    # DT_NEEDED. Only the release key so far.
    plain_files = {}

    if extra_binaries is None:
        for candidate in DMSETUP_CANDIDATES:
            source = os.path.join(target_dir, candidate)
            if os.path.exists(source):
                binaries["sbin/dmsetup"] = source
                break
        else:
            # Without dmsetup the initramfs cannot load a verity table, and
            # neodct.verity=enforce would drop every boot into the rescue
            # shell. Fail the build instead of shipping that.
            sys.exit("mkinitramfs: no dmsetup in %s (looked in %s) -- enable "
                     "BR2_PACKAGE_LVM2; dm-verity cannot work without it"
                     % (target_dir, ", ".join(DMSETUP_CANDIDATES)))

        # The SPI panel daemon, so recovery is visible on hardware. The
        # phone's fb0 is vfb: the framebuffer console draws the recovery
        # menu into it, but the pixels only reach the ST7789 if something
        # mirrors them.
        #
        # Optional, and the architecture check stays even though the daemon
        # is built from source now and so cannot be the wrong architecture
        # any more. It costs one readelf and it is the check that would
        # catch a stale target/ left over from a build for another board --
        # shipping a binary the kernel cannot exec is exactly the failure
        # this whole file has to avoid.
        # The signature verifier and the key it checks against. Both are
        # required, for the same reason dmsetup is: an initramfs without
        # them cannot tell a release image from one somebody staged, and it
        # would apply either. Shipping that silently is worse than failing
        # the build, because it looks finished.
        found = find_verifier(target_dir, verifier)
        if found is None:
            sys.exit("mkinitramfs: no nd-verify (looked at %s%s) -- build "
                     "neodct/src and run its install-boot target; the update "
                     "signature check cannot work without it"
                     % ("--verifier %s, " % verifier if verifier else "",
                        ", ".join(VERIFIER_CANDIDATES)))
        binaries["bin/nd-verify"] = found

        key = os.path.join(target_dir, RELEASE_KEY)
        if not os.path.exists(key):
            sys.exit("mkinitramfs: no %s in %s -- the initramfs has nothing "
                     "to check update signatures against" % (RELEASE_KEY, target_dir))
        plain_files[RELEASE_KEY_TARGET] = key

        panel = os.path.join(target_dir, PANEL_DAEMON)
        if os.path.exists(panel):
            try:
                if elf_machine(panel) == elf_machine(busybox):
                    binaries["bin/neodct_displayd"] = panel
                else:
                    print("mkinitramfs: %s is a different architecture; "
                          "recovery will have no panel output"
                          % PANEL_DAEMON, file=sys.stderr)
            except ValueError as exc:
                print("mkinitramfs: %s unreadable (%s); skipping"
                      % (PANEL_DAEMON, exc), file=sys.stderr)

        # The recovery UI, gated on the SAME architecture check the panel
        # daemon gets. install-boot writes into BINARIES_DIR, which is not
        # architecture-tagged, so a stale cross build left over from another
        # board is a real way to ship a binary the kernel cannot exec -- and
        # this one is reached from the screen a person is standing in front
        # of, where "nothing happened" is the whole failure report.
        found = find_recui(target_dir, recui)
        if found is None:
            print("mkinitramfs: no nd-recui (looked at %s%s); recovery will "
                  "fall back to its text menu"
                  % ("--recui %s, " % recui if recui else "",
                     ", ".join(RECUI_CANDIDATES)), file=sys.stderr)
        else:
            try:
                if elf_machine(found) == elf_machine(busybox):
                    binaries["bin/nd-recui"] = found
                else:
                    print("mkinitramfs: %s is a different architecture; "
                          "recovery will fall back to its text menu"
                          % found, file=sys.stderr)
            except ValueError as exc:
                print("mkinitramfs: %s unreadable (%s); skipping"
                      % (found, exc), file=sys.stderr)
    else:
        for relative in extra_binaries:
            source = os.path.join(target_dir, relative)
            if os.path.exists(source):
                binaries[relative] = source
            else:
                print("mkinitramfs: warning: %s missing" % relative,
                      file=sys.stderr)

    if lib_dirs is None:
        lib_dirs = [os.path.join(target_dir, d) for d in DEFAULT_LIB_DIRS]
    lib_dirs = [d for d in lib_dirs if os.path.isdir(d)]

    staging = tempfile.mkdtemp(prefix="neodct-initramfs-")
    LAST_STAGING = staging
    for directory in ("bin", "sbin", "lib", "usr", "proc", "sys", "dev", "mnt",
                      "mnt/root", "mnt/user", "mnt/sdcard", "newroot"):
        os.makedirs(os.path.join(staging, directory), exist_ok=True)

    # Everything lands in /lib, but the dynamic loader does not necessarily
    # look there. This glibc's built-in search path is /lib64 and /usr/lib64,
    # and an initramfs has no /etc/ld.so.cache to redirect it -- so without
    # these aliases the loader cannot find a single library and boot dies with
    # "error while loading shared libraries". The target tree solves it the
    # same way (lib64 -> lib); mirror that so every directory the loader might
    # try resolves to the one real lib dir, whichever libc this is.
    for alias, target in (("lib64", "lib"),
                          ("usr/lib", "../lib"),
                          ("usr/lib64", "../lib")):
        link = os.path.join(staging, alias)
        if not os.path.lexists(link):
            os.symlink(target, link)

    for archive_path, source in binaries.items():
        destination = os.path.join(staging, archive_path)
        os.makedirs(os.path.dirname(destination), exist_ok=True)
        shutil.copy2(source, destination)
        os.chmod(destination, 0o755)

    for archive_path, source in plain_files.items():
        destination = os.path.join(staging, archive_path)
        os.makedirs(os.path.dirname(destination) or staging, exist_ok=True)
        shutil.copy2(source, destination)
        os.chmod(destination, 0o644)

    for archive_path, source in resolve_libs(binaries.values(), lib_dirs).items():
        destination = os.path.join(staging, archive_path)
        shutil.copy2(source, destination)

    # The loader is looked up by its absolute PT_INTERP path, so mirror it.
    for _, interpreter in (elf_needed(b) for b in binaries.values()):
        if not interpreter:
            continue
        destination = os.path.join(staging, interpreter.lstrip("/"))
        if os.path.exists(destination):
            continue
        source = os.path.join(staging, "lib", os.path.basename(interpreter))
        if os.path.exists(source):
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            shutil.copy2(source, destination)

    for applet in APPLETS:
        link = os.path.join(staging, "bin", applet)
        if not os.path.exists(link):
            os.symlink("busybox", link)

    # init_script may be the init file itself or the directory holding it
    # plus the sourced helpers (ndsys-apply.sh).
    init_script = str(init_script)
    if os.path.isdir(init_script):
        for name in sorted(os.listdir(init_script)):
            source = os.path.join(init_script, name)
            if os.path.isfile(source):
                shutil.copy2(source, os.path.join(staging, name))
        if not os.path.exists(os.path.join(staging, "init")):
            sys.exit("mkinitramfs: no init in %s" % init_script)
        # Convert the splash to raw framebuffer bytes here rather than
        # committing the blob: the bitmap stays the one source of truth,
        # and a 2-colour image costs almost nothing once gzipped.
        for relative, blob in SPLASH_IMAGES:
            source = os.path.join(init_script, relative)
            if not os.path.exists(source):
                continue
            try:
                pixels = bmp_to_xrgb8888(source, SPLASH_W, SPLASH_H)
            except (ValueError, OSError) as exc:
                sys.exit("mkinitramfs: cannot convert %s: %s" % (source, exc))
            with open(os.path.join(staging, blob), "wb") as handle:
                handle.write(pixels)
    else:
        shutil.copy2(init_script, os.path.join(staging, "init"))
    os.chmod(os.path.join(staging, "init"), 0o755)

    names = subprocess.run(["find", ".", "-mindepth", "1", "-printf", "%P\\n"],
                           cwd=staging, capture_output=True, text=True,
                           check=True).stdout
    with open(str(output), "wb") as handle:
        cpio = subprocess.Popen(["cpio", "--quiet", "-o", "-H", "newc"],
                                cwd=staging, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, text=True)
        gzip = subprocess.Popen(["gzip", "-9", "-n"], stdin=cpio.stdout,
                                stdout=handle)
        cpio.stdout.close()
        cpio.communicate(names)
        gzip.communicate()
        if cpio.returncode or gzip.returncode:
            sys.exit("mkinitramfs: cpio/gzip failed")
    return str(output)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--target-dir", required=True)
    parser.add_argument("--init", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--verifier",
                        help="path to nd-verify (default: search --target-dir)")
    parser.add_argument("--recui",
                        help="path to nd-recui (default: search --target-dir)")
    args = parser.parse_args(argv)

    path = build(args.target_dir, args.init, args.output,
                 verifier=args.verifier, recui=args.recui)
    print("mkinitramfs: %s (%.1f KiB)"
          % (path, os.path.getsize(path) / 1024.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())

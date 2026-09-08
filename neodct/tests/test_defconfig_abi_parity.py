"""One ABI on both machines, and the things a future defconfig edit must not
quietly drop.

DECISIONS.md D1's prize is that the emulator and the phone are the same
armv7 hard-float musl target, so there is one app.so, one .nap arch tag, and
32-bit time_t/size_t/pointer/alignment bugs fail in QEMU instead of on the
bench. That is a paragraph in two defconfigs until something checks it: the
two files drift one convenient line at a time, and the day they disagree
nothing fails -- both images still build, and only the second one is wrong.

Everything here is a property that is INVISIBLE when broken. An image built
against the wrong kernel headers boots. A luckfox build that quietly regains
a BR2_LINUX_KERNEL block boots. A defconfig naming a kernel config that does
not exist fails minutes in, at the kernel step, long after it stopped being
obvious which edit caused it.

test_defconfig_copies.py already holds the two copies of each file equal; the
copies are checked here too, so that a property asserted on the file somebody
reads is also asserted on the file the build reads.
"""

import glob
import os

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NEODCT_CONFIGS = os.path.join(REPO, "neodct", "configs")
BUILDROOT_CONFIGS = os.path.join(REPO, "buildroot", "configs")
BUILDROOT = os.path.join(REPO, "buildroot")

NAMES = sorted(os.path.basename(p)
               for p in glob.glob(os.path.join(NEODCT_CONFIGS, "*_defconfig")))

# Both copies of every one of ours. buildroot/configs also holds several
# hundred upstream defconfigs, which are none of this project's business; the
# names in neodct/configs are what says which are ours.
OURS = [os.path.join(d, name)
        for name in NAMES
        for d in (NEODCT_CONFIGS, BUILDROOT_CONFIGS)
        if os.path.exists(os.path.join(d, name))]


def ident(path):
    return "%s/%s" % (os.path.basename(os.path.dirname(path)),
                      os.path.basename(path))


def settings(path):
    """The lines kconfig will act on. Comments are stripped because these
    files argue for their choices at length and several of those arguments
    name the very symbols asserted against below."""
    out = []
    for line in open(path).read().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            out.append(line)
    return out


# The full set from DECISIONS.md D1: instruction set, tuning, float ABI, code
# density, libc and kernel UAPI. Not a sample -- every one of these has to
# match or "one ABI" is not true, and each is silent when it does not.
ONE_ABI = [
    "BR2_arm=y",
    "BR2_cortex_a7=y",
    "BR2_ARM_FPU_NEON_VFPV4=y",
    "BR2_ARM_INSTRUCTIONS_THUMB2=y",
    "BR2_TOOLCHAIN_BUILDROOT_MUSL=y",
    "BR2_KERNEL_HEADERS_5_10=y",
]


def test_there_are_defconfigs_to_check():
    """Everything below is parametrised over a glob; an empty glob would make
    the whole file pass by vacuum, which is the worst way for it to fail."""
    assert len(OURS) >= 4, OURS


@pytest.mark.parametrize("defconfig", OURS, ids=ident)
@pytest.mark.parametrize("symbol", ONE_ABI)
def test_both_targets_describe_the_same_abi(defconfig, symbol):
    assert symbol in settings(defconfig), (
        "%s no longer sets %s. The emulator and the phone are meant to be one "
        "ABI -- one app.so, one .nap arch tag -- and a difference here is not "
        "a build failure, it is two images that look alike and are not."
        % (ident(defconfig), symbol))


@pytest.mark.parametrize("defconfig", OURS, ids=ident)
def test_nothing_still_builds_for_aarch64(defconfig):
    """The retired target, spelled out so it cannot come back by halves.

    ND_NAP_ARCH_QEMU was retired as a separate ABI because nd_nap.c resolves
    it from `uname -m`, and uname can no longer tell the two images apart.
    That retirement is only safe while no aarch64 image can be built here."""
    lines = settings(defconfig)
    for dead in ("BR2_aarch64=y", "BR2_cortex_a53=y"):
        assert dead not in lines, (
            "%s sets %s. uname -m would say aarch64 again, and the .nap arch "
            "tag is resolved from uname." % (ident(defconfig), dead))


@pytest.mark.parametrize("defconfig", OURS, ids=ident)
def test_the_kernel_headers_are_the_phones_and_not_the_emulators(defconfig):
    """5.10 on both sides, because that is what the Luckfox runs.

    Left alone, BR2_KERNEL_HEADERS_AS_KERNEL is the choice default whenever
    BR2_LINUX_KERNEL is set, so QEMU would compile its userland against 6.12's
    UAPI while the phone's kernel is 5.10.110 -- the same instruction set, the
    same libc, the same float ABI and a DIFFERENT kernel interface. A package
    reaching for a post-5.10 ioctl then builds and runs perfectly in the
    emulator and fails on hardware, which is precisely the failure this branch
    exists to move into the emulator.

    And the custom-series symbol must be gone with it: it belongs to a choice
    that depends on AS_KERNEL / a custom tarball / a custom git, so leaving it
    behind is at best noise and at worst a second opinion about the UAPI."""
    lines = settings(defconfig)

    assert "BR2_KERNEL_HEADERS_5_10=y" in lines, ident(defconfig)
    assert "BR2_KERNEL_HEADERS_AS_KERNEL=y" not in lines, (
        "%s takes the headers from whatever kernel it builds; the phone's "
        "kernel is not built here" % ident(defconfig))
    stale = [line for line in lines
             if line.startswith("BR2_PACKAGE_HOST_LINUX_HEADERS_CUSTOM")]
    assert not stale, (
        "%s still names a custom kernel-header series: %s" % (ident(defconfig), stale))


@pytest.mark.parametrize("defconfig", OURS, ids=ident)
def test_every_kernel_config_named_actually_exists(defconfig):
    """A path typo here does not fail until the kernel step, minutes in.

    The path is relative to the buildroot directory, which is where the build
    runs -- board/qemu/armv7-virt/linux.config, not buildroot/board/..."""
    for line in settings(defconfig):
        if not line.startswith("BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE="):
            continue
        path = line.split("=", 1)[1].strip('"')
        assert os.path.exists(os.path.join(BUILDROOT, path)), (
            "%s names %s, which is not in the buildroot tree"
            % (ident(defconfig), path))


@pytest.mark.parametrize(
    "defconfig", [p for p in OURS if "luckfox" in os.path.basename(p)], ids=ident)
def test_the_phone_builds_no_kernel_here(defconfig):
    """The phone's kernel comes out of the Rockchip SDK, not out of buildroot.

    docs/FLASHING.md builds it with ./build.sh kernel inside the SDK
    distrobox and mknand.sh copies no kernel artefact, so a BR2_LINUX_KERNEL
    block here is minutes of every build spent on an output nothing consumes.
    Worse, it recaptures the kernel headers through the AS_KERNEL choice
    default and re-breaks the pin above -- which is why this is asserted
    rather than left as a comment somebody can delete."""
    kernel = [line for line in settings(defconfig)
              if line.startswith("BR2_LINUX_KERNEL")]
    assert not kernel, (
        "%s builds a kernel again: %s. Removing it and pinning the headers "
        "are one edit; doing either alone puts the UAPI back where it was or "
        "moves it to 6.18." % (ident(defconfig), kernel))


@pytest.mark.parametrize(
    "defconfig", [p for p in OURS if "luckfox" in os.path.basename(p)], ids=ident)
def test_the_phone_still_asks_for_a_ubifs_rootfs(defconfig):
    """It looks like it builds an unused rootfs.ubifs, and it does. Keep it.

    DECISIONS.md D5: this block is the ONLY thing in the configuration that
    pulls host-mtd (fs/ubifs/ubifs.mk's ROOTFS_UBIFS_DEPENDENCIES; the only
    other route is jffs2, which is not enabled), and neodct/tools/mknand.sh
    takes ubinize and mkfs.ubifs from $HOST_DIR/sbin and dies without them.
    Deleting it breaks the hardware image assembler several steps away from
    anything that mentions UBIFS.

    The UBI half carries the second reason: mknand.sh cites the PEB and
    subpage values as its authority for the NAND geometry it writes. A comment
    can be deleted by whoever disagrees with it; a test explains itself to
    them first."""
    lines = settings(defconfig)
    assert "BR2_TARGET_ROOTFS_UBIFS=y" in lines, ident(defconfig)
    assert "BR2_TARGET_ROOTFS_UBI=y" in lines, ident(defconfig)

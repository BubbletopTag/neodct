"""The binaries call audio is piped through have to actually be installed.

A phone call on this hardware is two alsa-utils pipes: the modem's PCM
comes out of /dev/ttyUSB4 after AT+CPCMREG=1 and goes to `aplay`, and the
USB microphone goes through `arecord` back into the same device. Neither
is optional, and neither is imported -- they are spawned, so nothing fails
until somebody answers a call and hears silence.

They went missing for six weeks. alsa-utils was built on 10 July;
BR2_PACKAGE_ALSA_UTILS_APLAY was added to the defconfig on 22 July.
Buildroot treats a built package as up to date and does not re-run its
install step when a sub-option changes, so the binary sat compiled in the
build directory and was never copied to the phone. The defconfig said
yes, the build said nothing at all, and the image said no.
"""

import os

import pytest

REQUIRED = ("aplay", "arecord")


def target_dirs():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = os.path.dirname(here)
    found = []
    for candidate in ("build-luckfox/target", "buildroot/output/target"):
        path = os.path.join(root, candidate)
        if os.path.isdir(os.path.join(path, "usr", "bin")):
            found.append(path)
    return found


@pytest.mark.parametrize("tool", REQUIRED)
def test_the_call_audio_tools_are_installed(tool):
    targets = target_dirs()
    if not targets:
        pytest.skip("no built target tree to inspect")

    missing = [t for t in targets
               if not os.path.exists(os.path.join(t, "usr", "bin", tool))]

    assert not missing, (
        "%s is not installed in: %s\n"
        "The defconfig selects BR2_PACKAGE_ALSA_UTILS_APLAY, which provides "
        "both aplay and arecord. If the option was added after alsa-utils "
        "was first built, buildroot will not reinstall it on its own:\n"
        "    make alsa-utils-reinstall\n"
        % (tool, ", ".join(missing)))


def test_the_defconfigs_ask_for_them():
    """Both flavours: the QEMU one is where this gets tested before it
    reaches a phone somebody is trying to call from."""
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for name in ("luckfox_pico_mini_defconfig", "neodct_qemu_defconfig"):
        path = os.path.join(here, "configs", name)
        body = open(path).read()
        assert "BR2_PACKAGE_ALSA_UTILS_APLAY=y" in body, name

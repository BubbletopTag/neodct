"""The update downloader spawns curl, so curl has to be on the phone.

lib/nd_remote.c does not link libcurl. It runs /usr/bin/curl as a child
process, for a measured reason: linking OpenSSL into libneodct.so for the
signature verifier already costs +1.3 MB of idle RSS in every process that
maps the library, because nd-core maps it at boot and never verifies a
signature. A download happens a few times a year and its memory should
exist only while one is running.

The cost of that decision is that nothing fails at build time if curl goes
missing. There is no unresolved symbol, no link error and no import to
fail -- the phone just says "cannot reach GitHub: no curl on this phone"
the next time somebody looks for an update, which is a message nobody sees
until they need it. That is exactly how aplay and arecord went missing for
six weeks (test_audio_tools.py). So the three things nd_remote.c needs are
pinned here instead:

    /usr/bin/curl                        the transport
    libcurl                              which it is linked against
    /etc/ssl/certs/ca-certificates.crt   what verifies GitHub's certificate

The CA bundle is not optional and there is no --insecure anywhere in
nd_remote.c. An unverified fetch would make the signature check the only
thing standing between the phone and a hostile package, and one line of
defence is not enough for something that replaces the rootfs.
"""

import os

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
NEODCT = os.path.dirname(HERE)
ROOT = os.path.dirname(NEODCT)

DEFCONFIGS = ("luckfox_pico_mini_defconfig", "neodct_qemu_defconfig")

REQUIRED_OPTIONS = (
    "BR2_PACKAGE_LIBCURL=y",
    "BR2_PACKAGE_LIBCURL_CURL=y",
    "BR2_PACKAGE_CA_CERTIFICATES=y",
)


def target_dirs():
    found = []
    for candidate in ("build-luckfox/target", "buildroot/output/target"):
        path = os.path.join(ROOT, candidate)
        if os.path.isdir(os.path.join(path, "usr", "bin")):
            found.append(path)
    return found


@pytest.mark.parametrize("option", REQUIRED_OPTIONS)
@pytest.mark.parametrize("name", DEFCONFIGS)
def test_the_defconfigs_ask_for_curl(name, option):
    """Both flavours. The QEMU one is where an update is tested before it
    reaches a phone."""
    body = open(os.path.join(NEODCT, "configs", name)).read()
    assert option in body, (
        "%s does not select %s.\n"
        "lib/nd_remote.c spawns /usr/bin/curl and verifies TLS against the "
        "ca-certificates bundle. Without these the Update app can still "
        "install from the card, but it can never find a release."
        % (name, option))


def test_curl_is_actually_installed():
    targets = target_dirs()
    if not targets:
        pytest.skip("no built target tree to inspect")

    missing = [t for t in targets
               if not os.path.exists(os.path.join(t, "usr", "bin", "curl"))]

    assert not missing, (
        "/usr/bin/curl is not installed in: %s\n"
        "The defconfig selects BR2_PACKAGE_LIBCURL_CURL. If that option was "
        "added after libcurl was first built, buildroot will not reinstall "
        "it on its own:\n"
        "    make libcurl-reinstall\n" % ", ".join(missing))


def test_the_ca_bundle_is_actually_installed():
    targets = target_dirs()
    if not targets:
        pytest.skip("no built target tree to inspect")

    bundle = os.path.join("etc", "ssl", "certs", "ca-certificates.crt")
    missing = [t for t in targets if not os.path.exists(os.path.join(t, bundle))]

    assert not missing, (
        "/%s is not installed in: %s\n"
        "curl verifies GitHub's certificate against it. Without it every "
        "download fails, which is the correct failure -- but it is a "
        "packaging mistake, not a network one." % (bundle, ", ".join(missing)))


def test_libneodct_does_not_link_libcurl():
    """The other half of the decision, and the half that would regress
    quietly.

    Adding libcurl to PKG_DEPS would build and pass every test. It would
    also map libcurl and its TLS stack into nd-core, nd-apprun and all
    twenty-four apps, for the sake of a subprocess that runs a few times a
    year -- which is the cost this design exists to avoid. If a future
    change genuinely needs to link it, delete this test deliberately and
    say why in the commit."""
    makefile = open(os.path.join(NEODCT, "src", "Makefile")).read()
    for line in makefile.splitlines():
        if line.startswith("PKG_DEPS"):
            assert "curl" not in line, (
                "neodct/src/Makefile links libcurl:\n    %s\n"
                "lib/nd_remote.c spawns /usr/bin/curl instead. See its "
                "header comment." % line.strip())
            return
    raise AssertionError("neodct/src/Makefile has no PKG_DEPS line any more")

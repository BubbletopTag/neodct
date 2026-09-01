"""Two users, and the properties the whole confinement design rests on.

SECURITY-PLAN.md section 1. On a 5.10 vendor kernel that has no Landlock,
DAC is the primary confinement mechanism rather than a stopgap, so the
contents of this table are not configuration -- they are the boundary.

Three things here are load-bearing and each has a test:

  ndusr_ut is NOT in group ndusr   the separation itself
  ndusr_ut has no dialout          SECURITY-AUDIT.md 4 Q1, premium dialling
  the ids are written out          /NeoDCT/User stores numeric ids and
                                   survives every update; an id that shifts
                                   is a partition full of files owned by
                                   nobody

The table is also run through buildroot's real mkusers where that script is
available, because the format has traps -- a colon anywhere corrupts
/etc/passwd, and `-` in the password field means "no password" rather than
"no login".
"""

import os
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TABLE = os.path.join(REPO, "neodct", "configs", "users-table.txt")
RULES = os.path.join(REPO, "neodct", "overlay", "etc", "udev", "rules.d",
                     "61-neodct-devices.rules")
MKUSERS = os.path.join(REPO, "buildroot", "support", "scripts", "mkusers")
SKELETON = os.path.join(REPO, "buildroot", "system", "skeleton", "etc")
DEFCONFIGS = [os.path.join(REPO, "buildroot", "configs", name)
              for name in ("neodct_qemu_defconfig", "luckfox_pico_mini_defconfig")]


def rows():
    """(username, uid, group, gid, passwd, home, shell, groups, comment)."""
    out = []
    for line in open(TABLE):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        out.append(line.split(None, 8))
    return out


def row(username):
    for fields in rows():
        if fields[0] == username:
            return fields
    raise AssertionError("%s is not in the users table" % username)


def groups_of(username):
    field = row(username)[7]
    return set() if field == "-" else set(field.split(","))


# --- the shape of the file -----------------------------------------------

def test_every_row_has_nine_fields():
    for fields in rows():
        assert len(fields) == 9, fields


def test_no_field_contains_a_colon():
    """mkusers does not check, and prints the comment straight into
    /etc/passwd where a colon is the separator."""
    for fields in rows():
        for value in fields:
            assert ":" not in value, fields


def test_both_users_exist():
    assert row("ndusr")[0] == "ndusr"
    assert row("ndusr_ut")[0] == "ndusr_ut"


# --- the boundary --------------------------------------------------------

def test_the_untrusted_user_is_not_in_the_trusted_users_group():
    """SECURITY-PLAN.md section 1: "ndusr_ut must NOT be in group ndusr. The
    group is what separates 'trusted apps share the user's data' from 'the
    browser does not'." Every mode bit in S00userdata rests on this line."""
    assert "ndusr" not in groups_of("ndusr_ut")
    assert row("ndusr_ut")[2] != "ndusr", "nor as its primary group"
    assert row("ndusr_ut")[3] != row("ndusr")[3], "nor by sharing its gid"


def test_the_untrusted_user_cannot_open_the_modem():
    """SECURITY-AUDIT.md section 4 Q1. `echo ATD... > /dev/ttyUSB2` is the
    classic feature-phone attack and it costs the owner real money. The tty
    is root:dialout; not being in dialout is what makes it unopenable."""
    assert "dialout" not in groups_of("ndusr_ut")


def test_the_untrusted_user_cannot_reach_the_i2c_bus():
    """A bus is a bus: everything on it is reachable by address, so the fuel
    gauge and the keypad expander come together."""
    assert "i2c" not in groups_of("ndusr_ut")


def test_the_untrusted_user_does_get_the_screen_and_the_speaker():
    """Deliberate, and worth pinning so it is not read as an oversight. The
    untrusted set is netsurf-fb and mpv; denying video and audio would not
    confine them, it would break video playback and ringtones. Devices are
    section 2's job -- a minimal /dev in a mount namespace, which removes the
    nodes rather than arguing about their group."""
    assert {"video", "audio"} <= groups_of("ndusr_ut")


def test_the_trusted_user_gets_everything_the_ui_touches():
    """The audit's table in section 6, Phase 1."""
    assert {"video", "audio", "dialout", "input", "i2c"} <= groups_of("ndusr")


# --- the ids -------------------------------------------------------------

@pytest.mark.parametrize("username", ["ndusr", "ndusr_ut"])
def test_the_ids_are_explicit(username):
    """-1 and -2 ask mkusers to allocate, and buildroot's own manual warns
    what that means: adding a package shifts every automatic id. /NeoDCT/User
    stores numeric ids on the medium and is the one thing an update does not
    replace, so a shift is a partition of files owned by nobody."""
    uid, gid = row(username)[1], row(username)[3]

    assert uid.isdigit() and gid.isdigit(), row(username)
    assert int(uid) >= 1000, "below 1000 collides with mkusers' system block"
    assert int(gid) >= 1000


def test_the_two_users_do_not_share_an_id():
    assert row("ndusr")[1] != row("ndusr_ut")[1]
    assert row("ndusr")[3] != row("ndusr_ut")[3]


@pytest.mark.parametrize("username", ["ndusr", "ndusr_ut"])
def test_neither_user_can_log_in(username):
    """`*` is "login not allowed". `-` would write an EMPTY shadow field,
    which is a passwordless login for a user that has a passwd entry."""
    assert row(username)[4] == "*"
    assert row(username)[6] in ("/bin/false", "/bin/nologin", "-")


# --- the groups the rules name -------------------------------------------

def test_every_group_the_rules_name_exists():
    """eudev resolves GROUP= at rule-parse time and, when the name is
    unknown, logs one line and falls back to gid 0 -- which also drops the
    node to 0600. A typo here is a phone with no keypad, not a build error."""
    import re

    named = set(re.findall(r'GROUP="([^"]+)"', open(RULES).read()))
    named |= set(re.findall(r'chgrp (\w+)', open(RULES).read()))
    # The ones buildroot's skeleton and the udev package already provide.
    stock = {"video", "audio", "dialout", "input", "tty", "disk", "kvm", "sgx"}
    ours = {fields[2] for fields in rows()} | {fields[0] for fields in rows()}

    assert named, "the rules file names no groups at all"
    for group in named:
        assert group in stock | ours, (
            "%s is named by a udev rule but created by nothing" % group)


def test_the_i2c_group_is_created_here_because_nothing_else_creates_it():
    """/dev/i2c-3 is the keypad expander and the fuel gauge, it has no stock
    udev rule anywhere, and a node with no GROUP= is 0600."""
    assert any(fields[2] == "i2c" for fields in rows())
    assert 'GROUP="i2c"' in open(RULES).read()


# --- wiring --------------------------------------------------------------

@pytest.mark.parametrize("defconfig", DEFCONFIGS)
def test_both_defconfigs_ask_for_the_table(defconfig):
    """A table nothing reads creates no users, and every mode bit downstream
    then applies to a user that does not exist."""
    body = open(defconfig).read()

    assert 'BR2_ROOTFS_USERS_TABLES="../neodct/configs/users-table.txt"' in body


NEODCT_MK = os.path.join(REPO, "buildroot", "package", "neodct", "neodct.mk")


POST_IMAGE = os.path.join(REPO, "neodct", "scripts", "post-image-neodct.sh")


@pytest.mark.skipif(not os.path.exists(NEODCT_MK), reason="buildroot not vendored")
def test_the_package_supplies_the_users_too():
    """The defconfig alone is not enough, and this is the line that covers it.

    Buildroot generates output/.config from the defconfig ONCE. A checkout made
    before the users table was added keeps its old .config through every
    rebuild, never gains BR2_ROOTFS_USERS_TABLES, and produces images with no
    ndusr in them -- so nd_priv_lookup() finds nothing and EVERY APP RUNS AS
    ROOT. Found with `top` on a real build, showing netsurf as root.

    PACKAGES_USERS is collected from enabled packages regardless of the
    .config's rootfs settings, so declaring the users in neodct.mk reaches a
    stale tree that the defconfig cannot."""
    body = open(NEODCT_MK).read()

    assert "NEODCT_USERS" in body, "the package no longer supplies the users"
    assert "users-table.txt" in body, "and it must read THE table, not a copy"


@pytest.mark.skipif(not os.path.exists(NEODCT_MK), reason="buildroot not vendored")
def test_the_package_fallback_is_not_guarded():
    """It must NOT be wrapped in ifeq on BR2_ROOTFS_USERS_TABLES.

    That guard looks right -- it stops the two paths both firing -- but it
    assumes the reason the users are missing is a .config without the line. If
    that assumption is ever wrong the guard switches the fallback off in
    exactly the case it exists for. mkusers accepts an entry it has already
    seen (the table listed twice gives rc=0 and two users, not four), so being
    unguarded costs nothing and removes the assumption."""
    body = open(NEODCT_MK).read()

    assert "ifeq ($(call qstrip,$(BR2_ROOTFS_USERS_TABLES)),)" not in body, (
        "guarding the fallback on the .config re-introduces the assumption it "
        "was written to remove"
    )


@pytest.mark.skipif(not os.path.exists(NEODCT_MK), reason="buildroot not vendored")
def test_the_table_is_not_read_with_a_make_4_only_function():
    """$(file <...) is GNU make 4.0. Buildroot's stated minimum is 3.81, where
    it is not a function at all -- it parses as an undefined variable and
    expands to the EMPTY STRING. No error, no users, an image where everything
    runs as root. $(shell) and $(subst) work everywhere."""
    # Comment lines only EXPLAIN why $(file <) is avoided; they must not fail
    # this. Strip them and look at what make will actually evaluate.
    code = "\n".join(line for line in open(NEODCT_MK).read().splitlines()
                     if not line.lstrip().startswith("#"))

    assert "$(file <" not in code, "make 3.81 expands $(file <...) to nothing"
    assert "$(shell sed" in code, "read the table with $(shell), which is portable"


@pytest.mark.skipif(not os.path.exists(POST_IMAGE), reason="script missing")
def test_the_build_refuses_an_image_with_no_users():
    """Belt and braces, and the braces matter more.

    Both mechanisms above can fail -- a renamed file, an odd .config, a make
    that behaves unexpectedly. What must never happen again is that such a
    build SUCCEEDS and hands over an image where every app is root. post-image
    greps the users table buildroot actually handed mkusers and exits non-zero
    if the users are not in it."""
    body = open(POST_IMAGE).read()

    assert "full_users_table.txt" in body, "the check must read what mkusers got"
    assert "ndusr_ut" in body, "both users have to be checked, not just ndusr"
    assert "exit 1" in body, "and it has to FAIL the build, not warn"
    # BUILD_DIR is not exported to post-image scripts; BASE_DIR is. Deriving it
    # wrongly left the path non-existent, which took the "cannot verify" branch
    # and passed -- a check that silently passes is worse than no check.
    assert "BASE_DIR" in body, "BUILD_DIR is not exported; derive it from BASE_DIR"


@pytest.mark.skipif(not os.path.exists(MKUSERS), reason="buildroot not vendored")
def test_buildroot_accepts_the_table(tmp_path):
    """The real script, against a real skeleton. The format has traps that
    only it will find."""
    target = tmp_path / "target"
    (target / "etc").mkdir(parents=True)
    for name in ("passwd", "group", "shadow"):
        shutil.copy(os.path.join(SKELETON, name), target / "etc" / name)

    result = subprocess.run(
        [MKUSERS, TABLE, str(target)], capture_output=True, text=True,
        env=dict(os.environ, BR2_CONFIG=os.path.join(REPO, "buildroot", ".config")))

    assert result.returncode == 0, result.stderr
    passwd = (target / "etc" / "passwd").read_text().splitlines()
    for line in passwd:
        assert len(line.split(":")) == 7, line
    names = [line.split(":")[0] for line in passwd]
    assert "ndusr" in names and "ndusr_ut" in names


@pytest.mark.skipif(not os.path.exists(MKUSERS), reason="buildroot not vendored")
def test_the_group_memberships_land_the_way_they_are_written(tmp_path):
    """The end state, read back out of /etc/group rather than out of the
    table it was written in."""
    target = tmp_path / "target"
    (target / "etc").mkdir(parents=True)
    for name in ("passwd", "group", "shadow"):
        shutil.copy(os.path.join(SKELETON, name), target / "etc" / name)
    subprocess.run([MKUSERS, TABLE, str(target)], capture_output=True, check=True,
                   env=dict(os.environ,
                            BR2_CONFIG=os.path.join(REPO, "buildroot", ".config")))

    members = {}
    for line in (target / "etc" / "group").read_text().splitlines():
        name, _, _, listed = line.split(":")
        members[name] = set(filter(None, listed.split(",")))

    assert "ndusr_ut" not in members.get("ndusr", set())
    assert "ndusr_ut" not in members.get("dialout", set())
    assert "ndusr" in members["dialout"]
    assert {"ndusr", "ndusr_ut"} <= members["video"]
    assert {"ndusr", "ndusr_ut"} <= members["audio"]

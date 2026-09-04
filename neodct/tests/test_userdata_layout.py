"""S00userdata lays out /NeoDCT/User, and that layout IS the confinement.

SECURITY-PLAN.md section 1. On a 5.10 vendor kernel with no Landlock, DAC is
the primary mechanism rather than a stopgap, so these mode bits are the
boundary between "the browser" and "the phone" -- not housekeeping.

The design turns on one thing that is easy to get wrong, and the tests are
written to catch getting it wrong in either direction:

    TRAVERSAL AND LISTING ARE DIFFERENT BITS.

0751 on the partition root lets ndusr_ut reach browser/ by name while
`ls /NeoDCT/User` returns EACCES. Take the o+x away and the browser has
nowhere to write; add o+r and it can enumerate the ssh keys, the databases
and the update records by name.

The script is driven for real -- busybox ash semantics, a fake partition in
tmp_path -- rather than grepped, because the parsing of the layout table is
itself somewhere a bug could hide. Ownership cannot be checked without root,
so the tests assert modes, and assert the ownership by capturing what chown
was asked to do.
"""

import os
import re
import stat
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPT = os.path.join(REPO, "neodct", "overlay", "etc", "init.d", "S00userdata")
RUN_SH = os.path.join(REPO, "neodct", "overlay", "bin", "run_neodct.sh")


def run_layout(tmp_path, have_user=True, preexisting=None, owned_already=False):
    """Run the script against a fake /NeoDCT/User.

    chown is stubbed -- the tests do not run as a user that can hand files to
    somebody else, and what matters is what it was ASKED to do. id and find
    are stubbed too, so "is there an ndusr" and "has this run before" can be
    driven from the test rather than from the machine.
    """
    user_dir = tmp_path / "NeoDCT" / "User"
    user_dir.mkdir(parents=True)
    for name, mode in (preexisting or {}).items():
        path = user_dir / name
        if name.endswith("/"):
            path = user_dir / name.rstrip("/")
            path.mkdir(parents=True, exist_ok=True)
        else:
            path.write_text("x")
        path.chmod(mode)

    chowns = tmp_path / "chowns"
    body = open(SCRIPT).read().replace("USER_DIR=/NeoDCT/User",
                                       'USER_DIR="%s"' % user_dir)
    stubs = (
        'chown() { echo "$*" >> "%s"; }\n'
        'id() { %s; }\n'
        % (chowns, "return 0" if have_user else "return 1")
    )
    # find is used twice and the two uses have to be told apart: once as the
    # "has this already run" probe, and once as the recursive ownership pass.
    finds = tmp_path / "finds"
    stubs += (
        'find() {\n'
        '  case "$*" in\n'
        '    *-maxdepth\\ 0*) %s ;;\n'
        '    *) echo "$*" >> "%s"; echo "recursive ndusr:ndusr" >> "%s" ;;\n'
        '  esac\n'
        '}\n' % ("echo owned" if owned_already else "return 0", finds, chowns)
    )
    # The script ends in a `case "$1"` dispatch, so it is invoked with start.
    script = stubs + body
    result = subprocess.run(["sh", "-s", "start"], input=script,
                            capture_output=True, text=True)
    asked = chowns.read_text().splitlines() if chowns.exists() else []
    swept = finds.read_text().splitlines() if finds.exists() else []
    return result, user_dir, asked, swept


def mode_of(path):
    return stat.S_IMODE(os.stat(path).st_mode)


# --- the directories exist at all ----------------------------------------

def test_it_creates_every_directory_the_runtime_writes_to(tmp_path):
    _, user_dir, _, _ = run_layout(tmp_path)

    for name in ("db", "logs", ".ndsys", ".seedrng", "sdcard", "tones",
                 "wallpapers", "browser"):
        assert (user_dir / name).is_dir(), name


def test_the_browser_directory_is_created_here_not_by_the_browser(tmp_path):
    """Its OWNER is the point -- ndusr:ndusr_ut, group-writable. A directory
    the browser made for itself would belong to the browser alone, and the
    core could not read what it downloaded."""
    _, user_dir, asked, _ = run_layout(tmp_path)

    assert (user_dir / "browser").is_dir()
    assert any("ndusr:ndusr_ut" in line and "browser" in line for line in asked), asked


# --- the modes, which are the boundary -----------------------------------

def test_the_partition_root_can_be_traversed_but_not_listed(tmp_path):
    """0751. The whole design rests on this one number."""
    _, user_dir, _, _ = run_layout(tmp_path)

    assert mode_of(user_dir) == 0o751


def test_the_untrusted_directory_is_group_writable(tmp_path):
    _, user_dir, _, _ = run_layout(tmp_path)

    assert mode_of(user_dir / "browser") == 0o770


@pytest.mark.parametrize("name", ["db", "logs", "tones", "wallpapers",
                                  ".bluetooth", ".bluealsa"])
def test_the_untrusted_user_cannot_even_enter(tmp_path, name):
    """Not hidden -- entered. A path that is guessed still fails at the
    directory that denies entry, which is why o+x comes off here rather than
    r coming off the parent."""
    _, user_dir, _, _ = run_layout(tmp_path, preexisting={name + "/": 0o755})

    assert mode_of(user_dir / name) & 0o007 == 0, oct(mode_of(user_dir / name))


@pytest.mark.parametrize("name", [".ndsys", ".remote", ".seedrng"])
def test_the_three_that_are_owner_only(tmp_path, name):
    """SECURITY-AUDIT.md section 4 Q5: .remote is the reverse shell, .ndsys
    is the update that replaces the operating system, and .seedrng is the
    entropy seed, which is worthless if it is readable."""
    _, user_dir, _, _ = run_layout(tmp_path, preexisting={name + "/": 0o755})

    assert mode_of(user_dir / name) == 0o700


def test_the_card_mountpoint_stays_traversable(tmp_path):
    """A mounted card brings its own modes, but with nothing mounted the o+x
    is what lets a card path resolve at all.

    It matters more than it did. Apps the owner installed live UNDER here now,
    at sdcard/apps, so this is the first component of the path nd-apprun
    dlopens an app.so from -- take the o+x off and no installed app starts,
    and the browser has nowhere to download to either. 0751 and not 0755 for
    the usual reason: traversal and listing are different bits, and the
    listing is not owed to anybody."""
    _, user_dir, _, _ = run_layout(tmp_path)

    assert mode_of(user_dir / "sdcard") == 0o751


def test_the_partition_does_not_grow_an_apps_directory_back(tmp_path):
    """Apps were briefly at /NeoDCT/User/apps, and the reason they are not is
    arithmetic rather than taste: on the Luckfox this partition is EIGHT
    MEGABYTES, shared with the databases, the settings, the logs, the browser
    profile and the pending update record. An apps directory here is a feature
    whose success fills the partition the phone needs in order to save
    anything at all.

    Nothing reads it any more -- nd_ui_scan_apps() is pointed at
    ND_PATH_USER_APPS_DIR, which is on the card -- so an entry restored to
    this table would be a directory that quietly accumulates apps that never
    launch, on the partition that can least afford them."""
    _, user_dir, _, _ = run_layout(tmp_path)

    assert not (user_dir / "apps").exists(), (
        "S00userdata is making an apps directory on the user partition again; "
        "installed apps live at /NeoDCT/User/sdcard/apps, on the card"
    )
    assert "apps:" not in open(SCRIPT).read(), (
        "the user-partition layout table names an apps directory again"
    )


def test_loose_files_at_the_root_are_not_world_readable(tmp_path):
    """0751 stops ndusr_ut LISTING the directory, not opening a name it
    already knows -- and "settings.prop" is not a hard name to guess."""
    _, user_dir, _, _ = run_layout(
        tmp_path, preexisting={"settings.prop": 0o644, "keymap.json": 0o644})

    assert mode_of(user_dir / "settings.prop") == 0o640
    assert mode_of(user_dir / "keymap.json") == 0o640


# --- degrading, which has to work ----------------------------------------

def test_an_image_with_no_ndusr_still_gets_its_directories(tmp_path):
    """An image built without BR2_ROOTFS_USERS_TABLES must still boot. This
    is the old behaviour, unchanged."""
    result, user_dir, asked, _ = run_layout(tmp_path, have_user=False)

    assert result.returncode == 0
    assert (user_dir / "db").is_dir()
    assert asked == [], "it chowned to a user that does not exist"


def test_a_read_only_partition_is_not_fatal(tmp_path):
    """The comment this script has always carried: a partition that will not
    mount is a phone with nowhere to save, not a phone that will not boot."""
    body = open(SCRIPT).read().replace(
        "USER_DIR=/NeoDCT/User", 'USER_DIR="%s"' % (tmp_path / "nope" / "User"))
    (tmp_path / "nope").write_text("not a directory")

    result = subprocess.run(["sh", "-s", "start"], input=body,
                            capture_output=True, text=True)

    assert result.returncode == 0


# --- the one-time migration ----------------------------------------------

def test_a_partition_from_an_older_image_is_taken_over_once(tmp_path):
    """/NeoDCT/User survives every update, so a phone in the field keeps
    root-owned directories forever unless something fixes them at boot."""
    result, _, asked, swept = run_layout(tmp_path, owned_already=False)

    assert "taking ownership" in result.stderr
    assert swept, "nothing walked the partition"
    assert any("-xdev" in line for line in swept), swept
    assert any("chown ndusr:ndusr" in line for line in swept), swept


def test_a_partition_that_has_been_through_it_is_left_alone(tmp_path):
    """The recursive pass is once, not every boot."""
    result, _, _, swept = run_layout(tmp_path, owned_already=True)

    assert "taking ownership" not in result.stderr
    assert not swept, "it walked the whole partition anyway"


# --- what creates the files in the first place ---------------------------

def test_the_ui_runs_with_a_umask_that_matches_the_layout():
    """The per-boot chmod above only reaches files that already exist. What
    covers everything written afterwards is the umask, and it has to agree:
    0027 gives 0640 files and 0750 directories, which is what the layout is."""
    body = open(RUN_SH).read()

    assert re.search(r"^umask 0027$", body, re.M), "run_neodct.sh sets no umask"
    assert body.index("umask 0027") < body.index("nd-core"), "too late to matter"


# ---------------------------------------------------------------- #
# The mode of the partition root, in the three places that know it
# ---------------------------------------------------------------- #
#
# 0751 is written down in a shell table, a C header and a self-test, because
# S00userdata cannot include a header and nd-selftest must not read its
# expectations out of the thing it is checking. Three copies of a number is a
# bug waiting to happen unless something pins them together, so this does.
#
# It matters more than an ordinary duplicated constant: 0755 is the reflex,
# 0755 differs by exactly o+r, and o+r here is the whole confinement. A drift
# to 0755 breaks no test that looks for "not group-writable", starts nothing,
# logs nothing, and lets the browser enumerate the ssh keys.

PATHS_H = os.path.join(REPO, "neodct", "src", "include", "nd_paths.h")
SRC_DIR = os.path.join(REPO, "neodct", "src")


def nd_mode_user_dir():
    text = open(PATHS_H).read()
    m = re.search(r"#define\s+ND_MODE_USER_DIR\s+0([0-7]+)u", text)
    assert m, "nd_paths.h no longer defines ND_MODE_USER_DIR"
    return int(m.group(1), 8)


def s00_root_mode():
    text = open(SCRIPT).read()
    m = re.search(r'NEODCT_USER_LAYOUT="\s*\n:([0-7]+)', text)
    assert m, "S00userdata's layout table no longer starts with the root entry"
    return int(m.group(1), 8)


def test_the_header_and_the_init_script_agree():
    assert nd_mode_user_dir() == s00_root_mode(), (
        "nd_paths.h and S00userdata disagree about the mode of /NeoDCT/User"
    )


def test_the_mode_still_grants_traversal_and_withholds_listing():
    """Pinned as the two properties rather than as the number, so that a
    deliberate change has to argue with the design and not just with a
    constant."""
    mode = nd_mode_user_dir()
    assert mode & stat.S_IXOTH, (
        "o+x is gone: ndusr_ut cannot reach /NeoDCT/User/browser, and the "
        "browser has nowhere to write"
    )
    assert not mode & stat.S_IROTH, (
        "o+r is set: ndusr_ut can list /NeoDCT/User and find .remote, .ndsys "
        "and db by name"
    )
    assert not mode & (stat.S_IWGRP | stat.S_IWOTH), (
        "group- or world-writable: sshd refuses authorized_keys underneath it"
    )


def test_nothing_in_the_c_tree_chmods_the_partition_to_a_literal():
    """The regression guard for the bug this test was written after.

    nd_rs_start() repaired a loose partition by chmod'ing it to 0755 -- which
    satisfies "not group-writable", which is what it was checking for, and
    silently grants the o+r that the whole boundary is the absence of. Any
    future repair path is going to be written by someone reaching for the
    same reflex, so the rule is: chmod ND_PATH_USER only to ND_MODE_USER_DIR.
    """
    offenders = []
    for root, _dirs, files in os.walk(SRC_DIR):
        if os.sep + "build" in root or os.sep + "test" in root:
            continue
        for name in files:
            if not name.endswith(".c"):
                continue
            path = os.path.join(root, name)
            text = open(path, errors="replace").read()
            # chmod(<anything>, 0<digits>) within four lines of ND_PATH_USER:
            # close enough to catch the shape without matching every chmod in
            # the tree, which are mostly on files this says nothing about.
            for m in re.finditer(r"chmod\s*\([^;]*?,\s*(0[0-7]{3,4})\s*\)", text):
                start = text.rfind("\n", 0, max(0, m.start() - 400))
                window = text[max(0, start):m.end()]
                if "ND_PATH_USER" in window:
                    offenders.append(
                        "%s: chmod to %s near ND_PATH_USER -- use ND_MODE_USER_DIR"
                        % (os.path.relpath(path, REPO), m.group(1))
                    )
    assert not offenders, "\n".join(offenders)

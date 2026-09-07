"""CARD_LAYOUT is the storage half of the confinement, and it is RESTATED ON
EVERY MOUNT rather than written once when the card is formatted.

That last part is the whole reason this file exists. A card is removable and,
since 0.5.0b, it is ext4 -- so it can be carried to any Linux PC, where the
owner can `chmod -R 0777` the lot, deliberately or by copying a tree across
with the wrong umask. The modes on a card are therefore a claim about the
past, not a fact about the present, and everything the phone believes about an
installed app rests on them:

    apps/<Name>         0755 ndusr:ndusr    the app may read and execute its
                                            own app.so and may not write it
    apps/<Name>/data    0770 ndusr:ndusr_ut the one thing under apps/ the app
                                            may write, made by root because
                                            an app that could create its own
                                            data directory could create
                                            SIBLINGS beside its code instead
    untrusted           0770 ndusr:ndusr_ut where a download lands
    music, wallpapers,  0750 ndusr:ndusr    the owner's, and unreadable to
    tones, backup_db,                       anything untrusted -- which on
    update                                  the old FAT card could not be
                                            expressed at all
    the card root       0751 ndusr:ndusr    traverse but do not list, exactly
                                            as /NeoDCT/User does

So neodct-sdcard's apply_layout() runs after every mount of one of its own
cards, for the same reason S00userdata reasserts the user partition on every
boot. A card off a PC comes back to the layout, or none of the rest is true.

Two halves, and they are tested differently:

  the modes         driven for real against a directory tree, because the
                    parsing of the layout table is itself somewhere a bug can
                    hide. chown is stubbed and what it was ASKED to do is
                    asserted -- the same trick test_userdata_layout.py uses,
                    for the same reason: these tests must run without root.

  the confinement   the modes only matter if they stop anything, so where the
                    machine has root and a real ndusr_ut, the tests below drop
                    to that uid and try. Skipped, loudly, where it cannot --
                    a confinement test that quietly did not run is worse than
                    one that fails.
"""

import errno
import os
import pwd
import shutil
import stat
import subprocess

import pytest

HELPER = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "overlay", "NeoDCT", "System", "hw", "neodct-sdcard",
)

# Everything the helper creates on a card of its own (FOLDERS), plus the
# entries CARD_LAYOUT gives a mode to. Spelled out here rather than parsed out
# of the script, because a test that reads its expectations from the thing it
# is checking cannot notice the thing changing.
MEDIA = ("music", "wallpapers", "tones", "backup_db", "update")
FOLDERS = MEDIA + ("apps", "untrusted")


def users_exist():
    for name in ("ndusr", "ndusr_ut"):
        try:
            pwd.getpwnam(name)
        except KeyError:
            return False
    return True


# The confinement half needs root -- to hand a directory to somebody else and
# then to become them -- and needs the two users to exist in /etc/passwd at
# all. Both are facts about the machine, so they skip with a reason rather
# than failing.
NEEDS_USERS = pytest.mark.skipif(
    os.geteuid() != 0 or not users_exist(),
    reason="needs root and an ndusr/ndusr_ut in /etc/passwd to try the layout "
           "as the untrusted user actually experiences it",
)


# The stub every case below records chowns with. It DROPS a leading -h, which
# apply_layout() passes on every chown: -h is about symlinks and none of the
# assertions here are, so recording it would only mean four places to edit the
# next time the flag changes. What -h is for, and why it must not go away, is
# test_every_chown_in_the_layout_refuses_to_follow_a_link().
def chown_stub(path):
    return 'chown() { [ "$1" = -h ] && shift; echo "$*" >> "%s"; }\n' % path


def sh(tmp_path, mount, body, stubs="", env=None):
    """Source the helper with MOUNT_POINT pointed at `mount`, and run `body`.

    `env` adds to the environment the script is given -- the only user of it
    is apply_layout_here(), which needs a CARD_USER this machine has."""
    (tmp_path / "cmdline").write_text("neodct.sys=/dev/vda neodct.user=/dev/vdb\n")
    (tmp_path / "mounts").write_text("")
    (tmp_path / "boot_state").write_text("")
    child_env = dict(os.environ,
                     NEODCT_SDCARD_SOURCE_ONLY="1",
                     NEODCT_CMDLINE=str(tmp_path / "cmdline"),
                     NEODCT_MOUNTS=str(tmp_path / "mounts"),
                     NEODCT_BOOT_STATE=str(tmp_path / "boot_state"),
                     NEODCT_RUN_DIR=str(tmp_path / "run"),
                     NEODCT_SDCARD_MOUNT=str(mount))
    child_env.update(env or {})
    script = '. "%s"\n%s\n%s\n' % (HELPER, stubs, body)
    return subprocess.run(["sh", "-c", script], capture_output=True, text=True,
                          env=child_env)


def build_card(mount, mode=0o777, apps=("Demo",), folders=FOLDERS):
    """A card as a PC would leave one: the right tree, the wrong modes.

    0777 by default, because that is what an owner gets by copying a directory
    across with a loose umask, and it is the exact shape apply_layout() exists
    to undo. The app carries an app.so and a manifest, and a data/ with
    something already in it -- an app that has run once before."""
    mount.mkdir(parents=True, exist_ok=True)
    for name in folders:
        (mount / name).mkdir(exist_ok=True)
    for name in apps:
        appdir = mount / "apps" / name
        appdir.mkdir(parents=True, exist_ok=True)
        (appdir / "app.so").write_bytes(b"\x7fELF not really")
        (appdir / "manifest.json").write_text('{"name": "%s"}\n' % name)
        (appdir / "data").mkdir(exist_ok=True)
        (appdir / "data" / "state.json").write_text("{}\n")
    # Modes last: mkdir applies the umask, and the point is a card whose modes
    # are wrong in the widest possible way.
    for root, dirs, files in os.walk(mount):
        for name in dirs + files:
            os.chmod(os.path.join(root, name), mode)
    mount.chmod(mode)
    return mount


def apply_layout(tmp_path, mount, real_chown=False):
    """Run apply_layout() over `mount`; return the chowns it asked for.

    chown is stubbed by default: these tests do not run as a user that can
    hand a file to somebody else, and what it was ASKED to do is the assertion
    either way. real_chown=True is for the root-only cases below, which need
    the ownership to actually land."""
    chowns = tmp_path / "chowns"
    stubs = "" if real_chown else chown_stub(chowns)
    result = sh(tmp_path, mount, "apply_layout", stubs=stubs)
    assert result.returncode == 0, result.stderr
    asked = chowns.read_text().splitlines() if chowns.exists() else []
    return asked


def mode_of(path):
    return stat.S_IMODE(os.stat(path).st_mode)


# --- the modes, restated -------------------------------------------------

def test_a_card_written_on_a_pc_comes_back_to_the_layout(tmp_path):
    """THE LOAD-BEARING PROPERTY OF REMOVABLE EXT4 MEDIA.

    Every mode below arrives as 0777, which is what a card that has been in a
    PC can look like, and every one of them is put back. If this test fails,
    an installed app can rewrite its own code and read the owner's music, and
    nothing else in the suite would notice: the card would still mount, the
    app would still start, and the phone would report a perfectly good card."""
    card = build_card(tmp_path / "sdcard", mode=0o777)

    apply_layout(tmp_path, card)

    assert mode_of(card) == 0o751, "the card root is listable"
    assert mode_of(card / "apps") == 0o755
    assert mode_of(card / "untrusted") == 0o770
    for name in MEDIA:
        assert mode_of(card / name) == 0o750, name


@pytest.mark.parametrize("mode", [0o777, 0o700, 0o000, 0o755])
def test_it_does_not_matter_what_the_card_arrived_as(tmp_path, mode):
    """Wide open, shut, or plausible-looking. apply_layout() states the modes
    rather than repairing the ones it does not like, so there is no shape of
    wrongness it declines to fix -- 0000 included, which is the one a
    "loosen it if it is too tight" implementation would leave alone."""
    card = build_card(tmp_path / "sdcard", mode=mode)

    apply_layout(tmp_path, card)

    assert mode_of(card) == 0o751
    assert mode_of(card / "untrusted") == 0o770
    assert mode_of(card / "music") == 0o750


def test_an_app_directory_and_its_code_are_taken_back_to_read_only(tmp_path):
    """0755 on the directory and 0644 on everything in it, so app.so is read
    and execute for the app and write for nobody but the owner.

    This is the whole of "an app cannot rewrite itself", and it is why
    persistence needs the owner to install something rather than an app to
    decide to stay."""
    card = build_card(tmp_path / "sdcard", mode=0o777)

    apply_layout(tmp_path, card)

    appdir = card / "apps" / "Demo"
    assert mode_of(appdir) == 0o755
    assert mode_of(appdir / "app.so") == 0o644
    assert mode_of(appdir / "manifest.json") == 0o644


def test_the_data_directory_is_made_here_and_not_by_the_app(tmp_path):
    """An app that could create its own data directory could create SIBLINGS
    instead -- another .so next to the one the launcher loads. So root makes
    it at mount time, and the app only ever writes inside a directory it did
    not make.

    It also has to BELONG to somebody else: 0770 ndusr:ndusr_ut, and changing
    a file's owner needs CAP_CHOWN, which is exactly the privilege the core
    gave up. This script runs from udev as root and can."""
    card = build_card(tmp_path / "sdcard", mode=0o777)
    # An app that has never run: no data directory at all.
    for entry in (card / "apps" / "Demo" / "data").iterdir():
        entry.unlink()
    (card / "apps" / "Demo" / "data").rmdir()

    asked = apply_layout(tmp_path, card)

    data = card / "apps" / "Demo" / "data"
    assert data.is_dir(), "the app would have had to create this itself"
    assert mode_of(data) == 0o770
    assert any(line.split()[0] == "ndusr:ndusr_ut" and line.endswith("/data")
               for line in asked), asked


def test_what_the_app_put_inside_data_is_left_alone(tmp_path):
    """The contents are the app's. A blanket chmod on every mount would take
    back the one directory the app was given, and would do it silently on the
    next insertion of the card."""
    card = build_card(tmp_path / "sdcard", mode=0o777)
    private = card / "apps" / "Demo" / "data" / "state.json"
    private.chmod(0o600)
    (card / "apps" / "Demo" / "data" / "sub").mkdir()
    (card / "apps" / "Demo" / "data" / "sub").chmod(0o700)

    apply_layout(tmp_path, card)

    assert mode_of(private) == 0o600
    assert mode_of(card / "apps" / "Demo" / "data" / "sub") == 0o700


def test_every_installed_app_gets_the_same_treatment(tmp_path):
    """One loop, no first-app special case. An app installed second is exactly
    as untrusted as the first one."""
    card = build_card(tmp_path / "sdcard", mode=0o777,
                      apps=("Demo", "Second", "Third"))

    apply_layout(tmp_path, card)

    for name in ("Demo", "Second", "Third"):
        assert mode_of(card / "apps" / name) == 0o755, name
        assert mode_of(card / "apps" / name / "app.so") == 0o644, name
        assert mode_of(card / "apps" / name / "data") == 0o770, name


# --- the ownership, which the modes are meaningless without ---------------

def test_the_ownership_asked_for_is_the_layouts(tmp_path):
    """0770 means nothing without a group, and the two groups here are
    different on purpose: untrusted/ and data/ are ndusr:ndusr_ut so the
    untrusted set can write them, everything else is ndusr:ndusr so it
    cannot."""
    card = build_card(tmp_path / "sdcard", mode=0o777)

    asked = apply_layout(tmp_path, card)

    def owner_of(path):
        # Paths are normalised on both sides: the app loop iterates a glob
        # ending in "/", so the directory arrives as ".../Demo/" and its data
        # directory as ".../Demo//data". Harmless to chown, and not something
        # to assert about either way.
        want = os.path.normpath(str(path))
        for line in asked:
            who, _, target = line.partition(" ")
            if os.path.normpath(target) == want:
                return who
        return None

    assert owner_of(card) == "ndusr:ndusr", asked
    assert owner_of(card / "apps") == "ndusr:ndusr", asked
    assert owner_of(card / "untrusted") == "ndusr:ndusr_ut", asked
    for name in MEDIA:
        assert owner_of(card / name) == "ndusr:ndusr", (name, asked)
    assert owner_of(card / "apps" / "Demo") == "ndusr:ndusr", asked
    assert owner_of(card / "apps" / "Demo" / "app.so") == "ndusr:ndusr", asked
    assert owner_of(card / "apps" / "Demo" / "data") == "ndusr:ndusr_ut", asked


def test_nothing_is_chowned_to_a_user_the_image_may_not_have(tmp_path):
    """An image built without BR2_ROOTFS_USERS_TABLES has neither user, and
    the helper takes both names from the environment for exactly that reason.
    A hard-coded name anywhere in the layout would be a card the phone
    silently failed to lay out on such an image."""
    card = build_card(tmp_path / "sdcard", mode=0o777)
    chowns = tmp_path / "chowns"
    result = sh(tmp_path, card, "apply_layout",
                stubs=chown_stub(chowns),
                )
    assert result.returncode == 0, result.stderr

    for line in chowns.read_text().splitlines():
        who = line.split()[0]
        assert who in ("ndusr:ndusr", "ndusr:ndusr_ut"), line


# --- what a PERSON put on the card, and what root will not reach -----------

def apply_layout_here(tmp_path, mount):
    """apply_layout() with CARD_USER naming a user this machine actually has.

    The media loop ends `find ... ! -user "$CARD_USER"`, and find refuses a
    user name that is not in /etc/passwd. That refusal is the last command in
    apply_layout(), so on a build host with no ndusr the function returns
    non-zero however well it did its job -- which is why every case above
    fails on such a machine and these do not. NEODCT_CARD_USER exists for
    exactly this (see the definition beside CARD_USER), the names are not what
    these three cases assert, and a test that cannot run is not a net."""
    chowns = tmp_path / "chowns"
    me = pwd.getpwuid(os.geteuid()).pw_name
    env = {"NEODCT_CARD_USER": me, "NEODCT_CARD_USER_UT": me}
    result = sh(tmp_path, mount, "apply_layout",
                stubs=chown_stub(chowns), env=env)
    assert result.returncode == 0, result.stderr
    return result


def test_a_game_folder_a_person_made_is_traversable_by_the_app(tmp_path):
    """THE DISC THE EMULATOR COULD NOT SEE.

    A PlayStation image is 600 MB and nd_nap.h caps a packaged file at 64 MiB,
    so the documented way to get one onto the card is to copy it there. The
    owner made apps/PSX/games/DigimonWorld/ from the root shell, put the .bin
    inside it and chowned the FILE. Under run_neodct.sh's umask 0027 a
    directory made that way is 0750 root:root; PCSX-ReARMed runs as ndusr_ut,
    which is "other" in an ndusr:ndusr tree, so opendir() returned EACCES,
    psx_library_scan() returned 0, and the screen said "No discs" with the
    disc sitting right there.

    An installed app's content is 0755 on directories and 0644 on files, owned
    by ndusr -- nd_nap.c writes 0644 and says "these are the modes
    apply_layout() will restate", and a directory has to carry +x as well or
    the app cannot reach what is inside it."""
    card = build_card(tmp_path / "sdcard", mode=0o755, apps=("PSX",))
    games = card / "apps" / "PSX" / "games" / "DigimonWorld"
    games.mkdir(parents=True)
    disc = games / "DigimonWorld.bin"
    disc.write_bytes(b"\x00" * 16)
    # Exactly what the root shell leaves behind, one level at a time.
    for path in (games.parent, games):
        path.chmod(0o750)
    disc.chmod(0o640)

    apply_layout_here(tmp_path, card)

    assert mode_of(games.parent) == 0o755, "games/ itself has to be traversable"
    assert mode_of(games) == 0o755, "and so does the folder the disc is in"
    assert mode_of(disc) == 0o644


def test_the_walk_still_stops_at_the_apps_own_data(tmp_path):
    """The other half of the same walk, and the half that must not move.

    data/ is 0770 ndusr:ndusr_ut -- the one subtree under apps/ the untrusted
    set can write -- so a root chown or chmod inside it is a root chown at a
    path an attacker chooses. -prune is what keeps it out, and widening the
    walk to reach a person's game folder must not widen it to reach here."""
    card = build_card(tmp_path / "sdcard", mode=0o755, apps=("PSX",))
    data = card / "apps" / "PSX" / "data"
    (data / "sub").mkdir()
    (data / "sub").chmod(0o700)
    (data / "state.json").chmod(0o600)

    apply_layout_here(tmp_path, card)

    assert mode_of(data) == 0o770, "the directory itself is still stated"
    assert mode_of(data / "sub") == 0o700, "and nothing inside it is"
    assert mode_of(data / "state.json") == 0o600


def test_root_touches_only_packages_in_the_arrival_area(tmp_path):
    """untrusted/ is 0770 ndusr:ndusr_ut -- THE ONE DIRECTORY on the card the
    untrusted set can write -- so every path root walks in here is a path an
    attacker can replace with a symlink while the walk is running. chown(1)
    and chmod(1) both dereference, and `-exec ... +` runs the batch after the
    walk, so the window is the whole of it.

    The answer is reach: one `chown -h` of *.nap at one level, because
    Settings' installer is the only reader that needs anything repaired here
    and nd_nap_find() looks exactly that far. Everything else in untrusted/
    belongs to ndusr_ut and is read by ndusr_ut -- the browser's state, the
    media player's -- and root taking it bought nobody anything.

    The chmod is gone rather than made safe: busybox chmod has no -h, and
    chown alone already makes ndusr the OWNER of a package, which is what the
    installer needs."""
    card = build_card(tmp_path / "sdcard", mode=0o755)
    untrusted = card / "untrusted"
    package = untrusted / "Bible.nap"
    package.write_text("nap")
    package.chmod(0o640)
    cookies = untrusted / "cookies.db"
    cookies.write_text("{}")
    cookies.chmod(0o600)
    deeper = untrusted / "browser"
    deeper.mkdir()
    deeper.chmod(0o700)
    buried = deeper / "Buried.nap"
    buried.write_text("nap")
    buried.chmod(0o600)

    apply_layout_here(tmp_path, card)

    assert mode_of(untrusted) == 0o770, "the directory itself comes from CARD_LAYOUT"
    assert mode_of(package) == 0o640, "no chmod: the chown is the whole repair"
    assert mode_of(cookies) == 0o600, "the browser's own state is not root's business"
    assert mode_of(deeper) == 0o700, "nor is a directory the browser made"
    assert mode_of(buried) == 0o600, "and nd_nap_find() never looks this deep anyway"


def test_every_chown_in_the_layout_refuses_to_follow_a_link():
    """The half of the fix above that a test cannot watch happen.

    Whether root followed a symlink is only visible from root, and the race
    that makes it reachable cannot be reproduced on demand -- so what is
    pinned instead is the flag. A chown without -h inside apply_layout() is
    the bug that held 0.5.15a, and it is one character away from coming back.
    busybox takes -h unconditionally (CONFIG_CHOWN, not behind
    FEATURE_CHOWN_LONG_OPTIONS), so there is no image where this costs
    anything."""
    body = open(HELPER).read().split("\napply_layout() {", 1)[1].split("\n}\n", 1)[0]
    calls = [line.strip() for line in body.splitlines()
             if "chown " in line and not line.lstrip().startswith("#")]

    assert calls, "apply_layout() chowns nothing at all -- has it moved?"
    for line in calls:
        assert "chown -h " in line, line


def test_the_apps_walk_chmods_before_it_descends():
    r"""THE HALF THE -h COULD NOT COVER.

    busybox chmod has no -h, so the only defence a chmod has is never to be
    pointed at a name that can change under it. `-exec chmod {} +` batches and
    the batch runs AFTER the whole walk, so between find's lstat deciding a
    path is -type f and root chmodding it there is the length of the walk in
    which it can be replaced by a symlink -- the untrusted/ hole, in the apps/
    loop.

    The release note claimed apps/ was safe because it prunes data/. That is
    true of a card this phone has already normalised and of no other: a card
    written on a PC arrives with the PC's modes, and apps/PSX/games/ at 0777
    is the exact case the recursion was added for. So the chmods are `\;` --
    run immediately, while find is standing on the entry, and BEFORE find
    descends into a directory it has just taken back to 0755 ndusr:ndusr.

    Pinned as source rather than behaviour for the reason the chown case
    above gives: the race cannot be reproduced on demand, and what must not
    come back is the one character that opens it."""
    body = open(HELPER).read().split("\napply_layout() {", 1)[1].split("\n}\n", 1)[0]
    execs = [line.strip() for line in body.splitlines()
             if "-exec chmod" in line and not line.lstrip().startswith("#")]

    assert execs, "the apps/ walk chmods nothing at all -- has it moved?"
    for line in execs:
        assert "+" not in line, (
            "a batched -exec chmod runs after the walk that collected it: " + line)
        assert "\\;" in line, line


def test_the_card_marker_is_never_written_through_a_link():
    """The two sites OUTSIDE apply_layout() that write a name on the card.

    `: > "$MOUNT_POINT/.neodct"` follows a symlink and truncates whatever it
    points at, as root, and the chown beside it followed one too. The card
    root is 0751 ndusr:ndusr so nothing untrusted can plant that link -- this
    is a card written on a PC, and mostly it is about the file not
    contradicting its own rule three hundred lines further down.

    The link is REMOVED rather than skipped: without a marker the phone does
    not recognise its own card, so refusing to write one would be refusing the
    card."""
    lines = open(HELPER).read().splitlines()
    writes = [i for i, line in enumerate(lines)
              if line.strip().startswith(': > "$MOUNT_POINT/$CARD_MARKER"')]

    assert len(writes) == 2, "the marker is written at two places; found %d" % len(writes)
    for i in writes:
        before = lines[i - 1].strip()
        assert before.startswith('[ -L "$MOUNT_POINT/$CARD_MARKER" ]') and "rm -f" in before, \
            "no -L guard above the truncate at line %d: %r" % (i + 1, before)

    chowns = [line.strip() for line in lines if "CARD_MARKER" in line and "chown" in line]
    assert len(chowns) == 2, chowns
    for line in chowns:
        assert "chown -h " in line, line


# --- the shape of the card the layout is applied to ------------------------

def test_every_folder_the_helper_creates_has_a_mode_of_its_own(tmp_path):
    """FOLDERS and CARD_LAYOUT are two lists and they have to agree.

    A folder created by after_mount() with no entry in the layout keeps
    whatever mkdir gave it -- root's umask, so 0755, so world-readable. On a
    directory holding the owner's music that is a quiet regression with no
    symptom, which is the kind this pins."""
    listed = sh(tmp_path, tmp_path / "sdcard", 'echo $FOLDERS').stdout.split()
    layout = sh(tmp_path, tmp_path / "sdcard",
                'printf "%s\\n" "$CARD_LAYOUT"').stdout.splitlines()
    named = set(line.split(":")[0] for line in layout if line.strip())

    assert set(listed) == set(FOLDERS), listed
    for folder in listed:
        assert folder in named, "%s is created but never given a mode" % folder


def test_a_folder_the_card_does_not_have_is_not_conjured_up(tmp_path):
    """apply_layout() states modes; after_mount() creates directories. Keeping
    them apart is what lets the layout be applied to a card mid-mount without
    it deciding to write to somebody's filesystem."""
    card = build_card(tmp_path / "sdcard", mode=0o755,
                      folders=("apps", "untrusted"))

    asked = apply_layout(tmp_path, card)

    for name in MEDIA:
        assert not (card / name).exists(), name
    assert not any(name in line for line in asked for name in MEDIA), asked


def test_an_empty_apps_directory_is_not_an_error(tmp_path):
    """The normal case: a card with no apps installed on it. The glob matches
    nothing and the loop has to notice, rather than chmod'ing a directory
    literally called "*"."""
    card = build_card(tmp_path / "sdcard", mode=0o777, apps=())

    apply_layout(tmp_path, card)

    assert mode_of(card / "apps") == 0o755
    assert not (card / "apps" / "*").exists()


# --- the verb the core asks for after an install ---------------------------
#
# apply_layout() used to run only from after_mount(). Installing a .nap
# happens on a card that is already mounted, as ndusr, and the one thing
# ndusr cannot do is give the new app's data/ to ndusr_ut -- so the core asks
# the helper, through the broker, for `layout`. These pin what that verb
# does and, more importantly, when it refuses to do anything.


def layout_verb(tmp_path, mount, mounts_line, marker=True):
    """Run do_layout() with the mount table saying `mounts_line`."""
    chowns = tmp_path / "chowns"
    # sh() writes an EMPTY mount table on every call, and the verb's first
    # question is what that table says is mounted -- so it is pointed at a
    # table of this test's own, after the helper has been sourced.
    table = tmp_path / "mounts.layout"
    table.write_text(mounts_line)
    stubs = (chown_stub(chowns) +
             'MOUNTS="%s"\n' % table)
    if marker:
        (mount / ".neodct").write_text("")
    result = sh(tmp_path, mount, "do_layout", stubs=stubs)
    asked = chowns.read_text().splitlines() if chowns.exists() else []
    return result, asked


def test_layout_restates_a_mounted_neodct_card(tmp_path):
    """The case the verb exists for: a card that is mounted, ext4 and ours,
    with an app on it that was just unpacked and has no data/ yet."""
    card = build_card(tmp_path / "sdcard", mode=0o777, apps=("Fresh",))
    shutil.rmtree(card / "apps" / "Fresh" / "data")
    line = "/dev/mmcblk1p1 %s ext4 rw,nosuid,nodev 0 0\n" % card

    result, asked = layout_verb(tmp_path, card, line)

    assert result.returncode == 0, result.stderr
    assert (card / "apps" / "Fresh" / "data").is_dir()
    assert mode_of(card / "apps" / "Fresh" / "data") == 0o770
    assert mode_of(card / "apps" / "Fresh" / "app.so") == 0o644
    assert mode_of(card / "apps") == 0o755
    # The glob apply_layout() walks leaves a trailing slash on the app
    # directory, so the path it names has a doubled one; the kernel does not
    # care and neither does this.
    data = str(card / "apps" / "Fresh" / "data")
    assert any(a.startswith("ndusr:ndusr_ut ") and a.split(" ", 1)[1].replace("//", "/") == data
               for a in asked), asked


def test_layout_refuses_when_nothing_is_mounted(tmp_path):
    """No card in the slot: exit 1, and not one chown -- the caller is told
    "not applicable" rather than "done"."""
    card = build_card(tmp_path / "sdcard", mode=0o777)

    result, asked = layout_verb(tmp_path, card, "")

    assert result.returncode == 1
    assert asked == []
    assert mode_of(card / "apps") == 0o777


def test_layout_refuses_a_fat_card(tmp_path):
    """A legacy card mounts and plays music, and FAT cannot hold the layout.
    Nothing is attempted: chmod on FAT is a no-op that reads as success."""
    card = build_card(tmp_path / "sdcard", mode=0o777)
    line = "/dev/mmcblk1p1 %s vfat rw 0 0\n" % card

    result, asked = layout_verb(tmp_path, card, line)

    assert result.returncode == 1
    assert asked == []


def test_layout_leaves_a_strangers_card_alone(tmp_path):
    """ext4, mounted, and not ours: no marker and not the folder set. The
    phone does not take ownership of somebody else's files because an app
    asked it to."""
    card = tmp_path / "sdcard"
    card.mkdir()
    (card / "DCIM").mkdir()
    (card / "apps").mkdir()
    line = "/dev/mmcblk1p1 %s ext4 rw 0 0\n" % card

    result, asked = layout_verb(tmp_path, card, line, marker=False)

    assert result.returncode == 1
    assert asked == []


# --- and what all of that is FOR ------------------------------------------
# Everything above says what the modes are. These say what the modes DO, by
# becoming ndusr_ut and trying -- which is the only way to be sure that a
# layout that reads correctly also behaves correctly.


def make_reachable(path):
    """Give every directory from /tmp down to `path` o+x.

    Not part of the layout and not pretending to be: pytest's tmp_path lives
    under a 0700 directory owned by root, so without this the child is stopped
    by the harness rather than by the card, and every test below would pass
    for the wrong reason."""
    path = os.path.abspath(str(path))
    while path != "/":
        mode = stat.S_IMODE(os.stat(path).st_mode)
        if not mode & stat.S_IXOTH:
            os.chmod(path, mode | stat.S_IXOTH)
        path = os.path.dirname(path)


def as_untrusted(fn):
    """Run fn() as ndusr_ut, in a child, and report what happened.

    Returns "OK" or "ERR <errno name>". A fork rather than a thread because a
    uid is per process, and os._exit() rather than a return because the child
    is a copy of the pytest process and must not run its teardown."""
    ut = pwd.getpwnam("ndusr_ut")
    read_fd, write_fd = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(read_fd)
        try:
            os.setgroups([ut.pw_gid])
            os.setgid(ut.pw_gid)
            os.setuid(ut.pw_uid)
            fn()
            os.write(write_fd, b"OK")
        except OSError as exc:
            os.write(write_fd, ("ERR %s" % errno.errorcode.get(
                exc.errno, str(exc.errno))).encode())
        except BaseException as exc:  # noqa: BLE001 -- reported, not handled
            os.write(write_fd, ("ERR %s" % type(exc).__name__).encode())
        finally:
            os.close(write_fd)
        os._exit(0)
    os.close(write_fd)
    answer = os.read(read_fd, 256).decode()
    os.close(read_fd)
    os.waitpid(pid, 0)
    return answer


@pytest.fixture
def real_card(tmp_path):
    """A card laid out for real: real chown, real chmod, real users."""
    card = build_card(tmp_path / "sdcard", mode=0o777)
    make_reachable(card)
    apply_layout(tmp_path, card, real_chown=True)
    return card


@NEEDS_USERS
def test_an_app_cannot_rewrite_its_own_code(real_card):
    """app.so is 0644 ndusr:ndusr and the app is ndusr_ut, so the app is
    "other" against its own program: read, execute, and no more.

    This is the property PentestPersist probes for on the phone. Without it,
    "uninstall" means removing a directory an app can put back, and an app
    that has run once can decide to keep running."""
    target = real_card / "apps" / "Demo" / "app.so"

    def overwrite():
        with open(target, "r+b") as handle:
            handle.write(b"owned")

    assert as_untrusted(overwrite) == "ERR EACCES"
    assert target.read_bytes().startswith(b"\x7fELF"), "it was rewritten anyway"


@NEEDS_USERS
def test_an_app_cannot_drop_a_second_so_beside_its_own(real_card):
    """"Cannot overwrite app.so" would be worth nothing on its own: the
    directory is what the launcher reads, so an app that could add a file to
    it could leave a second library for something else to load. 0755
    ndusr:ndusr says no to that as well, and it is the same bit."""
    sibling = real_card / "apps" / "Demo" / "evil.so"

    def create():
        open(sibling, "wb").write(b"\x7fELF")

    assert as_untrusted(create) == "ERR EACCES"
    assert not sibling.exists()


@NEEDS_USERS
def test_an_app_cannot_remove_its_own_directory_either(real_card):
    """Unlinking a file is a write to the DIRECTORY, not to the file, which is
    the mistake that makes "the file is 0644" feel like enough. apps/ is 0755
    ndusr:ndusr, so an app cannot uninstall itself -- or anything else."""
    def unlink_the_code():
        os.unlink(str(real_card / "apps" / "Demo" / "app.so"))

    def unlink_the_app():
        os.rmdir(str(real_card / "apps" / "Demo"))

    assert as_untrusted(unlink_the_code) == "ERR EACCES"
    assert as_untrusted(unlink_the_app) == "ERR EACCES"


@NEEDS_USERS
def test_an_app_can_write_its_own_data(real_card):
    """The other half, and it has to be asserted or the tests above prove
    nothing.

    A probe that comes back with every line BLOCKED has not demonstrated
    confinement; it has demonstrated that the process could not do anything in
    the first place. This is the same process, writing where it is meant to
    write, so the refusals above are refusals rather than a broken setup."""
    landed = real_card / "apps" / "Demo" / "data" / "written-by-the-app"

    def write():
        open(landed, "w").write("mine\n")

    assert as_untrusted(write) == "OK"
    assert landed.read_text() == "mine\n"


@NEEDS_USERS
def test_the_untrusted_set_cannot_read_the_owners_media(real_card):
    """0750 ndusr:ndusr. The media folders are the owner's music, wallpapers,
    ringtones, contact backups and update packages, and ndusr_ut is neither
    the owner nor in the group -- so it is "other", and other has nothing.

    On the old FAT card this could not be said at all: dmask applied to every
    directory on the filesystem at once, so the arrival side and the music
    were the same regime and the split needed a second partition to exist."""
    (real_card / "music" / "private.mp3").write_bytes(b"ID3")
    os.chown(str(real_card / "music" / "private.mp3"),
             pwd.getpwnam("ndusr").pw_uid, pwd.getpwnam("ndusr").pw_gid)
    os.chmod(str(real_card / "music" / "private.mp3"), 0o640)

    assert as_untrusted(lambda: os.listdir(str(real_card / "music"))) == "ERR EACCES"
    assert as_untrusted(
        lambda: open(real_card / "music" / "private.mp3", "rb").read()) == "ERR EACCES"


@NEEDS_USERS
def test_the_untrusted_set_cannot_list_the_card_root(real_card):
    """0751: traverse but do not list, the same number and the same reasoning
    as /NeoDCT/User itself.

    Take the o+x away and a download has nowhere to go, because the path to
    untrusted/ stops resolving. Add the o+r -- which is what 0755 does, and
    0755 is the reflex -- and an untrusted process can enumerate every app
    installed on the phone and every folder the owner keeps, which is the
    inventory step of anything worse."""
    assert as_untrusted(lambda: os.listdir(str(real_card))) == "ERR EACCES"
    # ...and the traverse still works, or the listing refusal above would be
    # indistinguishable from a card nothing can reach.
    assert as_untrusted(lambda: os.stat(str(real_card / "untrusted"))) == "OK"


@NEEDS_USERS
def test_a_download_can_land_in_untrusted(real_card):
    """0770 ndusr:ndusr_ut -- write by GROUP, which is what makes this the one
    directory on the card the untrusted set may add to, and the reason the
    card root is 0751 rather than 0750.

    ndusr owns it so the core can read back what arrived, because installing
    something downloaded is an explicit copy the owner performs through the
    UI, and the UI is ndusr. A group it could not read would be a download
    nobody can ever do anything with."""
    landed = real_card / "untrusted" / "download.ndsw"

    def write():
        open(landed, "wb").write(b"arrived")

    assert as_untrusted(write) == "OK"
    assert landed.read_bytes() == b"arrived"


@NEEDS_USERS
def test_an_app_cannot_reach_the_owners_media_through_its_own_data(real_card):
    """The paths cross: data/ is writable and music/ is not, and both are on
    the same filesystem. A symlink is the obvious way to try to make one
    stand in for the other -- and it does not work, because the check is on
    the TARGET's directory and not on the name used to reach it."""
    link = real_card / "apps" / "Demo" / "data" / "shortcut"
    os.symlink(str(real_card / "music"), str(link))

    assert as_untrusted(lambda: os.listdir(str(link))) == "ERR EACCES"

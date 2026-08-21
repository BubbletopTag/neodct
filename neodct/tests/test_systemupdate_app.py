"""SystemUpdate app policy.

Which refusal appears, what its softkey says, and above all which ones
engineering mode is allowed to walk past: that is a specification, not an
implementation detail, so the widgets are recorded here rather than drawn.
What the screens *look like* is tested next door in test_update_ui.py (the
widgets themselves) and test_update_flow.py (whole flows, on real pixels).

The last test is the whole feature end to end: the app stages a package and
the real busybox applier installs it.
"""

import json
import os
import subprocess

import pytest
from PIL import Image, ImageDraw, ImageFont

from System.apps.Update import main as app
from System.core import Storage
from System.core.UpdateService import staging

from update_fixtures import build_image, make_ndsw, png, sign, write_public_key

APPLY_SH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "initramfs", "ndsys-apply.sh",
)

ENTER, BACK = 28, 14


class FakeUI:
    """Just enough UI for a widget to be constructed against."""

    def __init__(self):
        self.canvas = Image.new("RGB", (240, 175))
        self.draw = ImageDraw.Draw(self.canvas)
        self.font_n = self.font_md = self.font_s = ImageFont.load_default()
        self.fb = self
        self.engineering_mode = True
        self.frames = 0

    def update(self, image):
        self.frames += 1

    def get_text_size(self, text, font):
        return (len(text) * 6, 12)


class Recorder:
    """Stands in for every widget the app can put on screen."""

    def __init__(self):
        self.dialogs = []
        self.pages = []
        self.progress = []
        self.replies = []
        self.list_choice = 0

    def dialog_factory(self):
        recorder = self

        class FakeDialog:
            def __init__(self, ui, message, button_text="OK", cancel_keys=(14,),
                         **kwargs):
                self.message = message
                self.button_text = button_text
                self.cancel_keys = cancel_keys

            def show(self):
                recorder.dialogs.append(
                    (self.message, self.button_text, self.cancel_keys))
                return recorder.replies.pop(0) if recorder.replies else 28

        return FakeDialog

    def page_factory(self):
        recorder = self

        class FakePage:
            def __init__(self, ui, title="", subtitle="", body="", image=None,
                         badge=None, softkey_text="OK", cancel_keys=(14,),
                         **kwargs):
                self.title = title
                self.subtitle = subtitle or ""
                self.body = body or ""
                self.image = image
                self.badge = badge or ""
                self.softkey_text = softkey_text
                self.cancel_keys = cancel_keys

            def show(self):
                recorder.pages.append(self)
                return recorder.replies.pop(0) if recorder.replies else 28

        return FakePage

    def progress_factory(self):
        recorder = self

        class FakeProgress:
            def __init__(self, ui, step, **kwargs):
                self.steps = [step]
                self.calls = []
                recorder.progress.append(self)

            def set_step(self, step):
                self.steps.append(step)

            def draw(self, done, total):
                self.calls.append((done, total))

        return FakeProgress

    def list_factory(self):
        recorder = self

        class FakeList:
            def __init__(self, ui, title, items, **kwargs):
                self.items = items

            def show(self):
                return recorder.list_choice

        return FakeList

    # --- what ended up on screen -----------------------------------------

    def messages(self):
        return [dialog[0] for dialog in self.dialogs]

    def softkeys(self):
        return [dialog[1] for dialog in self.dialogs]

    def titles(self):
        return [page.title for page in self.pages]

    def page_text(self):
        return "\n".join(
            "\n".join([page.title, page.subtitle, page.badge, page.body])
            for page in self.pages)

    def steps(self):
        return [step for screen in self.progress for step in screen.steps]


@pytest.fixture
def env(tmp_path, monkeypatch):
    """A phone with a NeoDCT card in it and a temporary state directory.

    No carrier: these tests run on a build host whose /proc/net/route is the
    host's, not the phone's, and a phone that believes it is online takes
    the "look for a release" path instead of the card one. Tests that want
    the online path say so by patching _has_network themselves.
    """
    monkeypatch.setattr(app, "_has_network", lambda: False)
    mount = tmp_path / "sdcard"
    for folder in Storage.FOLDERS:
        (mount / folder).mkdir(parents=True)
    (tmp_path / "sdcard.prop").write_text(
        "state=mounted\ndevice=/dev/vdc\nfstype=vfat\nlabel=NEODCT\n")
    monkeypatch.setattr(Storage, "MOUNT_POINT", str(mount))
    monkeypatch.setattr(Storage, "STATE_FILE", str(tmp_path / "sdcard.prop"))

    state = tmp_path / "user" / ".ndsys"
    state.mkdir(parents=True)
    monkeypatch.setattr(staging, "STATE_DIR", str(state))

    databases = tmp_path / "user" / "db"
    databases.mkdir(parents=True)
    (databases / "phonebook.db").write_bytes(b"contacts")
    (databases / "sms.db").write_bytes(b"messages")
    monkeypatch.setattr(app, "USER_DB_DIR", str(databases))

    recorder = Recorder()
    monkeypatch.setattr(app, "MessageDialog", recorder.dialog_factory())
    monkeypatch.setattr(app, "DetailPage", recorder.page_factory())
    monkeypatch.setattr(app, "ProgressScreen", recorder.progress_factory())
    monkeypatch.setattr(app, "VerticalList", recorder.list_factory())
    monkeypatch.setattr(app, "SoftKeyBar", lambda ui: type(
        "S", (), {"update": lambda self, *a, **k: None})())
    # Never actually reboot the machine running the tests.
    rebooted = []
    monkeypatch.setattr(app, "_reboot", lambda ui, *a, **k: rebooted.append(True))
    monkeypatch.setattr(app, "RELEASE_KEY", str(write_public_key(tmp_path)))
    monkeypatch.setattr(app, "get_setting",
                        lambda key, default=None:
                        "qemu-aarch64" if key == "system.os.platform" else default)

    recorder.rebooted = rebooted
    recorder.state = state
    recorder.card = mount
    recorder.databases = databases
    return recorder


def put_package(env, name="UPDATE.ndsw", **kwargs):
    path = env.card / "update" / name
    make_ndsw(path, **kwargs)
    return path


def backups(env):
    root = env.card / "backup_db"
    return sorted(path for path in root.rglob("*.db"))


# --- refusals ---------------------------------------------------------------


def test_a_broken_zip_is_an_invalid_update(env):
    (env.card / "update" / "UPDATE.ndsw").write_bytes(b"not a zip at all")

    app.run(FakeUI())

    assert "INVALID UPDATE! UPDATE MAY BE CORRUPT!!" in env.messages()
    assert staging.read_pending(env.state) is None


def test_an_invalid_update_offers_only_ok(env):
    """No bypass at all: OK, and nothing else."""
    (env.card / "update" / "UPDATE.ndsw").write_bytes(b"not a zip at all")

    app.run(FakeUI())

    invalid = [d for d in env.dialogs if d[0].startswith("INVALID UPDATE")]
    assert invalid[0][1] == "OK"
    assert invalid[0][2] == ()          # C does not dismiss it either


def test_a_package_with_no_manifest_is_invalid(env):
    put_package(env, members=("rootfs.squashfs", "manifest.sig"))

    app.run(FakeUI())

    assert "INVALID UPDATE! UPDATE MAY BE CORRUPT!!" in env.messages()


def test_a_package_with_no_image_is_invalid(env):
    put_package(env, members=("manifest.json", "manifest.sig"))

    app.run(FakeUI())

    assert "INVALID UPDATE! UPDATE MAY BE CORRUPT!!" in env.messages()


def test_a_package_for_other_hardware_is_refused(env):
    """A luckfox image on a QEMU build (or the reverse) is the brick case."""
    put_package(env, platform="luckfox-armv7")

    app.run(FakeUI())

    assert any(m.startswith("WRONG UPDATE FOR THIS PHONE!")
               for m in env.messages())
    assert staging.read_pending(env.state) is None


def test_a_wrong_platform_package_can_never_be_overridden(env):
    put_package(env, platform="luckfox-armv7")

    app.run(FakeUI())

    wrong = [d for d in env.dialogs if d[0].startswith("WRONG UPDATE")]
    assert wrong[0][1] == "OK"
    assert wrong[0][2] == ()


def test_a_package_needing_a_newer_kernel_is_refused(env):
    put_package(env, min_kernel="99.0.0")

    app.run(FakeUI())

    assert any("WRONG UPDATE FOR THIS PHONE!" in m for m in env.messages())


def test_an_unsigned_package_warns_with_the_specified_wording(env):
    put_package(env, members=("rootfs.squashfs", "manifest.json"))
    env.replies = [ENTER, ENTER, ENTER, ENTER]

    app.run(FakeUI())

    assert "BAD SIGNATURE! UPDATE MAY BE CORRUPT!!" in env.messages()


def test_every_signature_warning_softkey_just_says_ok(env):
    """"Acknowledge" is a word from a lawyer, not from a phone."""
    put_package(env, members=("rootfs.squashfs", "manifest.json"))
    env.replies = [ENTER, BACK]          # OK, then decline to go on

    app.run(FakeUI())

    assert env.softkeys() == ["OK", "OK"]


def test_engineering_mode_is_asked_once_more_before_it_installs(env):
    """The warning is the news; this is the decision."""
    put_package(env, members=("rootfs.squashfs", "manifest.json"))
    env.replies = [ENTER, BACK]

    app.run(FakeUI())

    assert env.messages() == ["BAD SIGNATURE! UPDATE MAY BE CORRUPT!!",
                              "Install Anyway?"]


def test_declining_to_install_anyway_stops_the_update(env):
    put_package(env, members=("rootfs.squashfs", "manifest.json"))
    env.replies = [ENTER, BACK]          # OK at the warning, C at the question

    app.run(FakeUI())

    assert staging.read_pending(env.state) is None
    assert env.rebooted == []


def test_engineering_mode_can_install_an_unsigned_build(env):
    """Engineering mode's whole purpose: get an unsigned dev build on."""
    put_package(env, members=("rootfs.squashfs", "manifest.json"))
    env.replies = [ENTER, ENTER, ENTER]  # OK, install anyway, Install

    app.run(FakeUI())

    assert staging.read_pending(env.state) is not None
    assert env.rebooted == [True]


def test_a_signature_from_the_wrong_key_is_a_bad_signature(env):
    put_package(env, signature=sign(b'{"version":"9.9.9"}'))
    env.replies = [ENTER, BACK]

    app.run(FakeUI())

    assert any(m.startswith("BAD SIGNATURE") for m in env.messages())


def test_without_engineering_mode_a_bad_signature_is_a_dead_end(env):
    """No second chance outside engineering mode: an unsigned image is how
    you end up stuck on a phone that will not boot."""
    put_package(env, members=("rootfs.squashfs", "manifest.json"))
    ui = FakeUI()
    ui.engineering_mode = False

    app.run(ui)

    assert env.messages() == ["BAD SIGNATURE! UPDATE MAY BE CORRUPT!!"]
    assert env.dialogs[0][1] == "OK"
    assert env.dialogs[0][2] == ()       # C does not dismiss it either
    assert staging.read_pending(env.state) is None
    assert env.rebooted == []


def test_the_second_question_never_appears_outside_engineering_mode(env):
    put_package(env, members=("rootfs.squashfs", "manifest.json"))
    ui = FakeUI()
    ui.engineering_mode = False

    app.run(ui)

    assert "Install Anyway?" not in env.messages()


def test_a_properly_signed_package_never_shows_a_warning(env):
    put_package(env)

    app.run(FakeUI())

    assert env.dialogs == []
    assert staging.read_pending(env.state) is not None


# --- the update page --------------------------------------------------------


def test_the_update_page_is_the_only_thing_between_you_and_installing(env):
    """One page, one softkey. The old flow asked four questions first."""
    put_package(env)

    app.run(FakeUI())

    assert len(env.pages) == 2, [page.title for page in env.pages]
    assert env.pages[0].softkey_text == "Install"


def test_the_update_page_leads_with_the_version(env):
    put_package(env)

    app.run(FakeUI())

    assert "0.3.2a" in env.pages[0].title


def test_the_update_page_shows_the_size_and_the_build_date(env):
    put_package(env)

    app.run(FakeUI())

    assert "MB" in env.pages[0].subtitle
    assert "2026" in env.pages[0].subtitle


def test_the_release_notes_are_on_the_update_page(env):
    put_package(env, changelog="Fixed SMS database sorting bug.")

    app.run(FakeUI())

    assert "Fixed SMS database sorting bug." in env.pages[0].body


def test_a_package_with_no_changelog_says_so_rather_than_showing_nothing(env):
    put_package(env, changelog="")

    app.run(FakeUI())

    assert env.pages[0].body.strip()


def test_a_signed_package_says_so_on_the_page(env):
    put_package(env)

    app.run(FakeUI())

    assert "erified" in env.pages[0].badge


def test_an_unsigned_package_says_so_on_the_page(env):
    put_package(env, members=("rootfs.squashfs", "manifest.json"))
    env.replies = [ENTER, ENTER, ENTER]

    app.run(FakeUI())

    assert "not signed" in env.pages[0].badge.lower()


def test_the_picture_from_the_package_reaches_the_page(env):
    put_package(env, thumbnail=png())

    app.run(FakeUI())

    assert env.pages[0].image is not None


def test_a_package_with_a_tampered_picture_falls_back_to_the_stock_icon(env):
    """The picture is decoration; the system image's own hash is what says
    whether an update is safe, so broken art costs the art and nothing else."""
    put_package(env, thumbnail=png(), thumbnail_hash="d" * 64)

    app.run(FakeUI())

    assert env.pages[0].image == app.APP_ICON
    assert staging.read_pending(env.state) is not None


def test_a_package_with_no_picture_at_all_still_has_a_hero(env):
    put_package(env)

    app.run(FakeUI())

    assert env.pages[0].image == app.APP_ICON


def test_backing_out_of_the_update_page_changes_nothing(env):
    put_package(env)
    env.replies = [BACK]                 # C on the update page

    app.run(FakeUI())

    assert staging.read_pending(env.state) is None
    assert env.rebooted == []


# --- backing up -------------------------------------------------------------


def test_user_data_is_backed_up_without_anyone_being_asked(env):
    put_package(env)

    app.run(FakeUI())

    assert [path.name for path in backups(env)] == ["phonebook.db", "sms.db"]
    assert not any("back up" in m.lower() for m in env.messages())
    assert not any("back up" in page.title.lower() for page in env.pages)


def test_the_backup_is_part_of_the_progress_screen(env):
    """No extra dialog: it is a step on the way, like copying."""
    put_package(env)

    app.run(FakeUI())

    assert any("ack" in step for step in env.steps()), env.steps()
    assert any("opying" in step for step in env.steps()), env.steps()


def test_the_backup_goes_in_its_own_dated_folder(env):
    put_package(env)

    app.run(FakeUI())

    folder = backups(env)[0].parent
    assert folder.parent.name == "backup_db"
    assert folder.name[:2] == "20"


def test_a_backup_that_cannot_be_written_does_not_stop_the_update(env):
    """Userdata lives on its own partition and survives an update anyway,
    so a full card is no reason to refuse to install."""
    put_package(env)
    (env.card / "backup_db").chmod(0o500)
    try:
        app.run(FakeUI())
    finally:
        (env.card / "backup_db").chmod(0o755)

    assert staging.read_pending(env.state) is not None
    assert env.rebooted == [True]


def test_a_backup_that_failed_is_admitted_before_the_restart(env):
    put_package(env)
    (env.card / "backup_db").chmod(0o500)
    try:
        app.run(FakeUI())
    finally:
        (env.card / "backup_db").chmod(0o755)

    assert "not backed up" in env.page_text().lower()


# --- progress and staging ---------------------------------------------------


def test_the_copy_reports_progress(env):
    put_package(env)

    app.run(FakeUI())

    assert env.progress, "no progress screen was shown"
    assert env.progress[0].calls[-1][0] == env.progress[0].calls[-1][1]


def test_one_progress_screen_covers_the_whole_install(env):
    """Backing up and copying share a bar; two screens would flicker."""
    put_package(env)

    app.run(FakeUI())

    assert len(env.progress) == 1


def test_the_restart_page_says_what_is_about_to_happen(env):
    put_package(env)

    app.run(FakeUI())

    assert "estart" in env.pages[-1].softkey_text
    assert env.rebooted == [True]


# --- the states with no update in them --------------------------------------


def test_no_card_explains_where_updates_come_from_on_one_page(env, monkeypatch):
    monkeypatch.setattr(Storage, "STATE_FILE", "/nonexistent/sdcard.prop")

    app.run(FakeUI())

    assert len(env.pages) == 1
    assert "card" in env.pages[0].title.lower()
    assert "UPDATE.ndsw" in env.pages[0].body


def test_a_card_that_is_not_set_up_says_so(env):
    for folder in Storage.FOLDERS:
        os.rmdir(env.card / folder)

    app.run(FakeUI())

    assert any("not set up" in page.body.lower() or "not set up" in page.title.lower()
               for page in env.pages)


def test_an_empty_update_folder_reads_as_up_to_date(env):
    app.run(FakeUI())

    assert len(env.pages) == 1
    assert "up to date" in env.pages[0].title.lower()
    assert "UPDATE.ndsw" in env.pages[0].body


def test_the_up_to_date_page_names_the_version_you_are_on(env, monkeypatch):
    monkeypatch.setattr(app, "get_setting",
                        lambda key, default=None:
                        {"system.os.platform": "qemu-aarch64",
                         "system.os.versionnumber": "0.3.1a"}.get(key, default))

    app.run(FakeUI())

    assert "0.3.1a" in env.pages[0].subtitle


def test_the_conventional_filename_is_used_without_asking(env):
    put_package(env)
    put_package(env, name="other.ndsw")

    app.run(FakeUI())

    assert staging.read_pending(env.state).version == "0.3.2a"


# --- reporting the last attempt ---------------------------------------------


def test_the_previous_failure_is_reported_then_forgotten(env):
    staging.record_result(env.state, "failed", version="0.3.2a",
                          reason="read-back mismatch")

    app.run(FakeUI())

    assert any("read-back mismatch" in m for m in env.messages())
    assert staging.read_result(env.state) is None


def test_a_previous_success_is_reported_as_a_page_not_a_warning(env):
    staging.record_result(env.state, "ok", version="0.3.2a")

    app.run(FakeUI())

    assert any("0.3.2a" in page.subtitle for page in env.pages)
    assert any("Updated" in page.title for page in env.pages)
    assert env.dialogs == []


# --- end to end -------------------------------------------------------------


def test_what_the_app_stages_is_what_the_applier_installs(env, tmp_path):
    """End to end: verify, stage, then let the real busybox applier run."""
    image, tree = build_image(blocks=8)
    body = make_ndsw(env.card / "update" / "UPDATE.ndsw", image=image, tree=tree)
    app.run(FakeUI())
    assert staging.read_pending(env.state) is not None

    device = tmp_path / "system.img"
    device.write_bytes(b"\x00" * (len(image) + 4096))
    script = (
        'STATE_DIR="%s"; MNT_USER="%s"; SYS_DEV="%s"; USER_MOUNTED=1\n'
        '. "%s"\napply_pending\n'
        % (env.state, tmp_path / "user", device, APPLY_SH)
    )
    result = subprocess.run(["sh", "-c", script], capture_output=True, text=True)

    assert result.returncode == 0, result.stderr
    assert device.read_bytes()[:len(image)] == image
    assert staging.read_installed(env.state).version == "0.3.2a"
    assert staging.read_installed(env.state).verity_root_hash == tree.root_hash
    assert staging.read_pending(env.state) is None


# --- looking online when there is nothing on the card ---

def test_a_connected_phone_with_an_empty_card_offers_to_look_online(env, monkeypatch):
    """The card is no longer the only source, so an empty card is not the
    end of the conversation on a phone that has a carrier."""
    monkeypatch.setattr(app, "_has_network", lambda: True)
    asked = []
    monkeypatch.setattr(app, "_confirm",
                        lambda ui, message, button: asked.append(message) or False)

    app.run(FakeUI())

    assert asked, "a connected phone should have been offered the online check"
    assert "online" in asked[0].lower()


def test_a_phone_with_no_carrier_is_not_offered_a_download_it_cannot_do(env, monkeypatch):
    """Offering "look online?" to a phone with no route is a dialog whose
    only honest answer is no, and it costs the owner a keypress to say it."""
    monkeypatch.setattr(app, "_has_network", lambda: False)
    asked = []
    monkeypatch.setattr(app, "_confirm",
                        lambda ui, message, button: asked.append(message) or False)

    app.run(FakeUI())

    assert not asked
    assert env.pages, "it should still say what version it is on"

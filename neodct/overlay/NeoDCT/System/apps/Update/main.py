"""Update: install an UPDATE.ndsw from the SD card.

The app never writes the system partition itself -- it cannot, the rootfs is
a mounted read-only squashfs. It verifies a package, copies the image to the
user partition as a pending update and reboots; the initramfs applies it
before the root is mounted (neodct/initramfs/ndsys-apply.sh).

The shape of the flow is one page and one bar: a page describing the update
(picture, version, size, release notes) whose softkey installs it, then a
single progress screen that backs up the databases and copies the image.
Nothing is asked twice, and the backup is not a question -- it just happens.

What each refusal means, and whether it can be overridden:

  INVALID UPDATE     the zip is broken or incomplete. No override: there is
                     nothing installable in the file.
  WRONG UPDATE       built for other hardware or a newer kernel. No
                     override: installing it would brick the phone.
  BAD SIGNATURE      intact, but unsigned or signed by the wrong key.
                     Engineering mode may acknowledge and continue.
"""

import io
import os
import shutil
import subprocess
import time

from PIL import Image

from System.ui.framework import (DetailPage, MessageDialog, ProgressScreen,
                                 SoftKeyBar, VerticalList)
from System.core import Storage
from System.core.SettingsStorage import get_setting
from System.core.UpdateService import (BadSignature, IncompatibleUpdate,
                                       InvalidUpdate, UpdateError, package,
                                       staging)

ROOT_ID = 12

RELEASE_KEY = "/NeoDCT/System/keys/neodct-release.pub"
APP_ICON = "/NeoDCT/System/apps/Update/icon.png"
USER_DB_DIR = "/NeoDCT/User/db"

HEADER = "SOFTWARE UPDATE"
COPY_HINT = "Do not remove the card"

ENTER = 28
BACK = 14

NO_CARD_HELP = (
    "Updates are read from an SD card.\n"
    "\n"
    "Format a card as FAT32, make a folder called \"update\" on it and copy "
    "UPDATE.ndsw into that folder.\n"
    "\n"
    "Put the card in the phone and open Update again."
)

NOT_READY_HELP = (
    "The card in the phone is not set up for NeoDCT.\n"
    "\n"
    "Settings can prepare it for you: Settings, Memory card, Prepare card.\n"
    "\n"
    "Preparing a card makes the folders NeoDCT uses. It does not erase what "
    "is already on it."
)

NO_PACKAGE_HELP = (
    "There is nothing to install from the card.\n"
    "\n"
    "To update, copy UPDATE.ndsw into the \"update\" folder on the card.\n"
    "\n"
    "An update file is built by \"make update\" in the buildroot tree and "
    "fits one kind of hardware only -- a QEMU build will not install on a "
    "real phone, or the other way round."
)


def _engineering_mode(ui):
    """Engineering mode as the rest of the UI decides it."""
    flag = getattr(ui, "engineering_mode", None)
    if flag is not None:
        return bool(flag)
    return str(get_setting("system.ui.engineering_mode", "ON")).strip().upper() \
        in ("ON", "1", "TRUE", "YES", "ENABLED")


def _installed_version():
    return str(get_setting("system.os.versionnumber", "") or "").strip()


def _refuse(ui, message):
    """A dead end: one dialog, OK, no way through."""
    MessageDialog(ui, message, button_text="OK", cancel_keys=()).show()


def _confirm(ui, message, button_text):
    """True only if the user pressed the softkey; C (BACK) cancels."""
    return MessageDialog(ui, message, button_text=button_text).show() == ENTER


def _page(ui, title, subtitle="", body="", image=None, badge=None,
          softkey_text="Back", **kwargs):
    """Every screen that is a page rather than a warning goes through here."""
    return DetailPage(ui, title=title, subtitle=subtitle, body=body,
                      image=image, badge=badge, header=HEADER,
                      softkey_text=softkey_text, **kwargs).show()


def _format_size(count):
    return "%.1f MB" % (count / 1048576.0)


def _format_date(stamp):
    try:
        return time.strftime("%d %b %Y", time.gmtime(int(stamp)))
    except (TypeError, ValueError, OSError):
        return "unknown"


def _size_detail(done, total):
    return "%s of %s" % (_format_size(done), _format_size(total))


def _thumbnail(pkg):
    """The package's own picture, falling back to this app's icon.

    A picture that does not match the manifest is a broken attachment, not a
    reason to refuse an update whose image hashes fine -- so it is dropped
    and the stock icon stands in.
    """
    try:
        data = pkg.read_thumbnail()
    except (InvalidUpdate, OSError):
        data = None
    if data:
        try:
            image = Image.open(io.BytesIO(data))
            image.load()
            return image
        except Exception:
            pass
    return APP_ICON


def _report_last_result(ui):
    """Report how the last install went, then forget it."""
    result = staging.read_result()
    if not result:
        return
    version = result.get("version", "")
    if result.get("result") == "ok":
        _page(ui, "Updated",
              subtitle="NeoDCT %s" % version,
              body="Everything on the phone came across: your contacts, "
                   "messages and settings live on their own partition and "
                   "are untouched by an update.",
              image=APP_ICON, softkey_text="OK")
    else:
        _refuse(ui, "Update to %s failed.\n%s"
                % (version, result.get("reason", "unknown reason")))
    staging.clear_result()


def _backup_user_data(progress):
    """Copy the databases onto the card, reporting into `progress`.

    Best effort by design: /NeoDCT/User is its own partition and survives an
    update untouched, so a card that is full or write-protected is no reason
    to stop. True when a backup was actually written.
    """
    target = Storage.folder("backup_db")
    if not target:
        return False
    try:
        names = [name for name in sorted(os.listdir(USER_DB_DIR))
                 if name.endswith(".db")]
    except OSError:
        return False
    if not names:
        return False

    destination = os.path.join(target, time.strftime("%Y%m%d-%H%M%S"))
    try:
        os.makedirs(destination, exist_ok=True)
        for index, name in enumerate(names):
            progress.draw(index, len(names))
            shutil.copy2(os.path.join(USER_DB_DIR, name),
                         os.path.join(destination, name))
        progress.draw(len(names), len(names))
        subprocess.call(["sync"])
    except OSError:
        return False
    return True


def _stage(ui, pkg, progress):
    """Mark this package as the one to install on the next boot.

    Nothing is copied. The applier reads the image straight out of the
    .ndsw where it already sits on the card, because there is nowhere on
    the phone to put a second copy of it: the user partition is 8 MiB on
    the Luckfox and a system image is 51 MiB. Staging a copy there is what
    this used to do, and it could not work on any card of any size.
    """
    state_dir = staging.STATE_DIR
    try:
        os.makedirs(state_dir, exist_ok=True)
    except OSError as exc:
        _refuse(ui, "Cannot write to the user partition.\n%s" % exc)
        return False

    try:
        staging.stage_package(pkg.manifest, pkg.path, pkg.image_size,
                              state_dir)
    except OSError as exc:
        _refuse(ui, "Could not stage the update.\n%s" % exc)
        return False
    subprocess.call(["sync"])
    return True


def _reboot(ui):
    subprocess.call(["sync"])
    for command in (["reboot"], ["/sbin/reboot"], ["busybox", "reboot"]):
        try:
            subprocess.Popen(command)
            break
        except OSError:
            continue
    # init takes a moment to bring things down; sit here rather than
    # returning to the launcher and looking like nothing happened.
    time.sleep(30)


def _update_page(ui, pkg):
    """The "an update is available" page. ENTER means install it."""
    manifest = pkg.manifest
    # The header says what this screen is and the hero column is narrow, so
    # the title is the version alone -- "NeoDCT 0.3.2a" only fits beside a
    # picture at a size that stops looking like a heading.
    return _page(
        ui,
        title=manifest.version,
        subtitle="%s\n%s" % (_format_size(pkg.image_size),
                             _format_date(manifest.buildtime)),
        badge="Verified" if pkg.signed else "Not signed",
        body=manifest.changelog or "No release notes came with this build.",
        image=_thumbnail(pkg),
        softkey_text="Install",
    )


def _restart_page(ui, manifest, backed_up):
    body = ("The phone will restart to finish installing NeoDCT %s. It takes "
            "about a minute and the screen stays dark for part of it."
            % manifest.version)
    if not backed_up:
        body += ("\n\nYour contacts and messages were not backed up to the "
                 "card. They stay on the phone either way: user data is on "
                 "its own partition and an update does not touch it.")
    _page(ui, "Ready", subtitle="NeoDCT %s" % manifest.version,
          body=body, image=APP_ICON, softkey_text="Restart", cancel_keys=())
    _reboot(ui)


def _install(ui, path):
    try:
        pkg = package.open_package(path)
    except InvalidUpdate:
        # No override: a package with no manifest or no image has nothing to
        # install, so offering to continue would be a lie.
        _refuse(ui, "INVALID UPDATE! UPDATE MAY BE CORRUPT!!")
        return

    with pkg:
        manifest = pkg.manifest
        try:
            manifest.check_compatible(
                platform=get_setting("system.os.platform", "unknown"),
                kernel=os.uname().release)
        except IncompatibleUpdate as exc:
            # Never overridable: this is the brick case.
            _refuse(ui, "WRONG UPDATE FOR THIS PHONE!\n%s" % exc)
            return

        try:
            pkg.verify_signature(RELEASE_KEY)
        except BadSignature:
            # Same warning either way -- what differs is whether there is a
            # way past it. Outside engineering mode there is not: an image
            # nobody signed is how you end up stuck on a phone that will not
            # boot, and a dead phone cannot be talked out of it afterwards.
            _refuse(ui, "BAD SIGNATURE! UPDATE MAY BE CORRUPT!!")
            if not _engineering_mode(ui):
                return
            if not _confirm(ui, "Install Anyway?", "OK"):
                return

        if _update_page(ui, pkg) != ENTER:
            return

        # One bar for the whole job: backing up and copying are steps of the
        # same wait, not two screens.
        progress = ProgressScreen(ui, "Backing up data", header=HEADER,
                                  hint=COPY_HINT)
        backed_up = _backup_user_data(progress)
        # No copying step any more -- the image is installed from the card
        # at boot, so all that happens here is a record being written.
        progress.set_step("Preparing update")
        if _stage(ui, pkg, progress):
            _restart_page(ui, manifest, backed_up)


def _choose_package(ui, packages):
    if len(packages) == 1:
        return packages[0]
    names = [os.path.basename(path) for path in packages]
    vlist = VerticalList(ui, "Updates", names, app_id=ROOT_ID)
    SoftKeyBar(ui).update("Select", present=False)
    selection = vlist.show()
    if selection == -1:
        return None
    return packages[selection]



# --- fetching one over the network ----------------------------------------
# The card is still the only place a package lives: this downloads onto it
# and then hands the path to the same _install() a card-found package goes
# through, so the signature check, the staging and the applier are shared.
# Nothing here decides whether a package is trustworthy.

def _has_network():
    """True when a default route exists.

    Cheaper than trying GitHub and waiting for a timeout, and it keeps the
    offline phone to a single screen: offering "look online?" to a phone
    with no carrier is a dialog whose only honest answer is no.
    """
    # uistub drives this code on a build host whose /proc is not the
    # phone's; PathRemap cannot cover /proc, so the stub says so instead.
    if os.environ.get("NEODCT_STUB"):
        return False
    try:
        with open("/proc/net/route") as handle:
            for line in handle.read().splitlines()[1:]:
                fields = line.split()
                if len(fields) > 2 and fields[1] == "00000000":
                    return True
    except OSError:
        pass
    try:
        with open("/proc/net/ipv6_route") as handle:
            for line in handle:
                fields = line.split()
                # A default route is destination ::/0 -- all-zero prefix
                # with a zero prefix length. T-Mobile is IPv6-only, so on
                # this phone this is the branch that matters.
                if len(fields) > 1 and fields[0] == "0" * 32 \
                        and fields[1] == "00":
                    return True
    except OSError:
        pass
    return False


def _check_online(ui):
    """Look for a newer release, download it, return its path or None."""
    from System.core.UpdateService import remote

    platform = get_setting("system.os.platform", "unknown")
    installed = _installed_version()

    dialog = ProgressScreen(ui, "Checking for updates", header=HEADER)
    dialog.draw(0, 1)
    try:
        found = remote.latest(platform)
    except remote.NoRelease:
        _page(ui, "Nothing published", subtitle="for this phone",
              body="There is no update for %s in the latest release.\n\n"
                   "That is normal while a release is still being "
                   "uploaded. Try again shortly." % platform,
              image=APP_ICON)
        return None
    except remote.NetworkError as exc:
        _page(ui, "No connection", subtitle="Could not reach GitHub",
              body="%s\n\nMobile data has to be working before the phone "
                   "can look for updates. Updates can still be installed "
                   "from the card." % exc, image=APP_ICON)
        return None

    if not remote.is_newer(found["version"], installed):
        _page(ui, "Up to date", subtitle="NeoDCT %s" % (installed or "?"),
              body="The newest release is %s, which is what this phone is "
                   "already running." % found["version"], image=APP_ICON)
        return None

    if not _confirm(ui, "Download NeoDCT %s?\n%s"
                    % (found["version"], _format_size(found["size"])),
                    "Download"):
        return None

    folder = Storage.folder("update")
    if not folder:
        _refuse(ui, "The card has no update folder.")
        return None
    destination = os.path.join(folder, remote.asset_name(platform))

    progress = ProgressScreen(ui, "Downloading %s" % found["version"],
                              header=HEADER)
    try:
        remote.download(found["url"], destination,
                        size=found["size"],
                        progress=lambda done, total: progress.draw(
                            done, total or found["size"] or 1))
    except remote.NetworkError as exc:
        # Expected on this phone rather than exceptional: the carrier drops
        # and a 60MB download is a long time to hold a weak bearer.
        _refuse(ui, "Download failed.\n%s\n\nNothing was installed." % exc)
        return None
    except UpdateError as exc:
        _refuse(ui, "Download failed.\n%s" % exc)
        return None

    return destination


def run(ui):
    _report_last_result(ui)

    card = Storage.card()
    if card.state == "absent":
        _page(ui, "No SD card", subtitle="Updates come from a card",
              body=NO_CARD_HELP, image=APP_ICON)
        return
    if card.state != "ready":
        _page(ui, "Not ready", subtitle="The card is not set up",
              body=NOT_READY_HELP, image=APP_ICON)
        return

    packages = Storage.find_updates()
    if not packages:
        # Nothing on the card is the normal case now that the phone can
        # fetch its own updates. Only offer that when there is actually a
        # route out: a phone with no carrier should get the one "up to
        # date" screen, not a dialog it cannot usefully answer.
        if _has_network() and _confirm(ui, "No update on the card.\n"
                                       "Look online?", "Check"):
            path = _check_online(ui)
            if path:
                _install(ui, path)
            return
        version = _installed_version()
        _page(ui, "Up to date",
              subtitle=("NeoDCT %s" % version) if version else "Nothing to install",
              body=NO_PACKAGE_HELP, image=APP_ICON)
        return

    path = _choose_package(ui, packages)
    if path:
        _install(ui, path)

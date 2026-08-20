import os
import subprocess

from System.ui.framework import (SoftKeyBar, MessageDialog, VerticalList,
                                 TextScroller)
from System.core.SettingsStorage import get_setting, set_setting
from System.core import Storage

ROOT_ID = 4
# Stock wallpapers ship inside the read-only image; the card and the user
# directory add to them.
SYSTEM_WALLPAPER_DIR = "/NeoDCT/System/wallpapers"
WALLPAPER_DIR = "/NeoDCT/User/wallpapers"
SUPPORTED_WALLPAPERS = (".jpg", ".jpeg")

SDCARD_HELPER = "/NeoDCT/System/hw/neodct-sdcard"

GET_MORE_LABEL = "Get more..."

GET_MORE_HELP = (
    "Get more wallpapers by adding an SD card!\n"
    "\n"
    "Format a card as FAT32, make a folder called \"wallpapers\" on it, and "
    "copy your .jpg files into it.\n"
    "\n"
    "240x240 pictures look best. Put the card in the phone and they appear in "
    "this list. The phone can set a blank card up for you."
)

GET_MORE_HELP_WITH_CARD = (
    "Get more wallpapers from your SD card!\n"
    "\n"
    "Copy .jpg files into the \"wallpapers\" folder on the card that is in "
    "the phone and they appear in this list. 240x240 looks best."
)
ENGINEERING_MODE_KEY = "system.ui.engineering_mode"


def _wallpaper_dirs():
    """Stock wallpapers from the image, then the card's, then user-added."""
    dirs = list(Storage.media_dirs("wallpapers", system_dir=SYSTEM_WALLPAPER_DIR))
    if os.path.isdir(WALLPAPER_DIR) and WALLPAPER_DIR not in dirs:
        dirs.append(WALLPAPER_DIR)
    return dirs


def _scan_wallpapers():
    wallpapers = []
    for base in _wallpaper_dirs():
        for root, _, files in os.walk(base):
            for filename in sorted(files):
                if filename.lower().endswith(SUPPORTED_WALLPAPERS):
                    full_path = os.path.join(root, filename)
                    display = os.path.splitext(os.path.basename(filename))[0]
                    wallpapers.append({"name": display, "path": full_path})
    wallpapers.sort(key=lambda item: item["name"].lower())
    return wallpapers


def _show_wallpaper_menu(ui):
    while True:
        if _wallpaper_menu_once(ui) != "again":
            return


def _wallpaper_menu_once(ui):
    try:
        os.makedirs(WALLPAPER_DIR, exist_ok=True)
    except Exception:
        pass   # read-only or no user partition: stock wallpapers still work

    wallpapers = _scan_wallpapers()
    wallpapers.insert(0, {"name": "None", "path": "NONE"})
    # Trailing entry explains where more come from, so the SD card is
    # discoverable without a manual.
    wallpapers.append({"name": GET_MORE_LABEL, "path": None})
    names = [wallpaper["name"] for wallpaper in wallpapers]
    vlist = VerticalList(ui, "Wallpaper", names, app_id=ROOT_ID)
    SoftKeyBar(ui).update("Select", present=False)
    selection = vlist.show()
    if selection == -1:
        return None

    selected = wallpapers[selection]
    if selected["path"] is None:
        TextScroller(ui, GET_MORE_HELP_WITH_CARD if Storage.is_ready()
                     else GET_MORE_HELP).show()
        return "again"
    set_setting("system.ui.wallpaper", selected["path"])
    if selected["path"] == "NONE":
        ui.wallpaper = None
    else:
        ui.wallpaper = ui.load_wallpaper(selected["path"])
    MessageDialog(ui, f"Wallpaper set to\n{selected['name']}").show()


def _wrap_text(ui, text, max_width, font):
    words = (text or "").split()
    if not words:
        return [""]
    lines = []
    current = ""

    def fits(candidate):
        width, _ = ui.get_text_size(candidate, font)
        return width <= max_width

    for word in words:
        candidate = f"{current} {word}".strip() if current else word
        if fits(candidate):
            current = candidate
        else:
            if current:
                lines.append(current)
            current = word

    if current:
        lines.append(current)
    return lines


def _setting_is_enabled(value, default=True):
    if value is None:
        return default
    text = str(value).strip().lower()
    if text in ("1", "true", "on", "yes", "enabled"):
        return True
    if text in ("0", "false", "off", "no", "disabled"):
        return False
    return default


def _refresh_engineering_apps(ui, enabled):
    if not hasattr(ui, "apps"):
        return

    # Keep runtime flag in sync for services that branch on engineering mode.
    try:
        ui.engineering_mode = bool(enabled)
    except Exception:
        pass

    filtered = [
        app
        for app in ui.apps
        if "/NeoDCT/System/engineering/apps/" not in app.get("path", "")
    ]
    ui.apps = filtered

    if enabled and hasattr(ui, "_scan_apps_from_dir"):
        ui._scan_apps_from_dir("/NeoDCT/System/engineering/apps")

    try:
        ui.apps.sort(key=lambda item: item["id"])
    except Exception:
        pass


def _show_engineering_mode(ui):
    current_enabled = _setting_is_enabled(get_setting(ENGINEERING_MODE_KEY, "ON"), default=True)
    options = ["On", "Off"]
    menu = VerticalList(ui, "Eng. Mode", options, app_id=ROOT_ID)
    menu.selected_index = 0 if current_enabled else 1

    SoftKeyBar(ui).update("Select", present=False)
    selection = menu.show()
    if selection == -1:
        return

    enabled = selection == 0
    set_setting(ENGINEERING_MODE_KEY, "ON" if enabled else "OFF")
    _refresh_engineering_apps(ui, enabled)
    MessageDialog(ui, f"Engineering Mode set to {'ON' if enabled else 'OFF'}.").show()


def _show_about(ui):
    title = "NeoDCT"
    screen_w = getattr(ui, "W", 240)
    screen_h = getattr(ui, "H", 175)
    softkey_h = getattr(ui, "SOFTKEY_H", 30)
    content_bottom = getattr(ui, "content_bottom", screen_h - softkey_h)
    header_y = max(30, int(screen_h * 0.11))

    version_name = get_setting("system.os.versionname", "NeoDCT OS")
    version_number = get_setting("system.os.versionnumber", "")
    build_time = get_setting("system.os.buildtime", "Unknown")
    if not build_time or build_time.upper() == "NONE":
        build_time = "Unknown"

    ui.draw.rectangle((0, 0, screen_w, screen_h), fill="black")

    w, _ = ui.get_text_size(title, ui.font_n)
    ui.draw.text(((screen_w - w) // 2, 12), title, font=ui.font_n, fill="white")
    line_pad = max(10, int(screen_w * 0.12))
    ui.draw.line((line_pad, header_y, screen_w - line_pad, header_y), fill="white")

    y = header_y + 12
    if version_name:
        name_lines = _wrap_text(ui, version_name, screen_w - 20, ui.font_s)
        for line in name_lines[:2]:
            w, _ = ui.get_text_size(line, ui.font_s)
            if y > content_bottom - 18:
                break
            ui.draw.text(((screen_w - w) // 2, y), line, font=ui.font_s, fill="white")
            y += 16
        y += 6

    if version_number:
        if y <= content_bottom - 18:
            ui.draw.text((10, y), f"Version: {version_number}", font=ui.font_s, fill="gray")
        y += 16
    if y <= content_bottom - 18:
        ui.draw.text((10, y), "Build time:", font=ui.font_s, fill="gray")
    y += 16
    build_lines = _wrap_text(ui, build_time, screen_w - 20, ui.font_s)
    for line in build_lines[:2]:
        if y > content_bottom - 18:
            break
        ui.draw.text((10, y), line, font=ui.font_s, fill="gray")
        y += 16

    SoftKeyBar(ui).update("Back", present=False)
    ui.fb.update(ui.canvas)

    while True:
        key = ui.wait_for_key()
        if key == 14:
            return

SDCARD_HELP = (
    "A NeoDCT memory card is a FAT32 card with these folders on it:\n"
    "\n"
    "  wallpapers   .jpg pictures\n"
    "  tones        .mp3 ringtones\n"
    "  music        your music\n"
    "  backup_db    copies of your contacts\n"
    "  update       UPDATE.ndsw system updates\n"
    "\n"
    "You can make one on a computer, or let the phone do it. Setting up only "
    "adds the folders. Formatting erases everything on the card."
)


def _show_memory_card(ui):
    card = Storage.card()

    if card.state == "absent":
        MessageDialog(ui, "No memory card.", button_text="More").show()
        TextScroller(ui, SDCARD_HELP).show()
        return

    if card.state == "ready":
        MessageDialog(ui, "Memory card ready.\n%s on %s"
                      % (card.label or "unnamed", card.device or "card"),
                      button_text="More").show()
        TextScroller(ui, SDCARD_HELP).show()
        return

    if card.state == "needs_setup":
        # Mountable, just not laid out as a NeoDCT card: adding the folders
        # is enough and keeps whatever is already on it.
        if MessageDialog(ui, "This card is not set up for NeoDCT.\n"
                             "Add the NeoDCT folders to it?",
                         button_text="Set up").show() != 28:
            return
        if Storage.setup_folders():
            MessageDialog(ui, "Card is ready to use.", button_text="OK").show()
        else:
            MessageDialog(ui, "Could not write to the card.\n"
                              "It may be locked or damaged.",
                          button_text="OK", cancel_keys=()).show()
        return

    # Nothing we can mount: the only way forward is to reformat, which is
    # destructive, so make that unmistakable.
    if MessageDialog(ui, "This card cannot be read.\n"
                         "Format it? EVERYTHING ON IT WILL BE ERASED!",
                     button_text="Format").show() != 28:
        return
    if not card.device:
        MessageDialog(ui, "No card device to format.", button_text="OK",
                      cancel_keys=()).show()
        return
    result = subprocess.call([SDCARD_HELPER, "format", card.device])
    if result == 0:
        MessageDialog(ui, "Card formatted and ready.", button_text="OK").show()
    else:
        MessageDialog(ui, "Formatting failed.\nThe card may be write "
                          "protected.", button_text="OK",
                      cancel_keys=()).show()


def run(ui):
    while True:
        menu = VerticalList(ui, "Settings",
                            ["Wallpaper", "Memory card", "Engineering Mode",
                             "About"],
                            app_id=ROOT_ID)
        SoftKeyBar(ui).update("Select", present=False)
        selection = menu.show()
        if selection == -1:
            return
        if selection == 0:
            _show_wallpaper_menu(ui)
        elif selection == 1:
            _show_memory_card(ui)
        elif selection == 2:
            _show_engineering_mode(ui)
        elif selection == 3:
            _show_about(ui)

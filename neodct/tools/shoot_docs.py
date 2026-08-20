#!/usr/bin/env python3
"""Render every screenshot used by the NeoDCT documentation.

Drives the real UI code through the headless stub (uistub.py), so each
image is genuine output from System/core/main.py, System/ui/framework.py
and the shipped apps -- not a mockup.

    python3 neodct/tools/shoot_docs.py [--out DIR]

Key codes are evdev: 28 enter, 14 back/clear, 103/108 up/down,
105/106 left/right, 2..11 digits 1..9 then 0, 42 '*', 43 '#'.
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
NEODCT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(NEODCT, "overlay", "NeoDCT"))

from uistub import StubUI, run_app  # noqa: E402

DEFAULT_OUT = os.path.abspath(
    os.path.join(NEODCT, "..", "..", "neodct-docs", "img", "shots")
)

UP, DOWN, LEFT, RIGHT = 103, 108, 105, 106
ENTER, BACK = 28, 14
DIGIT = {1: 2, 2: 3, 3: 4, 4: 5, 5: 6, 6: 7, 7: 8, 8: 9, 9: 10, 0: 11}
STAR, HASH = 42, 43

_saved = []


def save(image, name, out_dir):
    path = os.path.join(out_dir, name + ".png")
    image.save(path)
    _saved.append(name)
    return path


def save_frame(frames, name, out_dir, index=-1):
    if not frames:
        print(f"  !! no frames for {name}")
        return None
    return save(frames[index], name, out_dir)


def shoot_home(out):
    """Home screen and dialer, with and without wallpaper."""
    with StubUI(wallpaper="Palestine.jpg") as ui:
        ui.stub.simulate_status(battery=4, signal=4, carrier="Tello")

        ui.update()
        save(ui.fb.frames[-1], "home", out)
        save(ui.fb.device_frame(), "home-panel", out)

        # Typing a number on the home screen switches to the dialer.
        for digit in (0, 7, 4, 1, 2, 3, 4, 5, 6, 7):
            ui.handle_input(DIGIT[digit])
        ui.update()
        save(ui.fb.frames[-1], "home-dialing", out)

    with StubUI() as ui:
        ui.stub.simulate_status(battery=4, signal=4, carrier="Tello")
        ui.update()
        save(ui.fb.frames[-1], "home-nowallpaper", out)

    # No fuel gauge and no modem: the honest QEMU/dev look.
    with StubUI(wallpaper="Palestine.jpg") as ui:
        ui.update()
        save(ui.fb.frames[-1], "home-simulation", out)


def shoot_app_selector(out):
    """The Nokia-style one-app-per-screen launcher."""
    from System.ui.framework import AppSelector

    with StubUI(wallpaper="Palestine.jpg") as ui:
        ui.stub.simulate_status(battery=4, signal=4, carrier="Tello")
        selector = AppSelector("Main Menu", ui.apps, ui, background=ui.wallpaper)

        by_name = {app["name"]: i for i, app in enumerate(ui.apps)}
        wanted = ["Phone book", "Messages", "Games", "Settings",
                  "Calculator", "Koki Mobile", "Browser", "Music"]
        for name in wanted:
            if name not in by_name:
                continue
            selector.selected_index = by_name[name]
            selector.draw()
            slug = name.lower().replace(" ", "-")
            save(ui.fb.frames[-1], f"menu-{slug}", out)
            if name == "Phone book":
                save(ui.fb.device_frame(), "menu-panel", out)


def shoot_stock_apps(out):
    """One representative screen from each shipped app."""
    cases = [
        # (manifest name, key script, output slug, frame index, budget)
        ("Phone book", [], "app-phonebook", -1, 240),
        ("Messages", [], "app-messages", -1, 240),
        ("Messages", [ENTER], "app-messages-inbox", -1, 240),
        ("Call Log", [], "app-calllog", -1, 240),
        ("Settings", [], "app-settings", -1, 240),
        ("Settings", [ENTER], "app-settings-wallpaper", -1, 240),
        ("Games", [], "app-games", -1, 240),
        ("Calculator", [DIGIT[1], DIGIT[2], DIGIT[3]], "app-calculator", -1, 240),
        ("Calculator", [DIGIT[7], ENTER], "app-calculator-options", -1, 240),
        ("Clock", [], "app-clock", -1, 240),
        ("Tones", [], "app-tones", -1, 240),
        ("Music", [], "app-musicplayer", -1, 240),
        # Koki is a real-time game: it never polls read_keypress, so the
        # frame budget is what ends it. ~60 frames reaches the title card.
        ("Koki Mobile", [], "app-koki", -1, 400),
    ]
    for name, keys, slug, index, budget in cases:
        with StubUI(wallpaper="Palestine.jpg") as ui:
            ui.stub.simulate_status(battery=4, signal=4, carrier="Tello")
            try:
                frames = run_app(ui, name, keys=keys, frame_budget=budget)
            except BaseException as exc:  # keep shooting the rest
                print(f"  !! {slug}: {type(exc).__name__}: {exc}")
                continue
            save_frame(frames, slug, out, index)


def shoot_engineering_apps(out):
    cases = [
        ("ModemInfo", [], "eng-modem"),
        ("FuelGauge", [], "eng-fuelgauge"),
        ("LCD Test", [], "eng-lcdtest"),
        ("Cube Bench", [], "eng-cubebench"),
        ("Tests", [], "eng-tests"),
    ]
    for name, keys, slug in cases:
        with StubUI() as ui:
            ui.stub.simulate_status(battery=4, signal=4, carrier="Tello")
            try:
                frames = run_app(ui, name, keys=keys)
            except BaseException as exc:
                print(f"  !! {slug}: {type(exc).__name__}: {exc}")
                continue
            save_frame(frames, slug, out)


def shoot_telephony(out):
    """Call screens, the SMS banner and the crash handler."""
    import System.ui.Dialer.call_screen as call_screen
    import System.ui.Dialer.incoming_screen as incoming_screen
    from System.core.CrashHandler import _draw_engineering_crash_screen

    with StubUI(wallpaper="Palestine.jpg") as ui:
        ui.stub.simulate_status(battery=4, signal=4, carrier="Tello")

        # The draw_* helpers only paint the canvas; the loops that own
        # them are what normally flush, so flush by hand here.
        call_screen.draw_call_screen(ui, "0741234567", name="Mum")
        ui.fb.update(ui.canvas)
        save(ui.fb.frames[-1], "call-active", out)

        incoming_screen.draw_incoming_screen(ui, "Mum", True)
        ui.fb.update(ui.canvas)
        save(ui.fb.frames[-1], "call-incoming", out)

        # 3310-style "N message(s) received" banner + flashing envelope.
        ui.notify.post_sms(1, tone=False)
        ui._unread_sms = 1
        ui.update()
        save(ui.fb.frames[-1], "home-sms-banner", out)

        # The contact picker the home screen opens on up/down.
        import System.apps.PhoneBook.shared.list_ui as contacts
        ui.keys.push(BACK)
        try:
            contacts.show_contact_selector(ui, title="Select", btn_text="Call")
        except BaseException:
            pass
        save(ui.fb.frames[-1], "contacts-picker", out)

    with StubUI() as ui:
        try:
            raise RuntimeError("example failure")
        except RuntimeError:
            _draw_engineering_crash_screen(ui, "RuntimeError: example failure")
        ui.fb.update(ui.canvas)
        save(ui.fb.frames[-1], "crash-screen", out)


def shoot_games(out):
    """Snake and Memory, driven through the Games app menus."""
    cases = [
        # Games menu lists Memory then Snake.
        ([DOWN, ENTER, ENTER], "game-snake", 300),
        ([ENTER, ENTER], "game-memory", 300),
    ]
    for keys, slug, budget in cases:
        with StubUI() as ui:
            ui.stub.simulate_status(battery=4, signal=4, carrier="Tello")
            try:
                frames = run_app(ui, "Games", keys=keys, frame_budget=budget)
            except BaseException as exc:
                print(f"  !! {slug}: {type(exc).__name__}: {exc}")
                continue
            save_frame(frames, slug, out)


def shoot_widgets(out):
    """The framework widget gallery, drawn directly."""
    from System.ui.framework import (
        HeaderWidget, InfoScreen, LevelSelector, MessageDialog, PagedList,
        SoftKeyBar, TextInput, TextInputLong, TextScroller, VerticalList,
    )

    with StubUI() as ui:
        ui.stub.simulate_status(battery=4, signal=4, carrier="Tello")

        vlist = VerticalList(ui, "Phonebook",
                             ["Search", "Add entry", "Edit", "Erase",
                              "Send entry", "Options"], app_id=1)
        SoftKeyBar(ui).update("Select", present=False)
        vlist.draw()
        save(ui.fb.frames[-1], "widget-verticallist", out)

        vlist.selected_index = 2
        SoftKeyBar(ui).update("Select", present=False)
        vlist.draw()
        save(ui.fb.frames[-1], "widget-verticallist-scrolled", out)

        paged = PagedList(ui, "Messages",
                          ["Inbox", "Outbox", "Write Message"], root_id=2)
        paged.draw()
        save(ui.fb.frames[-1], "widget-pagedlist", out)

        entry = TextInput(ui, "Phonebook", "Name:", initial_text="Sam")
        entry.draw()
        save(ui.fb.frames[-1], "widget-textinput", out)

        longtext = TextInputLong(ui, "Write Message")
        longtext.set_text("Meet me by the old phone box at six")
        longtext.draw()
        save(ui.fb.frames[-1], "widget-textinputlong", out)

        dialog = MessageDialog(ui, "This application has not been "
                                   "implemented yet.")
        dialog.render()
        save(ui.fb.frames[-1], "widget-messagedialog", out)

        scroller = TextScroller(
            ui,
            "Feed the snake by steering it to the food. Every bite makes it "
            "grow longer. Use keys 2, 4, 6 and 8 to change direction.",
        )
        scroller.draw()
        save(ui.fb.frames[-1], "widget-textscroller", out)

        level = LevelSelector(ui, current=3)
        level.draw()
        save(ui.fb.frames[-1], "widget-levelselector", out)

        info = InfoScreen(ui, "Top score", 1250)
        ui.keys.push(BACK)
        try:
            info.show()
        except BaseException:
            pass
        save(ui.fb.frames[-1], "widget-infoscreen", out)

        header = HeaderWidget(ui, 3)
        ui.draw.rectangle((0, 0, ui.W, ui.H), fill="black")
        ui.draw.text((5, 0), "Call log", font=ui.font_xl, fill="white")
        header.draw(2)
        ui.draw.line((0, 30, ui.W, 30), fill="white")
        SoftKeyBar(ui).update("Options")
        save(ui.fb.frames[-1], "widget-softkeybar", out)


def shoot_examples(out, examples_dir):
    """The tutorial apps, installed the same way a user would."""
    if not os.path.isdir(examples_dir):
        print(f"  !! no examples at {examples_dir}")
        return

    cases = [
        # (folder, manifest name, key script, output slug)
        ("HelloNeoDCT", "Hello NeoDCT", [], "example-hello"),
        ("Dice", "Dice", [DIGIT[5], DIGIT[5]], "example-dice"),
        ("Countdown", "Countdown", [], "example-countdown-menu"),
        ("Countdown", "Countdown", [ENTER], "example-countdown"),
    ]
    for folder, name, keys, slug in cases:
        source = os.path.join(examples_dir, folder)
        if not os.path.isdir(source):
            print(f"  !! missing example {folder}")
            continue
        with StubUI(wallpaper="Palestine.jpg") as ui:
            ui.stub.simulate_status(battery=4, signal=4, carrier="Tello")
            ui.stub.install_app(source)
            try:
                frames = run_app(ui, name, keys=keys)
            except BaseException as exc:
                print(f"  !! {slug}: {type(exc).__name__}: {exc}")
                continue
            save_frame(frames, slug, out)

    # And show one of them sitting in the launcher next to the stock apps.
    from System.ui.framework import AppSelector
    source = os.path.join(examples_dir, "Dice")
    if os.path.isdir(source):
        with StubUI(wallpaper="Palestine.jpg") as ui:
            ui.stub.simulate_status(battery=4, signal=4, carrier="Tello")
            ui.stub.install_app(source)
            selector = AppSelector("Main Menu", ui.apps, ui,
                                   background=ui.wallpaper)
            for i, app in enumerate(ui.apps):
                if app["name"] == "Dice":
                    selector.selected_index = i
                    break
            selector.draw()
            save(ui.fb.frames[-1], "example-dice-menu", out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=DEFAULT_OUT)
    parser.add_argument("--examples", default=None,
                        help="directory of tutorial apps to install")
    args = parser.parse_args()

    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    examples = args.examples or os.path.abspath(
        os.path.join(out, "..", "..", "examples")
    )

    print(f"[shoot] output: {out}")
    for label, fn in (
        ("home", lambda: shoot_home(out)),
        ("app selector", lambda: shoot_app_selector(out)),
        ("stock apps", lambda: shoot_stock_apps(out)),
        ("games", lambda: shoot_games(out)),
        ("telephony", lambda: shoot_telephony(out)),
        ("engineering apps", lambda: shoot_engineering_apps(out)),
        ("widgets", lambda: shoot_widgets(out)),
        ("examples", lambda: shoot_examples(out, examples)),
    ):
        print(f"[shoot] {label}...")
        fn()

    print(f"[shoot] wrote {len(_saved)} images")
    return 0


if __name__ == "__main__":
    sys.exit(main())

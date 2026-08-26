import evdev
from evdev import UInput, ecodes as e
import os
import sys

# -------- CONFIG --------
WIDTH = 240
HEIGHT = 240
STEP = 8

TOGGLE_KEY = e.KEY_TAB
EXIT_KEY = e.KEY_C

# ------------------------

def find_keyboard():
    for path in evdev.list_devices():
        dev = evdev.InputDevice(path)
        if "keyboard" in dev.name.lower():
            return dev
    return None


def clamp(v, lo, hi):
    return max(lo, min(v, hi))


def main():
    keyboard = find_keyboard()
    if not keyboard:
        print("No keyboard found")
        sys.exit(1)

    print(f"Using keyboard: {keyboard.name}")

    # Touchscreen-style absolute input
    cap = {
        e.EV_KEY: [e.BTN_TOUCH],
        e.EV_ABS: [
            (e.ABS_X, evdev.AbsInfo(0, 0, WIDTH - 1, 0, 0, 0)),
            (e.ABS_Y, evdev.AbsInfo(0, 0, HEIGHT - 1, 0, 0, 0)),
        ]
    }

    ui = UInput(cap, name="NeoDCT-Touch-Cursor")

    # Cursor starts centered
    x = WIDTH // 2
    y = HEIGHT // 2

    def send_position():
        # ALWAYS send both axes
        ui.write(e.EV_ABS, e.ABS_X, x)
        ui.write(e.EV_ABS, e.ABS_Y, y)
        ui.syn()

    send_position()

    cursor_mode = True
    keyboard.grab()
    print("Mode: CURSOR (TAB = typing)")

    try:
        for event in keyboard.read_loop():
            if event.type != e.EV_KEY or event.value != 1:
                continue

            # Toggle typing mode
            if event.code == TOGGLE_KEY:
                cursor_mode = not cursor_mode
                if cursor_mode:
                    keyboard.grab()
                    print("Mode: CURSOR")
                else:
                    keyboard.ungrab()
                    print("Mode: TYPING")
                continue

            # Exit NetSurf
            if event.code == EXIT_KEY:
                os.system("killall netsurf-fb")
                break

            if not cursor_mode:
                continue

            moved = False

            if event.code == e.KEY_W:
                y -= STEP
                moved = True
            elif event.code == e.KEY_S:
                y += STEP
                moved = True
            elif event.code == e.KEY_A:
                x -= STEP
                moved = True
            elif event.code == e.KEY_D:
                x += STEP
                moved = True
            elif event.code == e.KEY_M:
                # Touch click
                ui.write(e.EV_KEY, e.BTN_TOUCH, 1)
                send_position()
                ui.write(e.EV_KEY, e.BTN_TOUCH, 0)
                ui.syn()
                continue

            if moved:
                x = clamp(x, 0, WIDTH - 1)
                y = clamp(y, 0, HEIGHT - 1)
                send_position()

    finally:
        try:
            keyboard.ungrab()
        except:
            pass
        ui.close()


if __name__ == "__main__":
    main()

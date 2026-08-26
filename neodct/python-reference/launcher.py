import sys
import os
import time
# Add current directory to path so we can import 'System' modules
sys.path.append(os.path.dirname(os.path.abspath(__file__)))

# Import the Hardware Driver and UI
from System.core import main as ui_engine


def _redirect_stdio_to_serial():
    # Detect environment to route standard output correctly:
    # /dev/ttyFIQ0 is the hardware fast interrupt console (Real Rockchip/Luckfox hardware)
    # /dev/ttyAMA0 is the emulated PL011 UART (QEMU environment)
    if os.path.exists("/dev/ttyFIQ0"):
        serial_dev = "/dev/ttyFIQ0"
    elif os.path.exists("/dev/ttyAMA0"):
        serial_dev = "/dev/ttyAMA0"
    else:
        # Fallback just in case neither exists
        serial_dev = getattr(ui_engine, "SERIAL_CONSOLE_DEVICE", "/dev/ttyAMA0")
        
    try:
        serial_out = open(serial_dev, "w")
        sys.stdout = serial_out
        sys.stderr = serial_out
        # Colour goes on after the redirect, never before: it has to wrap the
        # serial stream, not the one that was replaced a line ago.
        try:
            from System.core import logstyle
            logstyle.install()
        except Exception:
            pass          # a log with no colour still has to boot the phone
        print(f"[Launcher] Serial console active: {serial_dev}")
    except Exception as exc:
        print(f"[Launcher] Serial redirect failed for {serial_dev}: {exc}")

def splash_version():
    """What the boot splash says under the name.

    Read from the image's own version.prop rather than typed in here: this
    line spent a release showing the version before the one it was running.
    """
    from System.core.SettingsStorage import get_setting

    return "System v%s" % (get_setting("system.os.versionnumber", "") or "?")


def show_boot_logo(fb):
    from PIL import Image, ImageDraw, ImageFont
    screen_w = getattr(ui_engine, "UI_WIDTH", 240)
    screen_h = getattr(ui_engine, "UI_HEIGHT", 175)

    canvas = Image.new("RGB", (screen_w, screen_h), "black")
    draw = ImageDraw.Draw(canvas)

    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 20)
        font_small = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 14)
    except:
        font = ImageFont.load_default()
        font_small = ImageFont.load_default()

    # --- FIX START ---
    # Draw "NeoDCT System"
    text = "Starting NeoDCT..."
    # textbbox returns (left, top, right, bottom)
    bbox = draw.textbbox((0, 0), text, font=font)
    w = bbox[2] - bbox[0] # Calculate width
    title_y = max(20, int(screen_h * 0.35))
    draw.text(((screen_w - w) // 2, title_y), text, font=font, fill="white")

    # Draw Version
    ver = splash_version()
    bbox = draw.textbbox((0, 0), ver, font=font_small)
    w = bbox[2] - bbox[0]
    draw.text(((screen_w - w) // 2, title_y + 30), ver, font=font_small, fill="gray")
    # --- FIX END ---

    fb.update(canvas)

def main():
    _redirect_stdio_to_serial()

    # Before anything else that might reach the network. A phone with no
    # battery-backed RTC boots at the epoch, and every TLS certificate has
    # a "not valid before" date -- so a clock reading 1970 fails validation
    # on every HTTPS site at once, which is what the browser's privacy
    # warnings were. The floor is applied synchronously here because it
    # needs no network; the NTP sync waits for a route on its own thread.
    try:
        from System.core import ClockService
        ClockService.start()
    except Exception as exc:
        print(f"[CLOCK] clock service unavailable: {exc}")

    # Remote Shell, if it was left on. Engineering-mode ssh/sftp, off unless
    # somebody turned it on -- see System/core/RemoteShell. It comes up here
    # rather than waiting for a route: the tunnel is a retry loop, and mobile
    # data on this phone can take a minute to attach or never attach at all.
    try:
        from System.core import RemoteShell
        RemoteShell.start_if_enabled()
    except Exception as exc:
        print(f"[RSHELL] remote shell unavailable: {exc}")

    # 1. Init Hardware
    print("[Launcher] Initializing Hardware...")
    fb = ui_engine.Framebuffer() # We reuse the driver from main.py

    # 2. Show Boot Splash
    show_boot_logo(fb)
    time.sleep(1) # Let it shine for 1 second

    # 3. Launch Main UI
    print("[Launcher] Starting UI...")
    ui_engine.run(fb)

if __name__ == "__main__":
    main()

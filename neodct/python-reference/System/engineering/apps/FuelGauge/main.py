# FuelGauge -- live MAX1704x register readout (engineering app).
#
# Reads through the system BatteryService (ui.battery), so it shares the
# already-open i2c handle and never fights the OS-level battery polling.
# Refreshes once per second. Softkey issues a gauge quick-start (re-seeds
# SOC from VCELL -- handy at the start of a bench supply sweep).

import time

from System.ui.framework import MessageDialog, SoftKeyBar

KEY_NAV = 28
KEY_BACK = 14

REFRESH_S = 1.0

HW_REQUIRED_MSG = (
    "No MAX1704x fuel gauge found, so BatteryService is running its QEMU "
    "simulation stub. This app needs real hardware."
)


def _content_bottom(ui):
    return getattr(ui, "content_bottom",
                   getattr(ui, "H", 175) - getattr(ui, "SOFTKEY_H", 30))


def _rows_from_snapshot(snap):
    if snap.get("error"):
        return [("ERROR", snap["error"][:24])]

    crate = snap["crate"]
    smoothed = snap["smoothed_v"]
    return [
        ("VCELL", "%.4f V  (0x%04X)" % (snap["vcell"], snap["raw_vcell"])),
        ("SOC", "%.2f %%  (0x%04X)" % (snap["soc"], snap["raw_soc"])),
        ("CRATE", "n/a (17043/44)" if crate is None else "%+.2f %%/hr" % crate),
        ("GAUGE", "%d/4  avg %s" % (snap["level"],
                                    "--" if smoothed is None else "%.3f V" % smoothed)),
        ("VER", "0x%04X" % snap["version"]),
        ("CFG", "0x%04X" % snap["config"]),
    ]


def _draw_readout(ui, battery, flash):
    screen_w = getattr(ui, "W", 240)
    bottom = _content_bottom(ui)

    ui.draw.rectangle((0, 0, screen_w, bottom), fill="black")
    ui.draw.text((5, 0), "FuelGauge", font=ui.font_xl, fill="white")
    ui.draw.line((0, 30, screen_w, 30), fill="white")

    snap = battery.debug_snapshot()
    if snap is None:
        rows = [("MODE", "SIMULATION")]
    else:
        rows = _rows_from_snapshot(snap)

    y = 36
    line_h = max(15, (bottom - y - 16) // max(1, len(rows)))
    for label, value in rows:
        ui.draw.text((8, y), label, font=ui.font_s, fill="gray")
        ui.draw.text((70, y), value, font=ui.font_s, fill="white")
        y += line_h

    # Bottom status line: quick-start feedback left, static bus info right.
    if snap is not None:
        bus_text = "i2c-%d @ 0x%02X" % (snap["bus"], snap["addr"])
        bw = ui.get_text_size(bus_text, ui.font_s)[0]
        ui.draw.text((screen_w - 5 - bw, bottom - 14), bus_text, font=ui.font_s, fill="gray")
    if flash:
        ui.draw.text((8, bottom - 14), flash, font=ui.font_s, fill="gray")


def run(ui):
    battery = getattr(ui, "battery", None)
    if battery is None or not battery.hardware:
        MessageDialog(ui, HW_REQUIRED_MSG).show()
        return

    softkey = SoftKeyBar(ui)
    flash = ""
    flash_until = 0.0
    last_draw = 0.0

    while True:
        now = time.monotonic()
        if flash and now >= flash_until:
            flash = ""
            last_draw = 0.0
        if now - last_draw >= REFRESH_S:
            _draw_readout(ui, battery, flash)
            softkey.update("QStart", present=False)
            ui.fb.update(ui.canvas)
            last_draw = now

        key = ui.read_keypress(0.1)
        if key == KEY_BACK:
            return
        if key == KEY_NAV:
            ok = battery.quickstart()
            flash = "quick-start sent" if ok else "quick-start FAILED"
            flash_until = time.monotonic() + 2.0
            last_draw = 0.0

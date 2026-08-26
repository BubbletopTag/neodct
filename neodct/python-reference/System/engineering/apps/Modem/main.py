# Modem -- live SIM7600 connection status (engineering app).
#
# Three pages walked with the softkey (Next, Next, Exit); Back exits from
# anywhere. Reads through the system ModemService (ui.modem), so every AT
# transaction shares the /tmp/neodct-modem.lock with the OS polling,
# S45modem and atcmd -- no port fights.
#
#   RADIO  operator / registration / CSQ+dBm / bars / call state (1 s refresh)
#   SIM    CPIN, phone number (+CNUM), IMEI, ICCID, IMSI, firmware --
#          queried ONCE on first visit (identity never changes mid-session)
#   DATA   S45modem status, wwan interface, IPv6 address, APN, DNS (1 s)
#
# Unlike FuelGauge this app does NOT bail out in Simulation Mode: when the
# modem is missing, seeing *why* (which ttyUSB nodes exist, what the boot
# script logged) from inside the environment is the whole point -- serial
# consoling the real hardware is annoying.

import os
import socket
import time

from System.ui.framework import MessageDialog, SoftKeyBar

KEY_NAV = 28
KEY_BACK = 14

REFRESH_S = 1.0
PAGES = ("RADIO", "SIM", "DATA")

REG_NAMES = {
    None: "--",
    0: "NOT REG",
    1: "HOME",
    2: "SEARCHING",
    3: "DENIED",
    4: "UNKNOWN",
    5: "ROAMING",
}

BOOT_STATUS_FILE = "/tmp/modem.status"
MODEM_DEFAULTS_FILE = "/etc/default/modem"
DEFAULT_APN = "fast.t-mobile.com"


def _shorten(text, limit=24):
    text = str(text)
    if len(text) <= limit:
        return text
    keep = (limit - 2) // 2
    return text[:keep] + ".." + text[-keep:]


def _content_bottom(ui):
    return getattr(ui, "content_bottom",
                   getattr(ui, "H", 175) - getattr(ui, "SOFTKEY_H", 30))


# --- row builders (kept drawing-free so they can be bench-tested) --------

def _radio_rows(modem):
    snap = modem.status_snapshot()
    csq = snap["csq"]
    if csq is None:
        csq_text = "--"
    elif csq == 99:
        csq_text = "99 (no signal)"
    else:
        csq_text = "%d/31  %d dBm" % (csq, -113 + 2 * csq)
    stat = snap["reg_stat"]
    reg_text = REG_NAMES.get(stat, str(stat))
    if stat is not None:
        reg_text += "  (CEREG %s)" % stat
    rows = [
        ("OPER", snap["operator"] or "--"),
        ("REG", reg_text),
        ("CSQ", csq_text),
        ("BARS", "--" if snap["bars"] is None else "%d/4" % snap["bars"]),
        ("CALL", snap["state"]),
    ]
    if not snap["hardware"]:
        ttys = sorted(n for n in _listdir("/dev") if n.startswith("ttyUSB"))
        rows.append(("PORTS", ",".join(ttys) if ttys else "no ttyUSB nodes!"))
    return rows


def _first_content(lines, prefix):
    """First reply line, with an optional '+PREFIX:' tag stripped."""
    for line in lines:
        line = line.strip()
        if not line:
            continue
        if line.upper().startswith(prefix.upper()):
            return line.split(":", 1)[1].strip()
        return line
    return None


def _sim_rows(modem):
    """One-shot identity queries; call once and cache."""
    if not modem.hardware:
        na = "n/a (sim)"
        return [("SIM", na), ("NUM", na), ("IMEI", na),
                ("ICCID", na), ("IMSI", na), ("FW", na)]

    final, lines = modem.send_at("AT+CPIN?", timeout=3.0)
    if final == "OK":
        sim = _first_content(lines, "+CPIN:") or "?"
    elif final is None:
        sim = "no reply"
    else:
        sim = "NOT DETECTED"

    number = "(not on SIM)"
    final, lines = modem.send_at("AT+CNUM", timeout=3.0)
    if final == "OK":
        for line in lines:
            if line.startswith("+CNUM:") and line.count('"') >= 4:
                candidate = line.split('"')[3]
                if candidate:
                    number = candidate
                break

    final, lines = modem.send_at("AT+CICCID", timeout=3.0)
    iccid = _first_content(lines, "+ICCID:") if final == "OK" else None
    if not iccid:
        final, lines = modem.send_at("AT+CCID", timeout=3.0)
        iccid = _first_content(lines, "+CCID:") if final == "OK" else None

    final, lines = modem.send_at("AT+CIMI", timeout=3.0)
    imsi = _first_content(lines, "+CIMI:") if final == "OK" else None

    final, lines = modem.send_at("AT+CGMR", timeout=3.0)
    fw = _first_content(lines, "+CGMR:") if final == "OK" else None

    return [
        ("SIM", sim),
        ("NUM", number),
        ("IMEI", modem.imei or "--"),
        ("ICCID", iccid or "--"),
        ("IMSI", imsi or "--"),
        ("FW", _shorten(fw or "--")),
    ]


def _listdir(path):
    try:
        return os.listdir(path)
    except Exception:
        return []


def _read_file(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except Exception:
        return None


def _wwan_interface():
    names = _listdir("/sys/class/net")
    # eudev predictable naming renames wwan0 to wwp<path>; trust the
    # bound driver over the name.
    for name in sorted(names):
        driver = os.path.realpath("/sys/class/net/%s/device/driver" % name)
        if os.path.basename(driver) == "qmi_wwan":
            return name
    for prefix in ("ww", "rmnet", "usb"):
        for name in sorted(names):
            if name.startswith(prefix):
                return name
    return None


def _iface_up(name):
    flags = _read_file("/sys/class/net/%s/flags" % name)
    try:
        return bool(int(flags, 16) & 1)
    except (TypeError, ValueError):
        return False


def _global_ipv6(ifname):
    """First global-scope IPv6 on ifname, from /proc/net/if_inet6."""
    try:
        with open("/proc/net/if_inet6") as f:
            for line in f:
                fields = line.split()
                if len(fields) >= 6 and fields[5] == ifname and fields[3] == "00":
                    return socket.inet_ntop(socket.AF_INET6,
                                            bytes.fromhex(fields[0]))
    except Exception:
        pass
    return None


def _configured_apn():
    content = _read_file(MODEM_DEFAULTS_FILE) or ""
    for line in content.splitlines():
        line = line.strip()
        if line.startswith("MODEM_APN="):
            return line.split("=", 1)[1].strip().strip('"') or DEFAULT_APN
    return DEFAULT_APN


def _dns_row():
    content = _read_file("/etc/resolv.conf") or ""
    servers = [ln.split()[1] for ln in content.splitlines()
               if ln.startswith("nameserver") and len(ln.split()) > 1]
    for ns in servers:          # prefer the DNS64/IPv6 entry
        if ":" in ns:
            return ns
    return servers[0] if servers else "--"


def _data_rows():
    iface = _wwan_interface()
    if iface:
        if_text = "%s %s" % (iface, "UP" if _iface_up(iface) else "DOWN")
        ipv6 = _global_ipv6(iface)
    else:
        if_text = "none found"
        ipv6 = None
    return [
        ("BOOT", _read_file(BOOT_STATUS_FILE) or "(no S45modem run)"),
        ("IF", if_text),
        ("IPV6", _shorten(ipv6) if ipv6 else "--"),
        ("APN", _shorten(_configured_apn())),
        ("DNS", _shorten(_dns_row())),
    ]


# --- drawing --------------------------------------------------------------

def _draw_page(ui, modem, page, rows):
    screen_w = getattr(ui, "W", 240)
    bottom = _content_bottom(ui)

    ui.draw.rectangle((0, 0, screen_w, bottom), fill="black")
    ui.draw.text((5, 0), "Modem", font=ui.font_xl, fill="white")
    page_name = PAGES[page]
    pw = ui.get_text_size(page_name, ui.font_s)[0]
    ui.draw.text((screen_w - 5 - pw, 8), page_name, font=ui.font_s, fill="gray")
    ui.draw.line((0, 30, screen_w, 30), fill="white")

    y = 36
    line_h = max(15, (bottom - y - 16) // max(1, len(rows)))
    for label, value in rows:
        ui.draw.text((8, y), label, font=ui.font_s, fill="gray")
        ui.draw.text((70, y), str(value), font=ui.font_s, fill="white")
        y += line_h

    # Bottom line: mode/port left, page position right.
    mode = modem.port if modem.hardware else "SIMULATION"
    ui.draw.text((8, bottom - 14), mode, font=ui.font_s, fill="gray")
    pos = "%d/%d" % (page + 1, len(PAGES))
    posw = ui.get_text_size(pos, ui.font_s)[0]
    ui.draw.text((screen_w - 5 - posw, bottom - 14), pos, font=ui.font_s, fill="gray")


def run(ui):
    modem = getattr(ui, "modem", None)
    if modem is None:
        MessageDialog(ui, "ModemService is not running.").show()
        return

    softkey = SoftKeyBar(ui)
    page = 0
    sim_rows_cache = None
    last_draw = 0.0

    while True:
        now = time.monotonic()
        if now - last_draw >= REFRESH_S:
            if page == 0:
                rows = _radio_rows(modem)
            elif page == 1:
                if sim_rows_cache is None:
                    sim_rows_cache = _sim_rows(modem)
                rows = sim_rows_cache
            else:
                rows = _data_rows()
            _draw_page(ui, modem, page, rows)
            softkey.update("Next" if page < len(PAGES) - 1 else "Exit",
                           present=False)
            ui.fb.update(ui.canvas)
            last_draw = now

        key = ui.read_keypress(0.1)
        if key == KEY_BACK:
            return
        if key == KEY_NAV:
            page += 1
            if page >= len(PAGES):
                return
            last_draw = 0.0

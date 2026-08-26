"""Keeping the phone's clock honest.

A phone with no battery-backed RTC boots at the Unix epoch, and a clock
that says 1970 breaks more than the time display: every TLS certificate on
the internet has a "not valid before" date, so an HTTPS request from 1970
fails validation on every site at once. That is what the browser's privacy
warnings actually were.

Two mechanisms, because one of them has to work before the network does.

  1. A floor, applied immediately at boot with no network at all. The clock
     is never allowed to read earlier than the moment this image was built,
     which version.prop already records as system.os.buildepoch. The date
     will be wrong -- it will be the build date -- but it will be wrong in
     the one direction that keeps certificates valid, and it costs nothing.

  2. SNTP, once there is a route. Written here rather than pulled in as a
     package: busybox has no ntpd (only rdate, whose protocol is long dead)
     and a whole NTP daemon is a lot of image for a phone that needs one
     query per boot. The wire format is 48 bytes and the useful part is one
     64-bit fixed-point number.

The synced time is written to the user partition, so the next boot starts
from the last known good time rather than the build date, and drifts
forward rather than jumping backwards.
"""

import os
import socket
import struct
import subprocess
import threading
import time

# Seconds between 1900-01-01 (NTP) and 1970-01-01 (Unix).
NTP_EPOCH_OFFSET = 2208988800

# The NTP pool, which is volunteer-operated and deliberately not run by
# any single company. The numbered names are the pool's own recommended
# form: each resolves to a different rotating set of servers, so trying
# 0/1/2 in turn reaches different operators rather than retrying one.
#
# No corporate time server here on purpose. An NTP query tells whoever
# answers that this device exists, roughly where it is, and when it was
# switched on -- which is not much, but it is not nothing, and it is not
# worth handing to an advertising company for a clock reading.
DEFAULT_SERVERS = (
    "0.pool.ntp.org",
    "1.pool.ntp.org",
    "2.pool.ntp.org",
)

VERSION_PROP = "/NeoDCT/System/version.prop"
STATE_FILE = "/NeoDCT/User/.clock"

QUERY_TIMEOUT = 5
NTP_PORT = 123

# A reply outside this range is not a time, it is a fault or an attack.
# 2020 rules out an unset server; 2100 rules out a garbage read.
SANE_MIN = 1577836800      # 2020-01-01
SANE_MAX = 4102444800      # 2100-01-01


def _read_prop(path, key):
    try:
        with open(path) as handle:
            for line in handle:
                if line.startswith(key + "="):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return None


def build_epoch():
    """When this image was built, or None."""
    raw = _read_prop(VERSION_PROP, "system.os.buildepoch")
    try:
        return int(raw)
    except (TypeError, ValueError):
        return None


def last_known():
    """The last time we were confident about, or None."""
    try:
        with open(STATE_FILE) as handle:
            return int(handle.read().strip())
    except (OSError, ValueError):
        return None


def remember(when):
    """Record a time we trust, for the next boot to start from."""
    try:
        os.makedirs(os.path.dirname(STATE_FILE), exist_ok=True)
        tmp = STATE_FILE + ".tmp"
        with open(tmp, "w") as handle:
            handle.write("%d\n" % int(when))
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp, STATE_FILE)
        return True
    except OSError:
        return False


def set_clock(when, reason="unspecified"):
    """Set the system clock, and the RTC if the board has one.

    Always logs. A clock that moves is the sort of thing that explains a
    later mystery -- an SSL error, a file with a future timestamp, an
    update that looks older than it is -- and none of that is diagnosable
    if the jump happened silently.

    date -s rather than settimeofday: this runs as a plain script on a
    busybox system and shelling out is what everything else here does.
    """
    previous = time.time()
    print("[CLOCK] setting time (%s): %s -> %s"
          % (reason,
             time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(previous)),
             time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(int(when)))))
    try:
        subprocess.call(["date", "-s", "@%d" % int(when)],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        return False
    # Push it into the RTC so a warm reboot keeps it. Harmless when there
    # is no RTC or no battery behind it.
    try:
        subprocess.call(["hwclock", "-w"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        pass
    return True


def apply_floor():
    """Never let the clock read earlier than what we already know.

    Returns the epoch it settled on, or None if the clock was already
    ahead of everything we know and was left alone.
    """
    floor = max(x for x in (build_epoch(), last_known(), 0) if x is not None)
    if not floor:
        return None
    if time.time() >= floor:
        return None
    set_clock(floor, reason="floor: build date or last sync")
    return floor


def query(server, timeout=QUERY_TIMEOUT):
    """One SNTP request. Returns the server's epoch, or raises OSError.

    The packet is 48 bytes: first byte is leap/version/mode -- 0x1b is
    version 3, mode 3 (client) -- and the rest is zero on the way out. The
    reply's transmit timestamp sits at offset 40 as seconds-since-1900 and
    a fraction we do not need to a phone's accuracy.
    """
    packet = b"\x1b" + 47 * b"\0"
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.settimeout(timeout)
        sock.sendto(packet, (server, NTP_PORT))
        data, _ = sock.recvfrom(48)
    finally:
        sock.close()
    if len(data) < 48:
        raise OSError("short NTP reply from %s" % server)
    seconds = struct.unpack("!I", data[40:44])[0]
    if not seconds:
        raise OSError("%s sent a zero timestamp" % server)
    epoch = seconds - NTP_EPOCH_OFFSET
    if not SANE_MIN <= epoch <= SANE_MAX:
        raise OSError("%s sent an implausible time (%d)" % (server, epoch))
    return epoch


def sync(servers=DEFAULT_SERVERS, timeout=QUERY_TIMEOUT):
    """Try each server until one answers. Returns the epoch set, or None."""
    for server in servers:
        try:
            when = query(server, timeout=timeout)
        except (OSError, socket.error):
            continue
        set_clock(when, reason="NTP from %s" % server)
        remember(when)
        return when
    return None


def start(background=True, servers=DEFAULT_SERVERS):
    """Floor the clock now; sync in the background when a route appears.

    The floor is synchronous because it needs no network and everything
    that follows -- the TLS handshake the browser is about to attempt --
    depends on it. The sync is not, because it must never delay boot on a
    phone whose carrier may take a minute to attach, or never attach.
    """
    apply_floor()      # logs the change itself if it makes one

    def worker():
        # Wait for a route rather than hammering a nameserver that cannot
        # be reached yet. The modem can take a while, and may never arrive.
        for _ in range(60):
            if _has_route():
                break
            time.sleep(5)
        else:
            print("[CLOCK] no route after 5 minutes; keeping %s"
                  % time.strftime("%Y-%m-%d", time.gmtime()))
            return
        when = sync(servers)
        if when:
            print("[CLOCK] synced: %s"
                  % time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(when)))
        else:
            print("[CLOCK] no NTP server answered; keeping %s"
                  % time.strftime("%Y-%m-%d", time.gmtime()))

    if not background:
        worker()
        return
    thread = threading.Thread(target=worker, name="clock-sync", daemon=True)
    thread.start()


def _has_route():
    """A default route exists, v4 or v6."""
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
                if len(fields) > 1 and fields[0] == "0" * 32 \
                        and fields[1] == "00":
                    return True
    except OSError:
        pass
    return False

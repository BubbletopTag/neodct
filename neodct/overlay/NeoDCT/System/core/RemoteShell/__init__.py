"""Remote Shell -- ssh and sftp to the phone, over the internet, on purpose.

Development on this phone means a serial console, which means two wires
soldered to pads with no strain relief, which means they come off. This is
the replacement: an ssh session and an sftp mount, reached from anywhere,
without opening the phone.

The phone is behind carrier-grade NAT and its mobile data is IPv6, so
nothing can connect *to* it. So it connects out, and carries a way back
with it:

    phone  ---- ssh -R, key auth ---->  relay (a VPS with a public address)
                                          ^
    laptop ---- ssh -J -------------------'

What that buys, and why it is shaped this way:

  * sshd binds 127.0.0.1 and nothing else. Not the modem's interface, not
    wlan, not "0.0.0.0 but the firewall will save us". The only route in is
    the tunnel the phone itself dialled, so a phone with Remote Shell on
    and no relay reachable is a phone with no way in at all.
  * The forwarded port lands on the relay's loopback (the relay keeps
    GatewayPorts off), so reaching the phone means getting into the relay
    first. The phone is not sitting on a public port waiting for the
    internet to find it.
  * Keys only, both directions. Password authentication is off, and so is
    keyboard-interactive -- turning one off and leaving the other is a
    classic way to think you did this and not have.
  * The relay's host key is checked. Dialling out with
    StrictHostKeyChecking=no would mean anything that can answer on that
    address gets offered the tunnel.
  * It is off until somebody turns it on, and it says so on the screen
    while it is running.

Nothing here is subtle enough to deserve being clever. Everything that
decides who can get in is written into the config this module generates,
and generated fresh every time it starts, so editing it by hand on the
phone cannot quietly weaken it and survive.
"""

import os
import signal
import subprocess
import time

USER_DIR = "/NeoDCT/User/.remote"
STATE_FILE = os.path.join(USER_DIR, "state.prop")
SSHD_CONFIG = os.path.join(USER_DIR, "sshd_config")
HOST_KEY = os.path.join(USER_DIR, "ssh_host_ed25519_key")
AUTHORIZED_KEYS = os.path.join(USER_DIR, "authorized_keys")
RELAY_KEY = os.path.join(USER_DIR, "relay_id_ed25519")
KNOWN_HOSTS = os.path.join(USER_DIR, "known_hosts")

# Where the operator drops the three files, on the card, from a PC. The
# card is vfat and has no permission bits, so everything is copied onto the
# user partition at 0600 before it is used -- ssh refuses a private key the
# world can read, and it is right to.
CARD_DIR = "remote"
CARD_FILES = {
    "id_ed25519": RELAY_KEY,
    "authorized_keys": AUTHORIZED_KEYS,
    "known_hosts": KNOWN_HOSTS,
}

SSHD = "/usr/sbin/sshd"
SSH = "/usr/bin/ssh"
KEYGEN = "/usr/bin/ssh-keygen"
SFTP_SERVER = "/usr/libexec/sftp-server"

# The port sshd listens on, on loopback. Not 22: nothing else on this phone
# wants 22, but a number nobody guesses costs nothing either.
LOCAL_PORT = 22
DEFAULT_RELAY_PORT = 2222
DEFAULT_RELAY_USER = "neodct"

RETRY_SECONDS = 15


class RemoteShellError(Exception):
    """Something is missing or wrong. The message is shown on the phone."""


# --- state ----------------------------------------------------------------

def _read_props(path):
    values = {}
    try:
        with open(path) as handle:
            for line in handle:
                if "=" in line and not line.startswith("#"):
                    key, value = line.split("=", 1)
                    values[key.strip()] = value.strip()
    except OSError:
        pass
    return values


def _write_props(path, values):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as handle:
        for key in sorted(values):
            handle.write("%s=%s\n" % (key, values[key]))
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(tmp, path)


def settings():
    """What the phone has been told about the relay."""
    values = _read_props(STATE_FILE)
    return {
        "enabled": values.get("enabled", "0") == "1",
        "host": values.get("host", ""),
        "user": values.get("user", DEFAULT_RELAY_USER),
        "port": values.get("port", str(DEFAULT_RELAY_PORT)),
    }


def save_settings(host=None, user=None, port=None, enabled=None):
    current = settings()
    if host is not None:
        current["host"] = host.strip()
    if user is not None:
        current["user"] = user.strip() or DEFAULT_RELAY_USER
    if port is not None:
        current["port"] = str(port).strip() or str(DEFAULT_RELAY_PORT)
    if enabled is not None:
        current["enabled"] = bool(enabled)
    _write_props(STATE_FILE, {
        "enabled": "1" if current["enabled"] else "0",
        "host": current["host"],
        "user": current["user"],
        "port": current["port"],
    })
    return current


# --- keys -----------------------------------------------------------------

def install_keys_from_card(card_root):
    """Copy the operator's keys off the card, at 0600. Returns what it took.

    The card is how keys get onto a phone with no keyboard. It is also
    removable and readable by anyone who takes it out, so this is a
    one-way trip: the files are copied to the user partition, and the
    operator is told to delete the originals.
    """
    source = os.path.join(str(card_root), CARD_DIR)
    if not os.path.isdir(source):
        raise RemoteShellError(
            "No \"%s\" folder on the card." % CARD_DIR)

    os.makedirs(USER_DIR, exist_ok=True)
    os.chmod(USER_DIR, 0o700)

    taken = []
    for name, destination in CARD_FILES.items():
        path = os.path.join(source, name)
        if not os.path.isfile(path):
            continue
        with open(path, "rb") as src:
            data = src.read()
        # Write with the mode set from the start rather than chmod after:
        # between the two there is a moment where the key is readable, and
        # on a phone that moment is as long as the flash is slow.
        handle = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_TRUNC,
                         0o600)
        try:
            os.write(handle, data)
        finally:
            os.close(handle)
        os.chmod(destination, 0o600)
        taken.append(name)

    if not taken:
        raise RemoteShellError("No keys in %s/ on the card." % CARD_DIR)
    return taken


def have_keys():
    """True when both directions have what they need."""
    return os.path.isfile(RELAY_KEY) and os.path.isfile(AUTHORIZED_KEYS)


def ensure_host_key():
    """The phone's own identity, made once and kept."""
    if os.path.isfile(HOST_KEY):
        return HOST_KEY
    os.makedirs(USER_DIR, exist_ok=True)
    os.chmod(USER_DIR, 0o700)
    subprocess.call([KEYGEN, "-q", "-t", "ed25519", "-N", "", "-f", HOST_KEY],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not os.path.isfile(HOST_KEY):
        raise RemoteShellError("Could not make a host key.")
    os.chmod(HOST_KEY, 0o600)
    return HOST_KEY


def host_fingerprint():
    """The phone's fingerprint, to compare against on first connection."""
    if not os.path.isfile(HOST_KEY + ".pub"):
        return ""
    try:
        out = subprocess.check_output([KEYGEN, "-lf", HOST_KEY + ".pub"],
                                      stderr=subprocess.DEVNULL)
        return out.decode("ascii", "replace").strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


# --- the daemon -----------------------------------------------------------

def write_sshd_config():
    """Generate sshd_config. Rewritten on every start, deliberately.

    A config file that persists is a config file somebody edits at 2am to
    get past something, and then it stays edited. This one is generated
    from here every time, so what the source says is what the phone runs.
    """
    os.makedirs(USER_DIR, exist_ok=True)
    os.chmod(USER_DIR, 0o700)
    text = "\n".join((
        "# Generated by System/core/RemoteShell. Edits are overwritten.",
        "ListenAddress 127.0.0.1",          # never a public interface
        "Port %d" % LOCAL_PORT,
        "HostKey %s" % HOST_KEY,
        "AuthorizedKeysFile %s" % AUTHORIZED_KEYS,
        "PermitRootLogin prohibit-password",
        "PasswordAuthentication no",
        "KbdInteractiveAuthentication no",
        "ChallengeResponseAuthentication no",
        "PermitEmptyPasswords no",
        "UsePAM no",
        "PubkeyAuthentication yes",
        "X11Forwarding no",
        "AllowAgentForwarding no",
        "AllowTcpForwarding no",           # a shell, not a proxy
        "PidFile %s" % os.path.join(USER_DIR, "sshd.pid"),
        "Subsystem sftp %s" % SFTP_SERVER,
        "",
    ))
    handle = os.open(SSHD_CONFIG, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.write(handle, text.encode("ascii"))
    finally:
        os.close(handle)
    return SSHD_CONFIG


def sshd_command():
    return [SSHD, "-f", SSHD_CONFIG, "-D", "-e"]


def tunnel_command(host, user, port):
    """The outbound connection, and every option that keeps it honest."""
    return [
        SSH, "-N", "-T",
        "-i", RELAY_KEY,
        "-o", "IdentitiesOnly=yes",
        "-o", "BatchMode=yes",                 # never prompt: nobody is there
        "-o", "StrictHostKeyChecking=yes",     # the relay must be the relay
        "-o", "UserKnownHostsFile=%s" % KNOWN_HOSTS,
        "-o", "ExitOnForwardFailure=yes",      # no tunnel is a failure, not a shell
        "-o", "ServerAliveInterval=30",
        "-o", "ServerAliveCountMax=3",
        "-o", "ConnectTimeout=20",
        "-R", "%s:127.0.0.1:%d" % (port, LOCAL_PORT),
        "%s@%s" % (user, host),
    ]


def check_ready():
    """Raise RemoteShellError unless this phone can actually do it."""
    if not os.path.isfile(SSHD):
        raise RemoteShellError("This build has no ssh server.")
    current = settings()
    if not current["host"]:
        raise RemoteShellError("No relay address set.")
    if not os.path.isfile(RELAY_KEY):
        raise RemoteShellError("No relay key. Copy one from the card.")
    if not os.path.isfile(AUTHORIZED_KEYS):
        raise RemoteShellError("No authorized_keys. Copy one from the card.")
    if not os.path.isfile(KNOWN_HOSTS):
        raise RemoteShellError(
            "No known_hosts for the relay. Copy one from the card.")
    return current


# --- running it -----------------------------------------------------------
#
# Two processes: sshd, and a loop that keeps the outbound connection up. The
# loop is a shell script rather than a thread here because it has to outlive
# the app that started it -- the operator turns Remote Shell on and then
# goes back to using the phone, or reboots it.

TUNNEL_SCRIPT = os.path.join(USER_DIR, "tunnel.sh")
TUNNEL_PID = os.path.join(USER_DIR, "tunnel.pid")
SSHD_PID = os.path.join(USER_DIR, "sshd.pid")
LOG_FILE = os.path.join(USER_DIR, "remote.log")


def _quote(word):
    return "'" + str(word).replace("'", "'\\''") + "'"


def write_tunnel_script(host, user, port):
    """A reconnect loop. Mobile data drops; that is not an error."""
    os.makedirs(USER_DIR, exist_ok=True)
    os.chmod(USER_DIR, 0o700)
    command = " ".join(_quote(part) for part in tunnel_command(host, user, port))
    text = "\n".join((
        "#!/bin/sh",
        "# Generated by System/core/RemoteShell. Edits are overwritten.",
        "while :; do",
        "    echo \"[RSHELL] dialling %s@%s\"" % (user, host),
        "    " + command,
        "    echo \"[RSHELL] connection ended (%s); retrying\" \"$?\"",
        "    sleep %d" % RETRY_SECONDS,
        "done",
        "",
    ))
    handle = os.open(TUNNEL_SCRIPT, os.O_WRONLY | os.O_CREAT | os.O_TRUNC,
                     0o700)
    try:
        os.write(handle, text.encode("ascii"))
    finally:
        os.close(handle)
    return TUNNEL_SCRIPT


def _pid_from(path):
    try:
        with open(path) as handle:
            return int(handle.read().strip())
    except (OSError, ValueError):
        return None


def _alive(pid):
    if not pid:
        return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def _stop_pid(path):
    pid = _pid_from(path)
    if _alive(pid):
        try:
            # The tunnel is a shell loop, so signal the group: killing the
            # script alone leaves the ssh it is waiting on connected, and
            # "off" has to mean off.
            os.killpg(os.getpgid(pid), signal.SIGTERM)
        except OSError:
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                pass
    try:
        os.unlink(path)
    except OSError:
        pass


def status():
    """What is actually running, not what was asked for."""
    return {
        "sshd": _alive(_pid_from(SSHD_PID)),
        "tunnel": _alive(_pid_from(TUNNEL_PID)),
        "enabled": settings()["enabled"],
    }


def start():
    """Bring it up. Raises RemoteShellError with something worth reading."""
    current = check_ready()
    ensure_host_key()
    write_sshd_config()
    write_tunnel_script(current["host"], current["user"], current["port"])

    stop(remember=False)          # never end up with two of either

    log = open(LOG_FILE, "ab", 0)
    try:
        subprocess.Popen(sshd_command(), stdout=log, stderr=log,
                         start_new_session=True)
        # sshd writes its own PidFile, but not instantly, and the app wants
        # to say "on" the moment it is. Give it a beat, then trust the file.
        for _ in range(20):
            if _alive(_pid_from(SSHD_PID)):
                break
            time.sleep(0.1)

        tunnel = subprocess.Popen(["/bin/sh", TUNNEL_SCRIPT],
                                  stdout=log, stderr=log,
                                  start_new_session=True)
        with open(TUNNEL_PID, "w") as handle:
            handle.write("%d\n" % tunnel.pid)
    except OSError as exc:
        raise RemoteShellError("Could not start: %s" % exc)

    save_settings(enabled=True)
    return status()


def stop(remember=True):
    """Take it down. Safe to call when it is already down."""
    _stop_pid(TUNNEL_PID)
    _stop_pid(SSHD_PID)
    if remember:
        save_settings(enabled=False)
    return status()


def start_if_enabled():
    """Called at boot. Silent when it was never turned on."""
    if not settings()["enabled"]:
        return None
    try:
        return start()
    except RemoteShellError as exc:
        print("[RSHELL] not starting: %s" % exc)
        return None

"""Remote Shell: ssh and sftp to the phone, over the internet.

This is the one feature on the phone whose whole purpose is to let someone
in from outside, so the tests are about the things that decide who that is.
Not "does it work" -- "can it be reached by anyone it should not be".

Nothing here starts a real sshd or opens a socket. What is checked is the
configuration the phone generates, because that is what the decisions
actually live in.
"""

import os
import stat

import pytest

from System.core import RemoteShell


@pytest.fixture
def phone(tmp_path, monkeypatch):
    """A phone with its own empty user partition and card."""
    user = tmp_path / "user" / ".remote"
    monkeypatch.setattr(RemoteShell, "USER_DIR", str(user))
    for name in ("STATE_FILE", "SSHD_CONFIG", "HOST_KEY", "AUTHORIZED_KEYS",
                 "RELAY_KEY", "KNOWN_HOSTS", "TUNNEL_SCRIPT", "TUNNEL_PID",
                 "SSHD_PID", "LOG_FILE"):
        monkeypatch.setattr(RemoteShell, name,
                            os.path.join(str(user),
                                         os.path.basename(
                                             getattr(RemoteShell, name))))
    monkeypatch.setattr(RemoteShell, "CARD_FILES", {
        "id_ed25519": RemoteShell.RELAY_KEY,
        "authorized_keys": RemoteShell.AUTHORIZED_KEYS,
        "known_hosts": RemoteShell.KNOWN_HOSTS,
    })
    card = tmp_path / "sdcard"
    (card / "remote").mkdir(parents=True)
    return type("Phone", (), {"user": user, "card": card, "tmp": tmp_path})()


# --- who can reach the phone -----------------------------------------------

def test_sshd_listens_on_loopback_and_nowhere_else(phone):
    """The point of the whole design. The phone is on mobile data with a
    public IPv6 address; an sshd on 0.0.0.0 or :: would be reachable by the
    entire internet, and the only thing between it and them would be the
    key check. The way in is the tunnel the phone dialled, or nothing."""
    RemoteShell.write_sshd_config()

    config = open(RemoteShell.SSHD_CONFIG).read()
    assert "ListenAddress 127.0.0.1" in config
    assert "0.0.0.0" not in config
    assert "ListenAddress ::" not in config


def test_passwords_cannot_log_in(phone):
    """Both of them. Turning PasswordAuthentication off and leaving
    keyboard-interactive on is a well-worn way to believe you did this."""
    RemoteShell.write_sshd_config()

    config = open(RemoteShell.SSHD_CONFIG).read()
    assert "PasswordAuthentication no" in config
    assert "KbdInteractiveAuthentication no" in config
    assert "ChallengeResponseAuthentication no" in config
    assert "PermitEmptyPasswords no" in config
    assert "PermitRootLogin prohibit-password" in config
    assert "PubkeyAuthentication yes" in config


def test_the_phone_is_not_a_proxy(phone):
    """A shell, not a way into whatever else the phone can route to."""
    RemoteShell.write_sshd_config()

    config = open(RemoteShell.SSHD_CONFIG).read()
    assert "AllowTcpForwarding no" in config
    assert "AllowAgentForwarding no" in config
    assert "X11Forwarding no" in config


def test_sftp_is_offered(phone):
    """Half the reason for this: pulling files off without a card."""
    RemoteShell.write_sshd_config()

    assert "Subsystem sftp" in open(RemoteShell.SSHD_CONFIG).read()


def test_the_config_is_rewritten_every_time(phone):
    """So an edit made at 2am to get past something does not stay made."""
    RemoteShell.write_sshd_config()
    with open(RemoteShell.SSHD_CONFIG, "a") as handle:
        handle.write("\nPasswordAuthentication yes\n")

    RemoteShell.write_sshd_config()

    config = open(RemoteShell.SSHD_CONFIG).read()
    assert "PasswordAuthentication yes" not in config


def test_the_config_is_not_world_readable(phone):
    RemoteShell.write_sshd_config()

    mode = stat.S_IMODE(os.stat(RemoteShell.SSHD_CONFIG).st_mode)
    assert mode & 0o077 == 0, oct(mode)


# --- who the phone dials ---------------------------------------------------

def test_the_relay_host_key_is_checked(phone):
    """Without this the tunnel goes to whoever answers on that address."""
    command = RemoteShell.tunnel_command("relay.example", "neodct", "2222")

    assert "StrictHostKeyChecking=yes" in command
    assert "UserKnownHostsFile=%s" % RemoteShell.KNOWN_HOSTS in command
    assert not any("StrictHostKeyChecking=no" in str(part) for part in command)


def test_dialling_out_never_prompts(phone):
    """Nobody is holding the phone when it reconnects at 3am."""
    command = RemoteShell.tunnel_command("relay.example", "neodct", "2222")

    assert "BatchMode=yes" in command
    assert "IdentitiesOnly=yes" in command


def test_a_tunnel_that_cannot_forward_is_a_failure(phone):
    """Without ExitOnForwardFailure the phone sits in a connection that
    looks fine and cannot be reached through, and the retry loop never
    retries because nothing ever ended."""
    command = RemoteShell.tunnel_command("relay.example", "neodct", "2222")

    assert "ExitOnForwardFailure=yes" in command


def test_the_forward_points_at_loopback(phone):
    """127.0.0.1 on the phone's side: the relay reaches the sshd through
    the tunnel, not through any interface the phone has."""
    command = RemoteShell.tunnel_command("relay.example", "neodct", "2222")

    assert "-R" in command
    forward = command[command.index("-R") + 1]
    assert forward == "2222:127.0.0.1:%d" % RemoteShell.LOCAL_PORT


def test_a_relay_address_with_a_quote_in_it_cannot_run_a_command(phone, tmp_path):
    """The address is typed on a keypad straight into a generated shell
    script. It is data, and a shell is very willing to disagree.

    Run the real thing: a stub ssh on PATH records the arguments it was
    handed, and the marker file says whether the shell was talked into
    running anything of its own."""
    import subprocess

    marker = tmp_path / "pwned"
    hostile = "a'; touch %s; '" % marker
    seen = tmp_path / "argv"
    stub = tmp_path / "bin"
    stub.mkdir()
    (stub / "ssh").write_text(
        '#!/bin/sh\nfor a in "$@"; do echo "$a"; done > %s\n' % seen)
    (stub / "ssh").chmod(0o755)

    monkey = RemoteShell.SSH
    try:
        RemoteShell.SSH = str(stub / "ssh")
        RemoteShell.write_tunnel_script(hostile, "neodct", "2222")
    finally:
        RemoteShell.SSH = monkey

    # Run one pass of the loop body rather than the loop.
    body = [line for line in open(RemoteShell.TUNNEL_SCRIPT)
            if str(stub / "ssh") in line][0]
    subprocess.run(["sh", "-c", body], cwd=str(tmp_path), check=False)

    assert not marker.exists(), "the address escaped into the shell"
    handed = seen.read_text().splitlines()
    assert handed[-1] == "neodct@%s" % hostile, handed[-1]


# --- refusing to start ------------------------------------------------------

def test_it_will_not_start_without_a_relay(phone):
    with pytest.raises(RemoteShell.RemoteShellError, match="relay address"):
        RemoteShell.check_ready()


def test_it_will_not_start_without_the_operators_key(phone):
    """No authorized_keys means nobody could log in anyway, and an sshd
    listening for nobody is just a thing that can go wrong."""
    RemoteShell.save_settings(host="relay.example")
    _touch(RemoteShell.RELAY_KEY)

    with pytest.raises(RemoteShell.RemoteShellError, match="authorized_keys"):
        RemoteShell.check_ready()


def test_it_will_not_start_without_knowing_the_relay(phone):
    RemoteShell.save_settings(host="relay.example")
    _touch(RemoteShell.RELAY_KEY)
    _touch(RemoteShell.AUTHORIZED_KEYS)

    with pytest.raises(RemoteShell.RemoteShellError, match="known_hosts"):
        RemoteShell.check_ready()


# --- it is off unless somebody turned it on ---------------------------------

def test_a_new_phone_has_it_off(phone):
    assert RemoteShell.settings()["enabled"] is False
    assert RemoteShell.status()["sshd"] is False


def test_boot_does_nothing_when_it_was_never_turned_on(phone, monkeypatch):
    started = []
    monkeypatch.setattr(RemoteShell, "start", lambda: started.append(True))

    RemoteShell.start_if_enabled()

    assert started == []


def test_boot_does_not_start_it_when_the_keys_are_gone(phone, capsys):
    """Enabled, but somebody wiped the user partition. It must say so and
    stay down rather than starting an sshd nobody can log in to."""
    RemoteShell.save_settings(host="relay.example", enabled=True)

    assert RemoteShell.start_if_enabled() is None
    assert "[RSHELL]" in capsys.readouterr().out


# --- keys off the card ------------------------------------------------------

def test_keys_copied_from_the_card_are_not_world_readable(phone):
    """The card is vfat, which has no permission bits, so everything on it
    arrives as 0777. ssh refuses a private key like that, and is right to."""
    for name in ("id_ed25519", "authorized_keys", "known_hosts"):
        (phone.card / "remote" / name).write_text("key material\n")

    RemoteShell.install_keys_from_card(phone.card)

    for path in (RemoteShell.RELAY_KEY, RemoteShell.AUTHORIZED_KEYS,
                 RemoteShell.KNOWN_HOSTS):
        mode = stat.S_IMODE(os.stat(path).st_mode)
        assert mode & 0o077 == 0, "%s is %s" % (path, oct(mode))


def test_the_key_directory_is_not_world_readable(phone):
    (phone.card / "remote" / "id_ed25519").write_text("key\n")

    RemoteShell.install_keys_from_card(phone.card)

    mode = stat.S_IMODE(os.stat(RemoteShell.USER_DIR).st_mode)
    assert mode & 0o077 == 0, oct(mode)


def test_a_card_with_no_remote_folder_says_so(phone, tmp_path):
    empty = tmp_path / "blank"
    empty.mkdir()

    with pytest.raises(RemoteShell.RemoteShellError, match="folder"):
        RemoteShell.install_keys_from_card(empty)


def test_an_empty_remote_folder_says_so(phone):
    with pytest.raises(RemoteShell.RemoteShellError, match="No keys"):
        RemoteShell.install_keys_from_card(phone.card)


def _touch(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as handle:
        handle.write("x\n")


# --- things a real boot found ----------------------------------------------

def test_the_config_has_no_option_this_sshd_rejects(phone):
    """sshd logged "Unsupported option UsePAM" on the first real boot: this
    build has no PAM. It carried on, but a warning in a log that is meant to
    be worth reading is a warning that trains you to skim it."""
    RemoteShell.write_sshd_config()

    assert "UsePAM" not in open(RemoteShell.SSHD_CONFIG).read()


def test_the_retry_message_carries_the_exit_code(phone):
    """It read "connection ended (%s); retrying 255" on the first boot --
    echo is not printf, so the code landed after the sentence instead of in
    it, and the placeholder was printed literally."""
    RemoteShell.write_tunnel_script("relay.example", "neodct", "2222")

    script = open(RemoteShell.TUNNEL_SCRIPT).read()
    assert "%s" not in script
    assert "($?)" in script


def test_the_loop_waits_before_dialling_again(phone):
    """No relay means this runs forever. Without the sleep it is a phone
    that spends its battery failing to connect as fast as it can."""
    RemoteShell.write_tunnel_script("relay.example", "neodct", "2222")

    assert "sleep %d" % RemoteShell.RETRY_SECONDS in open(
        RemoteShell.TUNNEL_SCRIPT).read()


# --- nothing else may start an sshd -----------------------------------------

def _target_dir():
    """The built target tree, if there is one to look at."""
    import glob
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for candidate in ("buildroot/output/target", "build-luckfox/target"):
        path = os.path.join(os.path.dirname(here), candidate)
        if os.path.isdir(os.path.join(path, "etc", "init.d")):
            return path
    return None


def test_no_boot_script_starts_sshd():
    """openssh ships /etc/init.d/S50sshd, which starts sshd at every boot
    with the stock config -- and the stock config listens on every
    interface. This phone has a public IPv6 address on mobile data, so that
    is an sshd facing the internet whether or not anyone enabled Remote
    Shell, on a phone whose root account has an empty password field.

    RemoteShell decides when sshd runs. The prune script drops that file;
    this is the check that it stayed dropped."""
    target = _target_dir()
    if target is None:
        pytest.skip("no built target tree to inspect")

    init_dir = os.path.join(target, "etc", "init.d")
    offenders = []
    for name in sorted(os.listdir(init_dir)):
        path = os.path.join(init_dir, name)
        if not os.path.isfile(path):
            continue
        try:
            body = open(path, "r", errors="replace").read()
        except OSError:
            continue
        # A script that launches the daemon, as opposed to one that merely
        # mentions it (RemoteShell's own name appears in logs).
        if "sshd" in body and ("start-stop-daemon" in body or "/usr/sbin/sshd" in body):
            offenders.append(name)

    assert not offenders, (
        "these boot scripts start sshd outside RemoteShell's control: %s"
        % ", ".join(offenders))


def test_the_prune_script_drops_the_openssh_boot_script(tmp_path):
    """Run the real prune script against a tree that has one."""
    import subprocess
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    prune = os.path.join(here, "scripts", "post-build-prune-tests.sh")

    target = tmp_path / "target"
    (target / "etc" / "init.d").mkdir(parents=True)
    (target / "etc" / "init.d" / "S50sshd").write_text("#!/bin/sh\nsshd\n")
    (target / "etc" / "init.d" / "S40network").write_text("#!/bin/sh\n")
    (target / "NeoDCT").mkdir()

    subprocess.run(["sh", prune, str(target), "qemu-aarch64"],
                   check=True, capture_output=True)

    assert not (target / "etc" / "init.d" / "S50sshd").exists()
    assert (target / "etc" / "init.d" / "S40network").exists()

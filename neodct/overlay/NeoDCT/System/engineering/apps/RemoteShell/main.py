"""Remote Shell: turn ssh and sftp to this phone on, and say who to dial.

The work is in System/core/RemoteShell, which explains the shape of the
thing and holds every decision about who can get in. This is the switch and
the address book: a list you can operate with a keypad and one thumb.

Deliberately plain. The phone is either reachable or it is not, and the
screen should say which without being read carefully.
"""

import os
import sys

_APP_DIR = os.path.dirname(os.path.abspath(__file__))
if _APP_DIR not in sys.path:
    sys.path.insert(0, _APP_DIR)

from System.core import RemoteShell
from System.core import Storage
from System.ui.framework import (MessageDialog, SoftKeyBar, TextInput,
                                 VerticalList)

APP_ID = 9007
TITLE = "Remote Shell"

KEY_ENTER = 28


def _tell(ui, message):
    MessageDialog(ui, message, title=TITLE, button_text="OK").show()


def _confirm(ui, question, button="Yes"):
    return MessageDialog(ui, question, title=TITLE,
                         button_text=button).show() == KEY_ENTER


def _menu_lines():
    """The list, rebuilt each time round: it is also the status display."""
    state = RemoteShell.status()
    current = RemoteShell.settings()

    if state["sshd"] and state["tunnel"]:
        running = "On"
    elif state["sshd"] or state["tunnel"]:
        # Half up is worth naming. It means the relay refused the tunnel or
        # dropped it, and "On" would be a lie while nothing can reach you.
        running = "Dialling"
    else:
        running = "Off"

    return [
        "Status: %s" % running,
        "Turn off" if (state["sshd"] or state["tunnel"]) else "Turn on",
        "Relay: %s" % (current["host"] or "not set"),
        "Login: %s" % current["user"],
        "Port: %s" % current["port"],
        "Copy keys from card",
        "This phone's key",
    ]


STATUS, TOGGLE, RELAY, LOGIN, PORT, KEYS, FINGERPRINT = range(7)


def _set_relay(ui):
    """The relay's address. IPv6 is the likely answer -- mobile data here
    is IPv6, so the relay has to be reachable over it."""
    current = RemoteShell.settings()
    entry = TextInput(ui, TITLE, "Relay host:", initial_text=current["host"])
    value = entry.show()
    if value is None:
        return
    RemoteShell.save_settings(host=value)


def _set_login(ui):
    current = RemoteShell.settings()
    entry = TextInput(ui, TITLE, "Login:", initial_text=current["user"],
                      input_filter="letters")
    value = entry.show()
    if value is None:
        return
    RemoteShell.save_settings(user=value)


def _set_port(ui):
    current = RemoteShell.settings()
    entry = TextInput(ui, TITLE, "Relay port:", initial_text=current["port"],
                      input_filter="numbers")
    value = entry.show()
    if value is None:
        return
    RemoteShell.save_settings(port=value)


def _copy_keys(ui):
    """Take the operator's keys off the card."""
    card = Storage.MOUNT_POINT
    if not os.path.isdir(card):
        _tell(ui, "No card in the phone.")
        return
    try:
        taken = RemoteShell.install_keys_from_card(card)
    except RemoteShell.RemoteShellError as exc:
        _tell(ui, str(exc))
        return
    _tell(ui, "Copied: %s.\n\nDelete them from the card now -- anyone who "
              "takes the card out can read them." % ", ".join(sorted(taken)))


def _show_fingerprint(ui):
    """So the first connection can be checked against something."""
    try:
        RemoteShell.ensure_host_key()
    except RemoteShell.RemoteShellError as exc:
        _tell(ui, str(exc))
        return
    fingerprint = RemoteShell.host_fingerprint() or "unknown"
    _tell(ui, "This phone:\n%s" % fingerprint)


def _turn_on(ui):
    try:
        RemoteShell.start()
    except RemoteShell.RemoteShellError as exc:
        _tell(ui, str(exc))
        return
    _tell(ui, "Remote Shell is on.\n\nIt stays on across restarts until you "
              "turn it off here.")


def _turn_off(ui):
    RemoteShell.stop()
    _tell(ui, "Remote Shell is off.")


def run(ui):
    while True:
        menu = VerticalList(ui, TITLE, _menu_lines(), app_id=APP_ID)
        SoftKeyBar(ui).update("Select", present=False)
        choice = menu.show()

        if choice < 0:
            return
        if choice == TOGGLE:
            state = RemoteShell.status()
            if state["sshd"] or state["tunnel"]:
                _turn_off(ui)
            elif _confirm(ui, "Let this phone be reached over the internet?",
                          button="Turn on"):
                _turn_on(ui)
        elif choice == RELAY:
            _set_relay(ui)
        elif choice == LOGIN:
            _set_login(ui)
        elif choice == PORT:
            _set_port(ui)
        elif choice == KEYS:
            _copy_keys(ui)
        elif choice == FINGERPRINT:
            _show_fingerprint(ui)

"""System-level error and notice screens.

Thin wrappers around `System.ui.framework.MessageDialog`.

- `show(...)` displays a modal warning/error and returns after OK.
- `show_alpha_security_notice_once(...)` shows the "no security yet" disclaimer once.
"""

from __future__ import annotations

import os
import time

from System.ui.framework import MessageDialog

DEFAULT_WARNING_ICON = "/NeoDCT/System/ui/resources/img/errorscreen/warning.png"
DEFAULT_ACK_FILE = "/NeoDCT/User/.ack_security_warning"


def show_error(
    ui,
    message,
    *,
    title=None,
    icon_path=DEFAULT_WARNING_ICON,
    button_text="OK",
    accept_keys=(28,),
    cancel_keys=(14,),
):
    dlg = MessageDialog(
        ui,
        message,
        title=title,
        icon_path=icon_path,
        button_text=button_text,
        accept_keys=accept_keys,
        cancel_keys=cancel_keys,
    )
    return dlg.show()


def show_alpha_security_notice_once(
    ui,
    *,
    ack_path=DEFAULT_ACK_FILE,
    message=None,
):
    """Returns True if the notice was shown, False if it was already acknowledged."""
    if message is None:
        message = (
            "This is alpha software. There is currently no real security implemented whatsoever and "
            "all development features are enabled by default. DO NOT STORE SENSITIVE DATA ON THIS DEVICE!!!"
        )

    try:
        if os.path.exists(ack_path):
            return False
    except Exception:
        pass

    show_error(ui, message, title="Security warning", icon_path=DEFAULT_WARNING_ICON, button_text="OK")

    # Best-effort persistence.
    try:
        os.makedirs(os.path.dirname(ack_path), exist_ok=True)
        with open(ack_path, "w", encoding="utf-8") as f:
            f.write(str(int(time.time())))
    except Exception:
        pass

    return True

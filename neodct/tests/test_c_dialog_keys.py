"""A MessageDialog with no accept key and no cancel key never returns.

nd_msgdialog_show()'s loop breaks only on a key in one of the two sets, so
passing both as empty is a screen the phone cannot be got off:

    for (;;) {
        int32_t key = nd_ui_wait_for_key(d->ui);
        if (key_in(accept, n_accept, key) || key_in(cancel, n_cancel, key))
            break;
    }

nd_ui_wait_for_key() never gives up and never polls nd_app_should_exit(), so
neither a keypress nor a SIGTERM ends it. The app never returns from app_run(),
nd-core stays blocked waiting for it, and the phone is dead until the battery
comes out.

THE PROJECT HAS HIT THIS TWICE. apps/Settings/main.c's install_notice() carries
the scar in a comment -- it froze the phone on the "Installed ..." notice after
every install -- and the same file still had seven more of them in the memory
card path: a format that failed, one that could not start, one that ran out of
time, a card with no device, a setup that could not write, and a card whose
status could not be read. Several are the ordinary answer for a card that is
merely busy.

The intent behind them is real and is kept: an EMPTY CANCEL SET means Clear
cannot back past a result the owner has to acknowledge. It is only the empty
ACCEPT set that is no way out. A notice that genuinely must not be dismissed --
the low-battery shutdown the library comment cites, which has no caller in the
tree -- wants nd_msgdialog_render(), which draws and returns.
"""

import os
import re

import pytest

SRC = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src")

# set_keys(d, <anything>, 0, <anything>, 0) -- both counts literally zero.
BAD = re.compile(r"nd_msgdialog_set_keys\s*\([^;]*?,\s*0u?\s*,[^;]*?,\s*0u?\s*\)",
                 re.S)


def c_sources():
    for root, dirs, files in os.walk(SRC):
        dirs[:] = [d for d in dirs if d not in ("build", "unit")]
        for name in files:
            if name.endswith((".c", ".h")):
                yield os.path.join(root, name)


def code(path):
    """The file with its comments removed; see test_c_string_helpers.py."""
    out, in_block, i = [], False, 0
    text = open(path, errors="replace").read()
    while i < len(text):
        if in_block:
            end = text.find("*/", i)
            if end < 0:
                break
            in_block, i = False, end + 2
            continue
        start, slash = text.find("/*", i), text.find("//", i)
        if slash >= 0 and (start < 0 or slash < start):
            nl = text.find("\n", slash)
            out.append(text[i:slash])
            i = len(text) if nl < 0 else nl
            continue
        if start < 0:
            out.append(text[i:])
            break
        out.append(text[i:start])
        in_block, i = True, start + 2
    return "".join(out)


def test_there_are_sources_to_check():
    assert sum(1 for _ in c_sources()) > 100


@pytest.mark.parametrize("path", sorted(c_sources()),
                         ids=lambda p: os.path.relpath(p, SRC))
def test_no_dialog_is_built_with_no_way_out(path):
    hits = BAD.findall(code(path))
    assert not hits, (
        "%s builds a MessageDialog with an empty accept set AND an empty "
        "cancel set. nd_msgdialog_show() never returns from that, and the "
        "phone is dead until the battery comes out. Keep the empty CANCEL "
        "set if Clear must not dismiss it, and give the accept set "
        "ND_KEY_ENTER; for a notice that genuinely must not be dismissed, "
        "use nd_msgdialog_render().\n  %s"
        % (os.path.relpath(path, SRC), "\n  ".join(h.strip() for h in hits)))

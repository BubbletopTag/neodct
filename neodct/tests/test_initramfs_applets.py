"""Every command the initramfs scripts call must exist in the initramfs.

busybox is one binary; an applet only becomes runnable if something creates
a symlink for it. mkinitramfs creates a fixed list, so a script that starts
using `tr` without that list being updated fails at boot -- or worse, fails
silently inside a command substitution and makes a good update look
truncated.

This scans the scripts for command names and checks the list covers them,
so the two cannot drift apart.
"""

import os
import re
import sys

import pytest

SCRIPTS_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"
)
if SCRIPTS_DIR not in sys.path:
    sys.path.insert(0, SCRIPTS_DIR)

import mkinitramfs

INITRAMFS_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "initramfs"
)

# ash builtins and shell keywords: no symlink needed for these.
SHELL_WORDS = {
    "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done",
    "case", "esac", "in", "return", "exit", "break", "continue", "shift",
    "export", "readonly", "local", "set", "unset", "eval", "exec", "trap",
    "read", "echo", "printf", "test", "true", "false", "cd", "pwd", "wait",
    "command", "type", "hash", "umask", "times", ".", ":", "[", "[[",
    "esac;", "fi;", "done;",
}

# Provided as real binaries rather than busybox applets.
EXTRA_BINARIES = {"dmsetup"}


def script_paths():
    return [os.path.join(INITRAMFS_DIR, name)
            for name in sorted(os.listdir(INITRAMFS_DIR))
            if os.path.isfile(os.path.join(INITRAMFS_DIR, name))]


def defined_functions(text):
    return set(re.findall(r"^\s*([a-z_][a-z0-9_]*)\s*\(\)", text, re.M))


def invoked_commands(text):
    """First word of every command position we can spot statically."""
    # Drop comments and here-doc bodies -- neither runs anything.
    lines = []
    in_heredoc = False
    for line in text.splitlines():
        if in_heredoc:
            if line.strip() == "EOF":
                in_heredoc = False
            continue
        if re.search(r"<<\s*EOF", line):
            in_heredoc = True
            continue
        stripped = line.strip()
        if stripped.startswith("#"):
            continue
        lines.append(line)
    body = "\n".join(lines)

    # Blank out quoted strings: a ";" inside a log message is not a command
    # separator, and words inside one are never in command position.
    body = re.sub(r'"[^"\n]*"', '""', body)
    body = re.sub(r"'[^'\n]*'", "''", body)

    # Split on everything that starts a fresh command position.
    fragments = re.split(r"\$\(|`|\||&&|\|\||;|\n|\{|\}|\bthen\b|\bdo\b|\belse\b",
                         body)
    commands = set()
    for fragment in fragments:
        fragment = fragment.strip()
        if not fragment:
            continue
        word = fragment.split()[0]
        # Redirections, assignments, expansions, options: not commands.
        if "=" in word or word.startswith(("$", "-", "<", ">", "!", "(", '"',
                                           "'", ")", "\\")):
            continue
        if not re.fullmatch(r"[a-z][a-z0-9_.-]*", word):
            continue
        commands.add(word)
    return commands


@pytest.mark.parametrize("path", script_paths())
def test_every_command_used_is_available_in_the_initramfs(path):
    with open(path) as handle:
        text = handle.read()

    used = invoked_commands(text)
    provided = (set(mkinitramfs.APPLETS) | SHELL_WORDS | EXTRA_BINARIES
                | defined_functions(text))
    # Functions defined in the other script are sourced in at runtime.
    for other in script_paths():
        if other != path:
            with open(other) as handle:
                provided |= defined_functions(handle.read())

    missing = sorted(used - provided)

    assert not missing, (
        "%s calls %s, which mkinitramfs.APPLETS does not create symlinks for"
        % (os.path.basename(path), ", ".join(missing)))


def test_the_applet_list_has_no_duplicates():
    assert len(mkinitramfs.APPLETS) == len(set(mkinitramfs.APPLETS))


def test_sh_is_available_because_the_kernel_needs_it_for_the_shebang():
    assert "sh" in mkinitramfs.APPLETS


# --- commands reached through a variable ---------------------------------
#
# The scan above can only see literal command names. ndsys-apply.sh calls the
# UBI writer as "$NDSYS_UBIUPDATEVOL", so that the host tests can substitute a
# stand-in -- which means the scanner cannot see it and would let the applet
# go missing without a word.
#
# It going missing is not a small thing. It is the ONLY way to write the
# phone's system partition: on the Luckfox that partition is a static UBI
# volume behind a read-only ubiblock disk, so if the applet is absent every
# update fails to install, retries three times across three boots, and is
# then discarded -- with the phone reporting nothing worse than having
# rebooted.
def test_the_ubi_writer_is_in_the_initramfs():
    assert "ubiupdatevol" in mkinitramfs.APPLETS


def test_the_ubi_resizer_is_in_the_initramfs():
    """Growing the volume happens in the initramfs, not on the booted phone.

    Easy to get wrong, and it was: neodct/tools/test_update_ubi.sh sources the
    applier on a RUNNING phone, where every busybox applet the target build
    installed is on the PATH. apply_pending really runs one step earlier, in
    the initramfs, which has only the applets mkinitramfs symlinks. A resize
    that works in that test and not on the phone is exactly the shape of bug
    this whole exercise is about.
    """
    assert "ubirsvol" in mkinitramfs.APPLETS


def test_busybox_is_configured_to_build_the_ubi_writer():
    """The applet symlink is useless if busybox was built without it."""
    fragment = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "..", "buildroot", "board", "qemu", "busybox.fragment")
    text = open(os.path.normpath(fragment)).read()
    assert "CONFIG_UBIUPDATEVOL=y" in text

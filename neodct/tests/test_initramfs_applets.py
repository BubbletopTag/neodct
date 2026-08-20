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

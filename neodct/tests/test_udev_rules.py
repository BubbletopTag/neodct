"""61-neodct-devices.rules is parsed by two parsers that disagree.

The rules file is what gives a non-root UI its keypad, its backlight and its
clock. It has no test at runtime: eudev logs one line to a kernel ring buffer
nobody reads and carries on, so every mistake in it looks exactly like
hardware that does not work.

Three separate things can go wrong, and each is checked here against what
eudev 3.2.14 actually does rather than against what a shell would do:

  THE VALUE PARSER      get_key() in src/udev/udev-rules.c. A rule value runs
                        from one double quote to the next.

  THE ARGV PARSER       udev_build_argv() in src/udev/udev-event.c. It splits
                        that value into argv with NO escapes at all, so a
                        quote character inside a `sh -c` script silently ends
                        the argument in the middle of itself.

  THE NAMES             GROUP= is resolved at parse time, and an unknown
                        group is not an error -- eudev falls back to gid 0,
                        which also drops the node to 0600. A typo here is a
                        keypad that does not work on the phone and nothing
                        at all on the build host.

Plus the ordinary one: every command a RUN+= invokes has to be an applet
this busybox was actually built with.
"""

import os
import re

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RULES = os.path.join(
    REPO, "neodct", "overlay", "etc", "udev", "rules.d", "61-neodct-devices.rules"
)
USERS_TABLE = os.path.join(REPO, "neodct", "configs", "users-table.txt")
SKELETON_GROUP = os.path.join(REPO, "buildroot", "system", "skeleton", "etc", "group")
UDEV_MK = os.path.join(REPO, "buildroot", "package", "udev", "udev.mk")
BUSYBOX_CONFIG = os.path.join(REPO, "buildroot", "package", "busybox", "busybox.config")

# eudev's substitution table, src/udev/udev-event.c. $<name> and %<char> both
# expand; $$ and %% are the literals.
SUBST_NAMES = [
    "devnode", "tempnode", "attr", "sysfs", "env", "kernel", "number",
    "driver", "devpath", "id", "major", "minor", "result", "parent", "name",
    "links", "root", "sys",
]

# ash builtins: no applet needed.
SHELL_BUILTINS = {
    "for", "do", "done", "if", "then", "else", "elif", "fi", "while", "until",
    "case", "esac", "in", "echo", "printf", "read", "test", "[", "true",
    "false", "export", "local", "set", "unset", "eval", "exec", "exit",
    "return", "break", "continue", "shift", "cd", "umask", ":", ".",
}


def logical_lines():
    """The file as eudev reads it: backslash-newline joined, comments gone.

    parse_file() joins a line ending in backslash with the next one before
    add_rule() ever sees it, so a check that works line by line would not be
    checking what is parsed.
    """
    out = []
    pending = ""
    start = 0
    for n, raw in enumerate(open(RULES).read().splitlines(), 1):
        line = raw.rstrip("\n")
        if not pending:
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            start = n
        if line.endswith("\\"):
            pending += line[:-1]
            continue
        out.append((start, pending + line))
        pending = ""
    assert not pending, "the file ends on a continuation"
    return out


def key_values(line):
    """What get_key() would pull out: (key, op, value) for each pair.

    Deliberately a re-implementation rather than a regex over the whole line,
    because the thing being tested IS where each value ends.
    """
    pairs = []
    pos = 0
    while pos < len(line):
        while pos < len(line) and (line[pos].isspace() or line[pos] == ","):
            pos += 1
        if pos >= len(line):
            break
        m = re.compile(r"([A-Za-z_]+(?:\{[^}]*\})?)\s*(==|!=|\+=|-=|:=|=)").match(line, pos)
        assert m, "cannot read a key at %r" % line[pos:]
        pos = m.end()
        assert line[pos] == '"', "value of %s does not start with a quote" % m.group(1)
        pos += 1
        value = []
        while True:
            assert pos < len(line), "unterminated value for %s" % m.group(1)
            if line[pos] == "\\" and pos + 1 < len(line) and line[pos + 1] == '"':
                value.append('"')
                pos += 2
                continue
            if line[pos] == '"':
                pos += 1
                break
            value.append(line[pos])
            pos += 1
        pairs.append((m.group(1), m.group(2), "".join(value)))
    return pairs


def build_argv(value):
    """udev_build_argv(), transcribed. No escapes, quotes are not nestable."""
    if " " not in value:
        return [value]
    argv = []
    pos = 0
    while pos < len(value):
        if value[pos] in "'\"":
            delim = value[pos]
            pos += 1
            end = value.find(delim, pos)
            if end < 0:
                argv.append(value[pos:])
                pos = len(value)
            else:
                argv.append(value[pos:end])
                pos = end + 1
        else:
            end = value.find(" ", pos)
            if end < 0:
                argv.append(value[pos:])
                pos = len(value)
            else:
                argv.append(value[pos:end])
                pos = end + 1
        while pos < len(value) and value[pos] == " ":
            pos += 1
    return argv


def apply_format(value):
    """The $$ and %% halves of udev_event_apply_format(), which is all this
    file uses; a real substitution is left as a marker so the shell check
    below does not mistake it for a command."""
    out = []
    i = 0
    while i < len(value):
        c = value[i]
        if c == "$" and i + 1 < len(value):
            if value[i + 1] == "$":
                out.append("$")
                i += 2
                continue
            for name in SUBST_NAMES:
                if value.startswith(name, i + 1):
                    out.append("SUBST")
                    i += 1 + len(name)
                    break
            else:
                out.append(c)
                i += 1
            continue
        if c == "%" and i + 1 < len(value):
            if value[i + 1] == "%":
                out.append("%")
                i += 2
                continue
            out.append("SUBST")
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def known_groups():
    groups = set()
    for line in open(SKELETON_GROUP):
        if ":" in line:
            groups.add(line.split(":", 1)[0])
    for line in open(UDEV_MK):
        fields = line.split()
        if len(fields) >= 4 and fields[0] == "-" and fields[1] == "-":
            groups.add(fields[2])
    for line in open(USERS_TABLE):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) < 4:
            continue
        if fields[0] == "-":
            groups.add(fields[2])
        else:
            groups.add(fields[0])
            groups.add(fields[2])
    return groups


def busybox_applets():
    """The applets this busybox is built with, by CONFIG_ name.

    Not `ls buildroot/output/target/bin`: that tree only exists after a
    build, and this test has to fail on a checkout.
    """
    enabled = set()
    for line in open(BUSYBOX_CONFIG):
        m = re.match(r"CONFIG_([A-Z0-9_]+)=y\s*$", line)
        if m:
            enabled.add(m.group(1).lower())
    # /bin/sh is not CONFIG_SH. It is whichever shell CONFIG_SH_IS_* names,
    # installed under a second name -- so asking for `sh` directly finds
    # nothing and would fail every RUN rule in the file.
    for alias, option in (("sh", "sh_is_ash"), ("sh", "sh_is_hush"),
                          ("bash", "bash_is_ash"), ("bash", "bash_is_hush")):
        if option in enabled:
            enabled.add(alias)
    return enabled


RULE_LINES = logical_lines()


def test_the_file_has_rules_in_it():
    """A guard on every other test here: a file this one failed to parse
    would otherwise pass all of them by having nothing to check."""
    assert len(RULE_LINES) >= 4


def test_every_line_parses_the_way_get_key_does():
    for lineno, line in RULE_LINES:
        try:
            pairs = key_values(line)
        except AssertionError as exc:
            pytest.fail("line %d: %s" % (lineno, exc))
        assert pairs, "line %d has no key/value pairs" % lineno


def test_no_quote_survives_into_a_run_script():
    """The one that matters, and the one a shell programmer gets wrong.

    udev_build_argv() has no escapes. `sh -c 'it isn't'` is not a syntax
    error anywhere -- it parses as a rule, splits into an argv whose third
    element is `it isn`, and runs that.
    """
    for lineno, line in RULE_LINES:
        for key, _op, value in key_values(line):
            if key != "RUN" and not key.startswith("RUN{"):
                continue
            argv = build_argv(apply_format(value))
            assert argv, "line %d: RUN with an empty argv" % lineno
            if "-c" not in argv:
                continue
            script = argv[argv.index("-c") + 1:]
            assert len(script) == 1, (
                "line %d: the script after -c split into %d arguments, %r -- "
                "there is a quote inside it" % (lineno, len(script), script)
            )
            assert "'" not in script[0] and '"' not in script[0], (
                "line %d: a quote character inside the script" % lineno
            )


def test_a_literal_dollar_is_written_twice():
    """$b works today only because eudev does not know a substitution called
    b. Renaming a shell variable to p, id or name would silently change what
    the script receives."""
    for lineno, line in RULE_LINES:
        for key, _op, value in key_values(line):
            if not key.startswith("RUN"):
                continue
            i = 0
            while i < len(value):
                if value[i] != "$":
                    i += 1
                    continue
                if i + 1 < len(value) and value[i + 1] == "$":
                    i += 2
                    continue
                for name in SUBST_NAMES:
                    if value.startswith(name, i + 1):
                        i += 1 + len(name)
                        break
                else:
                    pytest.fail(
                        "line %d: a bare $ at %r -- write $$ for a shell "
                        "variable" % (lineno, value[i:i + 12])
                    )


def test_every_group_named_exists():
    groups = known_groups()
    for lineno, line in RULE_LINES:
        for key, _op, value in key_values(line):
            if key != "GROUP":
                continue
            assert value in groups, (
                "line %d: GROUP=%s is not created anywhere; eudev would fall "
                "back to gid 0 and 0600" % (lineno, value)
            )


def test_every_group_granted_is_one_a_user_has():
    """A group nobody is in is a node nobody can open. Both directions of the
    users table matter: the rule grants, the table joins."""
    members = set()
    for line in open(USERS_TABLE):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) < 8 or fields[0] == "-":
            continue
        members.add(fields[2])
        if fields[7] != "-":
            members.update(fields[7].split(","))
    for lineno, line in RULE_LINES:
        for key, _op, value in key_values(line):
            if key == "GROUP" or (key.startswith("RUN") and "chgrp" in value):
                for name in re.findall(r"chgrp (\w+)", value) or [value]:
                    if key == "GROUP":
                        name = value
                    assert name in members, (
                        "line %d: nothing is in group %s" % (lineno, name)
                    )


def test_every_command_a_rule_runs_is_an_applet():
    applets = busybox_applets()
    for lineno, line in RULE_LINES:
        for key, _op, value in key_values(line):
            if not key.startswith("RUN"):
                continue
            argv = build_argv(apply_format(value))
            program = argv[0]
            assert program.startswith("/"), (
                "line %d: RUN must name an absolute path, got %r"
                % (lineno, program)
            )
            assert os.path.basename(program) in applets, (
                "line %d: %s is not built into busybox" % (lineno, program)
            )
            if "-c" not in argv:
                continue
            script = argv[argv.index("-c") + 1]
            for word in command_words(script):
                if word in SHELL_BUILTINS or "=" in word:
                    continue
                assert word in applets, (
                    "line %d: the script runs `%s`, which is not built into "
                    "busybox" % (lineno, word)
                )


def command_words(script):
    """First word of every command position we can spot statically."""
    words = set()
    for part in re.split(r"[;&|]+|\bdo\b|\bthen\b|\{|\}|\(|\)", script):
        part = part.strip()
        if not part:
            continue
        first = part.split()[0]
        if first == "[":
            words.add("test")
            continue
        words.add(first)
    return words


def test_the_i2c_rule_is_present_and_not_granted_to_the_untrusted_user():
    """SECURITY-PLAN.md section 1: an i2c bus is reachable by address, so the
    browser's user must not be on it. This is the one grant that would be
    easy to widen by hand and impossible to notice."""
    text = open(RULES).read()
    assert 'SUBSYSTEM=="i2c-dev"' in text, "the keypad expander lost its rule"
    for line in open(USERS_TABLE):
        if line.startswith("ndusr_ut "):
            fields = line.split()
            assert "i2c" not in fields[7].split(","), "ndusr_ut is on the i2c bus"
            assert "dialout" not in fields[7].split(","), "ndusr_ut can dial"

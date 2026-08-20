"""Colour for the serial log.

Every subsystem already prints lines that start with its own "[TAG]", so
nothing here asks call sites to change: install() wraps sys.stdout and
sys.stderr and paints the tag as the line goes past. A subsystem keeps the
same colour for its whole life, which is the point -- on a console where
the modem, the battery gauge and the UI all talk at once, colour is what
lets you find one of them without reading every line.

Unknown tags are not left plain. They get a colour derived from the tag
itself, so a tag added later is still consistent from its first boot,
without anyone having to remember to register it here.

Tracebacks go red in full. They arrive on stderr as several lines with no
tag at all, so the stderr wrapper colours everything it is given.

Set NEODCT_COLOR=0 (or NO_COLOR to anything) to turn all of this off.
"""

import os
import sys

RESET = "\033[0m"
BOLD = "\033[1m"

# 256-colour codes: far enough apart to tell apart on a small serial window.
_FG = "\033[38;5;%dm"

# The named palette. Chosen so the things you watch during a boot are the
# ones that stand out: modem blue, core green, anything failing red.
TAG_COLOURS = {
    "MODEM":    39,   # bright blue   -- the network stack
    "ndsys":    33,   # deeper blue   -- initramfs / update applier
    "UPDATE":   33,
    "CORE":     46,   # green         -- the OS itself
    "OS":       46,
    "Launcher": 82,   # lighter green
    "BATT":    226,   # yellow        -- power
    "FUEL":    226,
    "NOTIFY":  201,   # magenta       -- user-facing events
    "INPUT":    51,   # cyan          -- keypad / events
    "KEYMAP":   87,
    "SETUP":   214,   # orange        -- first-boot wizards
    "UI":       120,  # pale green
    "FB":       123,  # pale cyan
    "KERNEL":  244,   # grey          -- background noise
    "sdcard":  180,
    "Browser": 141,   # purple        -- netsurf and its noise
    "CRASH":   196,   # red           -- something broke
    "ERROR":   196,
    "FATAL":   196,
}

# Apps get their own band so they read as a group.
APP_TAGS = ("Koki", "Music", "CallLog", "Settings", "PB", "Tones", "Games",
            "Messages", "Clock", "Calculator", "Power")

ERROR_COLOUR = 196
_ENABLED = False


def _colour_for(tag):
    if tag in TAG_COLOURS:
        return TAG_COLOURS[tag]
    if tag in APP_TAGS:
        # 141..177 walks a purple/pink band, stable per app name.
        return 141 + (sum(ord(c) for c in tag) % 36)
    # Anything unregistered: stable colour from the name, avoiding the
    # darkest greys (unreadable) and the reds (reserved for failures).
    return 22 + (sum(ord(c) for c in tag) % 180)


def colour_enabled():
    if os.environ.get("NO_COLOR") is not None:
        return False
    return os.environ.get("NEODCT_COLOR", "1") not in ("0", "no", "off")


def paint(text, code, bold=False):
    """Wrap text in a 256-colour escape. Returns text unchanged when off."""
    if not _ENABLED:
        return text
    return "%s%s%s%s" % (BOLD if bold else "", _FG % code, text, RESET)


def _split_tag(line):
    """('MODEM', ' rest') for '[MODEM] rest', else (None, line)."""
    if not line.startswith("["):
        return None, line
    end = line.find("]")
    if end < 2:
        return None, line
    tag = line[1:end]
    if not tag or not all(c.isalnum() or c in "_-" for c in tag):
        return None, line
    return tag, line[end + 1:]


class _Painter:
    """Line-oriented wrapper: colours whole lines as they are written.

    Holds partial lines back until their newline arrives, because print()
    writes the text and the newline as separate calls and a tag split
    across two writes would not be recognised.
    """

    def __init__(self, stream, force_colour=None):
        self._stream = stream
        self._force = force_colour
        self._pending = ""

    def write(self, data):
        if not data:
            return 0
        self._pending += data
        while "\n" in self._pending:
            line, self._pending = self._pending.split("\n", 1)
            self._stream.write(self._render(line) + "\n")
        self._stream.flush()
        return len(data)

    def _render(self, line):
        if not _ENABLED or not line.strip():
            return line
        if self._force is not None:
            return paint(line, self._force)
        tag, rest = _split_tag(line)
        if tag is None:
            return line
        return paint("[" + tag + "]", _colour_for(tag), bold=True) + rest

    def flush(self):
        # A process dying mid-line still gets that line out.
        if self._pending:
            self._stream.write(self._render(self._pending))
            self._pending = ""
        self._stream.flush()

    def isatty(self):
        try:
            return self._stream.isatty()
        except Exception:
            return False

    def fileno(self):
        return self._stream.fileno()

    def __getattr__(self, name):
        return getattr(self._stream, name)


def install(stdout=True, stderr=True):
    """Wrap the standard streams. Safe to call more than once."""
    global _ENABLED
    _ENABLED = colour_enabled()
    if getattr(sys.stdout, "_neodct_painted", False):
        return _ENABLED
    if stdout:
        sys.stdout = _Painter(sys.stdout)
        sys.stdout._neodct_painted = True
    if stderr:
        # Everything on stderr is a failure worth seeing: tracebacks arrive
        # here as untagged lines, so the whole stream is painted red.
        sys.stderr = _Painter(sys.stderr, force_colour=ERROR_COLOUR)
        sys.stderr._neodct_painted = True
    return _ENABLED


def rule(char="=", width=72, code=46):
    """A full-width divider, used either side of the boot banner."""
    return paint(char * width, code, bold=True)


def banner_lines(path="/etc/neodct-banner"):
    """The pre-rendered boot banner, or [] when the image has none."""
    try:
        with open(path, "r") as handle:
            return handle.read().rstrip("\n").split("\n")
    except OSError:
        return []

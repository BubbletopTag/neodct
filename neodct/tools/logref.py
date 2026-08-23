"""Dump the exact bytes `logstyle.py` writes, so the C logger can be verified
byte for byte.

The project owner asked specifically that the colourful serial logging survive
the C rewrite unchanged. That is a stricter requirement than it sounds, because
`logstyle.py` does four separate things and only the first is obvious:

  1. a named palette -- MODEM blue, CORE green, CRASH red
  2. a *derived* colour for app tags, walking a purple/pink band
  3. a *derived* colour for any tag it has never heard of, stable from the
     name so a tag added later is consistent from its first boot
  4. line-oriented buffering, because print() writes the text and the newline
     as separate calls and a tag split across two writes must still be found

Items 2 and 3 are arithmetic over the tag's characters. A C port that gets the
palette right and the arithmetic wrong produces output that looks correct until
someone adds a new subsystem, and then quietly differs forever.

So this records the real escape sequences for every registered tag, every app
tag, a set of unregistered tags, and the edge cases in `_split_tag`.

    python3 neodct/tools/logref.py --out neodct/tests/golden/log/
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
NEODCT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(NEODCT, "overlay", "NeoDCT"))

from System.core import logstyle  # noqa: E402

#: Tags that must be recognised, split by how their colour is arrived at.
NAMED = sorted(logstyle.TAG_COLOURS)
APPS = sorted(logstyle.APP_TAGS)

#: Tags with no entry anywhere -- these exercise the derived-colour path,
#: which is the half a C port is most likely to get wrong.
UNREGISTERED = ["MEDIA", "GPS", "WIFI", "NFC", "Bluetooth", "Camera",
                "nd-apprun", "CUBE", "Dialer", "T9", "x", "ZZ_LONG_TAG_NAME"]

#: Lines that probe _split_tag's boundaries rather than its colours.
EDGE_LINES = [
    "[MODEM] ordinary line",
    "no tag at all",
    "[] empty tag",
    "[A] one character, too short for the end>=2 rule",
    "[AB] two characters",
    "[has space] not alphanumeric, must not be treated as a tag",
    "[under_score] allowed",
    "[with-dash] allowed",
    "[MODEM]no space after the bracket",
    "   [MODEM] leading whitespace means no tag",
    "",
    "   ",
    "[MODEM] trailing spaces   ",
    "[UPDATE] a line with an embedded ] bracket",
]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)

    # paint() is a no-op unless the module thinks colour is on; force it,
    # since we are recording the coloured form on purpose.
    logstyle._ENABLED = True

    def render(line):
        """One line, exactly as the stdout painter would emit it."""
        if not line.strip():
            return line
        tag, rest = logstyle._split_tag(line)
        if tag is None:
            return line
        return logstyle.paint("[" + tag + "]",
                              logstyle._colour_for(tag), bold=True) + rest

    data = {
        "reset": logstyle.RESET,
        "bold": logstyle.BOLD,
        "fg_format": logstyle._FG,
        "error_colour": logstyle.ERROR_COLOUR,
        "named_palette": {t: logstyle.TAG_COLOURS[t] for t in NAMED},
        "app_tags": APPS,
        # The two derived formulas, recorded as data so the C side can be
        # checked against results rather than against a re-reading of the
        # Python.
        "derived": {
            "app_band": "141 + (sum(ord(c) for c in tag) % 36)",
            "unknown": "22 + (sum(ord(c) for c in tag) % 180)",
        },
        "colour_for": {},
        "rendered": {},
    }

    for tag in NAMED + APPS + UNREGISTERED:
        data["colour_for"][tag] = logstyle._colour_for(tag)
        data["rendered"][tag] = render(f"[{tag}] hello")

    data["edge_cases"] = [{"input": ln, "output": render(ln)}
                          for ln in EDGE_LINES]

    path = os.path.join(args.out, "logref.json")
    with open(path, "w") as fh:
        json.dump(data, fh, indent=1, sort_keys=True)

    print(path)
    print(f"  {len(NAMED)} named, {len(APPS)} app, {len(UNREGISTERED)} "
          f"unregistered tags, {len(EDGE_LINES)} edge cases")
    print("  sample:", repr(data["rendered"]["MODEM"]))
    print("  derived:", repr(data["rendered"]["GPS"]),
          "->", data["colour_for"]["GPS"])
    return 0


if __name__ == "__main__":
    sys.exit(main())

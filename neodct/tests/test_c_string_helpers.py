"""nd_strlcpy() and nd_strlcat() return a LENGTH, and ND_OK is zero.

    size_t nd_strlcpy(char *dst, const char *src, size_t dst_sz);   nd_types.h
    #define ND_OK 0                                                 nd_types.h

So `nd_strlcpy(out, src, out_sz) == ND_OK` reads as "did the copy work" and
means "was the source string EMPTY". It shipped in
apps/Update/service.c, on both returns of nd_upd_package_thumbnail_path(), and
the effect was that the update screen never showed a package's own artwork:
main.c's thumbnail_path() falls back to the stock icon on anything but
ND_UPDSVC_OK, and the function could not return OK for any package that had a
thumbnail at all. The work was done and thrown away every time -- up to 256 KB
inflated, SHA-256'd against the manifest and written to /NeoDCT/User/.ndsys --
on a 64 MB single-core phone.

A C unit test cannot cover it where it lives: test/unit/test_update_app.c says
in its own header that there is no .ndsw writer in the tree, so the app tests
cannot produce a package with a picture in it to ask about. What CAN be done
cheaply is to stop the shape recurring anywhere, which is what this is. The
BSD contract is `result >= dst_sz` means truncated, so a correct call compares
against the buffer size and never against an error code.
"""

import os
import re

import pytest

SRC = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src")

# nd_strlcpy(...) or nd_strlcat(...) compared, either way round, with ND_OK or
# any other ND_ERR_* enumerator. Non-greedy up to the comparison so a call with
# nested parentheses still matches.
BAD = re.compile(r"nd_strl(?:cpy|cat)\s*\(.*?\)\s*(?:==|!=)\s*ND_(?:OK|ERR_\w+)")


def c_sources():
    for root, dirs, files in os.walk(SRC):
        dirs[:] = [d for d in dirs if d != "build"]
        for name in files:
            if name.endswith((".c", ".h")):
                yield os.path.join(root, name)


def test_there_are_sources_to_check():
    """An empty walk would make the test below pass by vacuum."""
    assert sum(1 for _ in c_sources()) > 100


def code_lines(path):
    """Lines with the comments taken out.

    Comment bodies matter here: this codebase documents the reasoning behind a
    change at length, and the fix for the very bug this guards against quotes
    the wrong line verbatim to explain it. A checker that cannot tell an
    explanation from an instruction would fail on the commit that fixed it.

    Deliberately crude -- block comments are tracked across lines and `//` to
    end of line is dropped, with no attempt at string literals, because a
    string containing "nd_strlcpy(...) == ND_OK" is not something this tree
    has any reason to hold.
    """
    out = []
    in_block = False
    for n, line in enumerate(open(path, errors="replace"), 1):
        text = ""
        i = 0
        while i < len(line):
            if in_block:
                end = line.find("*/", i)
                if end < 0:
                    break
                in_block = False
                i = end + 2
                continue
            start = line.find("/*", i)
            slash = line.find("//", i)
            if slash >= 0 and (start < 0 or slash < start):
                text += line[i:slash]
                break
            if start < 0:
                text += line[i:]
                break
            text += line[i:start]
            in_block = True
            i = start + 2
        out.append((n, text))
    return out


@pytest.mark.parametrize("path", sorted(c_sources()),
                         ids=lambda p: os.path.relpath(p, SRC))
def test_no_length_is_compared_against_an_error_code(path):
    hits = [(n, text.strip()) for n, text in code_lines(path) if BAD.search(text)]
    assert not hits, (
        "%s compares nd_strlcpy/nd_strlcat's LENGTH against an nd_err. It "
        "returns strlen(src); compare against the destination size instead "
        "(`< dst_sz` is 'it fitted'):\n  %s"
        % (os.path.relpath(path, SRC),
           "\n  ".join("line %d: %s" % h for h in hits)))

#!/usr/bin/env python3
"""Build a .nap -- a NeoDCT Application Package, an app the phone can install
from its memory card.

    mknap.py --app-dir Bible/ --so luckfox-armv7=build/luckfox/app.so -o Bible.nap
    mknap.py --app-dir Bible/ --so luckfox-armv7=a.so --so qemu-aarch64=b.so -o Bible.nap
    mknap.py --list Bible.nap

A .nap is a plain, uncompressed POSIX ustar archive. The phone's reader is
neodct/src/lib/nd_nap.c and nd_nap.h is the specification; this tool exists
so that a package follows it exactly rather than approximately:

  - every entry is relative to the app's directory, with no leading "./";
  - manifest.json is the first entry, so `tar tf` shows what the package is
    before anything else;
  - with ONE --so, app.so goes at the root and the manifest gains
    "arch": "<tag>" saying which phone it is for. With more than one, each
    goes to lib/<tag>/app.so and the manifest carries no "arch" -- the phone
    picks its own at install time and never installs lib/;
  - data/ is never packed: it is the app's writable storage, made by the
    phone with the owner the app needs;
  - no symlinks, no hard links, no pax headers, no long names, no
    ownership: files are 0644, directories 0755, uid and gid 0, mtime 0,
    so the same input always produces the same bytes.

The app directory holds the manifest, the icon and whatever the app reads.
An app.so or a lib/ found INSIDE it is ignored with a warning, because the
one that ships is the one named on the command line -- a stale host build
sitting next to the manifest is the mistake this guards against.
"""

import argparse
import io
import json
import os
import re
import stat
import sys
import tarfile

ARCH_TAGS = ("luckfox-armv7", "qemu-aarch64", "host-x86_64")
TAG_RE = re.compile(r"^[A-Za-z0-9_-]{1,31}$")
DATA_DIR = "data"
LIB_DIR = "lib"
MAX_NAME = 63           # ND_APP_NAME_MAX - 1
MAX_DIR = 47            # ND_NAP_DIR_MAX - 1
# ustar's name field. Anything longer needs the prefix field, which the
# phone reads but which no app should need; refuse rather than split.
MAX_PATH = 100


def die(msg):
    print("mknap: " + msg, file=sys.stderr)
    sys.exit(1)


def warn(msg):
    print("mknap: warning: " + msg, file=sys.stderr)


def dir_from_name(name):
    """nd_nap_dir_from_name(): keep letters, digits, '_' and '-'."""
    out = "".join(c for c in name if c.isalnum() or c in "_-")
    if not out or out.startswith("-") or len(out) > MAX_DIR:
        return None
    return out


def load_manifest(path):
    with open(path, "rb") as f:
        raw = f.read()
    try:
        doc = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as e:
        die("%s: not valid JSON (%s)" % (path, e))
    if not isinstance(doc, dict):
        die("%s: the manifest must be a JSON object" % path)
    name = doc.get("name")
    if not isinstance(name, str) or not name or len(name) > MAX_NAME:
        die("%s: \"name\" must be a string of 1..%d characters" % (path, MAX_NAME))
    if dir_from_name(name) is None:
        die("%s: \"name\" %r has nothing the card can use as a directory name"
            % (path, name))
    app_id = doc.get("id", 999)
    if isinstance(app_id, bool) or not (isinstance(app_id, int) or
                                       (isinstance(app_id, str) and app_id.strip().isdigit())):
        die("%s: \"id\" must be an integer or a string of digits" % path)
    icon = doc.get("icon", "icon.png")
    if not isinstance(icon, str) or icon.startswith("/") or ".." in icon.split("/"):
        die("%s: \"icon\" must be a file name inside the app" % path)
    return doc


def walk_app_dir(app_dir):
    """Every file and directory under app_dir, relative, sorted, minus the
    things the package must not carry."""
    files = []
    dirs = []
    for root, dnames, fnames in os.walk(app_dir):
        rel_root = os.path.relpath(root, app_dir)
        rel_root = "" if rel_root == "." else rel_root
        dnames.sort()
        fnames.sort()
        keep = []
        for d in dnames:
            rel = os.path.join(rel_root, d) if rel_root else d
            if rel == DATA_DIR:
                warn("skipping %s/: the phone makes the app's data directory" % rel)
                continue
            if rel == LIB_DIR:
                warn("skipping %s/: per-phone app.so files come from --so" % rel)
                continue
            if os.path.islink(os.path.join(root, d)):
                die("%s is a symlink; a package may not contain one" % rel)
            keep.append(d)
            dirs.append(rel)
        dnames[:] = keep
        for f in fnames:
            rel = os.path.join(rel_root, f) if rel_root else f
            full = os.path.join(root, f)
            if os.path.islink(full):
                die("%s is a symlink; a package may not contain one" % rel)
            if not os.path.isfile(full):
                die("%s is not a regular file" % rel)
            if rel == "app.so":
                warn("skipping app.so in the app directory: the one that ships is --so's")
                continue
            if rel == "manifest.json":
                continue  # written first, from the parsed document
            files.append(rel)
    return dirs, files


def check_path(rel):
    if len(rel) > MAX_PATH:
        die("%s: path longer than %d bytes" % (rel, MAX_PATH))
    if "\\" in rel:
        die("%s: a backslash is not a path separator the phone accepts" % rel)


def add_bytes(tar, name, data, mode=0o644):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    info.mtime = 0
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    info.type = tarfile.REGTYPE
    tar.addfile(info, io.BytesIO(data))


def add_dir(tar, name):
    info = tarfile.TarInfo(name)
    info.mode = 0o755
    info.mtime = 0
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    info.type = tarfile.DIRTYPE
    tar.addfile(info)


def add_file(tar, name, path, mode=0o644):
    with open(path, "rb") as f:
        add_bytes(tar, name, f.read(), mode)


def build(app_dir, sos, out):
    manifest_path = os.path.join(app_dir, "manifest.json")
    if not os.path.isfile(manifest_path):
        die("%s has no manifest.json" % app_dir)
    doc = load_manifest(manifest_path)

    for tag, path in sos:
        if not TAG_RE.match(tag):
            die("--so %s: not a phone tag (letters, digits, '-' and '_')" % tag)
        if tag not in ARCH_TAGS:
            warn("--so %s: not one of the phones this tree builds for (%s)"
                 % (tag, ", ".join(ARCH_TAGS)))
        if not os.path.isfile(path):
            die("--so %s=%s: no such file" % (tag, path))
        with open(path, "rb") as f:
            if f.read(4) != b"\x7fELF":
                die("--so %s=%s: not an ELF object" % (tag, path))
    tags = [t for t, _ in sos]
    if len(set(tags)) != len(tags):
        die("the same --so tag was given twice")

    # The manifest the phone will read. The tree's own manifests spell "id"
    # as a string; that is kept as given. "arch" is ours to set.
    doc = dict(doc)
    doc.pop("arch", None)
    if len(sos) == 1:
        doc["arch"] = sos[0][0]
    manifest_bytes = (json.dumps(doc, indent=1) + "\n").encode("utf-8")

    dirs, files = walk_app_dir(app_dir)
    for rel in dirs + files:
        check_path(rel)
    icon = doc.get("icon", "icon.png")
    if icon not in files:
        warn("the manifest's icon %r is not in the app directory; the menu will show "
             "a placeholder" % icon)

    tmp = out + ".tmp"
    with tarfile.open(tmp, "w", format=tarfile.USTAR_FORMAT) as tar:
        add_bytes(tar, "manifest.json", manifest_bytes)
        if len(sos) == 1:
            add_file(tar, "app.so", sos[0][1])
        else:
            add_dir(tar, LIB_DIR)
            for tag, path in sorted(sos):
                add_dir(tar, "%s/%s" % (LIB_DIR, tag))
                add_file(tar, "%s/%s/app.so" % (LIB_DIR, tag), path)
        for rel in dirs:
            add_dir(tar, rel)
        for rel in files:
            add_file(tar, rel, os.path.join(app_dir, rel))
    os.replace(tmp, out)

    size = os.path.getsize(out)
    print("%s: %s (id %s) for %s, %d file%s, %d bytes" % (
        out, doc["name"], doc.get("id", 999),
        ", ".join(t for t, _ in sos), len(files) + 2,
        "" if len(files) + 2 == 1 else "s", size))


def list_package(path):
    """What the phone would see. Applies the reader's own rules, so a
    package made some other way can be checked here before a card trip."""
    problems = []
    with tarfile.open(path, "r:", format=tarfile.USTAR_FORMAT) as tar:
        members = tar.getmembers()
        manifest = None
        arches = []
        top_so = False
        for m in members:
            name = m.name[2:] if m.name.startswith("./") else m.name
            name = name.rstrip("/")
            kind = "dir " if m.isdir() else "file"
            if not (m.isdir() or m.isfile()):
                kind = "????"
                problems.append("%s: not a file or a directory (type %r)" % (name, m.type))
            parts = name.split("/")
            if name.startswith("/") or ".." in parts or "" in parts[1:]:
                problems.append("%s: unsafe name" % name)
            if parts[0] == DATA_DIR:
                problems.append("%s: under data/, which the phone owns" % name)
            if parts[0] == LIB_DIR and not m.isdir():
                if len(parts) == 3 and parts[2] == "app.so" and TAG_RE.match(parts[1]):
                    arches.append(parts[1])
                else:
                    problems.append("%s: lib/ may only hold lib/<tag>/app.so" % name)
            if name == "manifest.json":
                manifest = json.loads(tar.extractfile(m).read().decode("utf-8"))
            if name == "app.so":
                top_so = True
            print("  %s  %9d  %s" % (kind, m.size, name))
    if manifest is None:
        problems.append("no manifest.json")
    else:
        arch = manifest.get("arch")
        if top_so and not arch:
            problems.append("app.so at the root but no \"arch\" in the manifest")
        if arch and not top_so:
            problems.append("\"arch\" in the manifest but no app.so at the root")
        if top_so and arches:
            problems.append("both an app.so at the root and a lib/ tree")
        if top_so and arch:
            arches = [arch]
        print("%s: %s (id %s), for %s" % (
            path, manifest.get("name"), manifest.get("id", 999),
            ", ".join(arches) if arches else "NO PHONE AT ALL"))
        if not arches:
            problems.append("no app.so for any phone")
    for p in problems:
        print("PROBLEM: " + p)
    return 1 if problems else 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__)
    parser.add_argument("--app-dir", help="the directory with manifest.json, the icon "
                                          "and the app's files")
    parser.add_argument("--so", action="append", default=[], metavar="TAG=PATH",
                        help="the app.so for one phone: luckfox-armv7=..., "
                             "qemu-aarch64=...; repeat for a universal package")
    parser.add_argument("-o", "--output", help="the .nap to write")
    parser.add_argument("--list", metavar="NAP", help="show what is in a .nap and "
                                                       "whether the phone would accept it")
    args = parser.parse_args(argv)

    if args.list:
        return list_package(args.list)
    if not args.app_dir or not args.so or not args.output:
        parser.error("--app-dir, at least one --so and -o are required")
    if not args.output.endswith(".nap"):
        parser.error("the output should end in .nap; the phone looks for that")
    sos = []
    for spec in args.so:
        if "=" not in spec:
            parser.error("--so wants TAG=PATH, not %r" % spec)
        tag, path = spec.split("=", 1)
        sos.append((tag, path))
    build(args.app_dir, sos, args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

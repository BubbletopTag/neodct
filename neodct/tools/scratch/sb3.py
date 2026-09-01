"""A minimal builder for Scratch 3 project files (.sb3).

Scratch's own format is a graph: every block is an entry in a flat dict keyed
by an opaque id, and the order of a script exists only as `next`/`parent`
pointers between those entries. Writing that by hand is unreadable, so this
module lets a script be built as nested Python objects and flattens it at the
end -- ids, parent links and `topLevel` flags are assigned by the serializer,
never by the caller.

Only the parts the NeoDCT port needs are here. Where the format offers two
encodings for the same thing (a variable as a compact `[12, name, id]`
primitive or as a `data_variable` block) this module always emits the block
form: it is what the flattener already does for everything else, and Scratch
normalises it on the next save anyway.
"""

import hashlib
import json
import os
import zipfile

# --- input primitive type numbers (project.json "inputs" arrays) -----------
NUMBER = 4
POSITIVE_NUMBER = 5
POSITIVE_INTEGER = 6
INTEGER = 7
ANGLE = 8
COLOR = 9
STRING = 10
BROADCAST = 11


class Lit:
    """A literal sitting in an input slot -- Scratch calls it a shadow."""

    __slots__ = ("type", "value")

    def __init__(self, type_, value):
        self.type = type_
        self.value = value

    def serialise(self):
        return [self.type, str(self.value)]


def num(v):
    return Lit(NUMBER, v)


def txt(v):
    return Lit(STRING, v)


def colour(v):
    return Lit(COLOR, v)


class Block:
    """One block. `inputs` values are Block, Lit, Sub or (Block, Lit) pairs.

    A (Block, Lit) pair is the "obscured shadow" case: a reporter dropped on
    top of a slot that still remembers the literal underneath it. Scratch
    needs both, or the value reappears when the reporter is pulled out.
    """

    __slots__ = ("opcode", "inputs", "fields", "shadow", "mutation")

    def __init__(self, opcode, inputs=None, fields=None, shadow=False,
                 mutation=None):
        self.opcode = opcode
        self.inputs = dict(inputs or {})
        self.fields = dict(fields or {})
        self.shadow = shadow
        self.mutation = mutation


class Sub:
    """A substack input (the C-shape of if/repeat/forever)."""

    __slots__ = ("script",)

    def __init__(self, script):
        self.script = script or []


class Serialiser:
    def __init__(self, prefix):
        self.prefix = prefix
        self.n = 0
        self.blocks = {}

    def new_id(self):
        self.n += 1
        return "%s%d" % (self.prefix, self.n)

    def script(self, stmts, x=0, y=0):
        """Emit a top-level script. Returns the id of its first block."""
        return self._chain(stmts, parent=None, top=True, x=x, y=y)

    def _chain(self, stmts, parent, top=False, x=0, y=0):
        stmts = [s for s in stmts if s is not None]
        if not stmts:
            return None
        ids = [self.new_id() for _ in stmts]
        for i, (bid, blk) in enumerate(zip(ids, stmts)):
            prev = ids[i - 1] if i else parent
            entry = self._entry(blk, bid, prev)
            entry["next"] = ids[i + 1] if i + 1 < len(ids) else None
            if top and i == 0:
                entry["topLevel"] = True
                entry["x"] = x
                entry["y"] = y
            self.blocks[bid] = entry
        return ids[0]

    def reporter(self, blk, parent):
        """Emit a reporter (or menu shadow). Returns its id."""
        bid = self.new_id()
        entry = self._entry(blk, bid, parent)
        entry["next"] = None
        self.blocks[bid] = entry
        return bid

    def _entry(self, blk, bid, parent):
        entry = {
            "opcode": blk.opcode,
            "next": None,
            "parent": parent,
            "inputs": {},
            "fields": {},
            "shadow": bool(blk.shadow),
            "topLevel": False,
        }
        for name, value in blk.inputs.items():
            encoded = self._input(value, bid)
            if encoded is not None:
                entry["inputs"][name] = encoded
        for name, value in blk.fields.items():
            entry["fields"][name] = list(value) if isinstance(value, (list, tuple)) \
                else [value, None]
        if blk.mutation:
            entry["mutation"] = blk.mutation
        return entry

    def _input(self, value, parent):
        if value is None:
            return None
        if isinstance(value, Sub):
            first = self._chain(value.script, parent=parent)
            return None if first is None else [2, first]
        if isinstance(value, Lit):
            return [1, value.serialise()]
        if isinstance(value, Block):
            # A menu block is a shadow: it lives in the slot rather than on
            # top of it, and Scratch writes that as [1, id], not [2, id].
            kind = 1 if value.shadow else 2
            return [kind, self.reporter(value, parent)]
        if isinstance(value, tuple):
            block, shadow = value
            if block is None:
                return self._input(shadow, parent)
            bid = self.reporter(block, parent)
            if shadow is None:
                return [2, bid]
            if isinstance(shadow, Block):
                return [3, bid, self.reporter(shadow, parent)]
            return [3, bid, shadow.serialise()]
        raise TypeError("cannot put %r in an input" % (value,))


class Target:
    def __init__(self, name, is_stage=False, layer=0):
        self.name = name
        self.is_stage = is_stage
        self.layer = layer
        self.variables = {}       # id -> [name, value]
        self.lists = {}           # id -> [name, [items]]
        self.broadcasts = {}
        self.costumes = []
        self.sounds = []
        self.current_costume = 0
        self.comments = {}
        self.scripts = []         # (stmts, x, y)
        self.volume = 100
        self.visible = True
        self.x = 0
        self.y = 0
        self.size = 100
        self.direction = 90

    # --- data -------------------------------------------------------------

    def variable(self, name, value=0):
        vid = "v_%s" % name
        self.variables[vid] = [name, value]
        return vid

    def list(self, name, items=()):
        lid = "l_%s" % name
        self.lists[lid] = [name, list(items)]
        return lid

    def costume(self, name, path, rotation_x=0, rotation_y=0, resolution=1):
        with open(path, "rb") as fh:
            data = fh.read()
        digest = hashlib.md5(data).hexdigest()
        fmt = os.path.splitext(path)[1].lstrip(".").lower()
        self.costumes.append({
            "name": name,
            "bitmapResolution": resolution,
            "dataFormat": fmt,
            "assetId": digest,
            "md5ext": "%s.%s" % (digest, fmt),
            "rotationCenterX": rotation_x,
            "rotationCenterY": rotation_y,
            "_path": path,
        })
        return len(self.costumes) - 1

    def script(self, stmts, x=0, y=0):
        self.scripts.append((stmts, x, y))

    def comment(self, text, x, y, width=380, height=180, block_id=None,
                minimised=False):
        cid = "c%d" % (len(self.comments) + 1)
        self.comments[cid] = {
            "blockId": block_id,
            "x": x,
            "y": y,
            "width": width,
            "height": height,
            "minimized": minimised,
            "text": text,
        }
        return cid

    # --- serialisation ----------------------------------------------------

    def serialise(self, prefix):
        ser = Serialiser(prefix)
        for stmts, x, y in self.scripts:
            ser.script(stmts, x, y)
        target = {
            "isStage": self.is_stage,
            "name": self.name,
            "variables": self.variables,
            "lists": self.lists,
            "broadcasts": self.broadcasts,
            "blocks": ser.blocks,
            "comments": self.comments,
            "currentCostume": self.current_costume,
            "costumes": [{k: v for k, v in c.items() if k != "_path"}
                         for c in self.costumes],
            "sounds": self.sounds,
            "volume": self.volume,
            "layerOrder": self.layer,
        }
        if self.is_stage:
            target.update({
                "tempo": 60,
                "videoTransparency": 50,
                "videoState": "off",
                "textToSpeechLanguage": None,
            })
        else:
            target.update({
                "visible": self.visible,
                "x": self.x,
                "y": self.y,
                "size": self.size,
                "direction": self.direction,
                "draggable": False,
                "rotationStyle": "all around",
            })
        return target


class Project:
    def __init__(self, extensions=("pen",)):
        self.targets = []
        self.extensions = list(extensions)
        self.monitors = []

    def target(self, name, is_stage=False):
        t = Target(name, is_stage, layer=len(self.targets))
        self.targets.append(t)
        return t

    def to_json(self):
        return {
            "targets": [t.serialise("b%d_" % i)
                        for i, t in enumerate(self.targets)],
            "monitors": self.monitors,
            "extensions": self.extensions,
            "meta": {
                "semver": "3.0.0",
                "vm": "0.2.0",
                "agent": "",
            },
        }

    def write(self, path):
        payload = json.dumps(self.to_json(), separators=(",", ":"))
        assets = {}
        for target in self.targets:
            for costume in target.costumes:
                assets[costume["md5ext"]] = costume["_path"]
            for sound in target.sounds:
                if "_path" in sound:
                    assets[sound["md5ext"]] = sound["_path"]
        tmp = path + ".tmp"
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zf:
            # project.json first: the editor streams the zip and reads it
            # before it has the assets, and some loaders assume that order.
            zf.writestr("project.json", payload)
            for name, source in sorted(assets.items()):
                zf.write(source, name)
        os.replace(tmp, path)
        return path

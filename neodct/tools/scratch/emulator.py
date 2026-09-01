"""Runs a generated .sb3 the way Scratch would, and draws what it draws.

This exists so the port can be checked against the thing it is a port of:
`neodct/tests/test_scratch_port.py` renders a screen here and the same screen
through the real Python framework, and requires them to be the same image.
Without it the only way to know whether `VerticalList` came out right is to
open the editor and look.

It is not a Scratch implementation. It covers the opcodes the generator emits
and nothing else, and it renders at the phone's 240x175 rather than the
stage's 480x360 -- pen strokes are turned back into the rectangles they were
meant to be. Scratch's pen antialiases the ends of a stroke, so the real
editor shows a fractionally dim pixel at the left and right edge of every
filled rectangle; that is a property of Scratch's pen, not of the port, and
modelling it here would only make the comparison fuzzy.
"""

import json
import math
import zipfile

from PIL import Image

W, H, SCALE = 240, 175, 2


# --- Scratch's value semantics --------------------------------------------

def js_number(value):
    """JavaScript's Number(), which is what Cast.toNumber calls."""
    if isinstance(value, bool):
        return 1.0 if value else 0.0
    if isinstance(value, (int, float)):
        return float(value)
    text = str(value).strip()
    if text == "":
        return 0.0
    try:
        if text[:2].lower() in ("0x", "-0", "+0") and text[:2].lower() == "0x":
            return float(int(text, 16))
        return float(text)
    except ValueError:
        return float("nan")


def to_number(value):
    n = js_number(value)
    return 0.0 if n != n else n


def to_string(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float):
        if value != value:
            return "NaN"
        if value == int(value) and abs(value) < 1e21:
            return str(int(value))
        return repr(value)
    return str(value)


def to_boolean(value):
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        return value != "" and value.lower() != "false" and value != "0"
    return to_number(value) != 0


def compare(a, b):
    n1, n2 = js_number(a), js_number(b)
    if n1 == 0 and str(a).strip() == "":
        n1 = float("nan")
    elif n2 == 0 and str(b).strip() == "":
        n2 = float("nan")
    if n1 != n1 or n2 != n2:
        s1, s2 = to_string(a).lower(), to_string(b).lower()
        return -1 if s1 < s2 else (1 if s1 > s2 else 0)
    return n1 - n2


# --- the canvas -----------------------------------------------------------

class Canvas:
    """The pen layer, at phone resolution."""

    def __init__(self):
        self.image = Image.new("RGB", (W, H), "black")

    def clear(self):
        self.image = Image.new("RGB", (W, H), "black")

    def fill(self, x0, y0, x1, y1, colour):
        x0, x1 = max(0, x0), min(W - 1, x1)
        y0, y1 = max(0, y0), min(H - 1, y1)
        if x0 > x1 or y0 > y1:
            return
        block = Image.new("RGB", (x1 - x0 + 1, y1 - y0 + 1), colour)
        self.image.paste(block, (x0, y0))

    def stamp(self, sprite_image, x, y):
        if sprite_image is None:
            return
        self.image.paste(sprite_image, (x, y), sprite_image)


class Keyboard:
    """A scripted keyboard.

    Each key is held down for a few frames and then released for a few, the
    way a finger does it. Holding for exactly one frame would be wrong in a
    way that hides real bugs: the port only reacts to a key going down, and
    a press that appears and vanishes between two polls is missed -- which
    is the correct behaviour for a phone, and an artefact of the harness
    rather than anything a user could do.
    """

    HOLD = 3
    IDLE_LIMIT = 400

    def __init__(self, queue=()):
        self.queue = list(queue)
        self.down = set()
        self.frames = 0
        self.idle = 0
        self.counter = 0

    def tick(self):
        self.frames += 1
        self.counter += 1
        if self.down:
            if self.counter >= self.HOLD:
                self.down = set()
                self.counter = 0
            return
        if self.queue:
            if self.counter >= self.HOLD:
                self.down = {self.queue.pop(0)}
                self.counter = 0
                self.idle = 0
            return
        self.idle += 1
        if self.idle > self.IDLE_LIMIT:
            raise RuntimeError("the key script ran out while a widget was "
                               "still waiting for a key")

    def is_down(self, key):
        if key == "any":
            return bool(self.down)
        return key in self.down


class Stop(Exception):
    pass


class Emulator:
    MAX_STEPS = 40_000_000

    def __init__(self, path, keys=(), clock=1 / 30.0):
        with zipfile.ZipFile(path) as zf:
            self.project = json.loads(zf.read("project.json"))
            self.assets = {name: zf.read(name) for name in zf.namelist()
                           if name != "project.json"}
        self.canvas = Canvas()
        self.keyboard = Keyboard(keys)
        self.steps = 0
        self.variables = {}
        self.lists = {}
        self.procedures = {}
        self.blocks = {}
        self.costume_images = {}
        self.costume_names = []
        self.costume = 1
        self.brightness = 0.0
        self.x = 0.0
        self.y = 0.0
        self.pen_down = False
        self.pen_size = 1.0
        self.pen_colour = (255, 255, 255)
        self.timer = 0.0
        self.clock = clock
        self._load()

    # --- loading ----------------------------------------------------------

    def _load(self):
        import io
        for target in self.project["targets"]:
            for vid, (name, value) in target["variables"].items():
                self.variables[name] = value
            for lid, (name, items) in target["lists"].items():
                self.lists[name] = list(items)
            if target["isStage"]:
                continue
            self.blocks.update(target["blocks"])
            for costume in target["costumes"]:
                self.costume_names.append(costume["name"])
                image = Image.open(io.BytesIO(self.assets[costume["md5ext"]]))
                image = image.convert("RGBA")
                image = image.resize((max(1, image.width // SCALE),
                                      max(1, image.height // SCALE)),
                                     Image.Resampling.NEAREST)
                self.costume_images[costume["name"]] = image
        for bid, block in self.blocks.items():
            if block["opcode"] == "procedures_prototype":
                self.procedures[block["mutation"]["proccode"]] = bid

    # --- entry points -----------------------------------------------------

    def call(self, proccode, *args):
        prototype = self.blocks[self.procedures[proccode]]
        definition = self.blocks[prototype["parent"]]
        names = json.loads(prototype["mutation"]["argumentnames"])
        frame = {name: value for name, value in zip(names, args)}
        warp = prototype["mutation"].get("warp") == "true"
        try:
            self._run(definition["next"], frame, warp)
        except Stop:
            pass

    def run_flag(self):
        for bid, block in self.blocks.items():
            if block["opcode"] == "event_whenflagclicked":
                try:
                    self._run(block["next"], {}, False)
                except Stop:
                    pass

    # --- execution --------------------------------------------------------

    def _run(self, bid, frame, warp):
        while bid is not None:
            self.steps += 1
            if self.steps > self.MAX_STEPS:
                raise RuntimeError("emulated script ran away")
            bid = self._step(self.blocks[bid], frame, warp)

    def _tick(self, warp):
        if not warp:
            self.keyboard.tick()
            self.timer += self.clock

    def _step(self, block, frame, warp):
        op = block["opcode"]
        method = getattr(self, "_op_" + op.replace(".", "_"), None)
        if method is None:
            raise NotImplementedError(op)
        method(block, frame, warp)
        return block["next"]

    # --- inputs -----------------------------------------------------------

    def _input(self, block, name, frame, default=""):
        entry = block["inputs"].get(name)
        if entry is None:
            return default
        kind = entry[0]
        if kind == 1:
            return self._primitive(entry[1])
        value = entry[1]
        if isinstance(value, list):
            return self._primitive(value)
        return self._evaluate(value, frame)

    def _substack(self, block, name):
        entry = block["inputs"].get(name)
        return None if entry is None else entry[1]

    def _primitive(self, prim):
        if isinstance(prim, str):
            return self._evaluate(prim, {})
        kind = prim[0]
        if kind in (4, 5, 6, 7, 8):
            return to_number(prim[1])
        if kind == 12:
            return self.variables[prim[1]]
        if kind == 13:
            return self.lists[prim[1]]
        return prim[1]

    def _evaluate(self, bid, frame):
        block = self.blocks[bid]
        op = block["opcode"]
        method = getattr(self, "_val_" + op, None)
        if method is None:
            raise NotImplementedError("reporter " + op)
        self.steps += 1
        return method(block, frame)

    def _num(self, block, name, frame, default=0):
        return to_number(self._input(block, name, frame, default))

    def _str(self, block, name, frame, default=""):
        return to_string(self._input(block, name, frame, default))

    def _bool(self, block, name, frame, default=False):
        entry = block["inputs"].get(name)
        if entry is None:
            return default
        return to_boolean(self._evaluate(entry[1], frame))

    # --- control ----------------------------------------------------------

    def _op_control_repeat(self, block, frame, warp):
        times = int(math.ceil(self._num(block, "TIMES", frame)))
        body = self._substack(block, "SUBSTACK")
        for _ in range(max(0, times)):
            if body:
                self._run(body, frame, warp)
            self._tick(warp)

    def _op_control_repeat_until(self, block, frame, warp):
        body = self._substack(block, "SUBSTACK")
        while not self._bool(block, "CONDITION", frame):
            if body:
                self._run(body, frame, warp)
            self._tick(warp)

    def _op_control_forever(self, block, frame, warp):
        body = self._substack(block, "SUBSTACK")
        while True:
            if body:
                self._run(body, frame, warp)
            self._tick(warp)

    def _op_control_if(self, block, frame, warp):
        if self._bool(block, "CONDITION", frame):
            body = self._substack(block, "SUBSTACK")
            if body:
                self._run(body, frame, warp)

    def _op_control_if_else(self, block, frame, warp):
        name = "SUBSTACK" if self._bool(block, "CONDITION", frame) else "SUBSTACK2"
        body = self._substack(block, name)
        if body:
            self._run(body, frame, warp)

    def _op_control_wait_until(self, block, frame, warp):
        while not self._bool(block, "CONDITION", frame):
            self._tick(warp)

    def _op_control_wait(self, block, frame, warp):
        self._tick(warp)

    def _op_control_stop(self, block, frame, warp):
        raise Stop()

    def _op_event_whenflagclicked(self, block, frame, warp):
        pass

    # --- procedures -------------------------------------------------------

    def _op_procedures_call(self, block, frame, warp):
        mutation = block["mutation"]
        proccode = mutation["proccode"]
        argids = json.loads(mutation["argumentids"])
        prototype = self.blocks[self.procedures[proccode]]
        names = json.loads(prototype["mutation"]["argumentnames"])
        inner = {}
        for argid, name in zip(argids, names):
            entry = block["inputs"].get(argid)
            if entry is None:
                inner[name] = ""
            elif entry[0] == 1:
                inner[name] = self._primitive(entry[1])
            elif isinstance(entry[1], list):
                inner[name] = self._primitive(entry[1])
            else:
                inner[name] = self._evaluate(entry[1], frame)
        definition = self.blocks[prototype["parent"]]
        inner_warp = prototype["mutation"].get("warp") == "true"
        # Warp is inherited downwards: once a "run without screen refresh"
        # block is running, nothing inside it yields either.
        self._run(definition["next"], inner, warp or inner_warp)

    def _val_argument_reporter_string_number(self, block, frame):
        return frame.get(block["fields"]["VALUE"][0], "")

    def _val_argument_reporter_boolean(self, block, frame):
        return frame.get(block["fields"]["VALUE"][0], False)

    # --- data -------------------------------------------------------------

    def _op_data_setvariableto(self, block, frame, warp):
        self.variables[block["fields"]["VARIABLE"][0]] = self._input(block, "VALUE", frame)

    def _op_data_changevariableby(self, block, frame, warp):
        name = block["fields"]["VARIABLE"][0]
        self.variables[name] = to_number(self.variables[name]) + self._num(block, "VALUE", frame)

    def _val_data_variable(self, block, frame):
        return self.variables[block["fields"]["VARIABLE"][0]]

    def _list(self, block):
        return self.lists[block["fields"]["LIST"][0]]

    def _op_data_addtolist(self, block, frame, warp):
        self._list(block).append(self._input(block, "ITEM", frame))

    def _op_data_deletealloflist(self, block, frame, warp):
        del self._list(block)[:]

    def _op_data_deleteoflist(self, block, frame, warp):
        items = self._list(block)
        index = int(to_number(self._input(block, "INDEX", frame)))
        if 1 <= index <= len(items):
            del items[index - 1]

    def _op_data_insertatlist(self, block, frame, warp):
        items = self._list(block)
        index = int(to_number(self._input(block, "INDEX", frame)))
        if 1 <= index <= len(items) + 1:
            items.insert(index - 1, self._input(block, "ITEM", frame))

    def _op_data_replaceitemoflist(self, block, frame, warp):
        items = self._list(block)
        index = int(to_number(self._input(block, "INDEX", frame)))
        if 1 <= index <= len(items):
            items[index - 1] = self._input(block, "ITEM", frame)

    def _val_data_itemoflist(self, block, frame):
        items = self._list(block)
        index = int(to_number(self._input(block, "INDEX", frame)))
        return items[index - 1] if 1 <= index <= len(items) else ""

    def _val_data_lengthoflist(self, block, frame):
        return len(self._list(block))

    def _val_data_itemnumoflist(self, block, frame):
        target = self._input(block, "ITEM", frame)
        for i, item in enumerate(self._list(block)):
            if compare(item, target) == 0:
                return i + 1
        return 0

    def _val_data_listcontainsitem(self, block, frame):
        target = self._input(block, "ITEM", frame)
        return any(compare(item, target) == 0 for item in self._list(block))

    # --- operators --------------------------------------------------------

    def _val_operator_add(self, b, f):
        return self._num(b, "NUM1", f) + self._num(b, "NUM2", f)

    def _val_operator_subtract(self, b, f):
        return self._num(b, "NUM1", f) - self._num(b, "NUM2", f)

    def _val_operator_multiply(self, b, f):
        return self._num(b, "NUM1", f) * self._num(b, "NUM2", f)

    def _val_operator_divide(self, b, f):
        a, c = self._num(b, "NUM1", f), self._num(b, "NUM2", f)
        return float("inf") if c == 0 else a / c

    def _val_operator_mod(self, b, f):
        a, c = self._num(b, "NUM1", f), self._num(b, "NUM2", f)
        if c == 0:
            return float("nan")
        result = math.fmod(a, c)
        if result / c < 0:
            result += c
        return result

    def _val_operator_round(self, b, f):
        return math.floor(self._num(b, "NUM", f) + 0.5)

    def _val_operator_mathop(self, b, f):
        value = self._num(b, "NUM", f)
        op = b["fields"]["OPERATOR"][0]
        return {"abs": abs, "floor": math.floor, "ceiling": math.ceil,
                "sqrt": math.sqrt}[op](value)

    def _val_operator_random(self, b, f):
        import random
        low, high = self._num(b, "FROM", f), self._num(b, "TO", f)
        if low == int(low) and high == int(high):
            return random.randint(int(low), int(high))
        return random.uniform(low, high)

    def _val_operator_join(self, b, f):
        return self._str(b, "STRING1", f) + self._str(b, "STRING2", f)

    def _val_operator_letter_of(self, b, f):
        text = self._str(b, "STRING", f)
        index = int(to_number(self._input(b, "LETTER", f)))
        return text[index - 1] if 1 <= index <= len(text) else ""

    def _val_operator_length(self, b, f):
        return len(self._str(b, "STRING", f))

    def _val_operator_contains(self, b, f):
        return self._str(b, "STRING1", f).lower().find(
            self._str(b, "STRING2", f).lower()) >= 0

    def _val_operator_equals(self, b, f):
        return compare(self._input(b, "OPERAND1", f), self._input(b, "OPERAND2", f)) == 0

    def _val_operator_lt(self, b, f):
        return compare(self._input(b, "OPERAND1", f), self._input(b, "OPERAND2", f)) < 0

    def _val_operator_gt(self, b, f):
        return compare(self._input(b, "OPERAND1", f), self._input(b, "OPERAND2", f)) > 0

    def _val_operator_and(self, b, f):
        return self._bool(b, "OPERAND1", f) and self._bool(b, "OPERAND2", f)

    def _val_operator_or(self, b, f):
        return self._bool(b, "OPERAND1", f) or self._bool(b, "OPERAND2", f)

    def _val_operator_not(self, b, f):
        return not self._bool(b, "OPERAND", f)

    # --- motion, looks, sensing ------------------------------------------

    def _op_motion_gotoxy(self, block, frame, warp):
        self.x = self._num(block, "X", frame)
        self.y = self._num(block, "Y", frame)

    def _val_motion_xposition(self, block, frame):
        return self.x

    def _val_motion_yposition(self, block, frame):
        return self.y

    def _op_looks_switchcostumeto(self, block, frame, warp):
        entry = block["inputs"]["COSTUME"]
        if entry[0] == 1:
            name = self.blocks[entry[1]]["fields"]["COSTUME"][0]
        else:
            name = to_string(self._evaluate(entry[1], frame))
        if name in self.costume_images:
            self.costume = self.costume_names.index(name) + 1
        else:
            raise KeyError("no costume %r" % name)

    def _val_looks_costumenumbername(self, block, frame):
        if block["fields"]["NUMBER_NAME"][0] == "number":
            return self.costume
        return self.costume_names[self.costume - 1]

    def _op_looks_seteffectto(self, block, frame, warp):
        if block["fields"]["EFFECT"][0] == "brightness":
            self.brightness = self._num(block, "VALUE", frame)

    def _op_looks_setsizeto(self, block, frame, warp):
        pass

    def _op_looks_show(self, block, frame, warp):
        pass

    def _op_looks_hide(self, block, frame, warp):
        pass

    def _val_sensing_keypressed(self, block, frame):
        entry = block["inputs"]["KEY_OPTION"]
        if entry[0] == 1:
            key = self.blocks[entry[1]]["fields"]["KEY_OPTION"][0]
        else:
            key = to_string(self._evaluate(entry[1], frame))
        return self.keyboard.is_down(key)

    def _val_sensing_timer(self, block, frame):
        return self.timer

    def _op_sensing_resettimer(self, block, frame, warp):
        self.timer = 0.0

    # --- pen --------------------------------------------------------------

    def _op_pen_clear(self, block, frame, warp):
        self.canvas.clear()

    def _op_pen_setPenSizeTo(self, block, frame, warp):
        self.pen_size = self._num(block, "SIZE", frame)

    def _op_pen_setPenColorToColor(self, block, frame, warp):
        value = self._input(block, "COLOR", frame)
        text = to_string(value).lstrip("#")
        self.pen_colour = tuple(int(text[i:i + 2], 16) for i in (0, 2, 4))

    def _op_pen_penDown(self, block, frame, warp):
        self.pen_down = True
        self._stroke(self.x, self.y, self.x, self.y)

    def _op_pen_penUp(self, block, frame, warp):
        self.pen_down = False

    def _op_pen_stamp(self, block, frame, warp):
        image = self.costume_images[self.costume_names[self.costume - 1]]
        if self.brightness:
            image = _brighten(image, self.brightness / 100.0)
        x = (self.x + W) / 2.0
        y = (H - self.y) / 2.0
        _assert_integral("stamp x", x)
        _assert_integral("stamp y", y)
        self.canvas.stamp(image, int(round(x)), int(round(y)))

    def _op_motion_gotoxy_pen(self, *a):
        pass

    # A `go to` while the pen is down is the only line the port ever draws,
    # and every one of them is a horizontal stroke standing in for a filled
    # rectangle. Turning it back into that rectangle keeps the comparison
    # against Pillow exact; see the module docstring on antialiasing.
    def _stroke(self, x0, y0, x1, y1):
        if not self.pen_down:
            return
        if y0 != y1:
            raise AssertionError("only horizontal pen strokes are emitted")
        thickness = self.pen_size
        rows = thickness / 2.0
        _assert_integral("pen thickness/2", rows)
        top_plus_bottom = H - 1 - y0
        first = (top_plus_bottom - rows + 1) / 2.0
        _assert_integral("pen row", first)
        py0 = int(first)
        py1 = py0 + int(rows) - 1
        px0 = (x0 + W - 1) / 2.0
        px1 = (x1 + W - 1) / 2.0
        _assert_integral("pen x0", px0)
        _assert_integral("pen x1", px1)
        self.canvas.fill(int(px0), py0, int(px1), py1, self.pen_colour)


def _assert_integral(what, value):
    if abs(value - round(value)) > 1e-6:
        raise AssertionError("%s is not on a pixel boundary: %r" % (what, value))


def _brighten(image, amount):
    out = image.copy()
    pixels = out.load()
    delta = int(round(amount * 255))
    for y in range(out.height):
        for x in range(out.width):
            r, g, b, a = pixels[x, y]
            pixels[x, y] = (max(0, min(255, r + delta)),
                            max(0, min(255, g + delta)),
                            max(0, min(255, b + delta)), a)
    return out


def _patch_goto():
    """`go to x y` draws when the pen is down; wire that in one place."""
    original = Emulator._op_motion_gotoxy

    def gotoxy(self, block, frame, warp):
        x0, y0 = self.x, self.y
        original(self, block, frame, warp)
        if self.pen_down:
            self._stroke(x0, y0, self.x, self.y)

    Emulator._op_motion_gotoxy = gotoxy


_patch_goto()

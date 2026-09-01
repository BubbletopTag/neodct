"""The Scratch block vocabulary the NeoDCT port is written in.

Two things live here. The first is one small factory per opcode, so the
widget code reads as blocks rather than as JSON. The second is `Ex`, an
expression wrapper with arithmetic operators on it: a coordinate like
`header_y + 10` is written that way instead of as three nested calls, which
matters when the thing being ported is nine hundred lines of coordinate
arithmetic.
"""

from sb3 import Block, Lit, Sub, colour, num, txt


class Ex:
    """A value in an input slot: a reporter block, or a plain literal."""

    __slots__ = ("value",)

    def __init__(self, value):
        self.value = value.value if isinstance(value, Ex) else value

    # arithmetic ---------------------------------------------------------
    def __add__(self, other):
        return add(self, other)

    def __radd__(self, other):
        return add(other, self)

    def __sub__(self, other):
        return sub(self, other)

    def __rsub__(self, other):
        return sub(other, self)

    def __mul__(self, other):
        return mul(self, other)

    def __rmul__(self, other):
        return mul(other, self)

    def __truediv__(self, other):
        return div(self, other)

    def __rtruediv__(self, other):
        return div(other, self)

    def __mod__(self, other):
        return mod(self, other)

    def __rmod__(self, other):
        return mod(other, self)

    def __neg__(self):
        return sub(0, self)


def unwrap(v):
    if isinstance(v, Ex):
        return v.value
    if isinstance(v, Var):
        return v.get().value
    return v


def inp(value, shadow=None):
    """Encode `value` for an input slot whose default literal is `shadow`."""
    value = unwrap(value)
    if isinstance(value, Sub):
        return value
    if isinstance(value, Lit):
        return value
    if isinstance(value, Block):
        return (value, shadow)
    if shadow is None:
        shadow = txt("")
    return Lit(shadow.type, value)


def _n(value):
    return inp(value, num(""))


def _s(value):
    return inp(value, txt(""))


def _b(value):
    """A boolean slot: no shadow, and an empty slot is simply absent."""
    value = unwrap(value)
    return None if value is None else (value, None)


# --- constant folding -----------------------------------------------------
#
# The widget code multiplies device pixels by the scale factor constantly, and
# almost all of it is constant. Folding here keeps a few thousand pointless
# operator blocks out of the project and out of the editor's way.

def _both_numbers(a, b):
    return isinstance(a, (int, float)) and isinstance(b, (int, float))


def add(a, b):
    a, b = unwrap(a), unwrap(b)
    if _both_numbers(a, b):
        return Ex(a + b)
    if a == 0:
        return Ex(b)
    if b == 0:
        return Ex(a)
    return Ex(Block("operator_add", {"NUM1": _n(a), "NUM2": _n(b)}))


def sub(a, b):
    a, b = unwrap(a), unwrap(b)
    if _both_numbers(a, b):
        return Ex(a - b)
    if b == 0:
        return Ex(a)
    return Ex(Block("operator_subtract", {"NUM1": _n(a), "NUM2": _n(b)}))


def mul(a, b):
    a, b = unwrap(a), unwrap(b)
    if _both_numbers(a, b):
        return Ex(a * b)
    if a == 1:
        return Ex(b)
    if b == 1:
        return Ex(a)
    return Ex(Block("operator_multiply", {"NUM1": _n(a), "NUM2": _n(b)}))


def div(a, b):
    a, b = unwrap(a), unwrap(b)
    if _both_numbers(a, b) and b:
        return Ex(a / b)
    if b == 1:
        return Ex(a)
    return Ex(Block("operator_divide", {"NUM1": _n(a), "NUM2": _n(b)}))


def mod(a, b):
    a, b = unwrap(a), unwrap(b)
    if _both_numbers(a, b) and b:
        return Ex(a % b)
    return Ex(Block("operator_mod", {"NUM1": _n(a), "NUM2": _n(b)}))


def rnd(a):
    a = unwrap(a)
    if isinstance(a, (int, float)):
        # Scratch rounds .5 away from zero for positives, like Python's
        # round() does not -- spell it out rather than inherit banker's
        # rounding into a constant.
        return Ex(int(a + 0.5) if a >= 0 else -int(-a + 0.5))
    return Ex(Block("operator_round", {"NUM": _n(a)}))


def mathop(op, a):
    a = unwrap(a)
    if isinstance(a, (int, float)):
        import math
        if op == "floor":
            return Ex(math.floor(a))
        if op == "ceiling":
            return Ex(math.ceil(a))
        if op == "abs":
            return Ex(abs(a))
    return Ex(Block("operator_mathop", {"NUM": _n(a)}, {"OPERATOR": [op, None]}))


def floor(a):
    return mathop("floor", a)


def ceil(a):
    return mathop("ceiling", a)


def absolute(a):
    return mathop("abs", a)


def random(a, b):
    return Ex(Block("operator_random", {"FROM": _n(a), "TO": _n(b)}))


# --- string operators -----------------------------------------------------

def join(*parts):
    parts = [unwrap(p) for p in parts]
    if not parts:
        return Ex("")
    out = parts[0]
    for part in parts[1:]:
        if isinstance(out, str) and isinstance(part, str):
            out = out + part
            continue
        out = Block("operator_join", {"STRING1": _s(out), "STRING2": _s(part)})
    return Ex(out)


def letter_of(index, string):
    return Ex(Block("operator_letter_of",
                    {"LETTER": _n(index), "STRING": _s(string)}))


def length_of(string):
    return Ex(Block("operator_length", {"STRING": _s(string)}))


def contains(haystack, needle):
    return Ex(Block("operator_contains",
                    {"STRING1": _s(haystack), "STRING2": _s(needle)}))


# --- predicates -----------------------------------------------------------

def eq(a, b):
    return Ex(Block("operator_equals", {"OPERAND1": _s(a), "OPERAND2": _s(b)}))


def lt(a, b):
    return Ex(Block("operator_lt", {"OPERAND1": _s(a), "OPERAND2": _s(b)}))


def gt(a, b):
    return Ex(Block("operator_gt", {"OPERAND1": _s(a), "OPERAND2": _s(b)}))


def le(a, b):
    return NOT(gt(a, b))


def ge(a, b):
    return NOT(lt(a, b))


def AND(*conds):
    conds = [c for c in conds if c is not None]
    out = conds[0]
    for cond in conds[1:]:
        out = Ex(Block("operator_and", {"OPERAND1": _b(out), "OPERAND2": _b(cond)}))
    return out


def OR(*conds):
    conds = [c for c in conds if c is not None]
    out = conds[0]
    for cond in conds[1:]:
        out = Ex(Block("operator_or", {"OPERAND1": _b(out), "OPERAND2": _b(cond)}))
    return out


def NOT(cond):
    return Ex(Block("operator_not", {"OPERAND": _b(cond)}))


# --- motion / looks / pen -------------------------------------------------

def goto(x, y):
    return Block("motion_gotoxy", {"X": _n(x), "Y": _n(y)})


def x_position():
    return Ex(Block("motion_xposition"))


def y_position():
    return Ex(Block("motion_yposition"))


def switch_costume(name):
    menu = Block("looks_costume", fields={"COSTUME": [_costume_default, None]},
                 shadow=True)
    value = unwrap(name)
    if isinstance(value, str):
        menu.fields["COSTUME"] = [value, None]
        return Block("looks_switchcostumeto", {"COSTUME": menu})
    return Block("looks_switchcostumeto", {"COSTUME": (value, None)},)


_costume_default = "costume1"


def set_costume_default(name):
    global _costume_default
    _costume_default = name


def costume_number():
    return Ex(Block("looks_costumenumbername", fields={"NUMBER_NAME": ["number", None]}))


def costume_name():
    return Ex(Block("looks_costumenumbername", fields={"NUMBER_NAME": ["name", None]}))


def set_size(percent):
    return Block("looks_setsizeto", {"SIZE": _n(percent)})


def show():
    return Block("looks_show")


def hide():
    return Block("looks_hide")


def go_to_front():
    return Block("looks_gotofrontback", fields={"FRONT_BACK": ["front", None]})


def pen_clear():
    return Block("pen_clear")


def pen_stamp():
    return Block("pen_stamp")


def pen_down():
    return Block("pen_penDown")


def pen_up():
    return Block("pen_penUp")


def pen_colour(value):
    return Block("pen_setPenColorToColor", {"COLOR": inp(value, colour("#ffffff"))})


def pen_size(value):
    return Block("pen_setPenSizeTo", {"SIZE": _n(value)})


# --- control --------------------------------------------------------------

def repeat(times, body):
    return Block("control_repeat", {"TIMES": _n(times), "SUBSTACK": Sub(body)})


def repeat_until(cond, body):
    return Block("control_repeat_until",
                 {"CONDITION": _b(cond), "SUBSTACK": Sub(body)})


def forever(body):
    return Block("control_forever", {"SUBSTACK": Sub(body)})


def if_(cond, body):
    return Block("control_if", {"CONDITION": _b(cond), "SUBSTACK": Sub(body)})


def if_else(cond, body, otherwise):
    return Block("control_if_else", {"CONDITION": _b(cond),
                                     "SUBSTACK": Sub(body),
                                     "SUBSTACK2": Sub(otherwise)})


def wait(seconds):
    return Block("control_wait", {"DURATION": inp(seconds, Lit(5, "1"))})


def wait_until(cond):
    return Block("control_wait_until", {"CONDITION": _b(cond)})


def stop(option="this script"):
    has_next = "true" if option == "other scripts in sprite" else "false"
    return Block("control_stop", fields={"STOP_OPTION": [option, None]},
                 mutation={"tagName": "mutation", "children": [],
                           "hasnext": has_next})


def when_flag_clicked():
    return Block("event_whenflagclicked")


# --- sensing --------------------------------------------------------------

def key_pressed(key):
    menu = Block("sensing_keyoptions", fields={"KEY_OPTION": ["space", None]},
                 shadow=True)
    value = unwrap(key)
    if isinstance(value, str):
        menu.fields["KEY_OPTION"] = [value, None]
        return Ex(Block("sensing_keypressed", {"KEY_OPTION": menu}))
    return Ex(Block("sensing_keypressed", {"KEY_OPTION": (value, menu)}))


def timer():
    return Ex(Block("sensing_timer"))


def reset_timer():
    return Block("sensing_resettimer")


# --- data -----------------------------------------------------------------

class Var:
    def __init__(self, name, vid):
        self.name = name
        self.id = vid

    def get(self):
        return Ex(Block("data_variable",
                        fields={"VARIABLE": [self.name, self.id]}))

    def set(self, value):
        return Block("data_setvariableto", {"VALUE": _s(value)},
                     {"VARIABLE": [self.name, self.id]})

    def change(self, value):
        return Block("data_changevariableby", {"VALUE": _n(value)},
                     {"VARIABLE": [self.name, self.id]})

    # sugar so a variable can be used straight in arithmetic
    def __add__(self, other):
        return self.get() + other

    def __radd__(self, other):
        return other + self.get()

    def __sub__(self, other):
        return self.get() - other

    def __rsub__(self, other):
        return other - self.get()

    def __mul__(self, other):
        return self.get() * other

    def __rmul__(self, other):
        return other * self.get()

    def __truediv__(self, other):
        return self.get() / other

    def __rtruediv__(self, other):
        return other / self.get()

    def __mod__(self, other):
        return self.get() % other

    def __neg__(self):
        return 0 - self.get()


class ScratchList:
    def __init__(self, name, lid):
        self.name = name
        self.id = lid

    def _fields(self):
        return {"LIST": [self.name, self.id]}

    def add(self, item):
        return Block("data_addtolist", {"ITEM": _s(item)}, self._fields())

    def clear(self):
        return Block("data_deletealloflist", fields=self._fields())

    def delete(self, index):
        return Block("data_deleteoflist", {"INDEX": _n(index)}, self._fields())

    def insert(self, index, item):
        return Block("data_insertatlist", {"INDEX": _n(index), "ITEM": _s(item)},
                     self._fields())

    def replace(self, index, item):
        return Block("data_replaceitemoflist",
                     {"INDEX": _n(index), "ITEM": _s(item)}, self._fields())

    def item(self, index):
        return Ex(Block("data_itemoflist", {"INDEX": _n(index)}, self._fields()))

    def length(self):
        return Ex(Block("data_lengthoflist", fields=self._fields()))

    def index_of(self, item):
        return Ex(Block("data_itemnumoflist", {"ITEM": _s(item)}, self._fields()))

    def contains(self, item):
        return Ex(Block("data_listcontainsitem", {"ITEM": _s(item)},
                        self._fields()))


# --- custom blocks --------------------------------------------------------

class Proc:
    """A custom block: its prototype, its call form and its argument
    reporters. `warp` is Scratch's "run without screen refresh"."""

    def __init__(self, proccode, argnames, warp=True, argtypes=None):
        self.proccode = proccode
        self.argnames = list(argnames)
        self.argtypes = list(argtypes or ["s"] * len(argnames))
        self.warp = warp
        self.argids = ["%s_arg%d" % (_slug(proccode), i)
                       for i in range(len(self.argnames))]

    def arg(self, name):
        index = self.argnames.index(name)
        opcode = ("argument_reporter_boolean" if self.argtypes[index] == "b"
                  else "argument_reporter_string_number")
        return Ex(Block(opcode, fields={"VALUE": [name, None]}))

    def call(self, *args):
        if len(args) != len(self.argnames):
            raise TypeError("%s takes %d arguments, got %d"
                            % (self.proccode, len(self.argnames), len(args)))
        inputs = {}
        for argid, argtype, value in zip(self.argids, self.argtypes, args):
            inputs[argid] = _b(value) if argtype == "b" else _s(value)
        return Block("procedures_call", inputs, mutation=self._mutation(False))

    def _mutation(self, prototype):
        import json as _json
        mutation = {
            "tagName": "mutation",
            "children": [],
            "proccode": self.proccode,
            "argumentids": _json.dumps(self.argids),
            "warp": "true" if self.warp else "false",
        }
        if prototype:
            mutation["argumentnames"] = _json.dumps(self.argnames)
            mutation["argumentdefaults"] = _json.dumps(
                [False if t == "b" else "" for t in self.argtypes])
        return mutation

    def definition(self, body):
        inputs = {}
        for argid, argtype, name in zip(self.argids, self.argtypes, self.argnames):
            opcode = ("argument_reporter_boolean" if argtype == "b"
                      else "argument_reporter_string_number")
            inputs[argid] = Block(opcode, fields={"VALUE": [name, None]},
                                  shadow=True)
        prototype = Block("procedures_prototype", inputs, shadow=True,
                          mutation=self._mutation(True))
        header = Block("procedures_definition", {"custom_block": prototype})
        return [header] + list(body)


def _slug(proccode):
    return "".join(ch if ch.isalnum() else "_" for ch in proccode)[:40]

from System.ui.framework import SoftKeyBar, VerticalList

# Nokia 5190-style calculator: type a number, open Options with the softkey,
# pick Equals/Add/Subtract/Multiply/Divide, keep typing. * or # inserts the
# decimal point, Clear deletes (and exits once the entry is empty).

APP_ID = 7
MAX_DIGITS = 12

OPTIONS = ["Equals", "Add", "Subtract", "Multiply", "Divide"]
OP_FOR_OPTION = {1: "+", 2: "-", 3: "*", 4: "/"}

KEY_ENTER = 28
KEY_BACK = 14
KEY_STAR = 42
KEY_HASH = 43
DIGIT_KEYS = {2: "1", 3: "2", 4: "3", 5: "4", 6: "5",
              7: "6", 8: "7", 9: "8", 10: "9", 11: "0"}


def format_number(value):
    if value != value or value in (float("inf"), float("-inf")):
        return "Error"
    if value == int(value) and abs(value) < 10 ** MAX_DIGITS:
        text = str(int(value))
    else:
        text = f"{value:.8f}".rstrip("0").rstrip(".")
    if len(text) > MAX_DIGITS + 2:
        text = f"{value:.6e}"
    return text


class Calculator:
    def __init__(self, ui):
        self.ui = ui
        self.softkey = SoftKeyBar(ui)
        self.screen_w = getattr(ui, "W", 240)
        self.content_bottom = getattr(ui, "content_bottom",
                                      getattr(ui, "H", 175) - getattr(ui, "SOFTKEY_H", 30))
        self.entry = ""       # what the user is typing
        self.acc = None       # folded left-hand value
        self.pending_op = None

    # --- math -------------------------------------------------------------

    def _fold(self):
        """Fold the current entry into the accumulator via the pending op."""
        if self.entry in ("", "Error"):
            return
        try:
            value = float(self.entry)
        except ValueError:
            value = 0.0

        if self.acc is None or self.pending_op is None:
            self.acc = value
        else:
            try:
                if self.pending_op == "+":
                    self.acc = self.acc + value
                elif self.pending_op == "-":
                    self.acc = self.acc - value
                elif self.pending_op == "*":
                    self.acc = self.acc * value
                elif self.pending_op == "/":
                    self.acc = self.acc / value
            except ZeroDivisionError:
                self.acc = None
                self.pending_op = None
                self.entry = "Error"
                return
        self.entry = ""

    def apply_option(self, choice):
        if choice == 0:  # Equals
            self._fold()
            if self.entry != "Error" and self.acc is not None:
                self.entry = format_number(self.acc)
            self.acc = None
            self.pending_op = None
        elif choice in OP_FOR_OPTION:
            self._fold()
            if self.entry != "Error":
                self.pending_op = OP_FOR_OPTION[choice]

    # --- input helpers ----------------------------------------------------

    def type_digit(self, ch):
        if self.entry == "Error":
            self.entry = ""
        if self.entry in ("0", "-0") and ch != ".":
            self.entry = self.entry[:-1]
        if len(self.entry.lstrip("-").replace(".", "")) >= MAX_DIGITS:
            return
        self.entry += ch

    def type_point(self):
        if self.entry == "Error":
            self.entry = ""
        if "." in self.entry:
            return
        self.entry += "0." if self.entry in ("", "-") else "."

    # --- drawing ----------------------------------------------------------

    def draw(self):
        ui = self.ui
        ui.draw.rectangle((0, 0, self.screen_w, self.content_bottom), fill="black")

        text = self.entry
        if not text:
            text = format_number(self.acc) if self.acc is not None else "0"

        w, h = ui.get_text_size(text, ui.font_xl)
        ui.draw.text((max(5, self.screen_w - 10 - w), 12), text, font=ui.font_xl, fill="white")

        # Small pending-operation hint at the left edge.
        if self.pending_op:
            ui.draw.text((8, 16), self.pending_op, font=ui.font_n, fill="white")

        self.softkey.update("Options")

    def open_options(self):
        menu = VerticalList(self.ui, "Options", OPTIONS, app_id=APP_ID)
        SoftKeyBar(self.ui).update("OK", present=False)
        choice = menu.show()
        if choice >= 0:
            self.apply_option(choice)

    # --- main loop ----------------------------------------------------------

    def loop(self):
        self.draw()
        while True:
            key = self.ui.wait_for_key()

            if key == KEY_ENTER:
                self.open_options()
                self.draw()
            elif key == KEY_BACK:
                if self.entry:
                    self.entry = "" if self.entry == "Error" else self.entry[:-1]
                    self.draw()
                elif self.acc is not None or self.pending_op is not None:
                    self.acc = None
                    self.pending_op = None
                    self.draw()
                else:
                    return
            elif key in DIGIT_KEYS:
                self.type_digit(DIGIT_KEYS[key])
                self.draw()
            elif key in (KEY_STAR, KEY_HASH, 52):  # * / # / '.' on dev keyboard
                self.type_point()
                self.draw()


def run(ui):
    Calculator(ui).loop()

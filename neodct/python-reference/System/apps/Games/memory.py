import random
import time

from games_common import (
    poll_key,
    DIR_KEYS,
    KEY_BACK,
    KEY_ENTER,
    KEY_NUM_5,
)

# Nokia-style Memory: a board of face-down cards, a cursor moved with the
# arrows / 2-4-6-8, key 5 turns a card. Find every pair; fewer misses score
# higher.

COLS = 8
ROWS = 5
PAIR_BASE = 10       # points for a pair found first try
MISS_PENALTY = 2     # deducted per miss since the previous match
PAIR_MIN = 2
REVEAL_SECS = 0.9


def draw_glyph(draw, box, kind, color):
    """Draw one of 20 little pictures inside box=(x0, y0, x1, y1)."""
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    w = x1 - x0

    if kind == 0:    # filled circle
        draw.ellipse(box, fill=color)
    elif kind == 1:  # ring
        draw.ellipse(box, outline=color)
        draw.ellipse((x0 + 3, y0 + 3, x1 - 3, y1 - 3), outline=color)
    elif kind == 2:  # filled square
        draw.rectangle(box, fill=color)
    elif kind == 3:  # hollow square
        draw.rectangle(box, outline=color)
    elif kind == 4:  # filled diamond
        draw.polygon([(cx, y0), (x1, cy), (cx, y1), (x0, cy)], fill=color)
    elif kind == 5:  # hollow diamond
        draw.polygon([(cx, y0), (x1, cy), (cx, y1), (x0, cy)], outline=color)
    elif kind == 6:  # triangle up
        draw.polygon([(cx, y0), (x1, y1), (x0, y1)], fill=color)
    elif kind == 7:  # triangle down
        draw.polygon([(x0, y0), (x1, y0), (cx, y1)], fill=color)
    elif kind == 8:  # plus
        t = max(2, w // 4)
        draw.rectangle((cx - t // 2, y0, cx + t // 2, y1), fill=color)
        draw.rectangle((x0, cy - t // 2, x1, cy + t // 2), fill=color)
    elif kind == 9:  # X
        draw.line((x0, y0, x1, y1), fill=color, width=2)
        draw.line((x0, y1, x1, y0), fill=color, width=2)
    elif kind == 10:  # star (4-point)
        q = max(1, w // 6)
        draw.polygon([(cx, y0), (cx + q, cy - q), (x1, cy), (cx + q, cy + q),
                      (cx, y1), (cx - q, cy + q), (x0, cy), (cx - q, cy - q)],
                     fill=color)
    elif kind == 11:  # heart
        r = max(2, w // 4)
        draw.ellipse((x0, y0, x0 + 2 * r, y0 + 2 * r), fill=color)
        draw.ellipse((x1 - 2 * r, y0, x1, y0 + 2 * r), fill=color)
        draw.polygon([(x0, y0 + r), (x1, y0 + r), (cx, y1)], fill=color)
    elif kind == 12:  # horizontal bars
        for yy in range(y0, y1 + 1, 4):
            draw.line((x0, yy, x1, yy), fill=color, width=2)
    elif kind == 13:  # vertical bars
        for xx in range(x0, x1 + 1, 4):
            draw.line((xx, y0, xx, y1), fill=color, width=2)
    elif kind == 14:  # checker
        step = max(3, w // 3)
        for i, yy in enumerate(range(y0, y1, step)):
            for j, xx in enumerate(range(x0, x1, step)):
                if (i + j) % 2 == 0:
                    draw.rectangle((xx, yy, min(xx + step - 1, x1),
                                    min(yy + step - 1, y1)), fill=color)
    elif kind == 15:  # hourglass
        draw.polygon([(x0, y0), (x1, y0), (x0, y1), (x1, y1)], fill=color)
    elif kind == 16:  # arrow up
        t = max(2, w // 4)
        draw.polygon([(cx, y0), (x1, cy), (x0, cy)], fill=color)
        draw.rectangle((cx - t // 2, cy, cx + t // 2, y1), fill=color)
    elif kind == 17:  # arrow right
        t = max(2, w // 4)
        draw.polygon([(x1, cy), (cx, y0), (cx, y1)], fill=color)
        draw.rectangle((x0, cy - t // 2, cx, cy + t // 2), fill=color)
    elif kind == 18:  # corners
        r = max(2, w // 3)
        draw.rectangle((x0, y0, x0 + r, y0 + r), fill=color)
        draw.rectangle((x1 - r, y0, x1, y0 + r), fill=color)
        draw.rectangle((x0, y1 - r, x0 + r, y1), fill=color)
        draw.rectangle((x1 - r, y1 - r, x1, y1), fill=color)
    else:            # 19: dot in a box
        draw.rectangle(box, outline=color)
        r = max(1, w // 5)
        draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=color)


class MemoryGame:
    def __init__(self, ui):
        self.ui = ui
        self.screen_w = getattr(ui, "W", 240)
        self.screen_h = getattr(ui, "H", 175)
        self.softkey_h = getattr(ui, "SOFTKEY_H", 30)
        self.content_bottom = getattr(ui, "content_bottom", self.screen_h - self.softkey_h)

        usable_h = self.content_bottom - 8
        self.cell = min((self.screen_w - 8) // COLS, usable_h // ROWS)
        self.board_x = (self.screen_w - COLS * self.cell) // 2
        self.board_y = (self.content_bottom - ROWS * self.cell) // 2

        kinds = list(range((COLS * ROWS) // 2)) * 2
        random.seed(time.time())
        random.shuffle(kinds)
        self.cards = kinds                      # index = row * COLS + col
        self.state = ["down"] * len(kinds)      # down | up | gone
        self.cursor = (0, 0)
        self.first_pick = None
        self.score = 0
        self.misses = 0                          # misses since last match

    # --- drawing ----------------------------------------------------------

    def _card_rect(self, col, row):
        px = self.board_x + col * self.cell
        py = self.board_y + row * self.cell
        return (px + 2, py + 2, px + self.cell - 3, py + self.cell - 3)

    def render(self):
        ui = self.ui
        ui.draw.rectangle((0, 0, self.screen_w, self.screen_h), fill="black")

        for row in range(ROWS):
            for col in range(COLS):
                idx = row * COLS + col
                rect = self._card_rect(col, row)
                if self.state[idx] == "down":
                    ui.draw.rectangle(rect, fill="white")
                elif self.state[idx] == "up":
                    ui.draw.rectangle(rect, outline="white")
                    glyph_box = (rect[0] + 4, rect[1] + 4, rect[2] - 4, rect[3] - 4)
                    draw_glyph(ui.draw, glyph_box, self.cards[idx], "white")

        # Cursor: a ring in the gap around the current cell.
        col, row = self.cursor
        px = self.board_x + col * self.cell
        py = self.board_y + row * self.cell
        ui.draw.rectangle((px, py, px + self.cell - 1, py + self.cell - 1),
                          outline="white")
        idx = row * COLS + col
        if self.state[idx] == "down":
            rect = self._card_rect(col, row)
            ui.draw.rectangle(rect, outline="black")

        ui.fb.update(ui.canvas)

    # --- game logic ----------------------------------------------------------

    def move_cursor(self, dx, dy):
        col, row = self.cursor
        self.cursor = ((col + dx) % COLS, (row + dy) % ROWS)

    def flip(self):
        col, row = self.cursor
        idx = row * COLS + col
        if self.state[idx] != "down":
            return

        self.state[idx] = "up"
        if self.first_pick is None:
            self.first_pick = idx
            self.render()
            return

        first = self.first_pick
        self.first_pick = None
        self.render()

        if self.cards[first] == self.cards[idx]:
            self.score += max(PAIR_MIN, PAIR_BASE - MISS_PENALTY * self.misses)
            self.misses = 0
            time.sleep(0.25)
            self.state[first] = "gone"
            self.state[idx] = "gone"
        else:
            self.misses += 1
            time.sleep(REVEAL_SECS)
            self.state[first] = "down"
            self.state[idx] = "down"
        self.render()

    def finished(self):
        return all(s == "gone" for s in self.state)

    def play(self):
        """Runs one game. Returns the final score, or None if the player quit."""
        self.render()
        while True:
            key = poll_key(self.ui, 0.1)
            if key is None:
                continue

            if key == KEY_BACK:
                return None
            if key in DIR_KEYS:
                dx, dy = DIR_KEYS[key]
                self.move_cursor(dx, dy)
                self.render()
            elif key in (KEY_ENTER, KEY_NUM_5):
                self.flip()
                if self.finished():
                    time.sleep(0.3)
                    return self.score

import random
import time

from games_common import (
    poll_key,
    DIR_KEYS,
    KEY_BACK,
)

# Nokia-style Snake: bordered playfield, score at the top, speed and points
# scale with the chosen level (1-9).

GRID_W = 29
GRID_H = 14
CELL = 8


def tick_delay(level):
    return max(0.09, 0.40 - 0.033 * level)


class SnakeGame:
    def __init__(self, ui, level):
        self.ui = ui
        self.level = max(1, min(9, int(level)))
        self.screen_w = getattr(ui, "W", 240)
        self.screen_h = getattr(ui, "H", 175)
        self.softkey_h = getattr(ui, "SOFTKEY_H", 30)
        self.content_bottom = getattr(ui, "content_bottom", self.screen_h - self.softkey_h)

        self.score_h = 20
        self.board_x = (self.screen_w - GRID_W * CELL) // 2
        self.board_y = self.score_h + 4
        self.board_w = GRID_W * CELL
        self.board_h = GRID_H * CELL

        cx, cy = GRID_W // 2, GRID_H // 2
        self.snake = [(cx + 1, cy), (cx, cy), (cx - 1, cy)]
        self.direction = (1, 0)
        self.turn_queue = []
        self.score = 0
        random.seed(time.time())
        self.spawn_food()

    def spawn_food(self):
        body = set(self.snake)
        open_cells = [(x, y) for x in range(GRID_W) for y in range(GRID_H)
                      if (x, y) not in body]
        self.food = random.choice(open_cells) if open_cells else None

    def queue_turn(self, new_dir):
        last = self.turn_queue[-1] if self.turn_queue else self.direction
        if (new_dir[0] + last[0], new_dir[1] + last[1]) == (0, 0):
            return  # no 180-degree turns
        if new_dir != last and len(self.turn_queue) < 2:
            self.turn_queue.append(new_dir)

    def step(self):
        if self.turn_queue:
            self.direction = self.turn_queue.pop(0)
        hx, hy = self.snake[0]
        nx, ny = hx + self.direction[0], hy + self.direction[1]

        if nx < 0 or nx >= GRID_W or ny < 0 or ny >= GRID_H:
            return False  # wall
        new_head = (nx, ny)
        tail = self.snake[-1]
        # the tail cell frees up this tick unless we grow into food
        body = set(self.snake) - ({tail} if new_head != self.food else set())
        if new_head in body:
            return False  # bit itself

        self.snake.insert(0, new_head)
        if self.food and new_head == self.food:
            self.score += self.level
            self.spawn_food()
        else:
            self.snake.pop()
        return True

    # --- drawing ----------------------------------------------------------

    def _cell_rect(self, x, y):
        px = self.board_x + x * CELL
        py = self.board_y + y * CELL
        return (px + 1, py + 1, px + CELL - 2, py + CELL - 2)

    def render(self):
        ui = self.ui
        ui.draw.rectangle((0, 0, self.screen_w, self.screen_h), fill="black")

        ui.draw.text((4, 1), str(self.score), font=ui.font_md, fill="white")

        ui.draw.rectangle(
            (self.board_x - 2, self.board_y - 2,
             self.board_x + self.board_w + 1, self.board_y + self.board_h + 1),
            outline="white",
        )

        if self.food:
            fx, fy, fx2, fy2 = self._cell_rect(*self.food)
            ui.draw.rectangle((fx, fy, fx2, fy2), outline="white")

        for x, y in self.snake:
            ui.draw.rectangle(self._cell_rect(x, y), fill="white")

        ui.fb.update(ui.canvas)

    # --- game loop ----------------------------------------------------------

    def play(self):
        """Runs one game. Returns the final score, or None if the player quit."""
        self.render()
        next_move = time.time() + tick_delay(self.level)

        while True:
            timeout = max(0.0, next_move - time.time())
            key = poll_key(self.ui, min(timeout, 0.05))

            if key is not None:
                if key == KEY_BACK:
                    return None
                new_dir = DIR_KEYS.get(key)
                if new_dir:
                    self.queue_turn(new_dir)

            if time.time() >= next_move:
                if not self.step():
                    self.render()
                    time.sleep(0.4)
                    return self.score
                self.render()
                next_move = time.time() + tick_delay(self.level)

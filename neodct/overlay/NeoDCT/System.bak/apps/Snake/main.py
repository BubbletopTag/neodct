import os
import random
import select
import struct
import time

from System.ui.framework import SoftKeyBar

# Input codes (Linux input layer)
KEY_UP = 103
KEY_DOWN = 108
KEY_LEFT = 105
KEY_RIGHT = 106
KEY_ENTER = 28
KEY_MENU = 50   # Key 'm'
KEY_BACK = 14   # Backspace (as mapped in your VM)

# Number keys on classic keypad
KEY_NUM_2 = 3
KEY_NUM_4 = 5
KEY_NUM_6 = 7
KEY_NUM_8 = 9

# Display + board sizing
GRID_W = 20
GRID_H = 14
CELL = 12
BOARD_TOP = 32  # Moved down slightly to give header room
BOARD_HEIGHT = GRID_H * CELL 

KEYPAD_PATH = "/dev/input/event0"

class SnakeGame:
    def __init__(self, ui):
        self.ui = ui
        self.softkey = SoftKeyBar(ui)
        self.keypad_fd = None
        self.reset()

    def reset(self):
        cx, cy = GRID_W // 2, GRID_H // 2
        self.snake = [(cx, cy - 1), (cx, cy), (cx, cy + 1)]
        self.direction = (0, -1)
        self.next_direction = self.direction
        self.score = 0
        random.seed(time.time())
        self.spawn_food()
        self.render()

    def ensure_keypad(self):
        if self.keypad_fd is not None: return self.keypad_fd
        if hasattr(self.ui, "keypad_fd"):
            self.keypad_fd = self.ui.keypad_fd
            return self.keypad_fd
        try:
            self.keypad_fd = os.open(KEYPAD_PATH, os.O_RDONLY | os.O_NONBLOCK)
        except Exception:
            self.keypad_fd = None
        return self.keypad_fd

    def poll_key(self, timeout):
        fd = self.ensure_keypad()
        if fd is None:
            time.sleep(timeout)
            return None

        r, _, _ = select.select([fd], [], [], timeout)
        if not r: return None

        try:
            data = os.read(fd, 24)
            # Handle both 32-bit (16 byte) and 64-bit (24 byte) input events
            if len(data) == 24:
                _, _, etype, code, val = struct.unpack("llHHI", data)
            elif len(data) == 16:
                _, _, etype, code, val = struct.unpack("IIHHI", data)
            else:
                return None
        except Exception:
            return None

        if etype == 1 and val == 1: # EV_KEY, Key Press
            return code
        return None

    def direction_from_key(self, key):
        if key in (KEY_UP, KEY_NUM_2): return (0, -1)
        if key in (KEY_DOWN, KEY_NUM_8): return (0, 1)
        if key in (KEY_LEFT, KEY_NUM_4): return (-1, 0)
        if key in (KEY_RIGHT, KEY_NUM_6): return (1, 0)
        return None

    def tick_delay(self):
        return 0.15

    def spawn_food(self):
        open_cells = [(x, y) for x in range(GRID_W) for y in range(GRID_H) if (x, y) not in self.snake]
        self.food = random.choice(open_cells) if open_cells else None

    def step(self):
        self.direction = self.next_direction
        hx, hy = self.snake[0]
        nx, ny = hx + self.direction[0], hy + self.direction[1]

        # Walls - Death
        if nx < 0 or nx >= GRID_W or ny < 0 or ny >= GRID_H:
            return False

        new_head = (nx, ny)
        if new_head in self.snake:
            return False # Self-collision - Death

        self.snake.insert(0, new_head)

        if self.food and new_head == self.food:
            self.score += 1
            self.spawn_food()
        else:
            self.snake.pop()

        return True

    def render(self):
        # 1. Clear Screen
        self.ui.draw.rectangle((0, 0, 240, 210), fill="black")

        # 2. Header (Smaller Font, Monochrome)
        self.ui.draw.text((5, 5), "Snake", font=self.ui.font_md, fill="white")
        
        score_text = f"{self.score}"
        w, h = self.ui.get_text_size(score_text, self.ui.font_md)
        self.ui.draw.text((235 - w, 5), score_text, font=self.ui.font_md, fill="white")

        # 3. Board Outline
        board_bottom = BOARD_TOP + BOARD_HEIGHT
        self.ui.draw.rectangle((0, BOARD_TOP, 239, board_bottom), outline="white")

        # 4. Food (Hollow Box style)
        if self.food:
            fx = self.food[0] * CELL
            fy = BOARD_TOP + (self.food[1] * CELL)
            # Draw outline white, fill black
            self.ui.draw.rectangle((fx + 1, fy + 1, fx + CELL - 2, fy + CELL - 2), outline="white", fill="black")

        # 5. Snake (Solid White)
        for idx, (x, y) in enumerate(self.snake):
            px = x * CELL
            py = BOARD_TOP + (y * CELL)
            self.ui.draw.rectangle((px + 1, py + 1, px + CELL - 2, py + CELL - 2), fill="white")

        # 6. Softkey
        self.softkey.update("Back") # Handles flush

    def game_over(self):
        # 1. Clear Entire Screen
        self.ui.draw.rectangle((0, 0, 240, 240), fill="black")
        
        # 2. Big "Game Over"
        text = "GAME OVER"
        w, h = self.ui.get_text_size(text, self.ui.font_xl)
        self.ui.draw.text(((240 - w) // 2, 60), text, font=self.ui.font_xl, fill="white")
        
        # 3. Score
        score_text = f"Score: {self.score}"
        w2, h2 = self.ui.get_text_size(score_text, self.ui.font_md)
        self.ui.draw.text(((240 - w2) // 2, 100), score_text, font=self.ui.font_md, fill="white")

        # 4. Instructions
        prompt = "Restart"
        self.softkey.update(prompt) # Updates bottom bar
        self.ui.fb.update(self.ui.canvas)

        # 5. Wait for Input (Blocking is fine here)
        while True:
            # We poll slightly slower here to save CPU
            key = self.poll_key(0.1)
            
            # Restart on: Menu, Enter, or '5' (common action key)
            if key in (KEY_MENU, KEY_ENTER, 5): 
                return "restart"
            # Exit on: Backspace/Clear
            if key in (KEY_BACK, 14):
                return "exit"

    def play(self):
        while True:
            result = self.loop()
            if result == "exit": return
            
            decision = self.game_over()
            if decision == "exit": return
            
            self.reset()

    def loop(self):
        # Similar loop to before
        next_move = time.time() + self.tick_delay()
        while True:
            now = time.time()
            timeout = max(0, next_move - now)
            key = self.poll_key(timeout)
            
            if key is not None:
                new_dir = self.direction_from_key(key)
                if key == KEY_BACK: return "exit"
                if new_dir:
                    # Prevent 180 turn
                    if (new_dir[0] + self.direction[0], new_dir[1] + self.direction[1]) != (0, 0):
                        self.next_direction = new_dir
            
            if time.time() >= next_move:
                if not self.step():
                    return "dead"
                self.render()
                next_move = time.time() + self.tick_delay()

def run(ui):
    game = SnakeGame(ui)
    game.play()
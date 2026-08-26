import os
import sys

# Apps are loaded by importlib under the name "neodct_app", so sibling
# modules need this directory on sys.path.
_APP_DIR = os.path.dirname(os.path.abspath(__file__))
if _APP_DIR not in sys.path:
    sys.path.insert(0, _APP_DIR)

from System.ui.framework import (
    SoftKeyBar,
    VerticalList,
    LevelSelector,
    TextScroller,
    InfoScreen,
)

from games_common import APP_ID, get_setting_int, set_setting_value
from snake import SnakeGame
from memory import MemoryGame

SNAKE_LEVEL_KEY = "games.snake.level"
SNAKE_TOP_KEY = "games.snake.topscore"
MEMORY_TOP_KEY = "games.memory.topscore"

SNAKE_MENU = ["New game", "Level", "Top score", "Instructions"]
MEMORY_MENU = ["New game", "Top score", "Instructions"]

SNAKE_INSTRUCTIONS = (
    "Feed the snake by steering it to the food. Every bite makes it grow "
    "longer. Use keys 2, 4, 6 and 8 to change direction. The game ends if "
    "the snake runs into the walls or into its own body. A higher level "
    "means more speed and more points for each bite."
)

MEMORY_INSTRUCTIONS = (
    "All the cards lie face down. Move the cursor with keys 2, 4, 6 and 8 "
    "and turn a card over with key 5. Two matching cards are cleared from "
    "the board. Find every pair to finish the game. The fewer tries you "
    "need, the higher your score."
)


def _show_menu(ui, title, items):
    menu = VerticalList(ui, title, items, app_id=APP_ID)
    SoftKeyBar(ui).update("Select", present=False)
    return menu.show()


def _finish_game(ui, score, top_key):
    """Shared end-of-game flow: show score, update the stored top score."""
    if score is None:
        return
    top = get_setting_int(top_key, 0)
    if score > top:
        set_setting_value(top_key, score)
        InfoScreen(ui, "New top score:", score, softkey_text="OK").show()
    else:
        InfoScreen(ui, "Game over! Score:", score, softkey_text="OK").show()


def snake_menu(ui):
    while True:
        choice = _show_menu(ui, "Snake", SNAKE_MENU)
        if choice < 0:
            return
        if choice == 0:
            level = get_setting_int(SNAKE_LEVEL_KEY, 5)
            score = SnakeGame(ui, level).play()
            _finish_game(ui, score, SNAKE_TOP_KEY)
        elif choice == 1:
            current = get_setting_int(SNAKE_LEVEL_KEY, 5)
            picked = LevelSelector(ui, current=current, app_id=APP_ID).show()
            if picked is not None:
                set_setting_value(SNAKE_LEVEL_KEY, picked)
        elif choice == 2:
            InfoScreen(ui, "Top score", get_setting_int(SNAKE_TOP_KEY, 0)).show()
        elif choice == 3:
            TextScroller(ui, SNAKE_INSTRUCTIONS).show()


def memory_menu(ui):
    while True:
        choice = _show_menu(ui, "Memory", MEMORY_MENU)
        if choice < 0:
            return
        if choice == 0:
            score = MemoryGame(ui).play()
            _finish_game(ui, score, MEMORY_TOP_KEY)
        elif choice == 1:
            InfoScreen(ui, "Top score", get_setting_int(MEMORY_TOP_KEY, 0)).show()
        elif choice == 2:
            TextScroller(ui, MEMORY_INSTRUCTIONS).show()


def run(ui):
    while True:
        choice = _show_menu(ui, "Games", ["Memory", "Snake"])
        if choice < 0:
            return
        if choice == 0:
            memory_menu(ui)
        elif choice == 1:
            snake_menu(ui)

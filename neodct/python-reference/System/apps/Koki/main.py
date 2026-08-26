# Koki for NeoDCT -- a faithful port of the Scratch game by (and with) a
# good friend of the project. Runs on the standard app contract: run(ui).
#
# Controls (QEMU keyboard / phone keypad):
#   left/right or 4/6 .. walk        up or 2 ......... enter door / plane up
#   z or 5 or * ........ jump/boost  down or 8 ....... plane down
#   x or 0 or # ........ action      Enter/navi ...... start / confirm
#   C (backspace) ...... pause/quit

import os
import sys

APP_DIR = os.path.dirname(os.path.abspath(__file__))


def run(ui):
    added = False
    if APP_DIR not in sys.path:
        sys.path.insert(0, APP_DIR)
        added = True
    # fresh modules per launch (the launcher re-imports main.py each time)
    for mod in ("engine", "game"):
        sys.modules.pop(mod, None)
    eng = None
    try:
        import engine
        import game
        eng = engine.Engine(ui, APP_DIR)
        game.register_all(eng)
        eng.run()
    finally:
        # drop everything the game allocated: on the 32MB Luckfox, stale
        # engines/caches surviving in sys.modules make the whole OS crawl
        eng = None
        for mod in ("engine", "game"):
            sys.modules.pop(mod, None)
        if added:
            try:
                sys.path.remove(APP_DIR)
            except ValueError:
                pass
        import gc
        gc.collect()

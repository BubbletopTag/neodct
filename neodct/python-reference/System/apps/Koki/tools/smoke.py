#!/usr/bin/env python3
"""Smoke-test every Koki level/screen headlessly.

Each scenario boots a fresh engine, injects broadcasts/keys at given frames,
and saves screenshots. Any script crash is printed by the engine, so a clean
run + sane screenshots = the wiring works.

Usage: python3 tools/smoke.py [scenario ...]   (default: all)
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
sys.path.insert(0, APP)
sys.path.insert(0, HERE)

os.environ["NEODCT_KOKI_NOSOUND"] = "1"

from harness import FakeUI  # noqa: E402


def run_scenario(name, frames, actions, holds, shots, outdir):
    """actions: {frame: callable(eng)}; holds: {key: [(a,b)...]}"""
    for mod in ("engine", "game"):
        sys.modules.pop(mod, None)
    import engine
    import game

    os.makedirs(outdir, exist_ok=True)
    ui = FakeUI(outdir)
    eng = engine.Engine(ui, APP)
    game.register_all(eng)
    eng.random.seed(42)
    eng.headless_frames = frames
    ui.fb.save_set = set(shots)

    n = {"f": 0}

    def poll():
        cur = set()
        for k, spans in holds.items():
            for a, b in spans:
                if a <= n["f"] <= b:
                    cur.add(k)
        eng.input.pressed = cur - eng.input.held
        eng.input.held = cur
    eng.input.poll = poll

    orig_render = eng.render

    def render():
        ui.fb.frame_no = n["f"]
        if n["f"] == 1:
            # kill the boot sequence: scenarios jump straight into a level
            eng.stop_all_scripts()
            eng.sprites["Dynaris Logo"].hide()
        fn = actions.get(n["f"])
        if fn:
            fn(eng)
        orig_render()
        n["f"] += 1
    eng.render = render

    print(f"--- scenario: {name} ---")
    eng.run()


S = {}


def bc(*msgs):
    def _do(eng):
        for m in msgs:
            eng.broadcast(m)
    return _do


def setvar(k, v):
    def _do(eng):
        eng.vars[k] = v
    return _do


S["lv1"] = dict(
    frames=1000,
    actions={2: bc("level1"),
             500: bc("enemy1damage"),   # simulated cannon hit
             800: bc("enemy1damage")},
    holds={"z": [(150, 152), (180, 182), (210, 212)]},   # hop the waves
    shots=[10, 120, 200, 420, 520, 560, 700, 999],
)

S["lv2"] = dict(
    frames=1200,
    actions={2: bc("startlv2", "planecutscene"),
             700: bc("enemy2 damage"),
             1000: bc("enemy2 damage")},
    holds={"up": [(200, 240), (400, 430)], "down": [(300, 340)]},
    shots=[30, 80, 160, 260, 420, 620, 750, 1100],
)

S["lv3"] = dict(
    frames=1000,
    actions={2: bc("level3"),
             500: bc("enemy 3 damage")},
    holds={"z": [(200, 205), (300, 305), (600, 605)]},
    shots=[30, 150, 250, 420, 520, 700, 999],
)

S["final"] = dict(
    frames=1400,
    actions={2: bc("go to lobby"),
             40: setvar("doors", 4),
             60: bc("final cutscene")},
    holds={},
    shots=[80, 140, 200, 260, 330, 420, 600, 800, 1000, 1399],
)

S["gameover"] = dict(
    frames=400,
    actions={2: setvar("lives", 0), 3: bc("game over"),
             200: None},
    holds={"enter": [(200, 205)]},
    shots=[50, 150, 260, 399],
)

S["ending"] = dict(
    frames=1400,
    actions={2: bc("ending cutscene")},
    holds={},
    shots=[50, 300, 620, 700, 760, 900, 1399],
)


def main():
    which = sys.argv[1:] or list(S)
    base = sys.argv[0] and os.environ.get(
        "KOKI_SMOKE_OUT", "/tmp/koki_smoke")
    for name in which:
        cfg = S[name]
        cfg["actions"] = {f: a for f, a in cfg["actions"].items() if a}
        run_scenario(name, cfg["frames"], cfg["actions"], cfg["holds"],
                     cfg["shots"], os.path.join(base, name))


if __name__ == "__main__":
    main()

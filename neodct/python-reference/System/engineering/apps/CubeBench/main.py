import math
import time

from System.ui.framework import SoftKeyBar

EXIT_KEYS = {14, 28, 46, 50}


def _rotate_x(v, a):
    x, y, z = v
    ca = math.cos(a)
    sa = math.sin(a)
    return (x, y * ca - z * sa, y * sa + z * ca)


def _rotate_y(v, a):
    x, y, z = v
    ca = math.cos(a)
    sa = math.sin(a)
    return (x * ca + z * sa, y, -x * sa + z * ca)


def _rotate_z(v, a):
    x, y, z = v
    ca = math.cos(a)
    sa = math.sin(a)
    return (x * ca - y * sa, x * sa + y * ca, z)


def _project(v, center_x, center_y, fov, view_dist):
    x, y, z = v
    # Simple perspective divide
    denom = (z + view_dist)
    if denom < 0.1:
        denom = 0.1
    scale = fov / denom
    sx = int(center_x + x * scale)
    sy = int(center_y + y * scale)
    return sx, sy


def run(ui):
    screen_w = getattr(ui, "W", 240)
    screen_h = getattr(ui, "H", 175)
    softkey_h = getattr(ui, "SOFTKEY_H", 30)
    content_bottom = getattr(ui, "content_bottom", screen_h - softkey_h)

    center_x = screen_w // 2
    center_y = content_bottom // 2
    softkey = SoftKeyBar(ui)

    # Cube vertices in model space
    size = min(screen_w, content_bottom) * 0.22
    vertices = [
        (-size, -size, -size),
        ( size, -size, -size),
        ( size,  size, -size),
        (-size,  size, -size),
        (-size, -size,  size),
        ( size, -size,  size),
        ( size,  size,  size),
        (-size,  size,  size),
    ]

    edges = [
        (0, 1), (1, 2), (2, 3), (3, 0),
        (4, 5), (5, 6), (6, 7), (7, 4),
        (0, 4), (1, 5), (2, 6), (3, 7),
    ]

    # Camera-ish params
    fov = min(screen_w, content_bottom) * 1.1
    view_dist = size * 5.5

    # Animation state
    ax = 0.0
    ay = 0.0
    az = 0.0

    last_time = time.perf_counter()
    fps_display = 0.0
    fps_inst = 0.0
    frame_counter = 0
    fps_window_start = last_time

    while True:
        now = time.perf_counter()
        dt = now - last_time
        last_time = now
        if dt <= 0.0:
            dt = 0.001

        # Check for exit without blocking frame updates
        key = ui.read_keypress(0)
        if key in EXIT_KEYS:
            return

        # Advance rotation (tuned for visible but smooth motion)
        ax += 1.30 * dt
        ay += 1.10 * dt
        az += 0.85 * dt

        # Clear drawable area
        ui.draw.rectangle((0, 0, screen_w, content_bottom), fill="black")

        # Rotate + project all vertices
        proj = []
        for v in vertices:
            r = _rotate_x(v, ax)
            r = _rotate_y(r, ay)
            r = _rotate_z(r, az)
            proj.append(_project(r, center_x, center_y, fov, view_dist))

        # Draw wireframe cube
        for a, b in edges:
            x1, y1 = proj[a]
            x2, y2 = proj[b]
            ui.draw.line((x1, y1, x2, y2), fill="white", width=1)

        # Frame stats (pure Python baseline indicator)
        fps_inst = 1.0 / dt
        frame_counter += 1
        elapsed = now - fps_window_start
        # Update displayed FPS every 0.5s using measured frames/time window.
        if elapsed >= 0.5:
            fps_display = frame_counter / elapsed
            frame_counter = 0
            fps_window_start = now

        title = "3D Cube"
        fps_text = "FPS %.1f" % fps_display
        hint = "BACK/OK to exit"

        ui.draw.text((6, 4), title, font=ui.font_s, fill="white")
        fw, _ = ui.get_text_size(fps_text, ui.font_s)
        ui.draw.text((screen_w - fw - 6, 16), fps_text, font=ui.font_s, fill="white")

        hw, hh = ui.get_text_size(hint, ui.font_s)
        ui.draw.text(((screen_w - hw) // 2, content_bottom - hh - 4), hint, font=ui.font_s, fill="gray")

        # Opaque app-mode softkey bar
        softkey.update("Exit", present=False)

        ui.fb.update(ui.canvas)

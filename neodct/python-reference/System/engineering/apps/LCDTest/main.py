from System.ui.framework import SoftKeyBar

# Key codes used across existing apps/framework.
KEY_NAV = 28       # Enter / Nav select
KEY_BACK = 14      # Backspace / Back


def _draw_color(ui, color):
    screen_w = getattr(ui, "W", 240)
    content_bottom = getattr(ui, "content_bottom", getattr(ui, "H", 175) - getattr(ui, "SOFTKEY_H", 30))
    ui.draw.rectangle((0, 0, screen_w, content_bottom), fill=color)


def _draw_tv_pattern(ui):
    screen_w = getattr(ui, "W", 240)
    content_bottom = getattr(ui, "content_bottom", getattr(ui, "H", 175) - getattr(ui, "SOFTKEY_H", 30))

    # Top bars (rough TV-style test bars)
    top_h = int(content_bottom * 0.7)
    bars = [
        "#FFFFFF", "#FFFF00", "#00FFFF", "#00FF00",
        "#FF00FF", "#FF0000", "#0000FF"
    ]
    bar_w = max(1, screen_w // len(bars))
    for i, color in enumerate(bars):
        x0 = i * bar_w
        x1 = screen_w if i == len(bars) - 1 else (i + 1) * bar_w
        ui.draw.rectangle((x0, 0, x1, top_h), fill=color)

    # Lower section stripes + black patch
    lower_h = content_bottom - top_h
    if lower_h > 0:
        mid_y = top_h + (lower_h // 2)
        ui.draw.rectangle((0, top_h, screen_w, mid_y), fill="#202020")
        stripe_colors = ["#00214A", "#FFFFFF", "#32006A", "#000000"]
        stripe_w = max(1, screen_w // len(stripe_colors))
        for i, color in enumerate(stripe_colors):
            x0 = i * stripe_w
            x1 = screen_w if i == len(stripe_colors) - 1 else (i + 1) * stripe_w
            ui.draw.rectangle((x0, mid_y, x1, content_bottom), fill=color)


def run(ui):
    softkey = SoftKeyBar(ui)

    patterns = [
        ("Red", lambda: _draw_color(ui, "#FF0000")),
        ("Green", lambda: _draw_color(ui, "#00FF00")),
        ("Blue", lambda: _draw_color(ui, "#0000FF")),
        ("TV Test", lambda: _draw_tv_pattern(ui)),
    ]

    idx = 0
    while True:
        _, draw_fn = patterns[idx]
        draw_fn()
        softkey.update("Next")
        ui.fb.update(ui.canvas)

        key = ui.wait_for_key()
        if key == KEY_NAV:
            idx = (idx + 1) % len(patterns)
        elif key == KEY_BACK:
            return

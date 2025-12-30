# messages/main.py (stub app)
# Uses the new PagedList to show a menu with placeholders.
# :contentReference[oaicite:1]{index=1}

from System.ui.framework import PagedList, HeaderWidget, SoftKeyBar

ROOT_ID_MESSAGES = 2  # matches "2-1" style header in your reference

def _show_stub_screen(ui, title, root_id, sub_index):
    ui.draw.rectangle((0, 0, 240, 240), fill="black")
    ui.draw.text((5, 5), title, font=ui.font_xl, fill="white")
    ui.draw.line((0, 35, 240, 35), fill="white")

    HeaderWidget(ui, root_id).draw(sub_index)

    ui.draw.text((10, 90), "Not implemented", font=ui.font_n, fill="white")
    ui.draw.text((10, 115), "Press BACK", font=ui.font_n, fill="gray")

    SoftKeyBar(ui).update("Back", present=False)
    ui.fb.update(ui.canvas)

    while True:
        key = ui.wait_for_key()
        if key == 14:  # BACKSPACE
            return

def run(ui):
    menu = PagedList(
        ui=ui,
        title="Messages",
        items=[
            "Text Messages",
            "idk yet",
            "idk yet 2",
        ],
        root_id=ROOT_ID_MESSAGES,
        show_select_hint=True,
    )

    while True:
        sel = menu.show()
        if sel < 0:
            return

        name = menu._get_item_name(sel)
        _show_stub_screen(ui, name, ROOT_ID_MESSAGES, sel + 1)

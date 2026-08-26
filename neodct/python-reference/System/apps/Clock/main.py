from System.ui.framework import SoftKeyBar, MessageDialog

def run(ui):
    screen_w = getattr(ui, "W", 240)
    content_bottom = getattr(ui, "content_bottom", getattr(ui, "H", 175) - getattr(ui, "SOFTKEY_H", 30))

    # Clear screen
    ui.draw.rectangle((0, 0, screen_w, content_bottom), fill="black")
    warningmsg = MessageDialog(ui, "This application has not been implemented yet.")
    ui.fb.update(ui.canvas)

    while True:
        warningmsg.show()
        # Wait for a key
        key = ui.wait_for_key()
        # BACK / MENU / ENTER exits app
        if key in (46, 28, 50):
            return

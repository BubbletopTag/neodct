from System.ui.framework import SoftKeyBar, MessageDialog

def run(ui):
    # Clear screen
    ui.draw.rectangle((0, 0, 240, 210), fill="black")
    softkey = SoftKeyBar(ui)
    softkey.update("Testing123")
    warningmsg = MessageDialog(ui, "This is a test of the error screen")

    # Draw Hello World centered
    text = "Hello World"
    w, h = ui.get_text_size(text, ui.font_xl)
    ui.draw.text(
        ((240 - w) // 2, (240 - h) // 2),
        text,
        font=ui.font_xl,
        fill="white"
    )

    ui.fb.update(ui.canvas)

    while True:
        warningmsg.show()
        # Wait for a key
        key = ui.wait_for_key()
        # BACK / MENU / ENTER exits app
        if key in (46, 28, 50):
            return

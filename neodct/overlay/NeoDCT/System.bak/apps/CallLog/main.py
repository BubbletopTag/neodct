from System.ui.framework import SoftKeyBar, MessageDialog

def run(ui):
    # Clear screen
    ui.draw.rectangle((0, 0, 240, 210), fill="black")
    warningmsg = MessageDialog(ui, "This application has not been implemented yet.")
    ui.fb.update(ui.canvas)

    while True:
        warningmsg.show()
        # Wait for a key
        key = ui.wait_for_key()
        # BACK / MENU / ENTER exits app
        if key in (46, 28, 50):
            return

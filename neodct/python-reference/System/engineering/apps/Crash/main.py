from System.ui.framework import VerticalList, SoftKeyBar

APP_ID = 9997


def run(ui):
    softkey = SoftKeyBar(ui)
    while True:
        menu = VerticalList(ui, "Crash", ["CRASH!"], app_id=APP_ID)
        softkey.update("Select", present=False)
        choice = menu.show()

        if choice == -1:
            return

        if choice == 0:
            raise RuntimeError("Intentional crash from Crash app (test)")

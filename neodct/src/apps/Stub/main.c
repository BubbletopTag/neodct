/* apps/Stub/main.c -- the app that says it is not an app yet.
 *
 * One app.so, built once, installed into all twenty-four app directories.
 * SESSION-SCOPE.md keeps every app in place with its real manifest.json and
 * its real icon.png so the menu looks complete and every icon shows; this is
 * what opens when you pick one.
 *
 * ============ THERE IS A PIXEL-EXACT TARGET FOR THIS ============
 *
 * golden/widget-messagedialog.png is this precise string rendered through the
 * real Python MessageDialog -- warning triangle, two lines of text, OK button
 * -- and spec-build-test.md section 3.6 records that app-clock is BYTE
 * IDENTICAL to it, because the Clock app is exactly this dialog. So the three
 * lines below are not "a placeholder": they are a screen with a reference
 * image, and changing any of them breaks a golden frame.
 *
 * The Python (System/apps/Clock/main.py) wraps the dialog in `while True:` and
 * exits on 46/28/50. MessageDialog's own key set already covers that, so the
 * loop is not reproduced -- see the note in OPEN-QUESTIONS.md.
 */

#include "nd_app.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

int app_run(nd_ui *ui)
{
    nd_msgdialog dlg;

    if (ui == NULL)
        return 1;

    nd_msgdialog_init(&dlg, ui, "This application has not been implemented yet.");
    (void)nd_msgdialog_show(&dlg);
    return 0;
}

/* MANDATORY even though there is nothing to release: nd_app.h requires every
 * app to export one, so that a missing symbol always means the author forgot
 * and never means there was nothing to do. */
void app_shutdown(void) {}

/* apps/Clock/main.c -- the Clock app, app id 8.
 *
 * A one-to-one port of System/apps/Clock/main.py, which is eighteen lines and
 * does not tell the time. It clears the content rows, flushes the canvas,
 * and then loops on a MessageDialog reading "This application has not been
 * implemented yet." until C, ENTER or MENU is pressed.
 *
 * ============ THIS APP OWNS A GOLDEN FRAME ============
 *
 * golden/app-clock.png IS golden/widget-messagedialog.png -- spec-build-test.md
 * section 3.6 records the two as byte-identical, because the Clock app is
 * exactly that dialog. Until now nd-shoot drew app-clock with apps/Stub, whose
 * comment says so at length; this file replaces it in that role and must
 * produce the same pixels.
 *
 * It does, and the reason is worth writing down rather than discovering:
 *
 *   * The extra `ui.fb.update(ui.canvas)` on line 10 of the Python commits a
 *     frame BEFORE the dialog is drawn -- a black band over rows 0..145 with
 *     whatever the launcher left in the softkey strip. shoot_docs.py saves
 *     frames[-1], so that frame is captured and then discarded. It is not the
 *     reference; the dialog drawn after it is.
 *   * The virtual clock ticks once for it (nd_capture.h), and nothing this
 *     app draws reads the clock, so the tick moves no pixel.
 *
 * OPEN-QUESTIONS.md X-18 recorded that the stub did NOT reproduce the
 * `while True:` loop and its 46/28/50 exit set. It is reproduced here, which
 * is what closes that entry -- see the CT-section.
 *
 * ============ WHY THE LOOP NEEDS TWO KEYS AND THE STUB NEEDED ONE ============
 *
 * `warningmsg.show()` runs its own key loop and returns the key that dismissed
 * it. The Python THROWS THAT KEY AWAY and calls wait_for_key() again, so
 * leaving this screen takes two presses: one to dismiss the dialog, one for
 * the loop's own test. Press ENTER once and the dialog is simply redrawn.
 * That is the behaviour on the phone today and it is ported as-is.
 */

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_keycodes.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#include "clock_app.h"

const char *const nd_clock_app_message = "This application has not been implemented yet.";

/* 46 is ND_KEY_C, 28 ND_KEY_ENTER, 50 ND_KEY_MENU. Written as the numbers
 * main.py holds so a renumbered keycode header cannot silently move them. */
const int32_t nd_clock_app_exit_keys[ND_CLOCK_APP_EXIT_KEYS] = {46, 28, 50};

bool nd_clock_app_is_exit_key(int32_t key)
{
    size_t i;

    for (i = 0u; i < ND_CLOCK_APP_EXIT_KEYS; i++) {
        if (nd_clock_app_exit_keys[i] == key)
            return true;
    }
    return false;
}

int app_run(nd_ui *ui)
{
    nd_msgdialog warningmsg;
    int32_t screen_w;
    int32_t content_bottom;

    if (ui == NULL)
        return 1;

    screen_w = nd_ui_width(ui);
    content_bottom = nd_ui_content_bottom(ui);

    /* Clear screen. The rect is INCLUSIVE of both corners, so this is
     * Pillow's (0, 0, screen_w, content_bottom) unchanged -- one column and
     * one row wider than the content area, both of them off the edge or
     * under the softkey strip. */
    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, screen_w, content_bottom), ND_BLACK);
    nd_msgdialog_init(&warningmsg, ui, nd_clock_app_message);
    (void)nd_ui_present(ui);

    for (;;) {
        int32_t key;

        (void)nd_msgdialog_show(&warningmsg);
        /* Wait for a key */
        key = nd_ui_wait_for_key(ui);
        /* BACK / MENU / ENTER exits app */
        if (nd_clock_app_is_exit_key(key))
            return 0;

        /* Not in the Python, which had exceptions to unwind it. nd_app.h:
         * a loop that outlives a frame polls this so an incoming call is
         * not waiting on a user who has walked away. */
        if (nd_app_should_exit())
            return 0;
    }
}

/* Nothing is held: no file, no child process, no sound card. The symbol
 * exists because nd_app.h requires every app to export one. */
void app_shutdown(void) {}

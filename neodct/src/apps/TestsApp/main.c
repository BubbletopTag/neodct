/* apps/TestsApp/main.c -- "Tests", engineering app 9999.
 *
 * A one-to-one port of System/engineering/apps/TestsApp/main.py, which is
 * thirty-one lines and is what its name says: somebody's scratch app for
 * checking that a softkey, a centred label and the error screen all still
 * work. It owns golden/eng-tests.png.
 *
 * ============ THREE FRAMES, AND ONLY THE THIRD IS THE REFERENCE ============
 *
 * shoot_docs.py saves frames[-1], and main.py commits three:
 *
 *   1  softkey.update("Testing123")  -- present defaults to True, so the
 *      black content rows and the new strip go to the panel with no
 *      "Hello World" on them yet
 *   2  ui.fb.update(ui.canvas)       -- the same screen plus the label
 *   3  warningmsg.show()             -- the MessageDialog, which repaints
 *      every row of the 240x175 and is therefore what the reference holds
 *
 * Frames 1 and 2 are captured and discarded. They are not dead weight: the
 * virtual clock ticks once per committed frame (nd_capture.h), so dropping
 * either would move every clock-reading pixel in any frame drawn after this
 * app -- and test_testsapp.c pins the count for that reason.
 *
 * ============ WHY THE LABEL IS DRAWN AND THEN BURIED ============
 *
 * `MessageDialog.render()` fills (0, 0, W, H) before it draws anything, so
 * "Hello World" survives for exactly one frame. main.py's order is
 * rectangle -> softkey -> dialog object -> label -> flush -> show(), and it is
 * kept: constructing the dialog draws nothing, but moving the construction
 * after the label would still be a different program, and this file is not in
 * the business of deciding which of the Python's orderings mattered.
 *
 * ============ THE LOOP IS THE CLOCK APP'S LOOP ============
 *
 * `while True: warningmsg.show(); key = ui.wait_for_key(); if key in (46, 28,
 * 50): return` throws away the key that dismissed the dialog and then asks
 * for another, so leaving takes two presses and CLEAR never leaves at all --
 * it cancels the dialog, the loop rejects it, and the dialog is drawn again.
 * apps/Clock/main.c has the same shape for the same reason (X-18); both are
 * ported as-is.
 */

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_keycodes.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#include "testsapp.h"

const char *const nd_testsapp_softkey = "Testing123";
const char *const nd_testsapp_message = "This is a test of the error screen";
const char *const nd_testsapp_greeting = "Hello World";

/* Written as the numbers main.py holds, not as nd_keycodes.h names, so a
 * renumbered header cannot move them without this line changing too. */
const int32_t nd_testsapp_exit_keys[ND_TESTSAPP_EXIT_KEYS] = {46, 28, 50};

bool nd_testsapp_is_exit_key(int32_t key)
{
    size_t i;

    for (i = 0u; i < ND_TESTSAPP_EXIT_KEYS; i++) {
        if (nd_testsapp_exit_keys[i] == key)
            return true;
    }
    return false;
}

/* Python's // floors toward negative infinity; C's / truncates toward zero.
 * They agree on every string that fits the panel and disagree the moment one
 * does not, which is the case that puts a label at x = -1 instead of x = 0. */
static int32_t floordiv(int32_t a, int32_t b)
{
    int32_t q = a / b;

    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

void nd_testsapp_greeting_pos(nd_ui *ui, int32_t *x, int32_t *y)
{
    int32_t w = 0;
    int32_t h = 0;

    if (ui == NULL)
        return;
    /* ui.get_text_size(text, ui.font_xl) -- the INK box, not the face
     * metrics, so a string with no descender sits lower than a naive
     * "centre the line" would put it. That is what the reference frame has. */
    nd_ui_text_size(ui, nd_testsapp_greeting, ui->font_xl, &w, &h);
    if (x != NULL)
        *x = floordiv(nd_ui_width(ui) - w, 2);
    if (y != NULL)
        *y = floordiv(nd_ui_content_bottom(ui) - h, 2);
}

int app_run(nd_ui *ui)
{
    nd_softkey softkey;
    nd_msgdialog warningmsg;
    int32_t screen_w;
    int32_t content_bottom;
    int32_t x = 0;
    int32_t y = 0;

    if (ui == NULL || ui->draw == NULL)
        return 1;

    screen_w = nd_ui_width(ui);
    content_bottom = nd_ui_content_bottom(ui);

    /* Clear screen. nd_rect is inclusive of both corners, so this is Pillow's
     * (0, 0, 240, 145) unchanged: one column off the right edge and one row
     * under the softkey strip, which the strip repaints immediately. */
    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, screen_w, content_bottom), ND_BLACK);

    /* Opaque: the core's own transparent bar already exists by the time any
     * app runs, so framework.py's hasattr check has already decided. */
    nd_softkey_init(&softkey, ui, false);
    nd_softkey_update(&softkey, nd_testsapp_softkey, true); /* frame 1 */

    nd_msgdialog_init(&warningmsg, ui, nd_testsapp_message);

    /* Draw Hello World centered */
    nd_testsapp_greeting_pos(ui, &x, &y);
    (void)nd_draw_text(ui->draw, x, y, nd_testsapp_greeting, ui->font_xl, ND_WHITE);

    if (nd_ui_present(ui) != ND_OK) /* frame 2 */
        return 0;

    for (;;) {
        int32_t key;

        (void)nd_msgdialog_show(&warningmsg); /* frame 3, and every one after */
        /* Wait for a key */
        key = nd_ui_wait_for_key(ui);
        /* BACK / MENU / ENTER exits app */
        if (nd_testsapp_is_exit_key(key))
            return 0;

        /* Not in the Python, which had IncomingCall to unwind it. nd_app.h:
         * a loop that outlives a frame polls this, so a call arriving while
         * this screen is up is not waiting on a user who has walked away. */
        if (nd_app_should_exit())
            return 0;
    }
}

/* Nothing held: no file, no child, no sound card. The symbol exists because
 * nd_app.h requires every app to export one, so a missing one always means
 * the author forgot rather than that there was nothing to do. */
void app_shutdown(void) {}

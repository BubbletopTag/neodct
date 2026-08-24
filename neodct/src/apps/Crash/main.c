/* apps/Crash/main.c -- the app whose only feature is dying, app id 9997.
 *
 * A one-to-one port of System/engineering/apps/Crash/main.py (17 lines). It
 * exists so the crash screen, the crash log and -- since the rewrite -- the
 * process boundary can be exercised by hand on a real phone, from the
 * engineering menu, without a debugger and without a test harness.
 *
 * ============ WHY THE RAISE BECOMES abort() ============
 *
 * The Python raises RuntimeError, and CrashHandler catches it, prints the
 * traceback and draws the crash screen from the exception. C has no
 * exception, and nd_app.h moved the whole mechanism across the process
 * boundary: the core sees the child die in waitpid(), learns the signal, and
 * draws THE SAME SCREEN with a summary built from the signal instead of from
 * RuntimeError. So the app's job is to die in a way the child's handler
 * reports.
 *
 * abort(), not a null dereference, out of the two spec-engineering.md section
 * 2.1 offers. Both reach the crash screen -- test/apps/CrashApp does one of
 * each and test_proc.c watches both -- but a null store is undefined
 * behaviour that the optimiser is entitled to delete or fold into a trap, so
 * keeping it honest costs a `volatile` and a no_sanitize attribute (see
 * test/apps/CrashApp/main.c). abort() needs neither, is a defined way to end
 * a process, and puts SIGABRT rather than a suppressed UBSan finding in the
 * ASan log. The screen is the same either way.
 *
 * ============ WHAT IS LOST, SAID PLAINLY ============
 *
 * "Intentional crash from Crash app (test)" is load-bearing in the Python:
 * spec-engineering.md section 2 says it is what the crash screen's summary
 * strip prints. It CANNOT reach the C screen -- the summary is built by the
 * core from a fixed binary report the child's signal handler wrote with
 * async-signal-safe calls only (nd_crash.h, OPEN-QUESTIONS X-8), and there is
 * no field in it for an app's own words. What the screen shows instead is
 * "SIGABRT in run at <addr> (si_code -6)".
 *
 * The string is not simply dropped, because Python printed it to the console
 * too, as the last line of the traceback. The C prints it there itself, from
 * ordinary code, immediately before the abort. Same words, same console, one
 * of the two places they used to appear. This is the one deliberate deviation
 * in the file.
 *
 * ============ THE SOFTKEY IS BUILT ONCE, THE LIST EVERY PASS ============
 *
 * main.py constructs SoftKeyBar OUTSIDE the loop and VerticalList INSIDE it,
 * so the cursor resets to 0 on every pass -- the same split Power has and for
 * the same non-reason. With one item it is invisible; it is kept because
 * "one-to-one" is not conditional on being able to see the difference.
 *
 * And the order within a pass is load-bearing: softkey.update(..., present=
 * False) runs BEFORE menu.show(), whose draw() clears rows 0..145 only, so
 * the "Select" bar survives into the frame the list presents. Swapping the
 * two lines loses the softkey. nd_widgets.h rule 1.
 */

#include <stdlib.h>

#include "nd_app.h"
#include "nd_log.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#include "crash.h"

const char *const nd_crashapp_title = "Crash";
const char *const nd_crashapp_menu[ND_CRASHAPP_MENU_ITEMS] = {"CRASH!"};
const char *const nd_crashapp_softkey = "Select";
const char *const nd_crashapp_message = "Intentional crash from Crash app (test)";

/* The app tag, spelled here rather than in nd_log.h: the header's named
 * palette is for the core's subsystems and ND_LOG_CRASH ("CRASH", red) is the
 * core crash handler's own tag. An unregistered tag gets a derived colour
 * that is stable from its first boot, which is exactly what nd_log.h says
 * that mechanism is for. */
#define CRASH_LOG_TAG "Crash"

void nd_crashapp_fault(void)
{
    /* See the header comment: this is the console half of what
     * `raise RuntimeError(...)` used to print. nd_log_err() flushes, so the
     * line is on the wire before the process stops existing. */
    nd_log_err(CRASH_LOG_TAG, "%s", nd_crashapp_message);
    abort();
}

int app_run(nd_ui *ui)
{
    nd_softkey softkey;

    if (ui == NULL)
        return 1;

    /* SoftKeyBar(ui), once, outside the loop. Opaque: the core's own bar
     * already exists by the time any app runs, so framework.py's hasattr
     * check has decided. See nd_ui.h. */
    nd_softkey_init(&softkey, ui, false);

    for (;;) {
        nd_vlist menu;
        int32_t choice;

        nd_vlist_init(&menu, ui, nd_crashapp_title, nd_crashapp_menu, ND_CRASHAPP_MENU_ITEMS,
                      ND_CRASHAPP_ID);
        nd_softkey_update(&softkey, nd_crashapp_softkey, false);

        choice = nd_vlist_show(&menu);

        if (choice == ND_WIDGET_BACK)
            return 0;
        if (choice == 0)
            nd_crashapp_fault(); /* does not return */

        /* Not in the Python, which had exceptions to unwind it. nd_app.h: a
         * loop that outlives a frame polls this. Unreachable in practice --
         * the list has one item and both of its answers are handled above --
         * and kept so the shape matches every other app's loop. */
        if (nd_app_should_exit())
            return 0;
    }
}

/* Nothing held: no sound card, no child, no file. Exported anyway, because
 * nd_app.h requires it so a missing symbol always means the author forgot.
 * It is also never reached on the interesting path -- SIGABRT kills this
 * process outright and nd-apprun never gets to run teardown. */
void app_shutdown(void) {}

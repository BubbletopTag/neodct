/* crash.h -- the parts of apps/Crash/main.c a unit test can reach.
 *
 * There is very little of it: seventeen lines of Python, one menu item and a
 * deliberate fault. The id and the four strings are pulled out so
 * test/unit/test_crash.c can check them against the Python directly rather
 * than only through a rendered frame, and the fault is a NAMED FUNCTION so
 * the test can call it in a forked child and watch the signal arrive.
 * Calling it in-process would take the test binary down with it.
 */

#ifndef ND_CRASHAPP_H_INCLUDED
#define ND_CRASHAPP_H_INCLUDED

#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* APP_ID = 9997 -- manifest.json, and the menu VerticalList's app_id. */
#define ND_CRASHAPP_ID 9997

/* VerticalList(ui, "Crash", ["CRASH!"], app_id=APP_ID). One item, and the
 * whole app is the one item. */
#define ND_CRASHAPP_MENU_ITEMS 1
extern const char *const nd_crashapp_title;
extern const char *const nd_crashapp_menu[ND_CRASHAPP_MENU_ITEMS];

/* softkey.update("Select", present=False), before menu.show() -- see the
 * ordering note in main.c. */
extern const char *const nd_crashapp_softkey;

/* RuntimeError("Intentional crash from Crash app (test)"). In Python this
 * string reaches BOTH the console (CrashHandler prints the traceback) and the
 * crash screen's summary strip. C keeps the console half; see main.c. */
extern const char *const nd_crashapp_message;

/* raise RuntimeError(...). DOES NOT RETURN: it abort()s, so the caller is
 * killed by SIGABRT and nd-apprun's handler reports it down the crash pipe.
 * Never call this from a process you want to keep. */
void nd_crashapp_fault(void);

#ifdef __cplusplus
}
#endif

#endif /* ND_CRASHAPP_H_INCLUDED */

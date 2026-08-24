/* test/apps/CrashApp/main.c -- an app whose entire job is to die.
 *
 * The process-per-app design exists so that a null dereference in an app kills
 * the app and NOT the phone. That claim is worth nothing until something has
 * actually dereferenced null and the core has been seen to survive it, so this
 * does, and test/unit/test_proc.c watches.
 *
 * It lives under test/apps/ rather than apps/ so that `make install` cannot
 * ship it: the Makefile installs the app.so under each apps/ directory and
 * never looks here.
 *
 * app_run() picks its failure from argv[3], which arrives as the entry
 * argument, so the one .so covers every classification the core has to make:
 *
 *   "segv"    dereference NULL             -> WIFSIGNALED, SIGSEGV
 *   "abort"   abort()                      -> WIFSIGNALED, SIGABRT
 *   "status"  return 3                     -> WIFEXITED, non-zero
 *   anything else (including no argument)  -> return 0, a clean exit
 */

#include <stdlib.h>
#include <string.h>

#include "nd_app.h"
#include "nd_types.h"
#include "nd_ui.h"

/* volatile so the optimiser cannot decide the store is unreachable and delete
 * the whole function -- with -O2 a plain *(int *)0 = 1 is folded to a trap
 * instruction on some targets and to nothing at all on others. */
static volatile int *const g_null = NULL;

static int g_mode; /* set by app_open_message, read by app_run */

int app_run(nd_ui *ui)
{
    ND_UNUSED(ui);

    switch (g_mode) {
    case 1:
        *g_null = 1; /* the point of the whole exercise */
        return 0;
    case 2:
        abort();
    case 3:
        return 3;
    default:
        return 0;
    }
}

/* The mode arrives through the one entry point that takes an argument, so the
 * launcher's argv plumbing is exercised at the same time. */
int app_open_message(nd_ui *ui, int64_t message_id)
{
    g_mode = (int)message_id;
    return app_run(ui);
}

int app_open_inbox(nd_ui *ui)
{
    return app_run(ui);
}

void app_shutdown(void)
{
}

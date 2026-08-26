/* apps/Koki/app.c -- the app contract: app_run() and app_shutdown().
 *
 * A one-to-one port of System/apps/Koki/main.py (43 lines). App id 10,
 * "Koki Mobile", stock menu.
 *
 * ============ WHAT main.py DID THAT THIS DOES NOT NEED TO ============
 *
 * The Python's run() spends most of its 43 lines purging `engine` and `game`
 * from sys.modules before AND after the run, dropping the Engine reference by
 * hand and calling gc.collect(). Its own comment says why: "on the 32MB
 * Luckfox, stale engines/caches surviving in sys.modules make the whole OS
 * crawl". Every launch re-imported the modules into the CORE's address space
 * and every launch left something behind.
 *
 * Under the new architecture that whole problem is gone: nd_app.h makes an
 * app a separate process, so process exit returns the manifest, the three
 * caches, the 45 sprites and the decoded costumes to the kernel with no
 * bookkeeping at all. What survives from main.py is the requirement that
 * teardown STILL RUNS -- the audio device has to be released whether the game
 * ended normally, quit through the pause menu, or was cut short by SIGTERM --
 * and that is what app_shutdown() is for.
 *
 * ============ THE CONTROLS, FROM main.py's HEADER ============
 *
 *   left/right or 4/6 .. walk        up or 2 ......... enter door / plane up
 *   z or 5 or * ........ jump/boost  down or 8 ....... plane down
 *   x or 0 or # ........ action      Enter/navi ...... start / confirm
 *   C (backspace) ...... pause/quit
 */

#include <stdio.h>
#include <stdlib.h>

#include "nd_app.h"
#include "nd_log.h"
#include "nd_types.h"
#include "nd_ui.h"

#include "koki.h"

/* The engine the running app owns, so that app_shutdown() -- which is called
 * from ordinary code after SIGTERM, never from the handler itself -- can
 * release the audio device. One process, one game. */
static koki_engine *g_eng;

int app_run(nd_ui *ui)
{
    const char *dir = nd_app_dir();
    int rc;

    if (ui == NULL)
        return 1;
    /* nd_app_dir() is never NULL inside an app process, and is "" inside the
     * core. Falling back to the installed path keeps a hand-run
     * `nd-apprun` and a test harness both working. */
    if (dir == NULL || dir[0] == '\0')
        dir = "/NeoDCT/System/apps/Koki";

    g_eng = koki_engine_new(ui, dir);
    if (g_eng == NULL) {
        nd_log_err(ND_LOG_KOKI, "cannot start: assets missing under %s", dir);
        return 1;
    }

    koki_register_all(g_eng);
    rc = koki_engine_run(g_eng);

    koki_engine_free(g_eng);
    g_eng = NULL;
    return rc;
}

void app_shutdown(void)
{
    /* SIGTERM arrived mid-frame: an incoming call is about to ring and the
     * sound card must be free before it does. Teardown is idempotent, so the
     * normal path calling koki_engine_free() first costs nothing here.
     * Must not draw, must not allocate, must not block (nd_app.h). */
    if (g_eng != NULL)
        koki_engine_teardown(g_eng);
}

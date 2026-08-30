/* nd_apprun.c -- the other side of the process boundary.
 *
 *     nd-apprun <app-dir> [entry] [arg]
 *
 * The core never loads an app. It fork()s and immediately execve()s this
 * program, which dlopen()s <app-dir>/app.so, resolves ONE entry point, calls
 * it once, and exits with its return value. The kernel enforces the boundary
 * in hardware, so a null dereference in an app kills the app and nothing else
 * -- which is the entire point of the design and the thing the Python could
 * not do, because an app was `exec_module`d straight into the core.
 *
 * ============ WHAT THIS PROCESS SETS UP, IN ORDER ============
 *
 *   1. the crash handlers, FIRST, so a fault while loading the .so is still
 *      reported down the pipe rather than vanishing into a bare signal;
 *   2. the SIGTERM handler, without SA_RESTART -- see the teardown contract
 *      in nd_app.h. A blocked read() must return EINTR, not resume;
 *   3. the app's directory, so nd_app_asset_path() can answer;
 *   4. the inherited framebuffer (NEODCT_FB_FD) and key channel
 *      (NEODCT_KEYPAD_FD), neither of which needs a device permission;
 *   5. nd_ui_init_app(), which is nd_ui_init() minus the modem, the battery
 *      and the notify service -- those live in the core and are NULL here.
 *
 * ============ WHY IT _exit()s ============
 *
 * app_shutdown() has already released the sound card and killed whatever the
 * app spawned. Running atexit handlers and flushing stdio a second time after
 * that buys nothing and, on the SIGTERM path, happens while the phone is
 * ringing. The UI teardown is still done by hand first, so a leak detector
 * stays useful (CODING-STANDARDS.md 1.7).
 */

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_bench.h"
#include "nd_crash.h"
#include "nd_fb.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_ui.h"

typedef int (*app_run_fn)(nd_ui *ui);
typedef int (*app_open_message_fn)(nd_ui *ui, int64_t message_id);
typedef int (*app_open_inbox_fn)(nd_ui *ui);
typedef int (*app_open_event_fn)(nd_ui *ui, int64_t event_id);
typedef void (*app_shutdown_fn)(void);

static int env_fd(const char *name)
{
    const char *s = getenv(name);
    char *end = NULL;
    long v;

    if (s == NULL || s[0] == '\0')
        return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0 || v > 1000000L)
        return -1;
    return (int)v;
}

static void usage(void)
{
    (void)fprintf(stderr, "usage: nd-apprun <app-dir> [entry] [arg]\n"
                          "  entry is one of: run, open_message, open_inbox, open_event\n");
}

int main(int argc, char **argv)
{
    const char *app_dir;
    const char *entry;
    const char *arg;
    char so_path[ND_PATH_MAX];
    char resolved[ND_PATH_MAX];
    void *handle = NULL;
    app_shutdown_fn shutdown_fn = NULL;
    nd_fb *fb = NULL;
    nd_ui ui;
    int keypad_fd;
    int crash_fd;
    int rc = 0;

    if (argc < 2) {
        usage();
        return 2;
    }
    app_dir = argv[1];
    entry = argc >= 3 && argv[2][0] != '\0' ? argv[2] : ND_APP_ENTRY_RUN;
    arg = argc >= 4 ? argv[3] : NULL;

    crash_fd = env_fd(ND_ENV_CRASH_FD);
    keypad_fd = env_fd(ND_ENV_KEYPAD_FD);

    /* Before anything that can fault, including dlopen. */
    (void)nd_crash_install_child(crash_fd);
    nd_crash_set_entry(entry);
    (void)nd_app_install_signal_handlers();
    (void)nd_app_set_dir(app_dir);

    /* so_path is what the log says; `resolved` is what dlopen gets. They differ
     * only under a test root, and nd_path_join() is the one that resolves --
     * calling nd_path_resolve() on its output would prefix ND_ROOT twice. */
    if (nd_snprintf(so_path, sizeof so_path, "%s/%s", app_dir, ND_APP_SO_NAME) != ND_OK ||
        nd_path_join(resolved, sizeof resolved, app_dir, ND_APP_SO_NAME) != ND_OK) {
        nd_log_err(ND_LOG_OS, "App load failed: path too long under %s", app_dir);
        return 1;
    }

    nd_bench_mark("apprun: exec + link");
    handle = dlopen(resolved, RTLD_NOW | RTLD_LOCAL);
    nd_bench_mark("apprun: dlopen(app.so)");
    if (handle == NULL) {
        nd_log_err(ND_LOG_OS, "App load failed: %s", dlerror());
        return 1;
    }

    /* app_shutdown is MANDATORY. An app with nothing to release still exports
     * an empty one, so that "missing" always means "the author forgot" and
     * never "nothing to do" -- nd_app.h is explicit, and an app that holds the
     * sound card through a ringtone is the failure this catches. */
    shutdown_fn = (app_shutdown_fn)(uintptr_t)dlsym(handle, ND_APP_SYM_SHUTDOWN);
    if (shutdown_fn == NULL)
        nd_log_err(ND_LOG_OS, "%s exports no %s(); teardown will not run", app_dir,
                   ND_APP_SYM_SHUTDOWN);

    if (nd_app_fb_from_env(&fb) != ND_OK)
        fb = NULL; /* headless: the widgets still render, nothing is presented */

    /* The key channel is opened by nd_ui_init_app(), which takes ownership of
     * the descriptor -- opening it here as well would close it twice. */
    nd_bench_mark("apprun: framebuffer");
    if (nd_ui_init_app(&ui, fb, keypad_fd) != ND_OK) {
        nd_log_err(ND_LOG_OS, "App load failed: no UI context for %s", app_dir);
        rc = 1;
        goto out;
    }
    if (strcmp(entry, ND_APP_ENTRY_OPEN_MESSAGE) == 0) {
        app_open_message_fn fn =
            (app_open_message_fn)(uintptr_t)dlsym(handle, ND_APP_SYM_OPEN_MESSAGE);

        if (fn == NULL) {
            nd_log_err(ND_LOG_OS, "App has no %s(ui): %s", ND_APP_SYM_OPEN_MESSAGE, app_dir);
            rc = 1;
        } else {
            rc = fn(&ui, arg != NULL ? (int64_t)strtoll(arg, NULL, 10) : 0);
        }
    } else if (strcmp(entry, ND_APP_ENTRY_OPEN_INBOX) == 0) {
        app_open_inbox_fn fn = (app_open_inbox_fn)(uintptr_t)dlsym(handle, ND_APP_SYM_OPEN_INBOX);

        if (fn == NULL) {
            nd_log_err(ND_LOG_OS, "App has no %s(ui): %s", ND_APP_SYM_OPEN_INBOX, app_dir);
            rc = 1;
        } else {
            rc = fn(&ui);
        }
    } else if (strcmp(entry, ND_APP_ENTRY_OPEN_EVENT) == 0) {
        app_open_event_fn fn = (app_open_event_fn)(uintptr_t)dlsym(handle, ND_APP_SYM_OPEN_EVENT);

        if (fn == NULL) {
            nd_log_err(ND_LOG_OS, "App has no %s(ui): %s", ND_APP_SYM_OPEN_EVENT, app_dir);
            rc = 1;
        } else {
            rc = fn(&ui, arg != NULL ? (int64_t)strtoll(arg, NULL, 10) : 0);
        }
    } else {
        app_run_fn fn = (app_run_fn)(uintptr_t)dlsym(handle, ND_APP_SYM_RUN);

        if (fn == NULL) {
            /* The Python's "[OS] App has no run(ui): <path>", one for one. */
            nd_log_err(ND_LOG_OS, "App has no run(ui): %s", so_path);
            rc = 1;
        } else {
            rc = fn(&ui);
        }
    }

    nd_bench_mark("apprun: app returned");
    nd_ui_teardown(&ui);
    nd_bench_mark("apprun: ui teardown");

out:
    /* Step 3 of the teardown contract: app_shutdown() runs from ORDINARY code,
     * never from the signal handler, whether we are leaving because the app
     * returned or because SIGTERM arrived and it noticed. */
    if (shutdown_fn != NULL)
        shutdown_fn();

    if (fb != NULL)
        nd_fb_close(fb);
    /* Not dlclose()d: the app's static destructors would run after its own
     * teardown, and on the SIGTERM path the phone is already ringing. The
     * process is one _exit() away from returning every page anyway. */

    (void)fflush(NULL);
    _exit(rc);
}

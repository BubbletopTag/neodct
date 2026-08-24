/* test_crash.c -- the Crash app, app id 9997.
 *
 * The app is seventeen lines of Python and it has exactly two outcomes: Back
 * returns to the menu, and Select kills the process. Both are checked, and
 * the second one is the reason this file is shaped the way it is.
 *
 * ============ WHY THERE IS A fork() IN A UNIT TEST ============
 *
 * nd_crashapp_fault() abort()s. Calling it here would take the test binary
 * down with it and every check after it would be lost, so the two tests that
 * reach it run it in a FORKED CHILD and watch waitpid() -- which is also the
 * only way to assert the thing that actually matters: that the process dies
 * of a SIGNAL, not with a return code, because a signal is what nd-apprun's
 * handler reports down the crash pipe and what the core turns into the crash
 * screen. test_proc.c proves the rest of that chain with its own faulting
 * app; this proves that THIS app enters it.
 *
 * The child gets an alarm(10) so that a fault which somehow does not happen
 * fails the check instead of hanging the suite.
 *
 * ============ WHAT IS NOT CHECKED, AND WHY ============
 *
 * No golden frame. shoot_docs.py never launches this app and there is no
 * "eng-crash" in tests/golden/manifest.json, so the menu's pixels are pinned
 * only indirectly, by widget-verticallist.png and widget-softkeybar.png --
 * this app makes the same two calls PhoneBook, Tones and Power make.
 *
 * The message string cannot be checked on the crash screen, because it never
 * reaches it: the C summary is built from the signal (apps/Crash/main.c has
 * the full reasoning). It is checked as a string, and its journey to the
 * serial console is by inspection only.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "smallapp_test.h"

#include "../../apps/Crash/crash.h"

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    void (*fault)(void);
    const char *const *title;
    const char *const *menu;
    const char *const *softkey;
    const char *const *message;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&api.fault = sa_sym(h, "nd_crashapp_fault");
    api.title = dlsym(h, "nd_crashapp_title");
    api.menu = dlsym(h, "nd_crashapp_menu");
    api.softkey = dlsym(h, "nd_crashapp_softkey");
    api.message = dlsym(h, "nd_crashapp_message");

    return api.run != NULL && api.shutdown != NULL && api.fault != NULL && api.title != NULL &&
           api.menu != NULL && api.softkey != NULL && api.message != NULL;
}

/* ------------------------------------------------------------------ *
 * 1. The strings and the id
 * ------------------------------------------------------------------ */

static void test_strings(void)
{
    /* VerticalList(ui, "Crash", ["CRASH!"], app_id=APP_ID). The exclamation
     * mark is the Python's and the only item is the only item. */
    CHECK_STR(*api.title, "Crash", "VerticalList title");
    CHECK_STR(api.menu[0], "CRASH!", "the one menu item");
    CHECK_INT(ND_CRASHAPP_MENU_ITEMS, 1, "one item");

    /* softkey.update("Select", present=False) */
    CHECK_STR(*api.softkey, "Select", "the softkey label");

    /* APP_ID = 9997, and manifest.json says the same. */
    CHECK_INT(ND_CRASHAPP_ID, 9997, "APP_ID");

    /* RuntimeError("Intentional crash from Crash app (test)") */
    CHECK_STR(*api.message, "Intentional crash from Crash app (test)", "the raised message");
}

/* ------------------------------------------------------------------ *
 * 2. Back
 * ------------------------------------------------------------------ */

static void test_back_leaves(void)
{
    sa_fixture fx;
    int rc;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    /* VerticalList does NOT flush before its first draw, so a queued press is
     * enough -- and Back is the one answer that does not end the process. */
    CHECK(sa_send(&fx, ND_KEY_CLEAR), "queued Back");

    nd_vclock_enable();
    rc = api.run(&fx.ui);
    nd_vclock_disable();

    /* `if choice == -1: return` -- and a normal return is 0, which is what
     * stops the core showing a crash screen for it. */
    CHECK_INT(rc, 0, "Back returns 0");
    /* One frame: the list's own draw. The softkey before it is present=False
     * and clears rows 145..175 only, which the list's partial clear then
     * leaves alone. */
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 1, "one frame: the menu");
    sa_fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 3 and 4. The fault
 * ------------------------------------------------------------------ */

/* Runs `body` in a child and returns its wait status, or -1 if it could not
 * be run at all. */
static int status_of_child(void (*body)(void *), void *arg)
{
    pid_t pid;
    int status = 0;

    (void)fflush(NULL); /* so the child does not re-flush our stdout */
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* A fault that does not happen must fail a check, not hang the
         * suite. */
        (void)alarm(10);
        body(arg);
        /* Only reached if nothing died. */
        _exit(0);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return status;
}

static void call_fault(void *arg)
{
    ND_UNUSED(arg);
    api.fault();
}

static void run_app(void *arg)
{
    sa_fixture *fx = (sa_fixture *)arg;

    _exit(api.run(&fx->ui));
}

static void check_died_of_sigabrt(int status, const char *what)
{
    char msg[128];

    if (status < 0) {
        CHECK(false, "fork");
        return;
    }
    (void)snprintf(msg, sizeof msg, "%s kills the process with a signal", what);
    CHECK(WIFSIGNALED(status) != 0, msg);
    if (WIFSIGNALED(status) == 0) {
        /* Say what it did instead: "exited 0" and "exited 3" are different
         * bugs and only the number tells you which. */
        (void)fprintf(stderr, "  (it exited %d instead)\n",
                      WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return;
    }
    (void)snprintf(msg, sizeof msg, "%s raises SIGABRT", what);
    CHECK_INT(WTERMSIG(status), SIGABRT, msg);
}

static void test_fault_aborts(void)
{
    check_died_of_sigabrt(status_of_child(call_fault, NULL), "nd_crashapp_fault()");
}

static void test_select_crashes(void)
{
    sa_fixture fx;
    int status;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    /* `if choice == 0: raise`. ENTER on a one-item list is index 0. */
    CHECK(sa_send(&fx, ND_KEY_ENTER), "queued Select");

    nd_vclock_enable();
    status = status_of_child(run_app, &fx);
    nd_vclock_disable();

    check_died_of_sigabrt(status, "Select");
    sa_fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 5. The contract
 * ------------------------------------------------------------------ */

static void test_null_safety(void)
{
    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    /* Mandatory, and must be safe with nothing held. It is also never reached
     * on the interesting path: SIGABRT kills the process outright. */
    api.shutdown();
    sa_checks++;
}

int main(void)
{
    void *h = sa_begin("Crash", "ndcrash");

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }

    RUN(test_strings);
    RUN(test_back_leaves);
    RUN(test_fault_aborts);
    RUN(test_select_crashes);
    RUN(test_null_safety);

    return sa_end(h, "test_crash");
}

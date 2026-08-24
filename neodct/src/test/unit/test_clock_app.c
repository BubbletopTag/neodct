/* test_clock_app.c -- the Clock app, app id 8.
 *
 * NOT test_clock.c: that one is the ClockService (lib/nd_clock.c), which sets
 * the machine's time. This is System/apps/Clock/main.py, eighteen lines that
 * put "This application has not been implemented yet." on the screen.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. The string is the one golden/app-clock.png and
 *     golden/widget-messagedialog.png are both a rendering of. A byte of it
 *     is two reference frames.
 *
 *  2. The exit set is (46, 28, 50) -- C, ENTER and MENU -- and nothing else.
 *     14 is NOT in it: Clear leaves the MessageDialog, not the app.
 *
 *  3. TWO KEYS LEAVE THIS SCREEN, NOT ONE. `warningmsg.show()` returns the
 *     key that dismissed it and the Python throws it away, so the loop's own
 *     wait_for_key() needs a second press. Press ENTER once and the dialog is
 *     simply redrawn. That is what the phone does today; OPEN-QUESTIONS.md
 *     X-18 recorded that the stub did not reproduce it, and this closes it.
 *
 *  4. THE GOLDEN FRAME. app-clock is byte-identical to
 *     widget-messagedialog (spec-build-test.md section 3.6) and this app is
 *     what nd-shoot now draws it with, so it is judged here by the same
 *     SHA-256 goldenframe.py compares.
 *
 *     The extra `ui.fb.update(ui.canvas)` on line 10 of the Python commits a
 *     frame BEFORE the dialog. shoot_docs.py saves frames[-1], so it is
 *     captured and discarded -- but it is committed, and the count is
 *     checked here so that a "tidy-up" removing it is a failing test rather
 *     than a silent change to what the virtual clock has ticked.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smallapp_test.h"

#include "../../apps/Clock/clock_app.h"

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    bool (*is_exit_key)(int32_t);
    const char *const *message;
    const int32_t *exit_keys;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&api.is_exit_key = sa_sym(h, "nd_clock_app_is_exit_key");
    api.message = dlsym(h, "nd_clock_app_message");
    api.exit_keys = dlsym(h, "nd_clock_app_exit_keys");

    return api.run != NULL && api.shutdown != NULL && api.is_exit_key != NULL &&
           api.message != NULL && api.exit_keys != NULL;
}

/* ------------------------------------------------------------------ *
 * 1 and 2. The two constants
 * ------------------------------------------------------------------ */

static void test_message(void)
{
    CHECK_STR(*api.message, "This application has not been implemented yet.",
              "the string two golden frames are made of");
}

static void test_exit_keys(void)
{
    int32_t code;
    int n = 0;

    CHECK_INT(api.exit_keys[0], 46, "46 (C)");
    CHECK_INT(api.exit_keys[1], 28, "28 (ENTER)");
    CHECK_INT(api.exit_keys[2], 50, "50 (MENU)");

    CHECK(api.is_exit_key(46), "46 exits");
    CHECK(api.is_exit_key(28), "28 exits");
    CHECK(api.is_exit_key(50), "50 exits");
    CHECK(!api.is_exit_key(14), "14 does NOT exit -- it dismisses the dialog only");
    CHECK(!api.is_exit_key(ND_KEY_NONE), "ND_KEY_NONE does not exit");

    for (code = -5; code < 256; code++) {
        if (api.is_exit_key(code))
            n++;
    }
    CHECK_INT(n, 3, "exactly three exit keys");
}

/* ------------------------------------------------------------------ *
 * 3 and 4. Running it
 * ------------------------------------------------------------------ */

/* The warning triangle lives at an absolute /NeoDCT/System/... path, so ND_ROOT
 * has to point at the overlay for the dialog to look like the reference.
 * READ ONLY: nothing in this file writes with this root set. */
static char saved_root[ND_PATH_MAX];

static bool root_to_overlay(void)
{
    char overlay[ND_PATH_MAX];

    (void)nd_strlcpy(saved_root, nd_path_root(), sizeof saved_root);
    if (!sa_overlay_root(overlay, sizeof overlay))
        return false;
    return nd_path_set_root(overlay) == ND_OK;
}

static void root_restore(void)
{
    (void)nd_path_set_root(saved_root[0] != '\0' ? saved_root : NULL);
}

static void test_golden_frame_and_two_keys(void)
{
    sa_fixture fx;
    int rc;

    if (!root_to_overlay()) {
        CHECK(false, "found the overlay for the warning icon");
        return;
    }
    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        root_restore();
        return;
    }

    /* MessageDialog drains the channel before its first draw, so a queued
     * press would be eaten. Holding ENTER survives the drain and arrives as
     * repeats afterwards -- once for show(), once for the loop. */
    if (!sa_hold(&fx, ND_KEY_ENTER)) {
        CHECK(false, "held key");
        sa_fx_free(&fx);
        root_restore();
        return;
    }

    nd_vclock_enable();
    rc = api.run(&fx.ui);

    CHECK_INT(rc, 0, "app_run returns 0 on ENTER");
    /* The Python's line-10 flush, then the dialog. If either goes, this is
     * the test that says so. */
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 2, "the pre-dialog flush AND the dialog");
    CHECK_INT(nd_vclock_frame(), 2, "the virtual clock ticked once per committed frame");

    sa_expect_golden(&fx, nd_capture_recent(fx.cap, 0u), "app-clock");

    nd_vclock_disable();
    sa_fx_free(&fx);
    root_restore();
}

/* One press is NOT enough: show() eats it and the loop asks again. Driven
 * with a queued press rather than a held one, so the channel really does run
 * dry -- and then app_run must still be inside its loop, which is checked by
 * it never returning within the frames it would need to. */
static void test_one_key_is_not_enough(void)
{
    sa_fixture fx;
    int rc;

    if (!root_to_overlay()) {
        CHECK(false, "found the overlay");
        return;
    }
    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        root_restore();
        return;
    }

    /* Hold C (46) instead of ENTER. MessageDialog's accept set is (28,) and
     * its cancel set is (14,), so 46 is ignored INSIDE the dialog and the
     * dialog never returns -- which is the other half of the same quirk: the
     * only keys that reach the app's own loop are the two the dialog itself
     * answers to. Give it CLEAR, which the dialog treats as cancel, and the
     * loop then sees the NEXT repeat, which is also CLEAR, which is not an
     * exit key -- so the dialog is redrawn instead of the app returning.
     *
     * The budget is what ends the run: three frames is the flush, the first
     * dialog and the redraw, and the fourth is refused. */
    if (!sa_hold(&fx, ND_KEY_CLEAR)) {
        CHECK(false, "held key");
        sa_fx_free(&fx);
        root_restore();
        return;
    }

    nd_vclock_enable();
    nd_capture_set_budget(fx.cap, 3);
    rc = api.run(&fx.ui);
    CHECK_INT(rc, 0, "app_run still returns 0 when it is stopped by the budget");
    CHECK(nd_capture_exhausted(fx.cap), "CLEAR redraws the dialog instead of leaving");
    nd_capture_clear_budget(fx.cap);

    nd_vclock_disable();
    sa_fx_free(&fx);
    root_restore();
}

static void test_null_safety(void)
{
    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown(); /* must be safe with nothing held */
    sa_checks++;
}

int main(void)
{
    void *h = sa_begin("Clock", "ndclock");

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }

    RUN(test_message);
    RUN(test_exit_keys);
    RUN(test_golden_frame_and_two_keys);
    RUN(test_one_key_is_not_enough);
    RUN(test_null_safety);

    return sa_end(h, "test_clock_app");
}

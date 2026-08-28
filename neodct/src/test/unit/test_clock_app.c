/* test_clock_app.c -- the Clock app, app id 8.
 *
 * NOT test_clock.c: that one is the ClockService (lib/nd_clock.c), which sets
 * the machine's time. This is the app that asks it to.
 *
 * ============ WHAT THIS REPLACED ============
 *
 * Until 0.4.5a this file pinned an eighteen-line stub whose whole behaviour
 * was a MessageDialog reading "This application has not been implemented
 * yet." -- and, because app-clock.png and widget-messagedialog.png were
 * byte-identical, pinned a golden frame to it as well. The app is real now,
 * so the stub's string, its (46, 28, 50) exit set and its two-presses-to-leave
 * loop are all gone, and app-clock.png has been re-cut from the menu that
 * replaced them. AGENTS.md: a frame that stops matching because the screen was
 * changed on purpose is the point, not a failure.
 *
 * ============ WHAT IT CLAIMS NOW ============
 *
 *  1. The three rows, in the order the menu pages through them.
 *  2. Clear on the root menu leaves the app.
 *  3. The NTP row round-trips through the setting, and the setting is what
 *     the clock service reads -- so turning it off here really does stop the
 *     background sync rather than only changing a label.
 *  4. The guard that refuses manual entry while NTP is on follows the
 *     setting. That refusal is the one behaviour in this app that prevents a
 *     confusing failure rather than causing one.
 *  5. The SIGTERM teardown contract (nd_app.h).
 *
 * The masked fields themselves are checked in test_widgets_text.c and the
 * parsing in test_timeset.c, which is where the decisions live.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_clock.h"
#include "nd_settings.h"

#include "smallapp_test.h"

#include "../../apps/Clock/clock_app.h"

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    const char *const *title;
    const char *const *rows;
    const char *const *ntp_options;
    const char *const *ntp_is_on;
    bool (*may_set_manually)(void);
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    api.title = dlsym(h, "nd_clock_app_title");
    api.rows = dlsym(h, "nd_clock_app_rows");
    api.ntp_options = dlsym(h, "nd_clock_app_ntp_options");
    api.ntp_is_on = dlsym(h, "nd_clock_app_ntp_is_on");
    *(void **)&api.may_set_manually = sa_sym(h, "nd_clock_app_may_set_manually");

    return api.run != NULL && api.shutdown != NULL && api.title != NULL && api.rows != NULL &&
           api.ntp_options != NULL && api.ntp_is_on != NULL && api.may_set_manually != NULL;
}

/* The warning triangle and the font live at absolute /NeoDCT/System/... paths,
 * so ND_ROOT has to point at the overlay for a dialog to render.
 *
 * THE OVERLAY IS READ-ONLY HERE, and that is a rule rather than an
 * observation: nd_settings_set() writes /NeoDCT/User/settings.prop, and with
 * this root in force that file is the one in the source tree. An earlier
 * draft of this file left "system.clock.ntp_sync=ON" committed into the
 * overlay. So every case that writes a setting runs under the case root and
 * restores it first -- see settings_under_case_root(). */
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

/* ------------------------------------------------------------------ *
 * 1. The rows
 * ------------------------------------------------------------------ */

static void test_rows(void)
{
    CHECK_STR(*api.title, "Clock", "the title the header draws");
    CHECK_STR(api.rows[ND_CLOCK_ROW_ALARM], "Alarm clock", "row 1");
    CHECK_STR(api.rows[ND_CLOCK_ROW_SETTINGS], "Clock settings", "row 2");
    CHECK_STR(api.rows[ND_CLOCK_ROW_NTP], "NTP time sync", "row 3");
    CHECK_INT(ND_CLOCK_APP_ROWS, 3, "three rows");

    /* On first, so a phone that arrives synced opens on the row it is
     * already set to. */
    CHECK_STR(api.ntp_options[0], "On", "On is offered first");
    CHECK_STR(api.ntp_options[1], "Off", "Off second");
}

/* ------------------------------------------------------------------ *
 * 2. Clear leaves
 * ------------------------------------------------------------------ */

static void test_clear_leaves_the_root_menu(void)
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
    /* PagedList drains the channel before its first draw, so a queued press
     * would be eaten. Holding CLEAR survives the drain, exactly as the Tones
     * capture does. */
    if (!sa_hold(&fx, ND_KEY_CLEAR)) {
        CHECK(false, "held key");
        sa_fx_free(&fx);
        root_restore();
        return;
    }

    nd_vclock_enable();
    rc = api.run(&fx.ui);
    CHECK_INT(rc, 0, "app_run returns 0 on Clear");
    CHECK(nd_capture_frames_drawn(fx.cap) >= 1u, "the menu was drawn");
    nd_vclock_disable();

    sa_fx_free(&fx);
    root_restore();
}

/* ------------------------------------------------------------------ *
 * 3. The NTP row is the setting the clock service reads
 * ------------------------------------------------------------------ */

/* Not a UI test: the point is that the app and the service name the SAME key,
 * so a change made on that screen reaches the thing that acts on it. A test
 * that only checked the app wrote "OFF" somewhere would pass with the two
 * halves pointing at different keys. */
/* Point the settings layer at the case root before writing anything. The
 * previous test may have left ND_ROOT on the overlay. */
static void settings_under_case_root(void)
{
    root_restore();
    CHECK(nd_settings_init() == ND_OK, "settings under the case root");
}

static void test_ntp_setting_is_what_the_clock_service_reads(void)
{
    settings_under_case_root();

    CHECK_INT(nd_settings_set(ND_SET_CLOCK_NTP, "OFF"), ND_OK, "write OFF");
    CHECK(!nd_clock_ntp_enabled(), "the clock service sees OFF");

    CHECK_INT(nd_settings_set(ND_SET_CLOCK_NTP, "ON"), ND_OK, "write ON");
    CHECK(nd_clock_ntp_enabled(), "the clock service sees ON");

    /* Absent means on. A phone that lost its settings should come back with a
     * clock that fixes itself rather than one that quietly stopped syncing. */
    CHECK_INT(nd_settings_set(ND_SET_CLOCK_NTP, ""), ND_OK, "clear it");
    CHECK(nd_clock_ntp_enabled(), "an empty value defaults to on");
}

/* ------------------------------------------------------------------ *
 * 4. Clock settings refuses while NTP is on
 * ------------------------------------------------------------------ */

/* The guard, called the way the app calls it. What this CANNOT do is prove
 * show_clock_settings() still consults it -- PagedList drains the key channel
 * before its first draw, so a scripted "Down, Enter" never arrives and a held
 * key can only ever be one code. So this pins the decision and the header
 * says out loud that the call site is not covered. */
static void test_the_manual_set_guard_follows_the_ntp_setting(void)
{
    settings_under_case_root();
    CHECK_INT(nd_settings_set(ND_SET_CLOCK_NTP, "ON"), ND_OK, "NTP on");
    CHECK(!api.may_set_manually(), "with NTP on the clock is not ours to set");

    CHECK_INT(nd_settings_set(ND_SET_CLOCK_NTP, "OFF"), ND_OK, "NTP off");
    CHECK(api.may_set_manually(), "with NTP off it is");
}

/* ------------------------------------------------------------------ *
 * 5. Teardown
 * ------------------------------------------------------------------ */

static void test_null_safety(void)
{
    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown(); /* must be safe with nothing held */
    sa_checks++;
}

/* THE SIGTERM TEARDOWN CONTRACT (nd_app.h). The flag is raised BEFORE
 * app_run() so the test is deterministic: the first time the loop reaches its
 * poll it returns, whatever the key channel holds.
 *
 * MUST RUN LAST. There is no way to lower g_should_exit again; it is a
 * volatile sig_atomic_t with no reset, by design. */
static void test_sigterm_leaves_the_loop(void)
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
    /* ENTER on the alarm row, then ENTER to dismiss its dialog -- a script
     * that would otherwise loop for ever, which is what makes the poll the
     * only way out. */
    if (!sa_hold(&fx, ND_KEY_ENTER)) {
        CHECK(false, "held key");
        sa_fx_free(&fx);
        root_restore();
        return;
    }

    CHECK_INT(nd_app_install_signal_handlers(), ND_OK, "handlers install");
    CHECK(!nd_app_should_exit(), "not yet");
    if (kill(getpid(), SIGTERM) != 0) {
        CHECK(false, "raise SIGTERM");
        sa_fx_free(&fx);
        root_restore();
        return;
    }
    CHECK(nd_app_should_exit(), "the handler set the flag");

    rc = api.run(&fx.ui);
    CHECK_INT(rc, 0, "app_run returns rather than looping on a key that never leaves");

    sa_fx_free(&fx);
    root_restore();
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

    RUN(test_rows);
    RUN(test_clear_leaves_the_root_menu);
    RUN(test_ntp_setting_is_what_the_clock_service_reads);
    RUN(test_the_manual_set_guard_follows_the_ntp_setting);
    RUN(test_null_safety);
    /* Last: nd_app_should_exit() cannot be lowered again. */
    RUN(test_sigterm_leaves_the_loop);

    return sa_end(h, "test_clock_app");
}

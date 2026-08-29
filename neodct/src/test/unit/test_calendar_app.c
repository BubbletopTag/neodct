/* test_calendar_app.c -- the Calendar app, app id 5.
 *
 * NOT test_calendar.c: that one is the event store (lib/nd_calendar.c), which
 * the core also reads. This is the app that writes into it, dlopen()ed from
 * the BUILT app.so so that what is tested is the artefact that ships.
 *
 * ============ WHAT THIS PINS, AND WHY EACH ONE ============
 *
 *  1. The rows and the strings, in the order the screens page through them.
 *  2. The month grid's LAYOUT, as data. Six rows of seven starting on the
 *     Monday on or before the 1st is the whole geometry of the screen, and it
 *     is checkable without a panel.
 *  3. THE KEY MAP. This is the case that matters most and the one a golden
 *     frame could never have covered: the phone's keypad is sixteen keys with
 *     no left and no right (nd_keypadsetup.c's enrolment list is the whole
 *     set), and the test machine has neither that keypad nor any way to
 *     simulate a person using one. So the map is a pure function and this
 *     drives it directly -- including a case asserting that every direction
 *     is reachable WITHOUT the two keycodes the hardware does not have.
 *  4. The two label formatters.
 *  5. That C leaves the month view, and that a frame was drawn to leave.
 *  6. app_open_event() on an id that is no longer there.
 *  7. The SIGTERM teardown contract (nd_app.h).
 *
 * There is no golden frame for the month grid and there should not be one:
 * CODING-STANDARDS.md section 7 is explicit that a new screen's test is its
 * unit test and not a picture of itself that can only ever agree with it.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_calendar.h"

#include "smallapp_test.h"

#include "../../apps/Calendar/calendar_app.h"

static struct {
    int (*run)(nd_ui *);
    int (*open_event)(nd_ui *, int64_t);
    void (*shutdown)(void);
    const char *const *title;
    const char *const *options;
    const char *const *event_options;
    const char *const *new_row;
    int32_t (*grid_fill)(int32_t, int32_t, int32_t, nd_cal_cell *);
    nd_cal_nav (*month_key)(int32_t, int32_t *, int32_t *, int32_t *);
    void (*row_label)(char *, size_t, int64_t, const char *);
    void (*day_title)(char *, size_t, int32_t, int32_t, int32_t);
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.open_event = sa_sym(h, "app_open_event");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    api.title = dlsym(h, "nd_cal_app_title");
    api.options = dlsym(h, "nd_cal_app_options");
    api.event_options = dlsym(h, "nd_cal_app_event_options");
    api.new_row = dlsym(h, "nd_cal_app_new_row");
    *(void **)&api.grid_fill = sa_sym(h, "nd_cal_grid_fill");
    *(void **)&api.month_key = sa_sym(h, "nd_cal_month_key");
    *(void **)&api.row_label = sa_sym(h, "nd_cal_app_row_label");
    *(void **)&api.day_title = sa_sym(h, "nd_cal_app_day_title");

    return api.run != NULL && api.open_event != NULL && api.shutdown != NULL && api.title != NULL &&
           api.options != NULL && api.event_options != NULL && api.new_row != NULL &&
           api.grid_fill != NULL && api.month_key != NULL && api.row_label != NULL &&
           api.day_title != NULL;
}

/* The font and the warning triangle live at absolute /NeoDCT/System/... paths,
 * so ND_ROOT has to point at the overlay for a dialog to render. THE OVERLAY
 * IS READ-ONLY HERE: nothing in this file writes an event while that root is
 * in force, or it would leave a calendar.db in the source tree. test_clock_app
 * carries the same warning about settings.prop. */
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
    CHECK_STR(*api.title, "Calendar", "the title the header draws");
    CHECK_INT(ND_CAL_APP_ID, 5, "app id 5");
    CHECK_STR(ND_CAL_APP_ROOT, "5", "and the breadcrumb agrees with it");

    /* "View day" is row 0 so that NaviKey, NaviKey opens the day: two presses
     * on one key with no navigation between them. Moving it costs a press on
     * the app's most common action. */
    CHECK_STR(api.options[ND_CAL_OPT_VIEW_DAY], "View day", "options row 1");
    CHECK_STR(api.options[ND_CAL_OPT_NEW], "New event", "options row 2");
    CHECK_STR(api.options[ND_CAL_OPT_GOTO], "Go to date", "options row 3");
    CHECK_STR(api.options[ND_CAL_OPT_DELETE_ALL], "Delete all", "options row 4");

    CHECK_STR(api.event_options[ND_CAL_EVOPT_EDIT], "Edit", "event options row 1");
    CHECK_STR(api.event_options[ND_CAL_EVOPT_DELETE], "Delete", "event options row 2");

    CHECK_STR(*api.new_row, "New event", "the day list's first row");
}

/* The store's own vocabulary, checked here because these are what the three
 * pick() screens page through and a reordering would silently change what an
 * existing row means. The enum values are stored in the database. */
static void test_the_stored_vocabulary_is_ordered(void)
{
    CHECK_STR(nd_cal_kind_names[ND_CAL_KIND_REMINDER], "Reminder", "kind 0");
    CHECK_STR(nd_cal_kind_names[ND_CAL_KIND_NOTE], "Note", "kind 4");
    CHECK_STR(nd_cal_repeat_names[ND_CAL_REPEAT_NONE], "Once", "repeat 0");
    CHECK_STR(nd_cal_repeat_names[ND_CAL_REPEAT_YEARLY], "Every year", "repeat 4");
    CHECK_STR(nd_cal_alarm_names[0], "No alarm", "alarm row 0");
    CHECK_INT(nd_cal_alarm_minutes[0], ND_CAL_ALARM_OFF, "and row 0 is the off value");
}

/* ------------------------------------------------------------------ *
 * 2. The grid, as data
 * ------------------------------------------------------------------ */

/* August 2026 begins on a Saturday, so the grid opens with the five days of
 * July that share its first week and needs all six rows to reach the 31st.
 * That makes it the useful month to pin: a five-row month would not show that
 * the sixth row is filled rather than blank. */
static void test_the_grid_starts_on_the_monday_before_the_first(void)
{
    nd_cal_cell cells[ND_CAL_GRID_CELLS];
    int32_t selected;

    selected = api.grid_fill(2026, 8, 29, cells);

    /* Five leading days, because 1 August 2026 is a Saturday. */
    CHECK_INT(cells[0].month, 7, "the grid opens in July");
    CHECK_INT(cells[0].day, 27, "on Monday the 27th");
    CHECK(!cells[0].in_month, "and July is not this month");

    CHECK_INT(cells[5].day, 1, "the 1st is in the sixth cell");
    CHECK_INT(cells[5].month, 8, "and it is August");
    CHECK(cells[5].in_month, "which IS this month");

    CHECK_INT(cells[35].day, 31, "the 31st is in cell 35");
    CHECK(cells[35].in_month, "still August");

    CHECK_INT(cells[36].month, 9, "and September starts in cell 36");
    CHECK_INT(cells[36].day, 1, "on the 1st");
    CHECK(!cells[36].in_month, "greyed, because it is context and not a choice");

    CHECK_INT(selected, 33, "the 29th is cell 33");
    CHECK_INT(cells[selected].day, 29, "which holds the 29th");
}

/* Whatever the month, all 42 cells are filled, the in-month days are a
 * contiguous run, and there are exactly as many of them as the month has
 * days. A grid with a hole in it, or one that stopped early, would draw a
 * calendar somebody could not navigate. */
static void test_every_cell_is_filled_for_every_month(void)
{
    int32_t year;
    int32_t month;
    bool all_ok = true;

    for (year = 2024; year <= 2027; year++) {
        for (month = 1; month <= 12; month++) {
            nd_cal_cell cells[ND_CAL_GRID_CELLS];
            int32_t i;
            int32_t first = -1;
            int32_t last = -1;
            int32_t count = 0;

            (void)api.grid_fill(year, month, 1, cells);
            for (i = 0; i < ND_CAL_GRID_CELLS; i++) {
                if (cells[i].day < 1 || cells[i].day > 31)
                    all_ok = false;
                if (!cells[i].in_month)
                    continue;
                if (first < 0)
                    first = i;
                last = i;
                count++;
                if (cells[i].month != month || cells[i].year != year)
                    all_ok = false;
            }
            if (first < 0 || last - first + 1 != count)
                all_ok = false; /* a gap in the run */
            if (cells[first].day != 1 || cells[last].day != count)
                all_ok = false;
            /* The first cell of the grid is always a Monday. */
            if (nd_cal_weekday(cells[0].year, cells[0].month, cells[0].day) != 0)
                all_ok = false;
        }
    }
    CHECK(all_ok, "48 months lay out with no gaps and a Monday in cell 0");
}

static void test_grid_fill_refuses_a_month_that_is_not_one(void)
{
    nd_cal_cell cells[ND_CAL_GRID_CELLS];

    CHECK_INT(api.grid_fill(2026, 0, 1, cells), -1, "month 0");
    CHECK_INT(api.grid_fill(2026, 13, 1, cells), -1, "month 13");
    CHECK_INT(api.grid_fill(2026, 8, 1, NULL), -1, "a NULL array");
    /* A day the month does not have is not an error -- the grid still draws,
     * it just has no cell selected. */
    CHECK_INT(api.grid_fill(2026, 2, 31, cells), -1, "31 February selects nothing");
}

/* ------------------------------------------------------------------ *
 * 3. The key map -- the case a golden frame could not have covered
 * ------------------------------------------------------------------ */

static void expect_move(int32_t key, int32_t in_y, int32_t in_m, int32_t in_d, int32_t want_y,
                        int32_t want_m, int32_t want_d, const char *what)
{
    int32_t y = in_y;
    int32_t m = in_m;
    int32_t d = in_d;

    CHECK_INT(api.month_key(key, &y, &m, &d), ND_CAL_NAV_MOVED, what);
    CHECK_INT(y, want_y, what);
    CHECK_INT(m, want_m, what);
    CHECK_INT(d, want_d, what);
}

static void test_the_3x3_block_is_the_d_pad(void)
{
    /* 4 and 6 are one day either way -- the axis the rocker cannot give. */
    expect_move(ND_KEY_4, 2026, 8, 29, 2026, 8, 28, "4 is the previous day");
    expect_move(ND_KEY_6, 2026, 8, 29, 2026, 8, 30, "6 is the next day");

    /* 2 and 8 are one WEEK, because that is what up and down mean on a grid
     * whose rows are weeks. */
    expect_move(ND_KEY_2, 2026, 8, 29, 2026, 8, 22, "2 is a week back");
    expect_move(ND_KEY_8, 2026, 8, 29, 2026, 9, 5, "8 is a week on");

    /* 1 and 3 page months, 7 and 9 page years. */
    expect_move(ND_KEY_1, 2026, 8, 29, 2026, 7, 29, "1 is the previous month");
    expect_move(ND_KEY_3, 2026, 8, 29, 2026, 9, 29, "3 is the next month");
    expect_move(ND_KEY_7, 2026, 8, 29, 2025, 8, 29, "7 is the previous year");
    expect_move(ND_KEY_9, 2026, 8, 29, 2027, 8, 29, "9 is the next year");
}

static void test_the_rocker_and_the_outer_keys_agree_with_the_block(void)
{
    expect_move(ND_KEY_UP, 2026, 8, 29, 2026, 8, 22, "Up is 2");
    expect_move(ND_KEY_DOWN, 2026, 8, 29, 2026, 9, 5, "Down is 8");
    expect_move(ND_KEY_STAR, 2026, 8, 29, 2026, 7, 29, "* is 1");
    expect_move(ND_KEY_HASH, 2026, 8, 29, 2026, 9, 29, "# is 3");

    /* Left and Right exist only on a development QWERTY keyboard. They are
     * folded onto 4 and 6 rather than given a meaning the phone could not
     * reach. */
    expect_move(ND_KEY_LEFT, 2026, 8, 29, 2026, 8, 28, "Left is 4");
    expect_move(ND_KEY_RIGHT, 2026, 8, 29, 2026, 8, 30, "Right is 6");
}

/* THE CASE THIS FILE EXISTS FOR. Every direction the grid can move in has to
 * be reachable from the sixteen keys the 5190 actually has. If a future edit
 * moved one of them onto Left or Right the phone would silently lose it, and
 * nothing else in the suite would notice -- the development keyboard has both
 * and every other test runs on that. */
static void test_every_direction_is_reachable_without_left_or_right(void)
{
    static const int32_t HARDWARE_KEYS[] = {
        ND_KEY_ENTER, ND_KEY_CLEAR, ND_KEY_UP,   ND_KEY_DOWN, ND_KEY_1, ND_KEY_2,
        ND_KEY_3,     ND_KEY_4,     ND_KEY_5,    ND_KEY_6,    ND_KEY_7, ND_KEY_8,
        ND_KEY_9,     ND_KEY_0,     ND_KEY_STAR, ND_KEY_HASH,
    };
    bool day_back = false;
    bool day_fwd = false;
    bool week_back = false;
    bool week_fwd = false;
    bool month_back = false;
    bool month_fwd = false;
    size_t i;

    CHECK_INT(ND_ARRAY_LEN(HARDWARE_KEYS), 16, "the 5190 has sixteen keys");

    for (i = 0u; i < ND_ARRAY_LEN(HARDWARE_KEYS); i++) {
        int32_t y = 2026;
        int32_t m = 8;
        int32_t d = 15; /* mid-month, so no step can be clamped or wrap */

        if (api.month_key(HARDWARE_KEYS[i], &y, &m, &d) != ND_CAL_NAV_MOVED)
            continue;
        if (y == 2026 && m == 8 && d == 14)
            day_back = true;
        else if (y == 2026 && m == 8 && d == 16)
            day_fwd = true;
        else if (y == 2026 && m == 8 && d == 8)
            week_back = true;
        else if (y == 2026 && m == 8 && d == 22)
            week_fwd = true;
        else if (y == 2026 && m == 7 && d == 15)
            month_back = true;
        else if (y == 2026 && m == 9 && d == 15)
            month_fwd = true;
    }

    CHECK(day_back && day_fwd, "a day either way, from the keypad alone");
    CHECK(week_back && week_fwd, "a week either way, from the keypad alone");
    CHECK(month_back && month_fwd, "a month either way, from the keypad alone");
}

static void test_navikey_opens_and_clear_leaves(void)
{
    int32_t y = 2026;
    int32_t m = 8;
    int32_t d = 29;

    CHECK_INT(api.month_key(ND_KEY_ENTER, &y, &m, &d), ND_CAL_NAV_OPEN, "NaviKey opens");
    CHECK_INT(api.month_key(ND_KEY_CLEAR, &y, &m, &d), ND_CAL_NAV_BACK, "C leaves");
    /* Neither moves the cursor: coming back from the day view has to land on
     * the day that was opened. */
    CHECK_INT(d, 29, "and the cursor stayed where it was");

    /* 0 is the one number key with nothing on it. An ignored key must not
     * cause a redraw, which is what ND_CAL_NAV_NONE means. */
    CHECK_INT(api.month_key(ND_KEY_0, &y, &m, &d), ND_CAL_NAV_NONE, "0 is unmapped");
    CHECK_INT(api.month_key(ND_KEY_MENU, &y, &m, &d), ND_CAL_NAV_NONE, "so is anything else");
    CHECK_INT(api.month_key(ND_KEY_4, NULL, NULL, NULL), ND_CAL_NAV_NONE, "NULL cursor");
}

/* 5 is the middle of the block and it goes to today. A calendar paged three
 * years out needs one key back. The virtual clock pins "today" to
 * 2024-01-01, which is also what the golden frames are captured at. */
static void test_five_goes_to_today(void)
{
    int32_t y = 2027;
    int32_t m = 11;
    int32_t d = 3;

    nd_vclock_enable();
    CHECK_INT(api.month_key(ND_KEY_5, &y, &m, &d), ND_CAL_NAV_MOVED, "5 moves");
    CHECK_INT(y, 2024, "to the pinned year");
    CHECK_INT(m, 1, "January");
    CHECK_INT(d, 1, "the 1st");
    nd_vclock_disable();
}

/* ------------------------------------------------------------------ *
 * 4. The labels
 * ------------------------------------------------------------------ */

static void test_the_row_and_title_labels(void)
{
    char out[ND_CAL_ROW_MAX];
    time_t when = 0;

    nd_vclock_enable(); /* TZ is forced to UTC, so the clock reading is fixed */
    CHECK(nd_cal_compose(2026, 8, 29, 9, 0, &when), "compose 09:00");

    api.row_label(out, sizeof out, (int64_t)when, "Team call");
    CHECK_STR(out, "9:00 am Team call", "the clock reading, then the name");

    /* An untitled event reads as its time alone rather than as a trailing
     * space -- a row that ends in whitespace looks like a bug. */
    api.row_label(out, sizeof out, (int64_t)when, "");
    CHECK_STR(out, "9:00 am", "no name, no trailing space");
    api.row_label(out, sizeof out, (int64_t)when, NULL);
    CHECK_STR(out, "9:00 am", "and NULL behaves the same");

    /* Short, because a VerticalList title is drawn at 24 px beside the
     * breadcrumb and a spelled-out month would be ellipsized to nothing. */
    api.day_title(out, sizeof out, 2026, 8, 29);
    CHECK_STR(out, "Sat 29", "the day list's title");
    api.day_title(out, sizeof out, 2026, 8, 31);
    CHECK_STR(out, "Mon 31", "and it follows the weekday");

    nd_vclock_disable();
}

/* ------------------------------------------------------------------ *
 * 5. The month view draws, and C leaves it
 * ------------------------------------------------------------------ */

static void test_clear_leaves_the_month_view(void)
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
    /* Held rather than queued: the grid's loop polls with a timeout, so a
     * held key and its synthesised repeat both arrive, and neither can be
     * eaten by a screen that drains before drawing. */
    if (!sa_hold(&fx, ND_KEY_CLEAR)) {
        CHECK(false, "held key");
        sa_fx_free(&fx);
        root_restore();
        return;
    }

    nd_vclock_enable();
    rc = api.run(&fx.ui);
    CHECK_INT(rc, 0, "app_run returns 0 on C");
    CHECK(nd_capture_frames_drawn(fx.cap) >= 1u, "the month grid was drawn");
    nd_vclock_disable();

    sa_fx_free(&fx);
    root_restore();
}

/* ------------------------------------------------------------------ *
 * 6. The banner, pressed after the event has gone
 * ------------------------------------------------------------------ */

/* The banner survives until it is pressed and the calendar can be edited from
 * anywhere in between, so this is a real state and not a defensive one.
 * Saying so beats opening the month view and leaving the user to work out
 * what happened. */
static void test_open_event_on_an_id_that_is_gone(void)
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
    /* The dialog drains the channel before its first draw, so the press has
     * to be a held one. */
    if (!sa_hold(&fx, ND_KEY_ENTER)) {
        CHECK(false, "held key");
        sa_fx_free(&fx);
        root_restore();
        return;
    }

    nd_vclock_enable();
    /* There is no calendar.db under the overlay root and this must not make
     * one -- the source tree is read-only here. */
    rc = api.open_event(&fx.ui, 4242);
    CHECK_INT(rc, 0, "a missing event is a handled state, not a failure");
    CHECK(!nd_path_is_file(ND_PATH_DB_CALENDAR), "and looking created no database");
    nd_vclock_disable();

    sa_fx_free(&fx);
    root_restore();
}

/* ------------------------------------------------------------------ *
 * 7. Teardown
 * ------------------------------------------------------------------ */

static void test_null_safety(void)
{
    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    CHECK_INT(api.open_event(NULL, 1), 1, "and so does app_open_event(NULL)");
    api.shutdown(); /* must be safe with nothing held */
    sa_checks++;
}

/* THE SIGTERM TEARDOWN CONTRACT (nd_app.h). The flag is raised BEFORE
 * app_run() so the test is deterministic: the first time the grid's loop
 * reaches its poll it returns, whatever the key channel holds.
 *
 * This is what the grid polling rather than blocking buys. The screen the app
 * sits on when nothing is happening is exactly the screen an incoming call
 * arrives at, and a blocking wait there would hold the process until the core
 * gave up and SIGKILLed it.
 *
 * MUST RUN LAST. There is no way to lower g_should_exit again. */
static void test_sigterm_leaves_the_month_view(void)
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
    /* A key that would otherwise loop for ever: 6 walks the cursor forward a
     * day at a time and never leaves the screen. */
    if (!sa_hold(&fx, ND_KEY_6)) {
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

    nd_vclock_enable();
    rc = api.run(&fx.ui);
    CHECK_INT(rc, 0, "app_run returns rather than walking the cursor for ever");
    nd_vclock_disable();

    sa_fx_free(&fx);
    root_restore();
}

int main(void)
{
    void *h = sa_begin("Calendar", "ndcal");

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }

    RUN(test_rows);
    RUN(test_the_stored_vocabulary_is_ordered);

    RUN(test_the_grid_starts_on_the_monday_before_the_first);
    RUN(test_every_cell_is_filled_for_every_month);
    RUN(test_grid_fill_refuses_a_month_that_is_not_one);

    RUN(test_the_3x3_block_is_the_d_pad);
    RUN(test_the_rocker_and_the_outer_keys_agree_with_the_block);
    RUN(test_every_direction_is_reachable_without_left_or_right);
    RUN(test_navikey_opens_and_clear_leaves);
    RUN(test_five_goes_to_today);

    RUN(test_the_row_and_title_labels);

    RUN(test_clear_leaves_the_month_view);
    RUN(test_open_event_on_an_id_that_is_gone);
    RUN(test_null_safety);

    /* Last: nd_app_should_exit() cannot be lowered again. */
    RUN(test_sigterm_leaves_the_month_view);

    return sa_end(h, "test_calendar_app");
}

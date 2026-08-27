/* apps/Clock/main.c -- the Clock app, app id 8.
 *
 * Replaces the eighteen-line stub that put "This application has not been
 * implemented yet." on the screen. That stub was a faithful port of
 * System/apps/Clock/main.py and it was also the reason the phone could not
 * tell you the time, which for a phone with a clock service running since
 * boot was an odd place to be.
 *
 * ============ THREE ROWS, ONE PER SCREEN ============
 *
 * A PagedList rather than a VerticalList, matching the phone this one
 * imitates: big type, one item at a time, and the value each row is currently
 * set to shown underneath its name. That last part is new -- see
 * nd_pagedlist_set_values() -- and it is what makes the alarm state, the
 * clock reading and the NTP setting all readable by paging through three
 * screens instead of opening three.
 *
 * ============ THE CLOCK IS SET IN TWO FIELDS, NOT ONE ============
 *
 * A keypad has ten digits and no colon, so both fields are masked: the
 * separators are the field's job, not the typist's. nd_timeset.h owns that
 * engine and the reasoning behind it.
 *
 * Both start EMPTY rather than prefilled with the current reading. A masked
 * field that is already full ignores every digit until something is deleted,
 * so prefilling would mean four presses of Clear before the first useful
 * press -- and the current reading is already on the row that was just
 * selected, which is where somebody would look for it anyway.
 *
 * ============ NTP AND MANUAL ENTRY CANNOT BOTH BE ON ============
 *
 * The sync thread would move the clock back within the minute, and it would
 * do it quietly. So Clock settings refuses up front, names the row that has
 * to change, and does not offer to change it for the user: turning off the
 * thing that keeps a phone's clock correct is a decision worth making on its
 * own screen rather than as a side effect of visiting another one.
 */

#include <string.h>
#include <time.h>

#include "clock_app.h"

#include "nd_app.h"
#include "nd_clock.h"
#include "nd_draw.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_settings.h"
#include "nd_timeset.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

const char *const nd_clock_app_title = "Clock";

const char *const nd_clock_app_rows[ND_CLOCK_APP_ROWS] = {
    "Alarm clock",
    "Clock settings",
    "NTP time sync",
};

const char *const nd_clock_app_ntp_options[ND_CLOCK_NTP_OPTIONS] = {"On", "Off"};

/* Every one of these is written to the MessageDialog's budget -- a title line
 * and three at 14 px. It truncates rather than scrolling, so a longer and
 * more precise sentence arrives on the phone with its second half missing.
 * Sleepy's commit records the same lesson. */
const char *const nd_clock_app_ntp_is_on =
    "NTP sync is on.\n\nTurn it off first, or the network will set the clock back.";
const char *const nd_clock_app_bad_time = "Not a time.\n\nFour digits, 00:00 to 23:59.";
const char *const nd_clock_app_bad_date = "Not a date.\n\nEight digits, day first, 2020 to 2099.";
const char *const nd_clock_app_set_failed =
    "The clock would not take it.\n\nSetting the time needs root.";
const char *const nd_clock_app_no_alarms = "No alarms yet.\n\nThis row is not built.";

/* ------------------------------------------------------------------ *
 * Small shared screens
 * ------------------------------------------------------------------ */

static void say(nd_ui *ui, const char *message)
{
    nd_msgdialog dialog;

    nd_msgdialog_init(&dialog, ui, message);
    nd_msgdialog_set_title(&dialog, nd_clock_app_title);
    (void)nd_msgdialog_show(&dialog);
}

/* One masked field. Returns false when it was cancelled -- which for a
 * one-line field is Clear on an empty one, so backing out of the time field
 * backs out of setting the clock altogether rather than advancing to the
 * date. */
static bool ask_masked(nd_ui *ui, const char *prompt, const char *mask, char *out, size_t out_sz)
{
    nd_textinput field;

    out[0] = '\0';
    if (nd_textinput_init(&field, ui, nd_clock_app_title, prompt, out, out_sz, "",
                          ND_T9_FILTER_NUMBERS) != ND_OK)
        return false;
    nd_textinput_set_mask(&field, mask);
    return nd_textinput_show(&field) != NULL;
}

/* ------------------------------------------------------------------ *
 * NTP time sync
 * ------------------------------------------------------------------ */

static void show_ntp_menu(nd_ui *ui)
{
    nd_vlist menu;
    nd_softkey bar;
    int32_t choice;
    bool enabled = nd_clock_ntp_enabled();

    nd_vlist_init(&menu, ui, "NTP sync", nd_clock_app_ntp_options, ND_CLOCK_NTP_OPTIONS,
                  ND_CLOCK_APP_ID);
    /* Opens on the value already in force, the way every other list of
     * choices in this OS does -- see Settings' show_engineering_mode(). */
    menu.selected_index = enabled ? 0u : 1u;

    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "Select", false);

    choice = nd_vlist_show(&menu);
    if (choice == ND_WIDGET_BACK)
        return;

    enabled = (choice == 0);
    (void)nd_settings_set(ND_SET_CLOCK_NTP, enabled ? "ON" : "OFF");
    nd_log(ND_LOG_CLOCK_APP, "NTP sync set to %s", enabled ? "ON" : "OFF");

    /* Says what it changed AND what that means, because "On" on its own does
     * not tell you the change lands at the next boot rather than now. Nothing
     * here starts or stops the running sync thread: the core owns it, and an
     * app reaching into the core's threads is the thing nd_app.h forbids. */
    say(ui, enabled ? "NTP sync is on.\n\nThe network sets the clock from the next boot."
                    : "NTP sync is off.\n\nThe clock is yours to set.");
}

/* ------------------------------------------------------------------ *
 * Clock settings
 * ------------------------------------------------------------------ */

bool nd_clock_app_may_set_manually(void)
{
    return !nd_clock_ntp_enabled();
}

static void show_clock_settings(nd_ui *ui)
{
    char time_text[ND_TIMESET_TEXT_MAX];
    char date_text[ND_TIMESET_TEXT_MAX];
    char message[192];
    char reading[32];
    int32_t hour;
    int32_t minute;
    int32_t day;
    int32_t month;
    int32_t year;
    time_t when;

    if (!nd_clock_app_may_set_manually()) {
        say(ui, nd_clock_app_ntp_is_on);
        return;
    }

    if (!ask_masked(ui, "Time (24h):", ND_TIMESET_TIME_MASK, time_text, sizeof time_text))
        return;
    if (!nd_timeset_parse_time(time_text, &hour, &minute)) {
        say(ui, nd_clock_app_bad_time);
        return;
    }

    if (!ask_masked(ui, "Date:", ND_TIMESET_DATE_MASK, date_text, sizeof date_text))
        return;
    if (!nd_timeset_parse_date(date_text, &day, &month, &year)) {
        say(ui, nd_clock_app_bad_date);
        return;
    }

    /* Composed only once BOTH fields are in hand. Setting the clock from the
     * time and then failing on the date would leave it on the right minute of
     * the wrong day, which is a worse state than the one it started in. */
    if (!nd_timeset_compose(year, month, day, hour, minute, &when)) {
        say(ui, nd_clock_app_bad_date);
        return;
    }

    if (!nd_clock_set(when, "set by hand in the Clock app")) {
        say(ui, nd_clock_app_set_failed);
        return;
    }

    /* Read back through the same formatter the menu row uses, so the
     * confirmation and the row behind it cannot disagree. */
    nd_timeset_format_clock(reading, sizeof reading, when);
    (void)nd_snprintf(message, sizeof message, "Clock set.\n\n%s\n%s", reading, date_text);
    say(ui, message);
}

/* ------------------------------------------------------------------ *
 * The root menu
 * ------------------------------------------------------------------ */

/* What each row is currently set to. Rebuilt every time round the loop rather
 * than once: the clock moves while the menu is up, and both other rows are
 * changed by the screens this menu opens. */
static void read_values(char *clock_out, size_t clock_sz, const char *values[ND_CLOCK_APP_ROWS])
{
    nd_timeset_format_clock(clock_out, clock_sz, (time_t)nd_time_now());

    /* Not "-" and not blank: the alarm row has a state even though nothing
     * can change it yet, and "Off" is the true one. */
    values[ND_CLOCK_ROW_ALARM] = "Off";
    values[ND_CLOCK_ROW_SETTINGS] = clock_out;
    values[ND_CLOCK_ROW_NTP] = nd_clock_ntp_enabled() ? "On" : "Off";
}

int app_run(nd_ui *ui)
{
    if (ui == NULL || ui->draw == NULL || ui->canvas == NULL)
        return 1;

    for (;;) {
        nd_pagedlist menu;
        char clock_text[32];
        const char *values[ND_CLOCK_APP_ROWS];
        int32_t choice;

        read_values(clock_text, sizeof clock_text, values);

        nd_pagedlist_init(&menu, ui, nd_clock_app_title, nd_clock_app_rows, ND_CLOCK_APP_ROWS,
                          ND_CLOCK_APP_ROOT, true);
        nd_pagedlist_set_values(&menu, values);

        choice = nd_pagedlist_show(&menu);
        if (choice == ND_WIDGET_BACK)
            return 0;

        switch (choice) {
        case ND_CLOCK_ROW_ALARM:
            say(ui, nd_clock_app_no_alarms);
            break;
        case ND_CLOCK_ROW_SETTINGS:
            show_clock_settings(ui);
            break;
        case ND_CLOCK_ROW_NTP:
            show_ntp_menu(ui);
            break;
        default:
            break;
        }

        /* nd_app.h: any loop that outlives a frame polls this, so an incoming
         * call is not waiting on a user who walked away mid-menu. */
        if (nd_app_should_exit())
            return 0;
    }
}

/* Nothing held: no file, no child process, no sound card. The clock is set by
 * a single settimeofday() that has already returned by the time this can be
 * called. The symbol exists because nd_app.h requires every app to export
 * one. */
void app_shutdown(void) {}

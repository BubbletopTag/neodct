/* apps/Calendar/main.c -- the Calendar app, app id 5.
 *
 * A diary the phone can remind you about. The month grid is in month.c; this
 * file is the screens either side of it -- the day, the event, and the five
 * questions that make a new one -- plus the two entry points nd-apprun calls.
 *
 * ============ WHAT IS DELIBERATELY NOT HERE ============
 *
 * No sync of any kind: no CalDAV, no Google, no network. The phone this is
 * for has 64 MB of RAM and a modem that spends most of its life detached, and
 * a calendar that quietly fails to sync is worse than one that never claimed
 * to. Events live in one sqlite table on the user partition and nowhere else.
 *
 * No multi-day events and no end time. Both would need a second instant on
 * every row and a second pass in the month mask, to describe something a
 * feature phone's owner writes as two notes anyway.
 *
 * The date of an existing event cannot be edited. Everything else about it
 * can. A masked field that is already full ignores every keypress until
 * something is deleted -- see clock_app's note on why its fields start empty
 * -- so prefilling a date would mean eight presses of C before the first
 * useful one, and an empty date field would have to invent a meaning for
 * "left blank". Moving an appointment is delete and re-make, on the day you
 * want it, which is two screens either way.
 *
 * ============ THE FLOW ASKS FIVE THINGS, IN THIS ORDER ============
 *
 *     kind -> title -> time -> repeat -> alarm
 *
 * Backing out of ANY of them abandons the whole event, which is the same
 * contract Clock's two-field time entry has: a half-entered appointment
 * saved anyway is worse than one that was not saved at all. The date is not
 * asked for -- it is the day the cursor was on, which is the day you were
 * looking at when you chose New event.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "calendar_app.h"

#include "nd_app.h"
#include "nd_calendar.h"
#include "nd_draw.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_text.h"
#include "nd_timeset.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

const char *const nd_cal_app_title = "Calendar";

const char *const nd_cal_app_options[ND_CAL_OPTIONS_COUNT] = {
    "View day",
    "New event",
    "Go to date",
    "Delete all",
};

const char *const nd_cal_app_event_options[ND_CAL_EVENT_OPTIONS_COUNT] = {
    "Edit",
    "Delete",
};

const char *const nd_cal_app_new_row = "New event";

const char *const nd_cal_app_bad_time = "Not a time.\n\nFour digits, 00:00 to 23:59.";
const char *const nd_cal_app_bad_date = "Not a date.\n\nEight digits, day first, 2020 to 2099.";
const char *const nd_cal_app_no_title = "An event needs a name.";
const char *const nd_cal_app_save_failed = "The calendar would not take it.";
/* 64 MB, and this is the one allocation in the app. Saying which screen could
 * not open beats a "View day" that silently does nothing. */
const char *const nd_cal_app_no_room = "Not enough memory to open that day.";
const char *const nd_cal_app_gone = "That event is no longer in the calendar.";
const char *const nd_cal_app_empty = "The calendar is already empty.";
const char *const nd_cal_app_delete_all_q = "Delete every event?\n\nThis cannot be undone.";
const char *const nd_cal_app_delete_q = "Delete this event?";

/* ------------------------------------------------------------------ *
 * Small shared screens
 * ------------------------------------------------------------------ */

static void say(nd_ui *ui, const char *message)
{
    nd_msgdialog dialog;

    nd_msgdialog_init(&dialog, ui, message);
    nd_msgdialog_set_title(&dialog, nd_cal_app_title);
    (void)nd_msgdialog_show(&dialog);
}

/* Yes on the NaviKey, no on C -- the two keys the dialog already accepts by
 * default (nd_widgets.h), so nothing has to be taught a third one. */
static bool confirm(nd_ui *ui, const char *message)
{
    nd_msgdialog dialog;

    nd_msgdialog_init(&dialog, ui, message);
    nd_msgdialog_set_title(&dialog, nd_cal_app_title);
    nd_msgdialog_set_button(&dialog, "Yes");
    return nd_msgdialog_show(&dialog) == ND_KEY_ENTER;
}

/* A VerticalList opened on the value already in force, which is what every
 * other list of choices in this OS does -- see Settings' engineering-mode
 * screen and Clock's NTP one. Returns the index, or -1 for Back. */
static int32_t pick(nd_ui *ui, const char *title, const char *const *items, size_t n,
                    size_t current)
{
    nd_vlist menu;
    nd_softkey bar;

    nd_vlist_init(&menu, ui, title, items, n, ND_CAL_APP_ID);
    if (current < n)
        menu.selected_index = current;

    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "Select", false);

    return nd_vlist_show(&menu);
}

/* One masked field. false when it was cancelled, which for a one-line field
 * is C on an empty one. */
static bool ask_masked(nd_ui *ui, const char *prompt, const char *mask, char *out, size_t out_sz)
{
    nd_textinput field;

    out[0] = '\0';
    if (nd_textinput_init(&field, ui, nd_cal_app_title, prompt, out, out_sz, "",
                          ND_T9_FILTER_NUMBERS) != ND_OK)
        return false;
    nd_textinput_set_mask(&field, mask);
    return nd_textinput_show(&field) != NULL;
}

/* The title field. Prefilled on an edit -- unlike the masked fields, an
 * ordinary TextInput accepts a keypress with text already in it, so there is
 * nothing to press C past. */
static bool ask_title(nd_ui *ui, const char *prompt, const char *initial, char *out, size_t out_sz)
{
    nd_textinput field;

    if (nd_textinput_init(&field, ui, nd_cal_app_title, prompt, out, out_sz,
                          (initial != NULL) ? initial : "", ND_T9_FILTER_ANY) != ND_OK)
        return false;
    return nd_textinput_show(&field) != NULL;
}

/* ------------------------------------------------------------------ *
 * Formatting
 * ------------------------------------------------------------------ */

void nd_cal_app_row_label(char *out, size_t out_sz, int64_t when, const char *title)
{
    char clock[32];

    if (out == NULL || out_sz == 0u)
        return;
    nd_timeset_format_clock(clock, sizeof clock, (time_t)when);
    if (title == NULL || title[0] == '\0') {
        (void)nd_strlcpy(out, clock, out_sz);
        return;
    }
    (void)nd_snprintf(out, out_sz, "%s %s", clock, title);
}

void nd_cal_app_day_title(char *out, size_t out_sz, int32_t year, int32_t month, int32_t day)
{
    if (out == NULL || out_sz == 0u)
        return;
    (void)nd_snprintf(out, out_sz, "%s %d", nd_cal_weekday_short[nd_cal_weekday(year, month, day)],
                      (int)day);
}

/* ------------------------------------------------------------------ *
 * Making and changing an event
 * ------------------------------------------------------------------ */

/* The five questions. `ev` carries the defaults in and the answers out; its
 * `start` must already hold the DATE the event belongs to, because this never
 * asks for one (see the file header).
 *
 * false means the user backed out, at whatever point, and nothing should be
 * written. */
static bool ask_event(nd_ui *ui, nd_cal_event *ev, bool editing)
{
    /* EVERY ANSWER LANDS HERE FIRST, and *ev is written in one go at the end.
     * Backing out of the alarm question has to leave the caller's event
     * exactly as it was -- not with the new type on it and the old time. Both
     * callers happen to pass a struct they would have thrown away, so this
     * costs nothing today and is the difference between the guarantee above
     * being true and being true by accident. */
    nd_cal_event work = *ev;
    char title[ND_CAL_TITLE_MAX];
    char time_text[ND_TIMESET_TEXT_MAX];
    char prompt[48];
    int32_t choice;
    int32_t hour;
    int32_t minute;
    int32_t y;
    int32_t m;
    int32_t d;
    int32_t cur_h;
    int32_t cur_min;
    time_t when;

    choice = pick(ui, "Type", nd_cal_kind_names, ND_CAL_KIND_COUNT, (size_t)work.kind);
    if (choice < 0)
        return false;
    work.kind = choice;

    /* An edit opens with the name already in the field: an ordinary TextInput
     * accepts a keypress with text in it, so unlike the masked field below
     * there is nothing to press C past. */
    if (!ask_title(ui, "Name:", editing ? ev->title : "", title, sizeof title))
        return false;
    if (title[0] == '\0') {
        say(ui, nd_cal_app_no_title);
        return false;
    }

    /* The current time is named in the PROMPT rather than typed into the
     * field, for the reason clock_app.h gives: a masked field that is already
     * full ignores every digit until something is deleted, so a prefill would
     * cost four presses of C before the first useful press. Naming it in the
     * prompt gives the same information at no cost. */
    nd_cal_split((time_t)work.start, &y, &m, &d, &cur_h, &cur_min);
    if (editing)
        (void)nd_snprintf(prompt, sizeof prompt, "Time (was %02d:%02d):", (int)cur_h, (int)cur_min);
    else
        (void)nd_strlcpy(prompt, "Time (24h):", sizeof prompt);

    if (!ask_masked(ui, prompt, ND_TIMESET_TIME_MASK, time_text, sizeof time_text))
        return false;
    if (!nd_timeset_parse_time(time_text, &hour, &minute)) {
        say(ui, nd_cal_app_bad_time);
        return false;
    }
    /* The DATE is the one the event already carries -- this never asks for
     * one. See the file header. */
    if (!nd_cal_compose(y, m, d, hour, minute, &when)) {
        say(ui, nd_cal_app_bad_date);
        return false;
    }
    work.start = (int64_t)when;

    choice = pick(ui, "Repeat", nd_cal_repeat_names, ND_CAL_REPEAT_COUNT, (size_t)work.repeat);
    if (choice < 0)
        return false;
    work.repeat = choice;

    choice = pick(ui, "Alarm", nd_cal_alarm_names, ND_CAL_ALARM_COUNT,
                  nd_cal_alarm_index(work.alarm_min));
    if (choice < 0)
        return false;
    work.alarm_min = nd_cal_alarm_minutes[choice];

    (void)nd_strlcpy(work.title, title, sizeof work.title);
    *ev = work;
    return true;
}

static void new_event(nd_ui *ui, int32_t year, int32_t month, int32_t day)
{
    nd_cal_event ev;
    char confirmation[160];
    char date[16];
    char clock[32];

    memset(&ev, 0, sizeof ev);
    ev.id = ND_CAL_NO_ID;
    ev.kind = ND_CAL_KIND_REMINDER;
    ev.repeat = ND_CAL_REPEAT_NONE;
    /* "At the time" rather than "No alarm": somebody making a note on a phone
     * that can remind them almost always wants reminding, and the row above
     * it is one press away for the times they do not. */
    ev.alarm_min = 0;

    /* Midday, so the date is a real instant before the time question is
     * asked and ask_event() has something to split. The hour is replaced by
     * whatever is typed; nothing is stored until then. */
    if (!nd_cal_compose(year, month, day, 12, 0, &ev.start)) {
        say(ui, nd_cal_app_bad_date);
        return;
    }

    if (!ask_event(ui, &ev, false))
        return;

    if (nd_cal_add(&ev) == ND_CAL_NO_ID) {
        say(ui, nd_cal_app_save_failed);
        return;
    }

    nd_cal_format_date(date, sizeof date, year, month, day);
    nd_timeset_format_clock(clock, sizeof clock, (time_t)ev.start);
    (void)nd_snprintf(confirmation, sizeof confirmation, "Saved.\n\n%s\n%s", clock, date);
    say(ui, confirmation);
}

static void edit_event(nd_ui *ui, nd_cal_event *ev)
{
    nd_cal_event edited = *ev;

    if (!ask_event(ui, &edited, true))
        return;
    if (nd_cal_save(&edited) != ND_OK) {
        say(ui, nd_cal_app_save_failed);
        return;
    }
    *ev = edited;
    say(ui, "Saved.");
}

/* ------------------------------------------------------------------ *
 * One event
 * ------------------------------------------------------------------ */

/* The detail page, then its two options. Returns true when the day list
 * behind it has to be reloaded -- which is either edit or delete, because
 * both change what that list says. */
static bool show_event(nd_ui *ui, nd_cal_event *ev)
{
    for (;;) {
        nd_detailpage page;
        char header[48];
        char subtitle[32];
        char body[192];
        char alarm[48];
        int32_t y;
        int32_t m;
        int32_t d;
        int32_t key;
        int32_t choice;

        nd_cal_split((time_t)ev->start, &y, &m, &d, NULL, NULL);
        nd_cal_format_day(header, sizeof header, y, m, d);
        nd_timeset_format_clock(subtitle, sizeof subtitle, (time_t)ev->start);

        /* An alarm that is off says so; one that is on says how early. Both
         * as a sentence rather than as a field name and a value, because the
         * page is prose and the phone has four of these lines to spend. */
        if (ev->alarm_min == ND_CAL_ALARM_OFF)
            (void)nd_strlcpy(alarm, "No alarm.", sizeof alarm);
        else if (ev->alarm_min == 0)
            (void)nd_strlcpy(alarm, "Alarm at the time.", sizeof alarm);
        else
            (void)nd_snprintf(alarm, sizeof alarm, "Alarm %s.",
                              nd_cal_alarm_names[nd_cal_alarm_index(ev->alarm_min)]);

        (void)nd_snprintf(body, sizeof body, "%s\n%s\n%s",
                          nd_cal_kind_names[nd_clamp32(ev->kind, 0, ND_CAL_KIND_COUNT - 1)],
                          nd_cal_repeat_names[nd_clamp32(ev->repeat, 0, ND_CAL_REPEAT_COUNT - 1)],
                          alarm);

        if (nd_detailpage_init(&page, ui, ev->title, subtitle, body, NULL, NULL, header,
                               "Options") != ND_OK)
            return false;
        key = nd_detailpage_show(&page);
        nd_detailpage_free(&page);

        if (key != ND_KEY_ENTER)
            return false;

        choice = pick(ui, "Event", nd_cal_app_event_options, ND_CAL_EVENT_OPTIONS_COUNT, 0u);
        if (choice == ND_CAL_EVOPT_EDIT) {
            edit_event(ui, ev);
            /* Back to the page, which now shows what was just written. The
             * caller still has to reload: an edit can move the event off this
             * day entirely (a repeat rule is enough to do it). */
            continue;
        }
        if (choice == ND_CAL_EVOPT_DELETE) {
            if (!confirm(ui, nd_cal_app_delete_q))
                continue;
            nd_cal_delete(ev->id);
            nd_log(ND_LOG_CALENDAR, "Event deleted (id %lld)", (long long)ev->id);
            return true;
        }
        /* Back out of the options list and the page is still there. */
        if (nd_app_should_exit())
            return true;
    }
}

/* ------------------------------------------------------------------ *
 * One day
 * ------------------------------------------------------------------ */

/* The list's rows and the events behind them, in one allocation. Thirty-two
 * events plus the "New event" row is about 7 kB, which belongs on the heap
 * and not on an app process's stack -- the same call Messages makes for its
 * own list screens, and for the same reason. */
typedef struct {
    nd_cal_event events[ND_CAL_DAY_MAX];
    char rows[ND_CAL_DAY_MAX + 1][ND_CAL_ROW_MAX];
    const char *ptrs[ND_CAL_DAY_MAX + 1];
    size_t n_events;
    size_t n_rows;
} day_list;

/* Row 0 is always "New event"; the events follow it in time order. Returns
 * the number of rows, which is never zero. */
static void load_day(day_list *dl, nd_ui *ui, int32_t year, int32_t month, int32_t day)
{
    const nd_font *font = (ui->font_md != NULL) ? ui->font_md : ui->font_n;
    size_t i;

    dl->n_events = nd_cal_day_events(year, month, day, dl->events, ND_CAL_DAY_MAX);

    (void)nd_strlcpy(dl->rows[0], nd_cal_app_new_row, ND_CAL_ROW_MAX);
    dl->ptrs[0] = dl->rows[0];

    for (i = 0u; i < dl->n_events; i++) {
        char raw[ND_CAL_ROW_MAX];

        nd_cal_app_row_label(raw, sizeof raw, dl->events[i].start, dl->events[i].title);
        /* VerticalList draws a row as it is given it, so a long name would
         * run out under the scrollbar. 215 px is x = 10 to the selection
         * bar's right edge at 225, which is the width a row actually has. */
        (void)nd_text_ellipsize(dl->rows[i + 1u], ND_CAL_ROW_MAX, raw, font, 215);
        dl->ptrs[i + 1u] = dl->rows[i + 1u];
    }
    dl->n_rows = dl->n_events + 1u;
}

static void show_day(nd_ui *ui, int32_t year, int32_t month, int32_t day)
{
    day_list *dl;
    char title[32];
    size_t selected = 0u;

    /* owned here; freed on every exit from this function */
    dl = calloc(1u, sizeof *dl);
    if (dl == NULL) {
        say(ui, nd_cal_app_no_room);
        return;
    }
    nd_cal_app_day_title(title, sizeof title, year, month, day);

    for (;;) {
        nd_vlist list;
        nd_softkey bar;
        int32_t choice;

        load_day(dl, ui, year, month, day);

        nd_vlist_init(&list, ui, title, dl->ptrs, dl->n_rows, ND_CAL_APP_ID);
        if (selected < dl->n_rows)
            list.selected_index = selected;

        nd_softkey_init(&bar, ui, false);
        nd_softkey_update(&bar, "Select", false);

        choice = nd_vlist_show(&list);
        if (choice == ND_WIDGET_BACK)
            break;

        selected = (size_t)choice;
        if (choice == 0) {
            new_event(ui, year, month, day);
        } else {
            size_t idx = (size_t)choice - 1u;

            if (idx < dl->n_events) {
                /* A COPY, because show_event() may edit it and the array is
                 * rebuilt from the database on the next turn of this loop
                 * either way. */
                nd_cal_event ev = dl->events[idx];

                if (show_event(ui, &ev)) {
                    /* Deleted or moved: the row that was selected may not
                     * exist any more, so go back to a row that certainly
                     * does. */
                    selected = 0u;
                }
            }
        }

        if (nd_app_should_exit())
            break;
    }

    free(dl);
}

/* ------------------------------------------------------------------ *
 * The month's options
 * ------------------------------------------------------------------ */

static void goto_date(nd_ui *ui, int32_t *year, int32_t *month, int32_t *day)
{
    char text[ND_TIMESET_TEXT_MAX];
    int32_t d;
    int32_t m;
    int32_t y;

    if (!ask_masked(ui, "Date:", ND_TIMESET_DATE_MASK, text, sizeof text))
        return;
    /* The Clock app's own parser and the Clock app's own refusal, so a date
     * this phone will not accept is refused with the same words wherever it
     * is typed. */
    if (!nd_timeset_parse_date(text, &d, &m, &y)) {
        say(ui, nd_cal_app_bad_date);
        return;
    }
    if (y < ND_CAL_YEAR_MIN || y > ND_CAL_YEAR_MAX) {
        say(ui, nd_cal_app_bad_date);
        return;
    }
    *year = y;
    *month = m;
    *day = d;
}

static void delete_all(nd_ui *ui)
{
    if (nd_cal_count() == 0u) {
        say(ui, nd_cal_app_empty);
        return;
    }
    if (!confirm(ui, nd_cal_app_delete_all_q))
        return;
    nd_cal_delete_all();
    nd_log(ND_LOG_CALENDAR, "Every event deleted");
    say(ui, "The calendar is empty.");
}

/* ------------------------------------------------------------------ *
 * The entry points
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    int32_t year;
    int32_t month;
    int32_t day;

    if (ui == NULL || ui->draw == NULL || ui->canvas == NULL)
        return 1;

    /* Opens on today, always. A calendar that remembered where it was left
     * would open on a month nobody asked for, and "what is today" is the
     * question it is opened to answer nine times out of ten. */
    nd_cal_split((time_t)nd_time_now(), &year, &month, &day, NULL, NULL);
    if (year < ND_CAL_YEAR_MIN || year > ND_CAL_YEAR_MAX) {
        /* A phone whose clock never got set. The grid still has to draw
         * something, and the bottom of the navigable range is the honest
         * choice -- it is where a 1970 clock would land if it could. */
        year = ND_CAL_YEAR_MIN;
        month = 1;
        day = 1;
    }

    for (;;) {
        int32_t choice;

        if (!nd_cal_month_show(ui, &year, &month, &day))
            return 0;

        choice = pick(ui, nd_cal_app_title, nd_cal_app_options, ND_CAL_OPTIONS_COUNT, 0u);
        switch (choice) {
        case ND_CAL_OPT_VIEW_DAY:
            show_day(ui, year, month, day);
            break;
        case ND_CAL_OPT_NEW:
            new_event(ui, year, month, day);
            break;
        case ND_CAL_OPT_GOTO:
            goto_date(ui, &year, &month, &day);
            break;
        case ND_CAL_OPT_DELETE_ALL:
            delete_all(ui);
            break;
        default:
            break; /* Back out of the options list, on to the grid again */
        }

        /* nd_app.h: any loop that outlives a frame polls this. The grid does
         * its own polling, so this catches a SIGTERM that arrived while a
         * dialog was up. */
        if (nd_app_should_exit())
            return 0;
    }
}

/* The home screen's banner, pressed. It names one event, so this opens that
 * event rather than the month it is in -- the same choice Messages makes
 * between open_message and open_inbox, and for the same reason: a banner that
 * said "Dentist" and then showed you a grid has not answered anything.
 *
 * The event can be gone by the time this runs, because the banner survives
 * until it is pressed and the calendar can be edited from anywhere in
 * between. Saying so is better than opening the month view and leaving the
 * user to work out what happened. */
int app_open_event(nd_ui *ui, int64_t event_id)
{
    nd_cal_event ev;

    if (ui == NULL)
        return 1;

    if (!nd_cal_get(event_id, &ev)) {
        say(ui, nd_cal_app_gone);
        return 0;
    }

    /* nd_cal_get() returns the event's FIRST occurrence, which for a repeat
     * is not the one that just rang. The page is about the appointment, and
     * the reminder's own time was on the banner that opened it, so this is
     * the right thing to show -- but it is worth saying out loud, because
     * the day list deliberately does the opposite. */
    (void)show_event(ui, &ev);
    return 0;
}

/* Nothing held: every database handle is closed by the statement that opened
 * it, the day list is freed before show_day() returns, and no child process
 * and no sound card is ever touched. The symbol exists because nd_app.h
 * requires every app to export one. */
void app_shutdown(void) {}

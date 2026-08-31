/* test_calendar.c -- the event store, the recurrence arithmetic and the alarm
 * scan (lib/nd_calendar.c).
 *
 * NOT test_calendar_app.c: that one dlopen()s the built app.so and asks about
 * the screens. This one is the half with no pixels in it, which is also the
 * half that decides whether a reminder ever appears.
 *
 * ============ WHAT THERE IS NO ORACLE FOR ============
 *
 * There is no Python calendar, so nothing here is transcribed from a
 * reference the way test_notify.c reads NotifyService's source. The oracle is
 * the header's stated behaviour, and every case below names the sentence it
 * pins. Where a rule could reasonably have gone the other way -- the 31st of
 * a month with thirty days, 29 February in a common year, an edited event's
 * alarm -- the case says which way it went and why, so that changing it is a
 * decision somebody makes on purpose rather than a test that quietly starts
 * failing.
 *
 * ============ THE CLOCK IS PINNED ============
 *
 * Anything that asks "is this due" is a function of now, so the cases that do
 * pass their own `now` explicitly rather than reading the machine's. The two
 * that cannot -- nd_cal_due()'s scan window is relative to the date it is
 * given -- compose their events around that same instant.
 *
 * TZ is whatever the runner has. That is deliberate: nd_cal_compose() goes
 * through mktime() precisely so that "09:00 on the 3rd" means nine o'clock
 * where the phone is, and a test that forced UTC would stop checking the one
 * thing that is easy to get wrong.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nd_calendar.h"
#include "nd_paths.h"
#include "nd_types.h"

#include "platform_test.h"

/* ------------------------------------------------------------------ *
 * Scaffolding
 * ------------------------------------------------------------------ */

/* An event on a date, with everything else defaulted. Returns the rowid. */
static int64_t add_at(const char *title, int32_t y, int32_t m, int32_t d, int32_t hh, int32_t mm,
                      int32_t repeat, int32_t alarm_min)
{
    nd_cal_event ev;

    memset(&ev, 0, sizeof ev);
    ev.id = ND_CAL_NO_ID;
    (void)nd_strlcpy(ev.title, title, sizeof ev.title);
    ev.kind = ND_CAL_KIND_REMINDER;
    ev.repeat = repeat;
    ev.alarm_min = alarm_min;
    if (!nd_cal_compose(y, m, d, hh, mm, &ev.start)) {
        CHECK(false);
        return ND_CAL_NO_ID;
    }
    return nd_cal_add(&ev);
}

static time_t at(int32_t y, int32_t m, int32_t d, int32_t hh, int32_t mm)
{
    time_t when = 0;

    CHECK(nd_cal_compose(y, m, d, hh, mm, &when));
    return when;
}

/* ------------------------------------------------------------------ *
 * 1. Dates, with no database in sight
 * ------------------------------------------------------------------ */

/* Monday is 0. Every other date function is indexed by this, so it is the
 * one that has to be right before anything else can be. */
static void test_the_week_starts_on_monday(void)
{
    CHECK_INT(nd_cal_weekday(2026, 8, 31), 0); /* a Monday        */
    CHECK_INT(nd_cal_weekday(2026, 8, 29), 5); /* the Saturday    */
    CHECK_INT(nd_cal_weekday(2026, 8, 30), 6); /* and the Sunday  */
    CHECK_INT(nd_cal_weekday(2024, 2, 29), 3); /* a leap day, Thursday */
    CHECK_INT(nd_cal_weekday(2000, 1, 1), 5);  /* the century that IS a leap year */
    CHECK_INT(nd_cal_weekday(1970, 1, 1), 3);  /* the epoch, a Thursday */
}

static void test_stepping_days_crosses_every_boundary(void)
{
    int32_t y = 2026;
    int32_t m = 8;
    int32_t d = 31;

    nd_cal_step_days(&y, &m, &d, 1);
    CHECK_INT(y, 2026);
    CHECK_INT(m, 9);
    CHECK_INT(d, 1);

    nd_cal_step_days(&y, &m, &d, -1);
    CHECK_INT(m, 8);
    CHECK_INT(d, 31);

    /* New Year, both ways. */
    y = 2026;
    m = 12;
    d = 31;
    nd_cal_step_days(&y, &m, &d, 1);
    CHECK_INT(y, 2027);
    CHECK_INT(m, 1);
    CHECK_INT(d, 1);
    nd_cal_step_days(&y, &m, &d, -1);
    CHECK_INT(y, 2026);
    CHECK_INT(m, 12);
    CHECK_INT(d, 31);

    /* February, in a leap year and out of one. */
    y = 2024;
    m = 2;
    d = 28;
    nd_cal_step_days(&y, &m, &d, 1);
    CHECK_INT(d, 29);
    y = 2025;
    m = 2;
    d = 28;
    nd_cal_step_days(&y, &m, &d, 1);
    CHECK_INT(m, 3);
    CHECK_INT(d, 1);
}

/* Clamped, not wrapped: walking off the end of the navigable range stops at
 * its edge rather than jumping to the other one. */
static void test_the_navigable_range_is_a_wall_not_a_loop(void)
{
    int32_t y = ND_CAL_YEAR_MAX;
    int32_t m = 12;
    int32_t d = 31;

    nd_cal_step_days(&y, &m, &d, 1);
    CHECK_INT(y, ND_CAL_YEAR_MAX);
    CHECK_INT(m, 12);
    CHECK_INT(d, 31);

    y = ND_CAL_YEAR_MIN;
    m = 1;
    d = 1;
    nd_cal_step_days(&y, &m, &d, -1);
    CHECK_INT(y, ND_CAL_YEAR_MIN);
    CHECK_INT(m, 1);
    CHECK_INT(d, 1);

    y = ND_CAL_YEAR_MAX;
    m = 12;
    d = 1;
    nd_cal_step_months(&y, &m, &d, 6);
    CHECK_INT(y, ND_CAL_YEAR_MAX);
    CHECK_INT(m, 12);
}

/* 31 March paging forward lands on 30 April, not on 1 May. Somebody paging
 * through months is looking at months. */
static void test_paging_a_month_keeps_the_cursor_in_the_month(void)
{
    int32_t y = 2026;
    int32_t m = 3;
    int32_t d = 31;

    nd_cal_step_months(&y, &m, &d, 1);
    CHECK_INT(m, 4);
    CHECK_INT(d, 30);

    y = 2026;
    m = 1;
    d = 31;
    nd_cal_step_months(&y, &m, &d, 1);
    CHECK_INT(m, 2);
    CHECK_INT(d, 28);

    y = 2024;
    m = 1;
    d = 31;
    nd_cal_step_months(&y, &m, &d, 1);
    CHECK_INT(m, 2);
    CHECK_INT(d, 29); /* a leap February keeps the 29th */

    /* Twelve months is a year, which is what the 7 and 9 keys are. */
    y = 2026;
    m = 8;
    d = 29;
    nd_cal_step_months(&y, &m, &d, 12);
    CHECK_INT(y, 2027);
    CHECK_INT(m, 8);
    CHECK_INT(d, 29);
}

/* The clock service's window, spelled as years. An appointment the clock
 * could never read back is not an appointment. */
static void test_compose_refuses_what_the_clock_would(void)
{
    time_t when = 0;

    CHECK(nd_cal_compose(2026, 8, 29, 10, 30, &when));
    CHECK(when > 0);

    CHECK(!nd_cal_compose(2019, 12, 31, 12, 0, NULL));
    CHECK(!nd_cal_compose(2100, 1, 1, 12, 0, NULL));
    CHECK(!nd_cal_compose(2026, 2, 30, 12, 0, NULL));
    CHECK(!nd_cal_compose(2025, 2, 29, 12, 0, NULL)); /* not a leap year */
    CHECK(nd_cal_compose(2024, 2, 29, 12, 0, NULL));  /* this one is      */
    CHECK(!nd_cal_compose(2026, 13, 1, 12, 0, NULL));
    CHECK(!nd_cal_compose(2026, 8, 29, 24, 0, NULL));
    CHECK(!nd_cal_compose(2026, 8, 29, 10, 60, NULL));
}

/* Round trip: what compose() built, split() takes apart. This is what makes
 * an occurrence's stored time-of-day survive being read back. */
static void test_compose_and_split_agree(void)
{
    time_t when = at(2026, 8, 29, 10, 30);
    int32_t y = 0;
    int32_t m = 0;
    int32_t d = 0;
    int32_t hh = 0;
    int32_t mm = 0;

    nd_cal_split(when, &y, &m, &d, &hh, &mm);
    CHECK_INT(y, 2026);
    CHECK_INT(m, 8);
    CHECK_INT(d, 29);
    CHECK_INT(hh, 10);
    CHECK_INT(mm, 30);
}

static void test_the_day_line_reads_as_a_date(void)
{
    char out[48];

    nd_cal_format_day(out, sizeof out, 2026, 8, 29);
    CHECK_STR(out, "Sat 29 August");

    nd_cal_format_date(out, sizeof out, 2026, 8, 29);
    CHECK_STR(out, "29/08/2026");
    /* Day first and zero-padded, which is the shape ND_TIMESET_DATE_MASK
     * types -- so what "Go to date" shows back is what would be typed in. */
    nd_cal_format_date(out, sizeof out, 2026, 1, 5);
    CHECK_STR(out, "05/01/2026");
}

/* ------------------------------------------------------------------ *
 * 2. Recurrence
 * ------------------------------------------------------------------ */

static nd_cal_event make(int32_t y, int32_t m, int32_t d, int32_t hh, int32_t mm, int32_t repeat)
{
    nd_cal_event ev;

    memset(&ev, 0, sizeof ev);
    ev.id = 1;
    ev.repeat = repeat;
    ev.alarm_min = ND_CAL_ALARM_OFF;
    (void)nd_cal_compose(y, m, d, hh, mm, &ev.start);
    return ev;
}

static void test_a_one_off_happens_once(void)
{
    nd_cal_event ev = make(2026, 8, 29, 10, 30, ND_CAL_REPEAT_NONE);

    CHECK(nd_cal_occurs_on(&ev, 2026, 8, 29, NULL));
    CHECK(!nd_cal_occurs_on(&ev, 2026, 8, 30, NULL));
    CHECK(!nd_cal_occurs_on(&ev, 2026, 9, 29, NULL));
    CHECK(!nd_cal_occurs_on(&ev, 2027, 8, 29, NULL));
}

/* Nothing occurs before its own first occurrence, whatever the rule. Paging
 * back through a repeating event's history must show an empty diary, not one
 * that has always been full. */
static void test_nothing_happens_before_it_was_made(void)
{
    int32_t rule;

    for (rule = 0; rule < ND_CAL_REPEAT_COUNT; rule++) {
        nd_cal_event ev = make(2026, 8, 29, 10, 30, rule);

        CHECK(!nd_cal_occurs_on(&ev, 2026, 8, 28, NULL));
        CHECK(!nd_cal_occurs_on(&ev, 2020, 8, 29, NULL));
        CHECK(nd_cal_occurs_on(&ev, 2026, 8, 29, NULL));
    }
}

static void test_daily_weekly_monthly_yearly(void)
{
    nd_cal_event daily = make(2026, 8, 29, 7, 0, ND_CAL_REPEAT_DAILY);
    nd_cal_event weekly = make(2026, 8, 29, 7, 0, ND_CAL_REPEAT_WEEKLY);
    nd_cal_event monthly = make(2026, 8, 29, 7, 0, ND_CAL_REPEAT_MONTHLY);
    nd_cal_event yearly = make(2026, 8, 29, 7, 0, ND_CAL_REPEAT_YEARLY);

    CHECK(nd_cal_occurs_on(&daily, 2026, 8, 30, NULL));
    CHECK(nd_cal_occurs_on(&daily, 2026, 12, 25, NULL));

    /* 29 August 2026 is a Saturday, so weekly is every Saturday. */
    CHECK(nd_cal_occurs_on(&weekly, 2026, 9, 5, NULL));
    CHECK(!nd_cal_occurs_on(&weekly, 2026, 9, 6, NULL));

    CHECK(nd_cal_occurs_on(&monthly, 2026, 9, 29, NULL));
    CHECK(!nd_cal_occurs_on(&monthly, 2026, 9, 28, NULL));

    CHECK(nd_cal_occurs_on(&yearly, 2027, 8, 29, NULL));
    CHECK(!nd_cal_occurs_on(&yearly, 2027, 9, 29, NULL));
}

/* The two decisions the header calls out. Both are "skip", not "move": an
 * appointment must never land on a day nobody chose. */
static void test_a_monthly_31st_skips_the_short_months(void)
{
    nd_cal_event ev = make(2026, 1, 31, 9, 0, ND_CAL_REPEAT_MONTHLY);

    CHECK(nd_cal_occurs_on(&ev, 2026, 3, 31, NULL));
    CHECK(nd_cal_occurs_on(&ev, 2026, 5, 31, NULL));
    /* February and April have no 31st, so nothing happens in them at all --
     * NOT the 28th, and NOT the 30th. */
    CHECK(!nd_cal_occurs_on(&ev, 2026, 2, 28, NULL));
    CHECK(!nd_cal_occurs_on(&ev, 2026, 4, 30, NULL));
}

static void test_a_yearly_leap_day_happens_every_four_years(void)
{
    nd_cal_event ev = make(2024, 2, 29, 9, 0, ND_CAL_REPEAT_YEARLY);

    CHECK(nd_cal_occurs_on(&ev, 2028, 2, 29, NULL));
    CHECK(!nd_cal_occurs_on(&ev, 2025, 2, 28, NULL));
    CHECK(!nd_cal_occurs_on(&ev, 2025, 3, 1, NULL));
}

/* An occurrence carries the event's own time of day forward. Without this a
 * repeating 07:00 alarm would fire at whatever hour the query happened to
 * compose. */
static void test_an_occurrence_keeps_the_time_of_day(void)
{
    nd_cal_event ev = make(2026, 8, 29, 7, 45, ND_CAL_REPEAT_DAILY);
    time_t when = 0;
    int32_t hh = 0;
    int32_t mm = 0;

    CHECK(nd_cal_occurs_on(&ev, 2026, 12, 25, &when));
    nd_cal_split(when, NULL, NULL, NULL, &hh, &mm);
    CHECK_INT(hh, 7);
    CHECK_INT(mm, 45);
}

/* ------------------------------------------------------------------ *
 * 3. The table
 * ------------------------------------------------------------------ */

/* A phone that has never had an appointment must not grow a calendar.db the
 * first time somebody looks at the month view. Every READ is guarded; only
 * nd_cal_add() creates. */
static void test_looking_at_an_empty_calendar_creates_nothing(void)
{
    nd_cal_event out[ND_CAL_DAY_MAX];
    nd_cal_event one;

    CHECK(!nd_path_is_file(ND_PATH_DB_CALENDAR));

    CHECK_INT(nd_cal_count(), 0);
    CHECK_INT(nd_cal_day_events(2026, 8, 29, out, ND_CAL_DAY_MAX), 0);
    CHECK_INT(nd_cal_month_mask(2026, 8), 0);
    CHECK(!nd_cal_get(1, &one));
    CHECK(!nd_cal_due((double)at(2026, 8, 29, 10, 0), NULL, NULL));
    nd_cal_delete(1);
    nd_cal_delete_all();
    nd_cal_mark_notified(1, 12345);

    CHECK(!nd_path_is_file(ND_PATH_DB_CALENDAR));
}

static void test_an_event_survives_being_written_and_read(void)
{
    nd_cal_event got;
    int64_t id;

    id = add_at("Dentist", 2026, 8, 29, 10, 30, ND_CAL_REPEAT_NONE, 15);
    CHECK(id > 0);
    CHECK(nd_path_is_file(ND_PATH_DB_CALENDAR));

    CHECK(nd_cal_get(id, &got));
    CHECK_INT(got.id, id);
    CHECK_STR(got.title, "Dentist");
    CHECK_INT(got.repeat, ND_CAL_REPEAT_NONE);
    CHECK_INT(got.alarm_min, 15);
    CHECK_INT(got.notified, 0); /* a new event has announced nothing */
    CHECK_INT(got.start, (int64_t)at(2026, 8, 29, 10, 30));
    CHECK_INT(nd_cal_count(), 1);
}

static void test_the_day_list_is_in_time_order(void)
{
    nd_cal_event out[ND_CAL_DAY_MAX];
    size_t n;

    /* Written out of order on purpose: the ORDER BY is on the rowid, so the
     * sort has to happen after the recurrence expansion, not in SQL. */
    (void)add_at("Lunch", 2026, 8, 29, 13, 0, ND_CAL_REPEAT_NONE, ND_CAL_ALARM_OFF);
    (void)add_at("Standup", 2026, 8, 29, 9, 30, ND_CAL_REPEAT_NONE, ND_CAL_ALARM_OFF);
    (void)add_at("Gym", 2026, 8, 29, 18, 0, ND_CAL_REPEAT_NONE, ND_CAL_ALARM_OFF);
    (void)add_at("Elsewhere", 2026, 8, 30, 9, 0, ND_CAL_REPEAT_NONE, ND_CAL_ALARM_OFF);

    n = nd_cal_day_events(2026, 8, 29, out, ND_CAL_DAY_MAX);
    CHECK_INT(n, 3);
    CHECK_STR(out[0].title, "Standup");
    CHECK_STR(out[1].title, "Lunch");
    CHECK_STR(out[2].title, "Gym");
}

/* The returned `start` is THE OCCURRENCE, not the event's first instant --
 * which is what lets the day list say "next Tuesday" for a weekly reminder
 * made a month ago. */
static void test_a_repeat_reads_back_as_the_day_it_was_asked_about(void)
{
    nd_cal_event out[ND_CAL_DAY_MAX];
    int64_t id = add_at("Bins", 2026, 8, 29, 7, 0, ND_CAL_REPEAT_WEEKLY, ND_CAL_ALARM_OFF);
    int32_t y = 0;
    int32_t m = 0;
    int32_t d = 0;

    CHECK_INT(nd_cal_day_events(2026, 9, 12, out, ND_CAL_DAY_MAX), 1);
    CHECK_INT(out[0].id, id); /* still names the row, so Edit and Delete work */
    nd_cal_split((time_t)out[0].start, &y, &m, &d, NULL, NULL);
    CHECK_INT(y, 2026);
    CHECK_INT(m, 9);
    CHECK_INT(d, 12);
}

static void test_the_month_mask_marks_every_day_with_something_on_it(void)
{
    uint32_t mask;

    (void)add_at("One", 2026, 8, 3, 9, 0, ND_CAL_REPEAT_NONE, ND_CAL_ALARM_OFF);
    (void)add_at("Two", 2026, 8, 3, 17, 0, ND_CAL_REPEAT_NONE, ND_CAL_ALARM_OFF);
    (void)add_at("Weekly", 2026, 8, 7, 9, 0, ND_CAL_REPEAT_WEEKLY, ND_CAL_ALARM_OFF);

    mask = nd_cal_month_mask(2026, 8);
    CHECK((mask & (1u << 2)) != 0u); /* the 3rd, marked once for two events */
    /* 7 August 2026 is a Friday: the 7th, 14th, 21st and 28th. */
    CHECK((mask & (1u << 6)) != 0u);
    CHECK((mask & (1u << 13)) != 0u);
    CHECK((mask & (1u << 20)) != 0u);
    CHECK((mask & (1u << 27)) != 0u);
    CHECK((mask & (1u << 1)) == 0u); /* the 2nd has nothing */

    /* A 31-day month can set bit 30 and no higher; nothing writes past the
     * end of the month, whatever the recurrence says. */
    CHECK((mask & 0x80000000u) == 0u);

    /* September has thirty days, so bit 30 (the 31st) can never be set. */
    mask = nd_cal_month_mask(2026, 9);
    CHECK((mask & (1u << 30)) == 0u);
}

static void test_editing_an_event_rewrites_it_and_clears_its_alarm_history(void)
{
    nd_cal_event ev;
    int64_t id = add_at("Dentist", 2026, 8, 29, 10, 30, ND_CAL_REPEAT_NONE, 15);

    nd_cal_mark_notified(id, (int64_t)at(2026, 8, 29, 10, 30));
    CHECK(nd_cal_get(id, &ev));
    CHECK_INT(ev.notified, (int64_t)at(2026, 8, 29, 10, 30));

    (void)nd_strlcpy(ev.title, "Dentist (moved)", sizeof ev.title);
    ev.start = (int64_t)at(2026, 8, 29, 16, 0);
    ev.alarm_min = 30;
    CHECK_INT(nd_cal_save(&ev), ND_OK);

    CHECK(nd_cal_get(id, &ev));
    CHECK_STR(ev.title, "Dentist (moved)");
    CHECK_INT(ev.alarm_min, 30);
    /* THE POINT OF THE CASE: an edited event is a different appointment and
     * has announced nothing. Moving a meeting an hour later must ring again. */
    CHECK_INT(ev.notified, 0);
}

static void test_deleting(void)
{
    int64_t a = add_at("A", 2026, 8, 29, 9, 0, ND_CAL_REPEAT_NONE, ND_CAL_ALARM_OFF);
    int64_t b = add_at("B", 2026, 8, 29, 10, 0, ND_CAL_REPEAT_NONE, ND_CAL_ALARM_OFF);
    nd_cal_event ev;

    CHECK_INT(nd_cal_count(), 2);
    nd_cal_delete(a);
    CHECK_INT(nd_cal_count(), 1);
    CHECK(!nd_cal_get(a, &ev));
    CHECK(nd_cal_get(b, &ev));

    nd_cal_delete_all();
    CHECK_INT(nd_cal_count(), 0);
    /* The file stays; only the rows go. Deleting every event is not the same
     * as never having had one, and re-creating the table on the next write
     * would be the slower answer to a question nobody asked. */
    CHECK(nd_path_is_file(ND_PATH_DB_CALENDAR));
}

/* ------------------------------------------------------------------ *
 * 4. The alarm scan
 * ------------------------------------------------------------------ */

static void test_an_alarm_comes_due_at_its_offset_and_not_before(void)
{
    nd_cal_event due;
    int64_t occurrence = 0;
    int64_t id = add_at("Dentist", 2026, 8, 29, 10, 30, ND_CAL_REPEAT_NONE, 15);

    /* 15 minutes before 10:30 is 10:15. */
    CHECK(!nd_cal_due((double)at(2026, 8, 29, 10, 14), &due, &occurrence));
    CHECK(nd_cal_due((double)at(2026, 8, 29, 10, 15), &due, &occurrence));
    CHECK_INT(due.id, id);
    CHECK_STR(due.title, "Dentist");
    /* `start` is the OCCURRENCE, which is what the banner's second line
     * reads, so it says 10:30 and not 10:15. */
    CHECK_INT(due.start, (int64_t)at(2026, 8, 29, 10, 30));
    CHECK_INT(occurrence, (int64_t)at(2026, 8, 29, 10, 30));
}

static void test_an_event_with_no_alarm_never_comes_due(void)
{
    (void)add_at("Quiet", 2026, 8, 29, 10, 30, ND_CAL_REPEAT_NONE, ND_CAL_ALARM_OFF);

    CHECK(!nd_cal_due((double)at(2026, 8, 29, 10, 30), NULL, NULL));
    CHECK(!nd_cal_due((double)at(2026, 8, 29, 10, 31), NULL, NULL));
    /* It is still an event: it just never reaches the banner. */
    CHECK_INT(nd_cal_count(), 1);
}

/* Marked, and it stops. This is the whole reason `notified` is a column. */
static void test_a_reminder_is_announced_once(void)
{
    nd_cal_event due;
    int64_t occurrence = 0;
    double now = (double)at(2026, 8, 29, 10, 30);

    (void)add_at("Dentist", 2026, 8, 29, 10, 30, ND_CAL_REPEAT_NONE, 0);

    CHECK(nd_cal_due(now, &due, &occurrence));
    nd_cal_mark_notified(due.id, occurrence);
    CHECK(!nd_cal_due(now, NULL, NULL));
    CHECK(!nd_cal_due(now + 60.0, NULL, NULL));
}

/* And a repeat comes round again, because `notified` holds the occurrence and
 * not a flag. A daily 07:00 reminder that fired once and never again would be
 * the obvious way to get this wrong. */
static void test_a_daily_reminder_comes_round_again_tomorrow(void)
{
    nd_cal_event due;
    int64_t occurrence = 0;

    (void)add_at("Pills", 2026, 8, 29, 7, 0, ND_CAL_REPEAT_DAILY, 0);

    CHECK(nd_cal_due((double)at(2026, 8, 29, 7, 0), &due, &occurrence));
    CHECK_INT(occurrence, (int64_t)at(2026, 8, 29, 7, 0));
    nd_cal_mark_notified(due.id, occurrence);
    CHECK(!nd_cal_due((double)at(2026, 8, 29, 7, 0), NULL, NULL));

    CHECK(nd_cal_due((double)at(2026, 8, 30, 7, 0), &due, &occurrence));
    CHECK_INT(occurrence, (int64_t)at(2026, 8, 30, 7, 0));
}

/* A phone left off overnight still tells you about this morning. One left off
 * for a week does not pretend last Tuesday just happened. */
static void test_a_missed_reminder_expires(void)
{
    (void)add_at("Dentist", 2026, 8, 29, 8, 0, ND_CAL_REPEAT_NONE, 0);

    CHECK(nd_cal_due((double)at(2026, 8, 29, 8, 0) + 3600.0, NULL, NULL));
    CHECK(nd_cal_due((double)at(2026, 8, 29, 8, 0) + (double)ND_CAL_MISSED_WINDOW_S - 1.0, NULL,
                     NULL));
    CHECK(!nd_cal_due((double)at(2026, 8, 29, 8, 0) + (double)ND_CAL_MISSED_WINDOW_S, NULL, NULL));
    CHECK(!nd_cal_due((double)at(2026, 9, 5, 8, 0), NULL, NULL));
}

/* The earliest due occurrence wins, so a phone catching up announces them in
 * the order they happened rather than in rowid order. */
static void test_the_earliest_due_reminder_is_the_one_returned(void)
{
    nd_cal_event due;

    /* Written newest-first, so rowid order and time order disagree. */
    (void)add_at("Later", 2026, 8, 29, 11, 0, ND_CAL_REPEAT_NONE, 0);
    (void)add_at("Earlier", 2026, 8, 29, 9, 0, ND_CAL_REPEAT_NONE, 0);

    CHECK(nd_cal_due((double)at(2026, 8, 29, 12, 0), &due, NULL));
    CHECK_STR(due.title, "Earlier");
}

/* The longest offer in nd_cal_alarm_minutes has to fit inside the window
 * nd_cal_due() actually scans, or the largest alarm silently never fires.
 * The two constants are checked against each other here BECAUSE they are
 * declared apart -- the header says so and this is the check it promises. */
static void test_the_scan_window_covers_the_longest_alarm(void)
{
    size_t i;
    int32_t longest = 0;
    nd_cal_event due;

    for (i = 0u; i < ND_CAL_ALARM_COUNT; i++) {
        if (nd_cal_alarm_minutes[i] > longest)
            longest = nd_cal_alarm_minutes[i];
    }
    CHECK(longest <= ND_CAL_SCAN_FWD_DAYS * 24 * 60);

    /* And in practice: a day-before alarm on tomorrow's appointment fires
     * today. */
    (void)add_at("Tomorrow", 2026, 8, 30, 9, 0, ND_CAL_REPEAT_NONE, 1440);
    CHECK(!nd_cal_due((double)at(2026, 8, 29, 8, 59), NULL, NULL));
    CHECK(nd_cal_due((double)at(2026, 8, 29, 9, 0), &due, NULL));
    CHECK_STR(due.title, "Tomorrow");
}

/* An offset a later version might store, read by this one. Row 0 is "No
 * alarm", which is the safe reading -- better a reminder that does not fire
 * than a row nobody can change. */
static void test_an_unknown_alarm_offset_reads_as_no_alarm(void)
{
    CHECK_INT(nd_cal_alarm_index(ND_CAL_ALARM_OFF), 0);
    CHECK_INT(nd_cal_alarm_index(0), 1);
    CHECK_INT(nd_cal_alarm_index(1440), ND_CAL_ALARM_COUNT - 1);
    CHECK_INT(nd_cal_alarm_index(7), 0);
    CHECK_INT(nd_cal_alarm_index(-99), 0);
}

/* ------------------------------------------------------------------ *
 * 5. Nothing here faults on a NULL
 * ------------------------------------------------------------------ */

static void test_null_safety(void)
{
    nd_cal_event out[1];

    CHECK_INT(nd_cal_add(NULL), ND_CAL_NO_ID);
    CHECK_INT(nd_cal_save(NULL), ND_ERR_INVAL);
    CHECK(!nd_cal_get(1, NULL));
    CHECK(!nd_cal_get(-1, out));
    CHECK(!nd_cal_occurs_on(NULL, 2026, 8, 29, NULL));
    CHECK_INT(nd_cal_day_events(2026, 8, 29, NULL, 4), 0);
    CHECK_INT(nd_cal_day_events(2026, 8, 29, out, 0), 0);
    CHECK_INT(nd_cal_month_mask(2026, 0), 0);
    CHECK_INT(nd_cal_month_mask(2026, 13), 0);
    nd_cal_step_days(NULL, NULL, NULL, 1);
    nd_cal_step_months(NULL, NULL, NULL, 1);
    nd_cal_split(0, NULL, NULL, NULL, NULL, NULL);
    nd_cal_format_day(NULL, 0, 2026, 8, 29);
    nd_cal_delete(-1);
    nd_cal_mark_notified(-1, 0);
    g_checks++;
}

int main(void)
{
    RUN(test_the_week_starts_on_monday);
    RUN(test_stepping_days_crosses_every_boundary);
    RUN(test_the_navigable_range_is_a_wall_not_a_loop);
    RUN(test_paging_a_month_keeps_the_cursor_in_the_month);
    RUN(test_compose_refuses_what_the_clock_would);
    RUN(test_compose_and_split_agree);
    RUN(test_the_day_line_reads_as_a_date);

    RUN(test_a_one_off_happens_once);
    RUN(test_nothing_happens_before_it_was_made);
    RUN(test_daily_weekly_monthly_yearly);
    RUN(test_a_monthly_31st_skips_the_short_months);
    RUN(test_a_yearly_leap_day_happens_every_four_years);
    RUN(test_an_occurrence_keeps_the_time_of_day);

    RUN(test_looking_at_an_empty_calendar_creates_nothing);
    RUN(test_an_event_survives_being_written_and_read);
    RUN(test_the_day_list_is_in_time_order);
    RUN(test_a_repeat_reads_back_as_the_day_it_was_asked_about);
    RUN(test_the_month_mask_marks_every_day_with_something_on_it);
    RUN(test_editing_an_event_rewrites_it_and_clears_its_alarm_history);
    RUN(test_deleting);

    RUN(test_an_alarm_comes_due_at_its_offset_and_not_before);
    RUN(test_an_event_with_no_alarm_never_comes_due);
    RUN(test_a_reminder_is_announced_once);
    RUN(test_a_daily_reminder_comes_round_again_tomorrow);
    RUN(test_a_missed_reminder_expires);
    RUN(test_the_earliest_due_reminder_is_the_one_returned);
    RUN(test_the_scan_window_covers_the_longest_alarm);
    RUN(test_an_unknown_alarm_offset_reads_as_no_alarm);

    RUN(test_null_safety);

    pt_cleanup();
    printf("test_calendar: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

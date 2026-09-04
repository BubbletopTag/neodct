/* nd_calendar.h -- the diary the phone keeps, and the alarm the core reads
 * out of it.
 *
 * A fifth sqlite database beside the four in nd_db.h. It is NOT declared
 * there: those four are byte-exact copies of the Python's schemas and an
 * existing phone's files have to keep opening after an upgrade, which is a
 * constraint this table does not have and must not be mixed up with. There
 * is no Python calendar to be one-to-one with.
 *
 * ============ WHY THE STORE AND THE APP ARE SEPARATE ============
 *
 * The Calendar app writes events; THE CORE READS THEM. A reminder has to
 * reach the home screen's banner while the app is closed -- that is the whole
 * point of it -- and the core cannot dlopen an app to ask (nd_app.h). So the
 * table lives in libneodct, the app is one caller, and nd_cal_due() is the
 * other. Neither knows about the other.
 *
 * ============ RECURRENCE IS COMPUTED, NEVER STORED ============
 *
 * A repeating event is ONE row. The occurrences are derived from it on
 * demand, by asking "does this event fall on that date" for the four days a
 * query cares about. Writing occurrences out would mean a daily reminder
 * growing the database forever on a phone with 128 MB of NAND, and it would
 * mean deciding how far ahead to expand -- a question with no good answer.
 *
 * The cost is that "when does this next occur" is a scan rather than an
 * index lookup. With one row per event and a phone-sized diary that is
 * cheaper than the index would have been.
 *
 * ============ THE ALARM FIRES ONCE PER OCCURRENCE ============
 *
 * `notified` holds the START of the occurrence already announced, not a
 * boolean, because a daily 08:00 reminder has to fire again tomorrow. Zero
 * means "never announced": a real occurrence can never be zero, since
 * nd_cal_compose() refuses anything outside the clock service's 2020..2100
 * window and 1970 is a long way outside it.
 *
 * ============ TIMES ARE LOCAL WALL CLOCK ============
 *
 * "09:00 on the 3rd" means nine o'clock wherever the phone is, so an
 * occurrence is composed with mktime() and tm_isdst = -1, exactly as
 * nd_timeset_compose() does and for the same reason. The phone ships on UTC,
 * where none of this bites, which is precisely why it has to be right before
 * somebody changes the zone rather than after.
 */

#ifndef ND_CALENDAR_H_INCLUDED
#define ND_CALENDAR_H_INCLUDED

#include <time.h>

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * The shape of an event
 * ------------------------------------------------------------------ */

/* One line of a list row at 14 px is about 48 characters. 64 bytes holds
 * that with room for UTF-8, and it is what the title field is capped at
 * everywhere -- the composer, the row and the banner all agree. */
#define ND_CAL_TITLE_MAX 64

/* What kind of note this is. The five a feature phone offers; the name is
 * shown on the detail page and nothing branches on it, so adding a sixth is
 * one row in the table below and one string. */
typedef enum {
    ND_CAL_KIND_REMINDER = 0,
    ND_CAL_KIND_MEETING,
    ND_CAL_KIND_CALL,
    ND_CAL_KIND_BIRTHDAY,
    ND_CAL_KIND_NOTE
} nd_cal_kind;

#define ND_CAL_KIND_COUNT 5
extern const char *const nd_cal_kind_names[ND_CAL_KIND_COUNT];

typedef enum {
    ND_CAL_REPEAT_NONE = 0,
    ND_CAL_REPEAT_DAILY,
    ND_CAL_REPEAT_WEEKLY,
    ND_CAL_REPEAT_MONTHLY,
    ND_CAL_REPEAT_YEARLY
} nd_cal_repeat;

#define ND_CAL_REPEAT_COUNT 5
extern const char *const nd_cal_repeat_names[ND_CAL_REPEAT_COUNT];

/* Minutes before the occurrence at which the banner appears. The list is
 * short on purpose: every entry is a row on a screen somebody pages through
 * on a numeric keypad, and the longest offset is what bounds the date window
 * nd_cal_due() has to scan -- see ND_CAL_SCAN_BACK_DAYS. */
#define ND_CAL_ALARM_OFF   (-1)
#define ND_CAL_ALARM_COUNT 7
extern const int32_t nd_cal_alarm_minutes[ND_CAL_ALARM_COUNT];
extern const char *const nd_cal_alarm_names[ND_CAL_ALARM_COUNT];

/* Index into the two arrays above for a stored alarm_min, or 0 (the "No
 * alarm" row) for anything not in the list -- so a database written by a
 * later version with an offset this one does not offer opens rather than
 * showing a blank row. */
size_t nd_cal_alarm_index(int32_t alarm_min);

typedef struct {
    int64_t id; /* rowid; ND_CAL_NO_ID for an event not yet stored */
    char title[ND_CAL_TITLE_MAX];
    int64_t start;     /* the FIRST occurrence, as a time_t                */
    int32_t kind;      /* nd_cal_kind                                      */
    int32_t repeat;    /* nd_cal_repeat                                    */
    int32_t alarm_min; /* minutes before; ND_CAL_ALARM_OFF for no reminder */
    int64_t notified;  /* the occurrence already announced; 0 for none     */
} nd_cal_event;

/* A negative id is C's spelling of "this row is not in the table yet", the
 * same convention ND_MSG_NO_ID uses. Real sqlite rowids start at 1. */
#define ND_CAL_NO_ID ((int64_t) - 1)

/* ------------------------------------------------------------------ *
 * The table
 * ------------------------------------------------------------------ */

/* Owned by libneodct. Exposed so a test can assert on the columns rather
 * than on what a query happened to return. */
extern const char *const ND_SCHEMA_CALENDAR;

/* CREATE TABLE IF NOT EXISTS, after mkdir -p of the database directory.
 * Called by the app before it writes and by nothing else: nd_db_init_all()
 * deliberately does NOT create this one, for the reason msg_db.c gives about
 * the outbox -- a phone that has never had an appointment must not grow a
 * calendar.db the first time somebody looks at the month view.
 *
 * NO `PRAGMA journal_mode=WAL`, matching the app-created tables rather than
 * the core-created ones. A -wal and a -shm file for a table the core reads
 * once every fifteen seconds is churn on NAND for nothing. */
nd_err nd_cal_init(void);

/* Every read below answers "nothing" when the database file is absent, and
 * NEVER creates it. nd_path_is_file(), the same guard msg_db.c uses. */

/* Returns the new rowid, or ND_CAL_NO_ID. `notified` is ignored and stored
 * as 0: a brand-new event has announced nothing. */
int64_t nd_cal_add(const nd_cal_event *ev);

/* UPDATE ... WHERE id = ev->id. `notified` is RESET TO ZERO, because an
 * edited event is a different appointment: moving a meeting an hour later
 * must ring again. */
nd_err nd_cal_save(const nd_cal_event *ev);

void nd_cal_delete(int64_t id);
void nd_cal_delete_all(void);

bool nd_cal_get(int64_t id, nd_cal_event *out);

/* SELECT COUNT(*). Zero for a missing database, which is the true answer. */
size_t nd_cal_count(void);

/* ------------------------------------------------------------------ *
 * Asking what happens when
 * ------------------------------------------------------------------ */

/* A day holds as many events as the day list can show. Thirty-two is far
 * past anything a phone diary carries and keeps the array a fixed size, per
 * CODING-STANDARDS.md section 1.5. */
#define ND_CAL_DAY_MAX 32

/* Every occurrence on that date, EARLIEST FIRST. `start` in each returned
 * copy is the occurrence's own instant, not the event's first one -- so a
 * daily reminder read back for next Tuesday says next Tuesday. `id` still
 * names the row, which is what edit and delete need. */
size_t nd_cal_day_events(int32_t year, int32_t month, int32_t day, nd_cal_event *out, size_t max);

/* Bit (day - 1) set for every day of that month with at least one
 * occurrence. One query for the whole month, because the month view needs
 * all thirty-one answers to draw one frame and thirty-one queries per
 * keypress is how a grid becomes slow. */
uint32_t nd_cal_month_mask(int32_t year, int32_t month);

/* Does `ev` fall on that date, and at what instant? *when may be NULL.
 * false when it does not, when the date precedes the event's own start, or
 * when the instant falls outside the clock service's sanity window.
 *
 * MONTHLY on the 31st simply skips the months that have no 31st, and YEARLY
 * on 29 February occurs only in leap years. Both are the honest reading of
 * "every month on the 31st" and neither silently moves the appointment to a
 * day the owner did not choose. */
bool nd_cal_occurs_on(const nd_cal_event *ev, int32_t year, int32_t month, int32_t day,
                      time_t *when);

/* ------------------------------------------------------------------ *
 * Dates, without a struct tm
 * ------------------------------------------------------------------ */

/* The navigable range, which is the clock service's own sanity window
 * (nd_clock.h) spelled as years. An appointment the clock could never read
 * back is not an appointment. */
#define ND_CAL_YEAR_MIN 2020
#define ND_CAL_YEAR_MAX 2099

/* 0 = MONDAY. The week starts on Monday on this phone, which is what the
 * month grid's header row spells out and what nd_cal_weekday_initials is
 * indexed by. Computed from the civil date arithmetically, NOT through
 * mktime: the grid asks this 42 times per frame and a month the local zone
 * cannot represent must still draw. */
int32_t nd_cal_weekday(int32_t year, int32_t month, int32_t day);

#define ND_CAL_WEEKDAY_COUNT 7
extern const char *const nd_cal_weekday_initials[ND_CAL_WEEKDAY_COUNT]; /* "M" "T" ... */
extern const char *const nd_cal_weekday_short[ND_CAL_WEEKDAY_COUNT];    /* "Mon" ...   */

#define ND_CAL_MONTH_COUNT 12
extern const char *const nd_cal_month_names[ND_CAL_MONTH_COUNT];

/* Step a date by whole days, clamped to the navigable range. `delta` may be
 * negative. This is what Up/Down and the digit keys move the grid cursor
 * with, so that "the day after the 31st" is one place rather than five. */
void nd_cal_step_days(int32_t *year, int32_t *month, int32_t *day, int32_t delta);

/* Step by whole months, keeping the day of the month where the target month
 * has one and clamping to its last day where it does not -- so paging from
 * 31 March to April lands on the 30th rather than on the 1st of May. */
void nd_cal_step_months(int32_t *year, int32_t *month, int32_t *day, int32_t delta);

/* Local wall clock, as ND_CAL_YEAR_MIN..MAX and a real day of that month.
 * false for a date that is not one, or one the clock service would refuse.
 * The same guarantee nd_timeset_compose() gives, over the same window. */
bool nd_cal_compose(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute,
                    time_t *out);

/* The date `when` falls on, in local time. */
void nd_cal_split(time_t when, int32_t *year, int32_t *month, int32_t *day, int32_t *hour,
                  int32_t *minute);

/* "Sat 29 August" -- the day line above the event list, and the subtitle on
 * the detail page. No year: the year is in the month view's title and the
 * detail page's own date line. */
void nd_cal_format_day(char *out, size_t out_sz, int32_t year, int32_t month, int32_t day);

/* "29/08/2026", the same shape ND_TIMESET_DATE_MASK types. */
void nd_cal_format_date(char *out, size_t out_sz, int32_t year, int32_t month, int32_t day);

/* ------------------------------------------------------------------ *
 * The alarm the core polls
 * ------------------------------------------------------------------ */

/* How long after its moment a missed reminder is still worth showing. Long
 * enough that a phone left off overnight still tells you about this
 * morning's appointment; short enough that it never surfaces one from last
 * week as though it were news. */
#define ND_CAL_MISSED_WINDOW_S (12 * 3600)

/* The dates nd_cal_due() has to look at, either side of today. One day back
 * covers an alarm set a whole day in advance (the longest offer in
 * nd_cal_alarm_minutes); two days forward covers a zone shift and the
 * midnight boundary. Keeping this a constant rather than deriving it from
 * the alarm list is deliberate: the two have to be checked against each
 * other by a human, and a test does exactly that. */
#define ND_CAL_SCAN_BACK_DAYS 1
#define ND_CAL_SCAN_FWD_DAYS  2

/* How often the core asks. Fifteen seconds is one stat() on a phone with no
 * calendar, and a reminder that is at worst a quarter-minute late. */
#define ND_CAL_POLL_S 15.0

/* The earliest occurrence whose alarm has come due and has not been
 * announced. `out` receives the event with `start` set to THAT OCCURRENCE;
 * *occurrence_out receives the same instant, which is what
 * nd_cal_mark_notified() takes. Both may be NULL.
 *
 * Allocates nothing and holds no connection: it opens, steps the statement,
 * evaluates each row against the scan window and closes. That is what lets
 * the core call it from the key-read path. */
bool nd_cal_due(double now, nd_cal_event *out, int64_t *occurrence_out);

/* Record that `occurrence` of `id` has been announced, so the next poll does
 * not announce it again. Silent on every failure, as the app's own writes
 * are: a reminder shown twice is better than a core that stops for a
 * database error. */
void nd_cal_mark_notified(int64_t id, int64_t occurrence);

#ifdef __cplusplus
}
#endif

#endif /* ND_CALENDAR_H_INCLUDED */

/* nd_calendar.c -- the event table, the recurrence arithmetic and the alarm
 * scan. nd_calendar.h has the reasoning; this is the machinery.
 *
 * ============ WHY THE DATE MATHS DOES NOT USE mktime() ============
 *
 * The month grid asks for a weekday 42 times per frame and a month mask on
 * every keypress. Every one of those going through mktime() would mean 42
 * timezone-database consultations to draw a calendar, and -- worse -- a date
 * the local zone cannot represent would come back as an error where the grid
 * needs a number.
 *
 * So the civil-date arithmetic here is closed-form: days_from_civil() is the
 * standard proleptic-Gregorian day count, and the weekday falls out of it.
 * mktime() is used in exactly one place, nd_cal_compose(), where the answer
 * genuinely is "what instant is nine o'clock on that day here" and the zone
 * is the whole question.
 *
 * ============ THE CONNECTION POLICY IS nd_db.h's ============
 *
 * Open, one statement, close. Per query, as everything else in this OS does,
 * and for the reason nd_db.h gives: a held sqlite connection costs page cache
 * this device cannot spare. nd_cal_due() steps a multi-row statement rather
 * than filling an array, so the core's poll allocates nothing at all.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

#include "nd_calendar.h"
#include "nd_clock.h"
#include "nd_db.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_vclock.h"

/* ------------------------------------------------------------------ *
 * The vocabulary
 * ------------------------------------------------------------------ */

const char *const nd_cal_kind_names[ND_CAL_KIND_COUNT] = {
    "Reminder", "Meeting", "Call", "Birthday", "Note",
};

const char *const nd_cal_repeat_names[ND_CAL_REPEAT_COUNT] = {
    "Once", "Every day", "Every week", "Every month", "Every year",
};

/* Ordered nearest-to-furthest from the appointment, so "No alarm" is row 0
 * and the list reads as a distance. ND_CAL_SCAN_BACK_DAYS covers the last
 * entry and a test asserts that it still does. */
const int32_t nd_cal_alarm_minutes[ND_CAL_ALARM_COUNT] = {
    ND_CAL_ALARM_OFF, 0, 5, 15, 30, 60, 1440,
};

const char *const nd_cal_alarm_names[ND_CAL_ALARM_COUNT] = {
    "No alarm",      "At the time",   "5 min before", "15 min before",
    "30 min before", "1 hour before", "1 day before",
};

const char *const nd_cal_weekday_initials[ND_CAL_WEEKDAY_COUNT] = {
    "M", "T", "W", "T", "F", "S", "S",
};

const char *const nd_cal_weekday_short[ND_CAL_WEEKDAY_COUNT] = {
    "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun",
};

const char *const nd_cal_month_names[ND_CAL_MONTH_COUNT] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December",
};

/* One table, seven columns, no index. A phone diary is tens of rows and
 * every query here is a full scan by design -- see the header on why
 * recurrence is computed rather than stored. An index would cost pages on
 * NAND to speed up a scan of thirty rows. */
const char *const ND_SCHEMA_CALENDAR = "CREATE TABLE IF NOT EXISTS events ("
                                       "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                                       "title TEXT, "
                                       "start INTEGER, "
                                       "kind INTEGER, "
                                       "repeat_rule INTEGER, "
                                       "alarm_min INTEGER, "
                                       "notified INTEGER)";

size_t nd_cal_alarm_index(int32_t alarm_min)
{
    size_t i;

    for (i = 0u; i < ND_CAL_ALARM_COUNT; i++) {
        if (nd_cal_alarm_minutes[i] == alarm_min)
            return i;
    }
    /* An offset this build does not offer -- a database written by a later
     * version. Row 0 is "No alarm", which is the safe reading: better a
     * reminder that does not fire than a blank row nobody can change. */
    return 0u;
}

/* ------------------------------------------------------------------ *
 * Civil dates
 * ------------------------------------------------------------------ */

static bool is_leap(int32_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int32_t days_in_month(int32_t month, int32_t year)
{
    static const int32_t LEN[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month < 1 || month > 12)
        return 0;
    if (month == 2 && is_leap(year))
        return 29;
    return LEN[month - 1];
}

/* Days since 1970-01-01, proleptic Gregorian. The shifted-era formulation:
 * March is treated as the first month of the year so that the leap day is
 * the last day of it and no case analysis is needed. Valid far outside the
 * range this phone navigates. */
static int64_t days_from_civil(int32_t y, int32_t m, int32_t d)
{
    int64_t yy = (int64_t)y - (m <= 2 ? 1 : 0);
    int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
    int64_t yoe = yy - era * 400;                                          /* [0, 399]   */
    int64_t doy = (153 * ((int64_t)m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; /* [0, 365] */
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                   /* [0, 146096] */

    return era * 146097 + doe - 719468;
}

/* The inverse, so stepping by days can come back to a date without a loop. */
static void civil_from_days(int64_t z, int32_t *y, int32_t *m, int32_t *d)
{
    int64_t era;
    int64_t doe;
    int64_t yoe;
    int64_t doy;
    int64_t mp;
    int64_t day;
    int64_t mon;
    int64_t year;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    year = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    mon = mp + (mp < 10 ? 3 : -9);
    if (mon <= 2)
        year++;

    *y = (int32_t)year;
    *m = (int32_t)mon;
    *d = (int32_t)day;
}

int32_t nd_cal_weekday(int32_t year, int32_t month, int32_t day)
{
    /* 1970-01-01 was a Thursday, which is index 3 with Monday at 0. The
     * double modulo keeps the answer non-negative for dates before it, which
     * this phone cannot navigate to but a test may still ask about. */
    int64_t z = days_from_civil(year, month, day);

    return (int32_t)(((z + 3) % 7 + 7) % 7);
}

void nd_cal_step_days(int32_t *year, int32_t *month, int32_t *day, int32_t delta)
{
    int64_t z;
    int32_t y;
    int32_t m;
    int32_t d;

    if (year == NULL || month == NULL || day == NULL)
        return;

    z = days_from_civil(*year, *month, *day) + (int64_t)delta;
    civil_from_days(z, &y, &m, &d);

    /* Clamped rather than wrapped: walking off the end of the navigable
     * range should stop at its edge, not jump to the other one. */
    if (y < ND_CAL_YEAR_MIN) {
        y = ND_CAL_YEAR_MIN;
        m = 1;
        d = 1;
    } else if (y > ND_CAL_YEAR_MAX) {
        y = ND_CAL_YEAR_MAX;
        m = 12;
        d = 31;
    }
    *year = y;
    *month = m;
    *day = d;
}

void nd_cal_step_months(int32_t *year, int32_t *month, int32_t *day, int32_t delta)
{
    int64_t total;
    int32_t y;
    int32_t m;
    int32_t len;

    if (year == NULL || month == NULL || day == NULL)
        return;

    total = (int64_t)*year * 12 + (*month - 1) + delta;
    y = (int32_t)(total / 12);
    m = (int32_t)(total % 12) + 1;
    if (m < 1) {
        m += 12;
        y--;
    }
    if (y < ND_CAL_YEAR_MIN) {
        y = ND_CAL_YEAR_MIN;
        m = 1;
    } else if (y > ND_CAL_YEAR_MAX) {
        y = ND_CAL_YEAR_MAX;
        m = 12;
    }

    /* 31 March paging forward lands on 30 April, not on 1 May. Somebody
     * paging through months is looking at months; the cursor should stay in
     * the one they asked for. */
    len = days_in_month(m, y);
    if (*day > len)
        *day = len;

    *year = y;
    *month = m;
}

bool nd_cal_compose(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute,
                    time_t *out)
{
    struct tm tm_in;
    time_t when;

    if (year < ND_CAL_YEAR_MIN || year > ND_CAL_YEAR_MAX)
        return false;
    if (month < 1 || month > 12)
        return false;
    if (day < 1 || day > days_in_month(month, year))
        return false;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
        return false;

    memset(&tm_in, 0, sizeof tm_in);
    tm_in.tm_year = year - 1900;
    tm_in.tm_mon = month - 1;
    tm_in.tm_mday = day;
    tm_in.tm_hour = hour;
    tm_in.tm_min = minute;
    /* -1 so libc decides for an hour that is ambiguous or does not exist.
     * nd_timeset.h makes the same call for the same reason. */
    tm_in.tm_isdst = -1;

    when = mktime(&tm_in);
    if (when == (time_t)-1)
        return false;
    if ((int64_t)when < ND_CLOCK_SANE_MIN || (int64_t)when >= ND_CLOCK_SANE_MAX)
        return false;

    if (out != NULL)
        *out = when;
    return true;
}

void nd_cal_split(time_t when, int32_t *year, int32_t *month, int32_t *day, int32_t *hour,
                  int32_t *minute)
{
    struct tm tm_out;

    /* nd_time_localtime(), not localtime_r(): under capture it is aliased to
     * gmtime so a frame rendered in Dublin matches one rendered in CI.
     * nd_vclock.h. */
    nd_time_localtime((double)when, &tm_out);

    if (year != NULL)
        *year = tm_out.tm_year + 1900;
    if (month != NULL)
        *month = tm_out.tm_mon + 1;
    if (day != NULL)
        *day = tm_out.tm_mday;
    if (hour != NULL)
        *hour = tm_out.tm_hour;
    if (minute != NULL)
        *minute = tm_out.tm_min;
}

void nd_cal_format_day(char *out, size_t out_sz, int32_t year, int32_t month, int32_t day)
{
    int32_t wd;

    if (out == NULL || out_sz == 0u)
        return;
    out[0] = '\0';
    if (month < 1 || month > 12)
        return;

    wd = nd_cal_weekday(year, month, day);
    (void)nd_snprintf(out, out_sz, "%s %d %s", nd_cal_weekday_short[wd], (int)day,
                      nd_cal_month_names[month - 1]);
}

void nd_cal_format_date(char *out, size_t out_sz, int32_t year, int32_t month, int32_t day)
{
    if (out == NULL || out_sz == 0u)
        return;
    (void)nd_snprintf(out, out_sz, "%02d/%02d/%04d", (int)day, (int)month, (int)year);
}

/* ------------------------------------------------------------------ *
 * Recurrence
 * ------------------------------------------------------------------ */

bool nd_cal_occurs_on(const nd_cal_event *ev, int32_t year, int32_t month, int32_t day,
                      time_t *when)
{
    int32_t sy;
    int32_t sm;
    int32_t sd;
    int32_t sh;
    int32_t smin;
    bool match;

    if (ev == NULL || month < 1 || month > 12)
        return false;
    if (day < 1 || day > days_in_month(month, year))
        return false;

    nd_cal_split((time_t)ev->start, &sy, &sm, &sd, &sh, &smin);

    /* Nothing occurs before its own first occurrence, whatever the rule. */
    if (days_from_civil(year, month, day) < days_from_civil(sy, sm, sd))
        return false;

    switch (ev->repeat) {
    case ND_CAL_REPEAT_DAILY:
        match = true;
        break;
    case ND_CAL_REPEAT_WEEKLY:
        match = nd_cal_weekday(year, month, day) == nd_cal_weekday(sy, sm, sd);
        break;
    case ND_CAL_REPEAT_MONTHLY:
        /* The 31st skips February rather than moving to the 28th. See the
         * header: an appointment must not land on a day nobody chose. */
        match = (day == sd);
        break;
    case ND_CAL_REPEAT_YEARLY:
        match = (day == sd && month == sm);
        break;
    case ND_CAL_REPEAT_NONE:
    default:
        match = (year == sy && month == sm && day == sd);
        break;
    }
    if (!match)
        return false;

    return nd_cal_compose(year, month, day, sh, smin, when);
}

/* ------------------------------------------------------------------ *
 * The table
 * ------------------------------------------------------------------ */

#define SELECT_COLS "id, title, start, kind, repeat_rule, alarm_min, notified"

static void read_row(sqlite3_stmt *st, nd_cal_event *ev)
{
    const unsigned char *title;

    memset(ev, 0, sizeof *ev);
    ev->id = (int64_t)sqlite3_column_int64(st, 0);
    title = sqlite3_column_text(st, 1);
    (void)nd_strlcpy(ev->title, (title != NULL) ? (const char *)title : "", sizeof ev->title);
    ev->start = (int64_t)sqlite3_column_int64(st, 2);
    ev->kind = (int32_t)sqlite3_column_int(st, 3);
    ev->repeat = (int32_t)sqlite3_column_int(st, 4);
    ev->alarm_min = (int32_t)sqlite3_column_int(st, 5);
    ev->notified = (int64_t)sqlite3_column_int64(st, 6);
}

/* A database that must already be there. NULL when it is not -- msg_db.c's
 * guard, and the reason looking at an empty month view does not create a
 * calendar.db. */
static sqlite3 *open_existing(void)
{
    sqlite3 *db = NULL;

    if (!nd_path_is_file(ND_PATH_DB_CALENDAR))
        return NULL;
    if (nd_db_open(ND_PATH_DB_CALENDAR, &db) != ND_OK)
        return NULL;
    return db;
}

nd_err nd_cal_init(void)
{
    sqlite3 *db = NULL;
    char *err = NULL;
    nd_err rc = ND_OK;

    if (nd_mkdir_p(ND_PATH_DB_DIR, 0755u) != ND_OK)
        return ND_ERR_IO;
    if (nd_db_open(ND_PATH_DB_CALENDAR, &db) != ND_OK)
        return ND_ERR_IO;

    if (sqlite3_exec(db, ND_SCHEMA_CALENDAR, NULL, NULL, &err) != SQLITE_OK) {
        nd_log_err(ND_LOG_CALENDAR, "calendar table: %s", err != NULL ? err : "unknown");
        rc = ND_ERR_IO;
    }
    sqlite3_free(err);
    nd_db_close(db);
    return rc;
}

int64_t nd_cal_add(const nd_cal_event *ev)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int64_t id = ND_CAL_NO_ID;

    if (ev == NULL)
        return ND_CAL_NO_ID;
    /* The one write that brings the database into existence, exactly as
     * _save_outbox_message() does for the outbox. */
    if (nd_cal_init() != ND_OK)
        return ND_CAL_NO_ID;
    if (nd_db_open(ND_PATH_DB_CALENDAR, &db) != ND_OK)
        return ND_CAL_NO_ID;

    if (sqlite3_prepare_v2(db,
                           "INSERT INTO events (title, start, kind, repeat_rule, alarm_min, "
                           "notified) VALUES (?, ?, ?, ?, ?, 0)",
                           -1, &st, NULL) == SQLITE_OK) {
        (void)sqlite3_bind_text(st, 1, ev->title, -1, SQLITE_TRANSIENT);
        (void)sqlite3_bind_int64(st, 2, (sqlite3_int64)ev->start);
        (void)sqlite3_bind_int(st, 3, ev->kind);
        (void)sqlite3_bind_int(st, 4, ev->repeat);
        (void)sqlite3_bind_int(st, 5, ev->alarm_min);
        if (sqlite3_step(st) == SQLITE_DONE)
            id = (int64_t)sqlite3_last_insert_rowid(db);
    }
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);

    if (id != ND_CAL_NO_ID)
        nd_log(ND_LOG_CALENDAR, "Event stored (id %lld): %s", (long long)id, ev->title);
    return id;
}

nd_err nd_cal_save(const nd_cal_event *ev)
{
    sqlite3 *db;
    sqlite3_stmt *st = NULL;
    nd_err rc = ND_ERR_IO;

    if (ev == NULL || ev->id < 0)
        return ND_ERR_INVAL;
    db = open_existing();
    if (db == NULL)
        return ND_ERR_NOTFOUND;

    /* notified goes back to 0: see the header. An edited appointment is a
     * different appointment and has announced nothing. */
    if (sqlite3_prepare_v2(db,
                           "UPDATE events SET title = ?, start = ?, kind = ?, repeat_rule = ?, "
                           "alarm_min = ?, notified = 0 WHERE id = ?",
                           -1, &st, NULL) == SQLITE_OK) {
        (void)sqlite3_bind_text(st, 1, ev->title, -1, SQLITE_TRANSIENT);
        (void)sqlite3_bind_int64(st, 2, (sqlite3_int64)ev->start);
        (void)sqlite3_bind_int(st, 3, ev->kind);
        (void)sqlite3_bind_int(st, 4, ev->repeat);
        (void)sqlite3_bind_int(st, 5, ev->alarm_min);
        (void)sqlite3_bind_int64(st, 6, (sqlite3_int64)ev->id);
        if (sqlite3_step(st) == SQLITE_DONE)
            rc = ND_OK;
    }
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);
    return rc;
}

/* One statement with no result rows. Silent on failure, as the app's own
 * writes are -- MSG-3's reasoning applies unchanged. */
static void run_void(const char *sql, int64_t id)
{
    sqlite3 *db = open_existing();
    sqlite3_stmt *st = NULL;

    if (db == NULL)
        return;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (id >= 0)
            (void)sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
        (void)sqlite3_step(st);
    }
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);
}

void nd_cal_delete(int64_t id)
{
    if (id < 0)
        return;
    run_void("DELETE FROM events WHERE id = ?", id);
}

void nd_cal_delete_all(void)
{
    run_void("DELETE FROM events", -1);
}

void nd_cal_mark_notified(int64_t id, int64_t occurrence)
{
    sqlite3 *db;
    sqlite3_stmt *st = NULL;

    if (id < 0)
        return;
    db = open_existing();
    if (db == NULL)
        return;
    if (sqlite3_prepare_v2(db, "UPDATE events SET notified = ? WHERE id = ?", -1, &st, NULL) ==
        SQLITE_OK) {
        (void)sqlite3_bind_int64(st, 1, (sqlite3_int64)occurrence);
        (void)sqlite3_bind_int64(st, 2, (sqlite3_int64)id);
        (void)sqlite3_step(st);
    }
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);
}

bool nd_cal_get(int64_t id, nd_cal_event *out)
{
    sqlite3 *db;
    sqlite3_stmt *st = NULL;
    bool found = false;

    if (out == NULL || id < 0)
        return false;
    db = open_existing();
    if (db == NULL)
        return false;

    if (sqlite3_prepare_v2(db, "SELECT " SELECT_COLS " FROM events WHERE id = ?", -1, &st, NULL) ==
        SQLITE_OK) {
        (void)sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            read_row(st, out);
            found = true;
        }
    }
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);
    return found;
}

size_t nd_cal_count(void)
{
    sqlite3 *db = open_existing();
    sqlite3_stmt *st = NULL;
    size_t n = 0u;

    if (db == NULL)
        return 0u;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM events", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            int count = sqlite3_column_int(st, 0);

            n = (count > 0) ? (size_t)count : 0u;
        }
    }
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);
    return n;
}

/* ------------------------------------------------------------------ *
 * Queries the screens make
 * ------------------------------------------------------------------ */

/* Insertion sort into an already-sorted prefix. The array is at most
 * ND_CAL_DAY_MAX and almost always two or three entries, so this is both the
 * simplest and the fastest thing available -- and it is stable, which keeps
 * two appointments at the same minute in rowid order rather than in
 * whichever order sqlite handed them over. */
static void insert_sorted(nd_cal_event *out, size_t n, const nd_cal_event *ev)
{
    size_t i = n;

    while (i > 0u && out[i - 1u].start > ev->start) {
        out[i] = out[i - 1u];
        i--;
    }
    out[i] = *ev;
}

size_t nd_cal_day_events(int32_t year, int32_t month, int32_t day, nd_cal_event *out, size_t max)
{
    sqlite3 *db;
    sqlite3_stmt *st = NULL;
    size_t n = 0u;

    if (out == NULL || max == 0u)
        return 0u;
    db = open_existing();
    if (db == NULL)
        return 0u;

    if (sqlite3_prepare_v2(db, "SELECT " SELECT_COLS " FROM events ORDER BY id ASC", -1, &st,
                           NULL) == SQLITE_OK) {
        while (n < max && sqlite3_step(st) == SQLITE_ROW) {
            nd_cal_event ev;
            time_t when;

            read_row(st, &ev);
            if (!nd_cal_occurs_on(&ev, year, month, day, &when))
                continue;
            /* THE OCCURRENCE, not the event's first instant -- so a weekly
             * reminder read back for next Tuesday says next Tuesday. */
            ev.start = (int64_t)when;
            insert_sorted(out, n, &ev);
            n++;
        }
    }
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);
    return n;
}

uint32_t nd_cal_month_mask(int32_t year, int32_t month)
{
    sqlite3 *db;
    sqlite3_stmt *st = NULL;
    uint32_t mask = 0u;
    int32_t len;

    if (month < 1 || month > 12)
        return 0u;
    len = days_in_month(month, year);
    db = open_existing();
    if (db == NULL)
        return 0u;

    if (sqlite3_prepare_v2(db, "SELECT " SELECT_COLS " FROM events", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            nd_cal_event ev;
            int32_t d;

            read_row(st, &ev);
            for (d = 1; d <= len; d++) {
                if ((mask & (1u << (unsigned)(d - 1))) != 0u)
                    continue; /* another event already marked this day */
                if (nd_cal_occurs_on(&ev, year, month, d, NULL))
                    mask |= 1u << (unsigned)(d - 1);
            }
        }
    }
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);
    return mask;
}

/* ------------------------------------------------------------------ *
 * The alarm scan
 * ------------------------------------------------------------------ */

bool nd_cal_due(double now, nd_cal_event *out, int64_t *occurrence_out)
{
    sqlite3 *db;
    sqlite3_stmt *st = NULL;
    nd_cal_event best;
    int64_t best_occ = 0;
    bool found = false;
    int32_t today_y;
    int32_t today_m;
    int32_t today_d;

    db = open_existing();
    if (db == NULL)
        return false;

    nd_cal_split((time_t)now, &today_y, &today_m, &today_d, NULL, NULL);

    /* Only the rows that can ring. An event with no alarm is still an event;
     * it simply never reaches the banner. */
    if (sqlite3_prepare_v2(db, "SELECT " SELECT_COLS " FROM events WHERE alarm_min >= 0", -1, &st,
                           NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            nd_cal_event ev;
            int32_t offset;

            read_row(st, &ev);

            for (offset = -ND_CAL_SCAN_BACK_DAYS; offset <= ND_CAL_SCAN_FWD_DAYS; offset++) {
                int32_t y = today_y;
                int32_t m = today_m;
                int32_t d = today_d;
                time_t when;
                double alarm_at;

                nd_cal_step_days(&y, &m, &d, offset);
                if (!nd_cal_occurs_on(&ev, y, m, d, &when))
                    continue;
                if ((int64_t)when == ev.notified)
                    continue; /* this occurrence has already been announced */

                alarm_at = (double)when - (double)ev.alarm_min * 60.0;
                if (now < alarm_at)
                    continue; /* not yet */
                if (now >= alarm_at + (double)ND_CAL_MISSED_WINDOW_S)
                    continue; /* too old to be news -- see the header */

                /* The earliest due occurrence wins, so a phone catching up
                 * after being off announces them in the order they happened
                 * rather than in rowid order. */
                if (!found || (int64_t)when < best_occ) {
                    best = ev;
                    best.start = (int64_t)when;
                    best_occ = (int64_t)when;
                    found = true;
                }
            }
        }
    }
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);

    if (found) {
        if (out != NULL)
            *out = best;
        if (occurrence_out != NULL)
            *occurrence_out = best_occ;
    }
    return found;
}

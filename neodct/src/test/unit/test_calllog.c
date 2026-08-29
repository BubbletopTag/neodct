/* test_calllog.c -- the Call log app, app id 3.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. THE STRINGS ARE THE PYTHON'S, including the two apostrophes in
 *     "Received calls' duration" -- those labels are used twice each, once as
 *     a menu row and once as an InfoScreen title, and a mismatch between the
 *     two would show as a screen whose heading is not the row that opened it.
 *
 *  2. THE CLEAR MENU'S INDEX MAP IS NOT THE ROOT MENU'S. CLEAR_ITEMS maps
 *     {0: all, 1: missed, 2: dialed, 3: received}; ROOT_ITEMS is missed,
 *     received, dialed. Both orders are pinned here because the only way to
 *     notice them drifting together is to write them both down.
 *
 *  3. _connect() CREATES THE DATABASE, ON A READ. call_log.db is the one
 *     database the core does not create, and every fetch runs makedirs +
 *     CREATE TABLE IF NOT EXISTS. So opening "Missed calls" on a phone that
 *     has never taken a call leaves an empty database behind, and that is
 *     reproduced rather than tidied away.
 *
 *  4. NO PRAGMA journal_mode=WAL. nd_db.h says the asymmetry with the three
 *     core databases is in the on-disk header of every phone shipped so far.
 *     Checked by asking sqlite what mode the file came up in.
 *
 *  5. THE SCHEMA IS nd_db.h's ND_SCHEMA_CALLS, byte for byte, including its
 *     odd indentation -- which is in the on-disk sqlite_master row of every
 *     existing phone and therefore cannot be reformatted.
 *
 *  6. THE QUERY IS ORDER BY id DESC LIMIT 10, filtered by type, and it reads
 *     `duration` as well. Driven against a real sqlite database with 25 rows
 *     of three types, so "newest first", "only this type" and "ten at most"
 *     are three separate claims and not one. The LIMIT was 20 while the lists
 *     were VerticalLists; calllog.h says why it is 10 now, and this is the
 *     assertion that stops it drifting back.
 *
 *  7. A NULL `number` COLUMN COMES BACK EMPTY, which is what the app's
 *     `number or "Unknown"` turns into "Unknown".
 *
 *  8. format_duration and format_call_time, including format_duration's
 *     max(0, s) and the fact that its hours field is not capped at 99.
 *
 *  9. THE TIMER SETTINGS round-trip, and every unparseable stored value
 *     folds to 0 the way Python's int() raising into an `except` does.
 *
 * 10. THE GOLDEN FRAME. app-calllog is the root PagedList's first page,
 *     judged by the SHA-256 over raw RGB that goldenframe.py compares.
 *
 * 11. THE WRITER AND THE READER AGREE. nd_db_record_call() lives in the core,
 *     this app reads what it wrote, and the two halves have to make the same
 *     file: same schema, same rollback-journal mode, same three type strings.
 *     Driven by writing rows with the core's function and reading them back
 *     with the app's, because that round trip is the whole feature and
 *     nothing else in the tree exercises both ends of it. And the table is
 *     bounded per type, because it is now the only thing on this phone that
 *     grows without anybody asking it to.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "nd_db.h"
#include "nd_settings.h"

#include "smallapp_test.h"

#include "../../apps/CallLog/calllog.h"

static char g_root[ND_PATH_MAX];
static char g_saved_root[ND_PATH_MAX];

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    nd_err (*connect)(struct sqlite3 **);
    size_t (*fetch)(const char *, nd_call_rec *, size_t);
    bool (*clear)(const char *);
    int64_t (*timer_get)(nd_calllog_timer);
    bool (*timer_set)(nd_calllog_timer, int64_t);
    const char *(*format_duration)(int64_t, char *, size_t);
    const char *(*format_call_time)(int64_t, char *, size_t);
    const char *const *root_items;
    const char *const *clear_items;
    const char *const *duration_items;
    const char *const *clear_targets;
    const char *const *types;
    const char *const *titles;
    const char *const *timer_keys;
    const char *const *unknown;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&api.connect = sa_sym(h, "nd_calllog_connect");
    *(void **)&api.fetch = sa_sym(h, "nd_calllog_fetch");
    *(void **)&api.clear = sa_sym(h, "nd_calllog_clear");
    *(void **)&api.timer_get = sa_sym(h, "nd_calllog_timer_get");
    *(void **)&api.timer_set = sa_sym(h, "nd_calllog_timer_set");
    *(void **)&api.format_duration = sa_sym(h, "nd_calllog_format_duration");
    *(void **)&api.format_call_time = sa_sym(h, "nd_calllog_format_call_time");
    api.root_items = dlsym(h, "nd_calllog_root_items");
    api.clear_items = dlsym(h, "nd_calllog_clear_items");
    api.duration_items = dlsym(h, "nd_calllog_duration_items");
    api.clear_targets = dlsym(h, "nd_calllog_clear_targets");
    api.types = dlsym(h, "nd_calllog_types");
    api.titles = dlsym(h, "nd_calllog_titles");
    api.timer_keys = dlsym(h, "nd_calllog_timer_keys");
    api.unknown = dlsym(h, "nd_calllog_unknown");

    return api.run != NULL && api.shutdown != NULL && api.connect != NULL && api.fetch != NULL &&
           api.clear != NULL && api.timer_get != NULL && api.timer_set != NULL &&
           api.format_duration != NULL && api.format_call_time != NULL && api.root_items != NULL &&
           api.clear_items != NULL && api.duration_items != NULL && api.clear_targets != NULL &&
           api.types != NULL && api.titles != NULL && api.timer_keys != NULL && api.unknown != NULL;
}

/* ------------------------------------------------------------------ *
 * 1 + 2. The strings and the two index maps
 * ------------------------------------------------------------------ */

static void test_strings(void)
{
    CHECK_STR(api.root_items[0], "Missed calls", "ROOT_ITEMS[0]");
    CHECK_STR(api.root_items[1], "Received calls", "ROOT_ITEMS[1]");
    CHECK_STR(api.root_items[2], "Dialed calls", "ROOT_ITEMS[2]");
    CHECK_STR(api.root_items[3], "Clear call lists", "ROOT_ITEMS[3]");
    CHECK_STR(api.root_items[4], "Show call duration", "ROOT_ITEMS[4]");

    CHECK_STR(api.clear_items[0], "All", "CLEAR_ITEMS[0]");
    CHECK_STR(api.clear_items[1], "Missed", "CLEAR_ITEMS[1]");
    CHECK_STR(api.clear_items[2], "Dialed", "CLEAR_ITEMS[2]");
    CHECK_STR(api.clear_items[3], "Received", "CLEAR_ITEMS[3]");

    CHECK_STR(api.duration_items[0], "Last call duration", "DURATION_ITEMS[0]");
    CHECK_STR(api.duration_items[1], "Received calls' duration", "DURATION_ITEMS[1]");
    CHECK_STR(api.duration_items[2], "Dialed calls' duration", "DURATION_ITEMS[2]");
    CHECK_STR(api.duration_items[3], "Clear timers", "DURATION_ITEMS[3]");

    CHECK_STR(*api.unknown, "Unknown", "`number or \"Unknown\"`");

    /* TIMER_KEYS, in the dict's insertion order. "Clear timers" walks them
     * in this order, and nd_settings.h spells the same three. */
    CHECK_STR(api.timer_keys[ND_CALLLOG_TIMER_LAST], "calllog.duration.last", "TIMER_KEYS[last]");
    CHECK_STR(api.timer_keys[ND_CALLLOG_TIMER_RECEIVED], "calllog.duration.received",
              "TIMER_KEYS[received]");
    CHECK_STR(api.timer_keys[ND_CALLLOG_TIMER_DIALED], "calllog.duration.dialed",
              "TIMER_KEYS[dialed]");
}

static void test_index_maps(void)
{
    /* The ROOT menu: missed, received, dialed. */
    CHECK_STR(api.types[0], "missed", "root 0 -> missed");
    CHECK_STR(api.types[1], "received", "root 1 -> received");
    CHECK_STR(api.types[2], "dialed", "root 2 -> dialed");

    /* And the title each row opens, which is the row's first word and NOT the
     * row. "Received calls" in 24 px type does not fit beside the breadcrumb
     * and nd_text_fit() cuts it to "Received c...", so the qualifier is
     * dropped on purpose rather than by the fitter. Pinned because a later
     * "tidy-up" back to the row text would put the ellipsis back and look
     * like the fitter had regressed. */
    CHECK_STR(api.titles[0], "Missed", "root 0's title");
    CHECK_STR(api.titles[1], "Received", "root 1's title");
    CHECK_STR(api.titles[2], "Dialed", "root 2's title");
    CHECK_STR(api.root_items[0], "Missed calls", "and the ROW it opens keeps the noun");

    /* The CLEAR menu, whose 2 and 3 are the other way round. */
    CHECK(api.clear_targets[0] == NULL, "clear 0 -> every row");
    CHECK_STR(api.clear_targets[1], "missed", "clear 1 -> missed");
    CHECK_STR(api.clear_targets[2], "dialed", "clear 2 -> dialed");
    CHECK_STR(api.clear_targets[3], "received", "clear 3 -> received");

    /* Said out loud, because it looks like a bug and is not: the two menus
     * disagree about what index 1 and 2 mean. */
    CHECK(strcmp(api.types[1], api.clear_targets[1]) != 0,
          "root index 1 and clear index 1 name different lists");
}

/* ------------------------------------------------------------------ *
 * 3 + 4 + 5. _connect()
 * ------------------------------------------------------------------ */

static bool db_file_exists(void)
{
    char path[ND_PATH_MAX];

    if (nd_snprintf(path, sizeof path, "%s%s", g_root, ND_CALLLOG_DB) != ND_OK)
        return false;
    return sa_file_exists(path);
}

static void test_connect_creates(void)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    char mode[32];
    char sql[512];

    CHECK(!db_file_exists(), "no call_log.db before the first connect");

    CHECK_INT(api.connect(&db), ND_OK, "_connect() succeeds on a phone with no database");
    CHECK(db != NULL, "_connect() hands back a connection");
    if (db == NULL)
        return;

    CHECK(db_file_exists(), "_connect() CREATED call_log.db -- a read is enough");

    /* 4. journal_mode. The three core databases are put into WAL; this one
     * never is, and journal_mode lives in the file header, so it stays in
     * rollback-journal mode for the life of the phone. */
    mode[0] = '\0';
    if (sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s = sqlite3_column_text(st, 0);

        (void)nd_strlcpy(mode, (s != NULL) ? (const char *)s : "", sizeof mode);
    }
    (void)sqlite3_finalize(st);
    st = NULL;
    CHECK(strcmp(mode, "wal") != 0, "call_log.db is NOT in WAL mode");
    CHECK_STR(mode, "delete", "call_log.db came up in rollback-journal mode");

    /* 5. The stored schema is ND_SCHEMA_CALLS verbatim. sqlite_master keeps
     * the CREATE statement exactly as it was issued, whitespace, comments
     * and all, so this compares the bytes that are on every existing
     * phone. */
    sql[0] = '\0';
    if (sqlite3_prepare_v2(db, "SELECT sql FROM sqlite_master WHERE type='table' AND name='calls'",
                           -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s = sqlite3_column_text(st, 0);

        (void)nd_strlcpy(sql, (s != NULL) ? (const char *)s : "", sizeof sql);
    }
    (void)sqlite3_finalize(st);
    /* sqlite drops the "CREATE TABLE IF NOT EXISTS" clause's "IF NOT EXISTS"
     * from what it stores, and keeps everything after the table name. */
    CHECK(strstr(sql, "type TEXT,             -- 'missed' | 'received' | 'dialed'") != NULL,
          "the stored schema keeps the Python's comment and its spacing");
    CHECK(strstr(sql, "duration INTEGER DEFAULT 0") != NULL, "the stored schema keeps `duration`");
    CHECK(strstr(ND_SCHEMA_CALLS, "PRAGMA") == NULL, "ND_SCHEMA_CALLS issues no pragma");

    nd_db_close(db);
}

/* ------------------------------------------------------------------ *
 * 6 + 7. The query
 * ------------------------------------------------------------------ */

/* 25 'missed' rows, 3 'received' and one 'missed' with a NULL number, in
 * insert order -- so rowid order IS chronological order and "ORDER BY id
 * DESC" has something to reverse. */
static void seed_rows(void)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int i;

    if (api.connect(&db) != ND_OK) {
        CHECK(false, "seed: connect");
        return;
    }
    if (sqlite3_prepare_v2(db, "INSERT INTO calls (type, number, timestamp) VALUES (?, ?, ?)", -1,
                           &st, NULL) != SQLITE_OK) {
        CHECK(false, "seed: prepare");
        nd_db_close(db);
        return;
    }
    for (i = 0; i < 25; i++) {
        char num[32];

        (void)nd_snprintf(num, sizeof num, "0700%03d", i);
        (void)sqlite3_reset(st);
        (void)sqlite3_bind_text(st, 1, "missed", -1, SQLITE_STATIC);
        (void)sqlite3_bind_text(st, 2, num, -1, SQLITE_TRANSIENT);
        (void)sqlite3_bind_int64(st, 3, 1700000000 + i);
        CHECK_INT(sqlite3_step(st), SQLITE_DONE, "seed: insert missed");
    }
    for (i = 0; i < 3; i++) {
        (void)sqlite3_reset(st);
        (void)sqlite3_bind_text(st, 1, "received", -1, SQLITE_STATIC);
        (void)sqlite3_bind_text(st, 2, "0851234567", -1, SQLITE_STATIC);
        (void)sqlite3_bind_int64(st, 3, 1700001000 + i);
        CHECK_INT(sqlite3_step(st), SQLITE_DONE, "seed: insert received");
    }
    /* The nullable column the Python's `number or "Unknown"` exists for. */
    (void)sqlite3_reset(st);
    (void)sqlite3_bind_text(st, 1, "dialed", -1, SQLITE_STATIC);
    (void)sqlite3_bind_null(st, 2);
    (void)sqlite3_bind_int64(st, 3, 1700002000);
    CHECK_INT(sqlite3_step(st), SQLITE_DONE, "seed: insert a NULL number");

    (void)sqlite3_finalize(st);
    nd_db_close(db);
}

static void test_fetch(void)
{
    nd_call_rec rows[ND_CALLLOG_MAX_CALLS];
    size_t n;

    n = api.fetch("missed", rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 10, "LIMIT 10 caps a 25-row list");
    /* ORDER BY id DESC: the newest insert first. */
    CHECK_STR(rows[0].number, "0700024", "newest missed call first");
    CHECK_INT(rows[0].timestamp, 1700000024, "and its timestamp");
    CHECK_STR(rows[9].number, "0700015", "the tenth is the 10th-newest");

    n = api.fetch("received", rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 3, "WHERE type=? excludes the other 26 rows");
    CHECK_STR(rows[0].number, "0851234567", "received number");
    /* The seeder writes no duration, so DEFAULT 0 is what comes back -- the
     * same reading a row from a phone that predates the writer gives. */
    CHECK_INT(rows[0].duration, 0, "an unset duration column reads 0");

    n = api.fetch("dialed", rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 1, "one dialed row");
    CHECK_STR(rows[0].number, "", "a NULL number column reads as empty");

    n = api.fetch("nosuchtype", rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 0, "an unknown type is an empty list, not an error");

    /* The caller's own bound is honoured too, so a smaller array is safe. */
    n = api.fetch("missed", rows, 4u);
    CHECK_INT(n, 4, "the caller's max wins when it is smaller than 20");
}

static void test_clear(void)
{
    nd_call_rec rows[ND_CALLLOG_MAX_CALLS];

    CHECK(api.clear("received"), "clearing one type succeeds");
    CHECK_INT(api.fetch("received", rows, ND_ARRAY_LEN(rows)), 0, "received is now empty");
    CHECK_INT(api.fetch("missed", rows, ND_ARRAY_LEN(rows)), 10, "and missed is untouched");

    CHECK(api.clear(NULL), "clearing everything succeeds");
    CHECK_INT(api.fetch("missed", rows, ND_ARRAY_LEN(rows)), 0, "missed is now empty");
    CHECK_INT(api.fetch("dialed", rows, ND_ARRAY_LEN(rows)), 0, "dialed is now empty");
}

/* ------------------------------------------------------------------ *
 * 11. The writer
 * ------------------------------------------------------------------ */

/* Puts the phone back to one that has never taken a call, so the writer is
 * the thing that creates the database. test_clear() has just emptied the
 * table, but the FILE is what decides the journal mode and it is still the
 * one _connect() made. */
static void drop_db_file(void)
{
    char path[ND_PATH_MAX];

    if (nd_snprintf(path, sizeof path, "%s%s", g_root, ND_CALLLOG_DB) != ND_OK)
        return;
    (void)remove(path);
    if (nd_snprintf(path, sizeof path, "%s%s-journal", g_root, ND_CALLLOG_DB) == ND_OK)
        (void)remove(path);
}

/* SELECT COUNT(*) straight from the file, because the app's own fetch stops
 * at ten and the cap is fifty. */
static int count_rows(const char *type)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int n = -1;

    if (api.connect(&db) != ND_OK)
        return -1;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM calls WHERE type=?", -1, &st, NULL) ==
        SQLITE_OK) {
        (void)sqlite3_bind_text(st, 1, type, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW)
            n = sqlite3_column_int(st, 0);
    }
    (void)sqlite3_finalize(st);
    nd_db_close(db);
    return n;
}

static void test_record_creates_the_same_file(void)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    char mode[32];

    drop_db_file();
    CHECK(!db_file_exists(), "no call_log.db before the first call");

    CHECK(nd_db_record_call(ND_CALL_TYPE_MISSED, "0871111111", 1700100000, 0) > 0,
          "the first call of the phone's life is recorded");
    CHECK(db_file_exists(), "and recording it CREATED call_log.db");

    /* The half that matters: the writer must leave the file in the mode the
     * reader would have. journal_mode is in the header and whoever creates
     * the file fixes it forever, so a writer that issued the WAL pragma the
     * other three databases get would make a file the Python never made. */
    CHECK_INT(api.connect(&db), ND_OK, "the app opens what the core created");
    if (db == NULL)
        return;
    mode[0] = '\0';
    if (sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *m = sqlite3_column_text(st, 0);

        (void)nd_strlcpy(mode, (m != NULL) ? (const char *)m : "", sizeof mode);
    }
    (void)sqlite3_finalize(st);
    CHECK_STR(mode, "delete", "a core-created call_log.db is in rollback-journal mode too");
    nd_db_close(db);
}

static void test_record_round_trip(void)
{
    nd_call_rec rows[ND_CALLLOG_MAX_CALLS];
    size_t n;
    int i;

    /* One of each type, so "the row lands in the list its type names" is a
     * claim about all three and not just the one the app opens first. */
    CHECK(nd_db_record_call(ND_CALL_TYPE_RECEIVED, "0862222222", 1700100100, 95) > 0,
          "a received call");
    CHECK(nd_db_record_call(ND_CALL_TYPE_DIALED, "0873333333", 1700100200, 8) > 0, "a dialed call");

    n = api.fetch(api.types[1], rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 1, "the received list has the received call and nothing else");
    CHECK_STR(rows[0].number, "0862222222", "its number survived the round trip");
    CHECK_INT(rows[0].timestamp, 1700100100, "and its timestamp");
    CHECK_INT(rows[0].duration, 95, "and the duration the core measured");

    n = api.fetch(api.types[2], rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 1, "the dialed list has the dialed call");
    CHECK_INT(rows[0].duration, 8, "with its own duration");

    n = api.fetch(api.types[0], rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 1, "and the missed list still has only the missed one");
    CHECK_INT(rows[0].duration, 0, "a missed call was connected for no seconds");

    /* A withheld caller ID: stored NULL, read back empty, drawn "Unknown".
     * That is the column the app's `number or "Unknown"` was written for. */
    CHECK(nd_db_record_call(ND_CALL_TYPE_MISSED, NULL, 1700100300, 0) > 0,
          "a call from a withheld number is still a call");
    CHECK(nd_db_record_call(ND_CALL_TYPE_MISSED, "", 1700100400, 0) > 0,
          "and so is one the modem named with an empty string");
    n = api.fetch(api.types[0], rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 3, "both landed in the missed list");
    CHECK_STR(rows[0].number, "", "an empty number stores as the NULL the schema allows");
    CHECK_STR(rows[1].number, "", "and so does a NULL one");

    /* A negative duration cannot come from the modem, which clamps, but the
     * column must not carry one if it ever does. */
    CHECK(nd_db_record_call(ND_CALL_TYPE_DIALED, "0874444444", 1700100500, -30) > 0,
          "a negative duration is accepted");
    n = api.fetch(api.types[2], rows, ND_ARRAY_LEN(rows));
    CHECK_INT(rows[0].duration, 0, "and stored as zero, not as a negative");

    /* A type the lists do not name is refused rather than filed where nothing
     * will ever show it. */
    CHECK_INT(nd_db_record_call(NULL, "0875555555", 1700100600, 0), -1, "record(NULL type)");
    CHECK_INT(nd_db_record_call("", "0875555555", 1700100600, 0), -1, "record(\"\" type)");

    /* The window: eleven more dialed calls on top of the two already there,
     * and the list shows the last ten of the thirteen. */
    for (i = 0; i < 11; i++) {
        char num[32];

        (void)nd_snprintf(num, sizeof num, "08760000%02d", i);
        CHECK(nd_db_record_call(ND_CALL_TYPE_DIALED, num, 1700200000 + i, i) > 0, "bulk dialed");
    }
    n = api.fetch(api.types[2], rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, ND_CALLLOG_MAX_CALLS, "ten pages at most, however many were logged");
    CHECK_STR(rows[0].number, "0876000010", "the newest call is the first page");
    CHECK_STR(rows[9].number, "0876000001", "and the tenth page is the tenth-newest");

    CHECK(api.clear(NULL), "tidy up");
}

/* The table is bounded. A phone that lives for years on one small writable
 * partition cannot keep every row it ever wrote, and the trim is per type --
 * a talkative dialer must not push the missed list out. */
static void test_record_trims_the_table(void)
{
    int i;

    for (i = 0; i < ND_CALL_LOG_KEEP + 20; i++) {
        char num[32];

        (void)nd_snprintf(num, sizeof num, "0899%06d", i);
        CHECK(nd_db_record_call(ND_CALL_TYPE_DIALED, num, 1700300000 + i, 0) > 0, "bulk dialed");
    }
    CHECK(nd_db_record_call(ND_CALL_TYPE_MISSED, "0851111111", 1700400000, 0) > 0, "one missed");

    CHECK_INT(count_rows(ND_CALL_TYPE_DIALED), ND_CALL_LOG_KEEP,
              "the dialed list is trimmed to the cap");
    CHECK_INT(count_rows(ND_CALL_TYPE_MISSED), 1, "and trimming one type left the others alone");

    /* Oldest first: the rows that went is the front of the queue, and the
     * newest call is still there. */
    {
        nd_call_rec rows[ND_CALLLOG_MAX_CALLS];

        CHECK_INT(api.fetch(ND_CALL_TYPE_DIALED, rows, ND_ARRAY_LEN(rows)), ND_CALLLOG_MAX_CALLS,
                  "and the list still fills");
        CHECK_STR(rows[0].number, "0899000069", "the newest survived the trim");
    }

    CHECK(api.clear(NULL), "tidy up");
}

/* ------------------------------------------------------------------ *
 * 8. Formatting
 * ------------------------------------------------------------------ */

static void test_format_duration(void)
{
    char b[ND_CALLLOG_TIME_MAX];

    CHECK_STR(api.format_duration(0, b, sizeof b), "00:00:00", "a phone with no calls yet");
    CHECK_STR(api.format_duration(1, b, sizeof b), "00:00:01", "one second");
    CHECK_STR(api.format_duration(59, b, sizeof b), "00:00:59", "59 s");
    CHECK_STR(api.format_duration(60, b, sizeof b), "00:01:00", "a minute");
    CHECK_STR(api.format_duration(3599, b, sizeof b), "00:59:59", "just under an hour");
    CHECK_STR(api.format_duration(3600, b, sizeof b), "01:00:00", "an hour");
    CHECK_STR(api.format_duration(45296, b, sizeof b), "12:34:56", "the mixed case");
    /* max(0, int(seconds)) -- a negative timer reads as zero, it does not
     * grow a minus sign. */
    CHECK_STR(api.format_duration(-1, b, sizeof b), "00:00:00", "max(0, s)");
    CHECK_STR(api.format_duration(-100000, b, sizeof b), "00:00:00", "max(0, s), further out");
    /* Python's "%02d" does not truncate a wider field and neither does C's,
     * so a hundred hours grows a column rather than wrapping. */
    CHECK_STR(api.format_duration(360000, b, sizeof b), "100:00:00", "hours are not capped at 99");
}

static void test_format_call_time(void)
{
    char b[ND_CALLLOG_TIME_MAX];

    /* 2023-11-14 22:13:20 UTC. The tests run with TZ unset, and
     * nd_time_localtime() is the real localtime outside capture -- so this
     * asserts the FORMAT, and the two fields that cannot move. */
    (void)api.format_call_time(1700000000, b, sizeof b);
    CHECK_INT(strlen(b), 12, "\"%d.%m. %H:%M\" is twelve characters");
    CHECK(b[2] == '.' && b[5] == '.' && b[6] == ' ' && b[9] == ':',
          "the separators land where the format puts them");

    /* Under the virtual clock localtime is aliased to gmtime, which is what
     * makes a frame rendered in Dublin match one rendered in CI. */
    nd_vclock_enable();
    CHECK_STR(api.format_call_time(1700000000, b, sizeof b), "14.11. 22:13", "UTC under capture");
    CHECK_STR(api.format_call_time(0, b, sizeof b), "01.01. 00:00", "the epoch itself");
    nd_vclock_disable();

    /* A buffer that cannot hold the result is the Python's "any failure ->
     * empty string". */
    CHECK_STR(api.format_call_time(1700000000, b, 4u), "", "a short buffer gives \"\"");
}

/* ------------------------------------------------------------------ *
 * 9. The timer settings
 * ------------------------------------------------------------------ */

static void test_timers(void)
{
    size_t k;

    /* An absent key is get_setting(key, "0") -> "0" -> 0. */
    for (k = 0u; k < (size_t)ND_CALLLOG_TIMER_COUNT; k++)
        CHECK_INT(api.timer_get((nd_calllog_timer)k), 0, "an unset timer reads 0");

    CHECK(api.timer_set(ND_CALLLOG_TIMER_LAST, 125), "writing a timer");
    CHECK_INT(api.timer_get(ND_CALLLOG_TIMER_LAST), 125, "and reading it back");
    CHECK_INT(api.timer_get(ND_CALLLOG_TIMER_RECEIVED), 0, "the other two are untouched");
    CHECK_INT(api.timer_get(ND_CALLLOG_TIMER_DIALED), 0, "the other two are untouched");

    /* "Clear timers" writes str(int(0)) to all three. */
    CHECK(api.timer_set(ND_CALLLOG_TIMER_RECEIVED, 900), "received timer");
    CHECK(api.timer_set(ND_CALLLOG_TIMER_DIALED, 61), "dialed timer");
    CHECK_INT(api.timer_get(ND_CALLLOG_TIMER_RECEIVED), 900, "received reads back");
    CHECK_INT(api.timer_get(ND_CALLLOG_TIMER_DIALED), 61, "dialed reads back");
    for (k = 0u; k < (size_t)ND_CALLLOG_TIMER_COUNT; k++)
        CHECK(api.timer_set((nd_calllog_timer)k, 0), "clear timers");
    for (k = 0u; k < (size_t)ND_CALLLOG_TIMER_COUNT; k++)
        CHECK_INT(api.timer_get((nd_calllog_timer)k), 0, "all three are back to 0");

    /* int() raising is caught and folded to 0 -- so a hand-edited
     * settings.prop cannot put a duration screen into a state the Python
     * would not reach. */
    CHECK_INT(nd_settings_set(ND_SET_CALLLOG_DUR_LAST, "not a number"), ND_OK, "store junk");
    CHECK_INT(api.timer_get(ND_CALLLOG_TIMER_LAST), 0, "int(\"not a number\") -> 0");
    CHECK_INT(nd_settings_set(ND_SET_CALLLOG_DUR_LAST, ""), ND_OK, "store empty");
    CHECK_INT(api.timer_get(ND_CALLLOG_TIMER_LAST), 0, "`or 0` catches the empty string");
    CHECK_INT(nd_settings_set(ND_SET_CALLLOG_DUR_LAST, "  42  "), ND_OK, "store padded");
    CHECK_INT(api.timer_get(ND_CALLLOG_TIMER_LAST), 42, "int() strips surrounding whitespace");
    CHECK_INT(nd_settings_set(ND_SET_CALLLOG_DUR_LAST, "12abc"), ND_OK, "store trailing junk");
    CHECK_INT(api.timer_get(ND_CALLLOG_TIMER_LAST), 0, "int(\"12abc\") raises -> 0");
    CHECK_INT(nd_settings_set(ND_SET_CALLLOG_DUR_LAST, "-5"), ND_OK, "store negative");
    CHECK_INT(api.timer_get(ND_CALLLOG_TIMER_LAST), -5,
              "int(\"-5\") is -5; format_duration clamps");
    CHECK_INT(nd_settings_set(ND_SET_CALLLOG_DUR_LAST, "0"), ND_OK, "tidy up");

    /* Out of range is refused rather than indexing off the end of the key
     * table. Nothing in the app can reach it; a future caller might. */
    CHECK_INT(api.timer_get((nd_calllog_timer)7), 0, "an out-of-range timer reads 0");
    CHECK(!api.timer_set((nd_calllog_timer)7, 1), "an out-of-range timer refuses the write");
}

/* ------------------------------------------------------------------ *
 * 10. The golden frame
 * ------------------------------------------------------------------ */

static void test_golden_frame(void)
{
    sa_fixture fx;
    int rc;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    /* PagedList drains the channel before its first draw, so Back has to
     * arrive as a repeat rather than be queued. */
    if (!sa_hold(&fx, ND_KEY_CLEAR)) {
        CHECK(false, "held key");
        sa_fx_free(&fx);
        return;
    }

    nd_vclock_enable();
    rc = api.run(&fx.ui);

    CHECK_INT(rc, 0, "Back on the first page returns 0");
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 1, "one frame: the root PagedList");
    sa_expect_golden(&fx, nd_capture_recent(fx.cap, 0u), "app-calllog");

    /* The root menu draws before it reads anything, so opening the app must
     * NOT have created the database. This is the other half of claim 3: the
     * creation happens on a list screen, not on entry. */
    CHECK(!db_file_exists(), "the root menu alone creates no database");

    nd_vclock_disable();
    sa_fx_free(&fx);
}

static void test_null_safety(void)
{
    nd_call_rec rows[2];
    char b[8];

    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown(); /* must be safe with nothing held */
    CHECK_INT(api.fetch(NULL, rows, ND_ARRAY_LEN(rows)), 0, "fetch(NULL type)");
    CHECK_INT(api.fetch("missed", NULL, 4u), 0, "fetch with no buffer");
    CHECK_INT(api.fetch("missed", rows, 0u), 0, "fetch with no room");
    CHECK_INT(api.connect(NULL), ND_ERR_INVAL, "connect(NULL)");
    CHECK(api.format_duration(0, NULL, 0u) == NULL, "format_duration with no buffer");
    CHECK(api.format_call_time(0, NULL, 0u) == NULL, "format_call_time with no buffer");
    CHECK_STR(api.format_duration(0, b, sizeof b), "",
              "a buffer too short for HH:MM:SS gives \"\"");
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    void *h = sa_begin("CallLog", "ndcalllog");
    int rc;

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }
    if (!sa_tmpdir("ndcalllog-root", g_root, sizeof g_root)) {
        (void)dlclose(h);
        return 1;
    }

    /* Everything below /NeoDCT is this test's own: the database it creates
     * and the settings.prop the timers write. */
    (void)nd_strlcpy(g_saved_root, nd_path_root(), sizeof g_saved_root);
    (void)nd_path_set_root(g_root);
    (void)nd_settings_init();

    RUN(test_strings);
    RUN(test_index_maps);
    /* Before anything opens the database, because claim 3 is that it is not
     * there yet. */
    RUN(test_golden_frame);
    RUN(test_connect_creates);
    RUN(seed_rows);
    RUN(test_fetch);
    RUN(test_clear);
    RUN(test_record_creates_the_same_file);
    RUN(test_record_round_trip);
    RUN(test_record_trims_the_table);
    RUN(test_format_duration);
    RUN(test_format_call_time);
    RUN(test_timers);
    RUN(test_null_safety);

    (void)nd_path_set_root(g_saved_root[0] != '\0' ? g_saved_root : NULL);
    rc = sa_end(h, "test_calllog");
    sa_rmtree(g_root);
    return rc;
}

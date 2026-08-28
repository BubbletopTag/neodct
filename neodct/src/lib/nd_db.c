/* nd_db.c -- the four sqlite databases: contacts, SMS in, SMS out, call log.
 *
 * The schema strings below are byte-exact copies of the Python's CREATE TABLE
 * text, whitespace and trailing spaces included. That is not fussiness:
 * sqlite stores the statement verbatim in sqlite_master, so a reformatted
 * schema is visible in a `.dump` of a phone that has been upgraded, and the
 * two builds would no longer produce identical files from the same history.
 *
 * ============ THREE THINGS THAT LOOK LIKE BUGS AND ARE NOT ============
 *
 *   1. call_log.db has no "PRAGMA journal_mode=WAL" while the other three do.
 *      journal_mode is persisted in the database header, so the call log
 *      stays in rollback-journal mode forever. The asymmetry is reproduced.
 *
 *   2. init_databases() does not create call_log.db at all. It is created
 *      lazily by whoever touches it first -- the CallLog app on its first
 *      read, or nd_db_record_call() on the phone's first call. Reproduced.
 *
 *   3. PhoneBook consumes "SELECT * FROM contacts" POSITIONALLY as
 *      (id, name, number, speed_dial), so the column order in CREATE TABLE is
 *      load-bearing. test_db.c asserts PRAGMA table_info comes back in
 *      exactly that order.
 *
 * ============ CONNECTION POLICY ============
 *
 * Open, query, close -- per query, as the Python does. That is both 1:1 and
 * right for the memory budget: a held connection keeps a page cache and, on
 * WAL, an -shm mapping resident, and no screen queries often enough to care.
 *
 * ============ SQL ============
 *
 * sqlite3_prepare_v2 with bound parameters, always. Never a concatenated
 * string, not even for an integer, not even from trusted input. sqlite3_exec
 * is used only for the fixed CREATE TABLE and PRAGMA text, which contains no
 * caller data at all.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

#include "nd_db.h"
#include "nd_log.h"
#include "nd_paths.h"

/* Verbatim from System/core/main.py init_databases(). The trailing spaces
 * after the commas in the contacts schema are in the original; leave them. */
const char *const ND_SCHEMA_CONTACTS =
    "CREATE TABLE IF NOT EXISTS contacts\n"
    "                     (id INTEGER PRIMARY KEY AUTOINCREMENT, \n"
    "                      name TEXT, \n"
    "                      number TEXT, \n"
    "                      speed_dial INTEGER)";

const char *const ND_SCHEMA_INBOX = "CREATE TABLE IF NOT EXISTS inbox\n"
                                    "                     (id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
                                    "                      message TEXT,\n"
                                    "                      sender TEXT,\n"
                                    "                      timestamp INTEGER,\n"
                                    "                      is_read INTEGER DEFAULT 0)";

const char *const ND_SCHEMA_OUTBOX = "CREATE TABLE IF NOT EXISTS outbox\n"
                                     "                     (id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
                                     "                      message TEXT,\n"
                                     "                      timestamp INTEGER)";

/* From System/apps/CallLog/main.py _connect(). Different indentation from the
 * three above because it was written in a different file; that difference is
 * in the on-disk schema of every phone shipped so far. */
const char *const ND_SCHEMA_CALLS =
    "CREATE TABLE IF NOT EXISTS calls\n"
    "           (id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
    "            type TEXT,             -- 'missed' | 'received' | 'dialed'\n"
    "            number TEXT,\n"
    "            timestamp INTEGER,\n"
    "            duration INTEGER DEFAULT 0)";

nd_err nd_db_open(const char *path, struct sqlite3 **out)
{
    char resolved[ND_PATH_MAX];
    sqlite3 *db = NULL;
    nd_err rc;

    if (path == NULL || out == NULL)
        return ND_ERR_INVAL;
    *out = NULL;

    rc = nd_path_resolve(resolved, sizeof resolved, path);
    if (rc != ND_OK)
        return rc;

    if (sqlite3_open_v2(resolved, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) !=
        SQLITE_OK) {
        /* sqlite hands back a handle even on failure so the error can be
         * read off it; closing it is the caller's job and we are the caller. */
        nd_log_err(ND_LOG_CORE, "sqlite open %s: %s", resolved,
                   db != NULL ? sqlite3_errmsg(db) : "out of memory");
        sqlite3_close(db);
        return ND_ERR_IO;
    }

    *out = db;
    return ND_OK;
}

void nd_db_close(struct sqlite3 *db)
{
    if (db != NULL)
        (void)sqlite3_close(db);
}

/* Opens a database that must already exist. Used by the read paths, where a
 * missing file is an empty result and not an error -- creating it there would
 * leave a stray zero-byte inbox behind on a phone that has never had a text. */
static nd_err open_existing(const char *path, sqlite3 **out)
{
    char resolved[ND_PATH_MAX];
    sqlite3 *db = NULL;
    nd_err rc;

    *out = NULL;
    if (!nd_path_is_file(path))
        return ND_ERR_NOTFOUND;

    rc = nd_path_resolve(resolved, sizeof resolved, path);
    if (rc != ND_OK)
        return rc;

    if (sqlite3_open_v2(resolved, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return ND_ERR_IO;
    }
    *out = db;
    return ND_OK;
}

static nd_err exec_sql(sqlite3 *db, const char *sql)
{
    char *msg = NULL;

    if (sqlite3_exec(db, sql, NULL, NULL, &msg) != SQLITE_OK) {
        nd_log_err(ND_LOG_CORE, "sqlite: %s", msg != NULL ? msg : "unknown error");
        sqlite3_free(msg);
        return ND_ERR_IO;
    }
    return ND_OK;
}

/* One database's worth of init: open, optionally WAL, create the table. */
static nd_err init_one(const char *path, const char *schema, bool wal, sqlite3 **keep_open)
{
    sqlite3 *db = NULL;
    nd_err rc = nd_db_open(path, &db);

    if (rc != ND_OK)
        return rc;

    /* WAL hardening so the database is not corrupted by a power loss
     * mid-write. journal_mode lives in the file header, so every later
     * connection picks it up without asking. */
    if (wal)
        rc = exec_sql(db, "PRAGMA journal_mode=WAL");
    if (rc == ND_OK)
        rc = exec_sql(db, schema);

    if (rc != ND_OK || keep_open == NULL) {
        nd_db_close(db);
        return rc;
    }
    *keep_open = db;
    return ND_OK;
}

static nd_err seed_contacts(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    nd_err rc = ND_OK;
    int64_t count = 0;

    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM contacts", -1, &st, NULL) != SQLITE_OK) {
        rc = ND_ERR_IO;
        goto done;
    }
    if (sqlite3_step(st) == SQLITE_ROW)
        count = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    st = NULL;

    if (count != 0)
        goto done;

    nd_log(ND_LOG_CORE, "Seeding default contacts...");

    if (sqlite3_prepare_v2(db, "INSERT INTO contacts (name, number, speed_dial) VALUES (?, ?, ?)",
                           -1, &st, NULL) != SQLITE_OK) {
        rc = ND_ERR_IO;
        goto done;
    }
    (void)sqlite3_bind_text(st, 1, "NeoDCT Support", -1, SQLITE_STATIC);
    (void)sqlite3_bind_text(st, 2, "555-1234", -1, SQLITE_STATIC);
    (void)sqlite3_bind_int(st, 3, 2);
    if (sqlite3_step(st) != SQLITE_DONE)
        rc = ND_ERR_IO;

done:
    if (st != NULL)
        sqlite3_finalize(st);
    return rc;
}

nd_err nd_db_init_all(void)
{
    sqlite3 *pb = NULL;
    nd_err rc;

    if (!nd_path_is_dir(ND_PATH_DB_DIR)) {
        nd_log(ND_LOG_CORE, "Creating User DB directory: %s", ND_PATH_DB_DIR);
        rc = nd_mkdir_p(ND_PATH_DB_DIR, 0755u);
        if (rc != ND_OK)
            return rc;
    }

    rc = init_one(ND_PATH_DB_PHONEBOOK, ND_SCHEMA_CONTACTS, true, &pb);
    if (rc != ND_OK)
        return rc;
    rc = seed_contacts(pb);
    nd_db_close(pb);
    if (rc != ND_OK)
        return rc;

    rc = init_one(ND_PATH_DB_SMS_INBOX, ND_SCHEMA_INBOX, true, NULL);
    if (rc != ND_OK)
        return rc;
    rc = init_one(ND_PATH_DB_SMS_OUTBOX, ND_SCHEMA_OUTBOX, true, NULL);
    if (rc != ND_OK)
        return rc;

    /* call_log.db is deliberately absent from this list. See the header. */

    nd_log(ND_LOG_CORE, "Databases initialized successfully.");
    return ND_OK;
}

int64_t nd_db_store_incoming_sms(const char *sender, const char *body)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int64_t row_id = -1;

    if (sender == NULL || body == NULL)
        return -1;
    if (nd_db_open(ND_PATH_DB_SMS_INBOX, &db) != ND_OK)
        return -1;

    /* The Python re-creates the table here even though init_databases()
     * already did. Keep it: an SMS arriving before the first boot finished,
     * or after a user wiped the database from the engineering menu, is
     * otherwise lost. */
    if (exec_sql(db, ND_SCHEMA_INBOX) != ND_OK)
        goto done;

    if (sqlite3_prepare_v2(db,
                           "INSERT INTO inbox (message, sender, timestamp, is_read) "
                           "VALUES (?, ?, ?, 0)",
                           -1, &st, NULL) != SQLITE_OK)
        goto done;

    (void)sqlite3_bind_text(st, 1, body, -1, SQLITE_STATIC);
    (void)sqlite3_bind_text(st, 2, sender, -1, SQLITE_STATIC);
    (void)sqlite3_bind_int64(st, 3, (sqlite3_int64)time(NULL));

    if (sqlite3_step(st) != SQLITE_DONE)
        goto done;

    row_id = (int64_t)sqlite3_last_insert_rowid(db);
    nd_log(ND_LOG_NOTIFY, "SMS stored (id %lld) from %s", (long long)row_id, sender);

done:
    if (st != NULL)
        sqlite3_finalize(st);
    nd_db_close(db);
    return row_id;
}

int32_t nd_db_count_unread_sms(void)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int32_t n = 0;

    /* A missing inbox is an empty inbox, not a failure -- the Python wraps
     * the whole thing in `except Exception: return 0`. */
    if (open_existing(ND_PATH_DB_SMS_INBOX, &db) != ND_OK)
        return 0;

    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM inbox WHERE is_read = 0", -1, &st, NULL) ==
        SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            n = sqlite3_column_int(st, 0);
    }

    if (st != NULL)
        sqlite3_finalize(st);
    nd_db_close(db);
    return n;
}

/* ------------------------------------------------------------------ *
 * The call log
 * ------------------------------------------------------------------ */

/* The app's own _connect(), rewritten from this side: makedirs, open, CREATE
 * TABLE IF NOT EXISTS, and NOTHING ELSE. In particular no journal_mode pragma
 * -- nd_db.h point 1. The mode is baked into the file header by whoever
 * creates it, and on a phone that takes a call before anybody opens the Call
 * log menu, that is this function. */
static nd_err calls_connect(sqlite3 **out)
{
    sqlite3 *db = NULL;
    nd_err rc;

    *out = NULL;

    rc = nd_mkdir_p(ND_PATH_DB_DIR, 0755u);
    if (rc != ND_OK)
        return rc;

    rc = nd_db_open(ND_PATH_DB_CALL_LOG, &db);
    if (rc != ND_OK)
        return rc;

    if (exec_sql(db, ND_SCHEMA_CALLS) != ND_OK) {
        nd_db_close(db);
        return ND_ERR_IO;
    }

    *out = db;
    return ND_OK;
}

int64_t nd_db_record_call(const char *type, const char *number, int64_t timestamp, int64_t duration)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int64_t row_id = -1;

    if (type == NULL || type[0] == '\0')
        return -1;

    if (calls_connect(&db) != ND_OK) {
        nd_log_err(ND_LOG_CALLLOG, "DB write failed: cannot open %s", ND_PATH_DB_CALL_LOG);
        return -1;
    }

    if (sqlite3_prepare_v2(db,
                           "INSERT INTO calls (type, number, timestamp, duration) "
                           "VALUES (?, ?, ?, ?)",
                           -1, &st, NULL) != SQLITE_OK) {
        nd_log_err(ND_LOG_CALLLOG, "DB write failed: %s", sqlite3_errmsg(db));
        goto done;
    }

    (void)sqlite3_bind_text(st, 1, type, -1, SQLITE_STATIC);
    /* A withheld or unknown caller ID binds as NULL rather than "", because
     * that is the column the app's `number or "Unknown"` was written for and
     * the one an existing phone's rows would have. */
    if (number != NULL && number[0] != '\0')
        (void)sqlite3_bind_text(st, 2, number, -1, SQLITE_STATIC);
    else
        (void)sqlite3_bind_null(st, 2);
    (void)sqlite3_bind_int64(st, 3, (sqlite3_int64)timestamp);
    (void)sqlite3_bind_int64(st, 4, (sqlite3_int64)((duration > 0) ? duration : 0));

    if (sqlite3_step(st) != SQLITE_DONE) {
        nd_log_err(ND_LOG_CALLLOG, "DB write failed: %s", sqlite3_errmsg(db));
        goto done;
    }

    row_id = (int64_t)sqlite3_last_insert_rowid(db);
    nd_log(ND_LOG_CALLLOG, "Logged %s call (id %lld) %s", type, (long long)row_id,
           (number != NULL && number[0] != '\0') ? number : "unknown");

    /* Trim this type back to ND_CALL_LOG_KEEP, newest kept. A failure here is
     * NOT a failure of the call: the row is already in and the caller is told
     * so. It is logged and the table is simply one row longer than it should
     * be until the next call trims it. */
    (void)sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(db,
                           "DELETE FROM calls WHERE type=? AND id NOT IN "
                           "(SELECT id FROM calls WHERE type=? ORDER BY id DESC LIMIT ?)",
                           -1, &st, NULL) != SQLITE_OK) {
        nd_log_err(ND_LOG_CALLLOG, "DB trim failed: %s", sqlite3_errmsg(db));
        goto done;
    }
    (void)sqlite3_bind_text(st, 1, type, -1, SQLITE_STATIC);
    (void)sqlite3_bind_text(st, 2, type, -1, SQLITE_STATIC);
    (void)sqlite3_bind_int(st, 3, ND_CALL_LOG_KEEP);
    if (sqlite3_step(st) != SQLITE_DONE)
        nd_log_err(ND_LOG_CALLLOG, "DB trim failed: %s", sqlite3_errmsg(db));

done:
    if (st != NULL)
        sqlite3_finalize(st);
    nd_db_close(db);
    return row_id;
}

/* ------------------------------------------------------------------ *
 * Contacts
 * ------------------------------------------------------------------ */

static void copy_column(char *dst, size_t dst_sz, sqlite3_stmt *st, int col)
{
    const unsigned char *text = sqlite3_column_text(st, col);

    (void)nd_strlcpy(dst, text != NULL ? (const char *)text : "", dst_sz);
}

size_t nd_contacts_query(const char *search, nd_contact *out, size_t max)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    size_t n = 0u;
    char like[ND_CONTACT_NAME_MAX + 4];

    if (out == NULL || max == 0u)
        return 0u;
    if (open_existing(ND_PATH_DB_PHONEBOOK, &db) != ND_OK)
        return 0u;

    /* SELECT * is what the Python issues and PhoneBook reads the columns
     * positionally, so the column list is spelled out here instead: same
     * columns, same order, and immune to a future ALTER TABLE. */
    if (search != NULL && search[0] != '\0') {
        if (sqlite3_prepare_v2(db,
                               "SELECT id, name, number, speed_dial FROM contacts "
                               "WHERE name LIKE ? ORDER BY name ASC",
                               -1, &st, NULL) != SQLITE_OK)
            goto done;
        if (nd_snprintf(like, sizeof like, "%%%s%%", search) != ND_OK)
            goto done;
        (void)sqlite3_bind_text(st, 1, like, -1, SQLITE_STATIC);
    } else {
        if (sqlite3_prepare_v2(db,
                               "SELECT id, name, number, speed_dial FROM contacts "
                               "ORDER BY name ASC",
                               -1, &st, NULL) != SQLITE_OK)
            goto done;
    }

    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        out[n].id = (int64_t)sqlite3_column_int64(st, 0);
        copy_column(out[n].name, sizeof out[n].name, st, 1);
        copy_column(out[n].number, sizeof out[n].number, st, 2);
        out[n].speed_dial = sqlite3_column_int(st, 3);
        n++;
    }

done:
    if (st != NULL)
        sqlite3_finalize(st);
    nd_db_close(db);
    return n;
}

/* Keeps only the ASCII digits, so a stored "555-1234" still matches a
 * "+15551234" caller ID. Returns the length written. */
static size_t digits_only(const char *s, char *out, size_t out_sz)
{
    size_t w = 0u;

    if (s == NULL) {
        out[0] = '\0';
        return 0u;
    }
    while (*s != '\0' && w + 1u < out_sz) {
        if (isdigit((unsigned char)*s))
            out[w++] = *s;
        s++;
    }
    out[w] = '\0';
    return w;
}

/* The last n characters of s, or all of s when it is shorter -- Python's
 * s[-10:], which does not raise on a short string. */
static const char *tail(const char *s, size_t len, size_t n)
{
    return len > n ? s + (len - n) : s;
}

bool nd_contacts_lookup_name(const char *number, char *out, size_t out_sz)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    char wanted[ND_CONTACT_NUMBER_MAX];
    size_t wanted_len;
    bool found = false;

    if (number == NULL || out == NULL || out_sz == 0u)
        return false;

    wanted_len = digits_only(number, wanted, sizeof wanted);
    if (wanted_len == 0u)
        return false;

    if (open_existing(ND_PATH_DB_PHONEBOOK, &db) != ND_OK)
        return false;
    if (sqlite3_prepare_v2(db, "SELECT name, number FROM contacts ORDER BY name ASC", -1, &st,
                           NULL) != SQLITE_OK)
        goto done;

    while (!found && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *raw = sqlite3_column_text(st, 1);
        char stored[ND_CONTACT_NUMBER_MAX];
        size_t stored_len =
            digits_only(raw != NULL ? (const char *)raw : "", stored, sizeof stored);

        if (stored_len == 0u)
            continue;

        /* Compare the last ten digits: country codes and trunk prefixes
         * differ between the SIM's caller ID and what the user typed in. */
        if (strcmp(tail(stored, stored_len, 10u), tail(wanted, wanted_len, 10u)) == 0 ||
            strcmp(stored, wanted) == 0) {
            copy_column(out, out_sz, st, 0);
            found = true;
        }
    }

done:
    if (st != NULL)
        sqlite3_finalize(st);
    nd_db_close(db);
    return found;
}

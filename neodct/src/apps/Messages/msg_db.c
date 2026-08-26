/* msg_db.c -- the seven statements Messages runs against sms_inbox.db and
 * sms_outbox.db.
 *
 * System/apps/Messages/main.py opens a connection per query with
 * sqlite3.connect / cursor / execute / (commit) / close, which is exactly
 * nd_db.h's connection policy. Each function here does the same: open, one
 * prepared statement, close. No handle is held between screens.
 *
 * ============ THE EXISTENCE CHECK IS PART OF THE BEHAVIOUR ============
 *
 * Every read in the Python starts with `if not os.path.exists(DB): return []`.
 * That is not defensiveness, it is what stops a phone that has never received
 * a text from growing a zero-byte sms_inbox.db the first time somebody opens
 * the Inbox -- sqlite3.connect() creates the file. The same guard is here,
 * spelled nd_path_is_file(), and it applies to the writes too: _mark_read and
 * both deletes return without doing anything when their database is absent.
 *
 * _save_outbox_message() is the one exception. It creates the directory and
 * the table on purpose, because saving a draft is the operation that brings
 * the outbox into existence.
 *
 * ============ NO WAL ON THE APP'S OWN CREATE ============
 *
 * The core's init_databases() creates both tables with PRAGMA
 * journal_mode=WAL. _save_outbox_message() re-creates the outbox table
 * WITHOUT that pragma. On a phone the core has already booted this is
 * invisible -- journal_mode is persisted in the file header and the table
 * already exists -- but the asymmetry is real and is reproduced rather than
 * tidied away, for the same reason nd_db.c reproduces the call log's.
 */

#include <string.h>

#include <sqlite3.h>

#include "nd_db.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_vclock.h"

#include "messages.h"

/* The Python's DB_DIR / INBOX_DB / OUTBOX_DB are ND_PATH_DB_DIR,
 * ND_PATH_DB_SMS_INBOX and ND_PATH_DB_SMS_OUTBOX -- byte for byte the same
 * absolute paths, already declared once in nd_paths.h. */

/* sqlite3_column_text() returns NULL for a NULL column; every one of these
 * columns is nullable, because the Python schema declares no NOT NULL. */
static void take_text(char *dst, size_t dst_sz, sqlite3_stmt *st, int col)
{
    const unsigned char *s = sqlite3_column_text(st, col);

    (void)nd_strlcpy(dst, (s != NULL) ? (const char *)s : "", dst_sz);
}

/* Open a database that must already be there. Read-write, because two of the
 * three callers of this go on to UPDATE or DELETE, and because
 * sqlite3.connect() opens read-write. NULL when the file is absent. */
static sqlite3 *open_existing(const char *path)
{
    sqlite3 *db = NULL;

    if (!nd_path_is_file(path))
        return NULL;
    if (nd_db_open(path, &db) != ND_OK)
        return NULL;
    return db;
}

/* One statement with no result rows, run and discarded. Silent on failure:
 * the Python wraps none of these in a try, so a failure there would unwind
 * out of the app -- and OPEN-QUESTIONS.md MSG-3 records why the C logs
 * nothing and carries on instead. */
static void run_void(const char *path, const char *sql, int64_t id)
{
    sqlite3 *db = open_existing(path);
    sqlite3_stmt *st = NULL;

    if (db == NULL)
        return;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        (void)sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
        (void)sqlite3_step(st);
    }
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);
}

size_t nd_msg_fetch_inbox(nd_msg_rec *out, size_t max)
{
    sqlite3 *db;
    sqlite3_stmt *st = NULL;
    size_t n = 0u;

    if (out == NULL || max == 0u)
        return 0u;

    db = open_existing(ND_PATH_DB_SMS_INBOX);
    if (db == NULL)
        return 0u;

    if (sqlite3_prepare_v2(db,
                           "SELECT id, message, sender, timestamp, is_read FROM inbox "
                           "ORDER BY timestamp DESC",
                           -1, &st, NULL) == SQLITE_OK) {
        while (n < max && sqlite3_step(st) == SQLITE_ROW) {
            nd_msg_rec *r = &out[n];

            memset(r, 0, sizeof *r);
            r->id = (int64_t)sqlite3_column_int64(st, 0);
            take_text(r->message, sizeof r->message, st, 1);
            take_text(r->sender, sizeof r->sender, st, 2);
            r->timestamp = (int64_t)sqlite3_column_int64(st, 3);
            r->is_read = (int32_t)sqlite3_column_int(st, 4);
            n++;
        }
    }
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);
    return n;
}

static void ensure_recipient_column(sqlite3 *db);

size_t nd_msg_fetch_outbox(nd_msg_rec *out, size_t max)
{
    sqlite3 *db;
    sqlite3_stmt *st = NULL;
    size_t n = 0u;

    if (out == NULL || max == 0u)
        return 0u;

    db = open_existing(ND_PATH_DB_SMS_OUTBOX);
    if (db == NULL)
        return 0u;

    /* The recipient is selected with a COALESCE rather than by asking whether
     * the column exists: on a phone whose outbox predates it the ALTER has
     * not run yet, and a SELECT naming a missing column fails the whole
     * statement. So the column is added here first -- reading the outbox is
     * also the moment to bring it up to date. */
    ensure_recipient_column(db);

    if (sqlite3_prepare_v2(db,
                           "SELECT id, message, timestamp, COALESCE(recipient, '') "
                           "FROM outbox ORDER BY timestamp DESC",
                           -1, &st, NULL) == SQLITE_OK) {
        while (n < max && sqlite3_step(st) == SQLITE_ROW) {
            nd_msg_rec *r = &out[n];

            memset(r, 0, sizeof *r);
            r->id = (int64_t)sqlite3_column_int64(st, 0);
            take_text(r->message, sizeof r->message, st, 1);
            r->timestamp = (int64_t)sqlite3_column_int64(st, 2);
            take_text(r->recipient, sizeof r->recipient, st, 3);
            n++;
        }
    }
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);
    return n;
}

bool nd_msg_fetch_inbox_one(int64_t id, nd_msg_rec *out)
{
    sqlite3 *db;
    sqlite3_stmt *st = NULL;
    bool found = false;

    if (out == NULL)
        return false;

    db = open_existing(ND_PATH_DB_SMS_INBOX);
    if (db == NULL)
        return false;

    if (sqlite3_prepare_v2(db,
                           "SELECT id, message, sender, timestamp, is_read FROM inbox "
                           "WHERE id = ?",
                           -1, &st, NULL) == SQLITE_OK) {
        (void)sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            memset(out, 0, sizeof *out);
            out->id = (int64_t)sqlite3_column_int64(st, 0);
            take_text(out->message, sizeof out->message, st, 1);
            take_text(out->sender, sizeof out->sender, st, 2);
            out->timestamp = (int64_t)sqlite3_column_int64(st, 3);
            out->is_read = (int32_t)sqlite3_column_int(st, 4);
            found = true;
        }
    }
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);
    return found;
}

void nd_msg_mark_read(int64_t id)
{
    /* `if not os.path.exists(INBOX_DB) or message_id is None: return` -- the
     * second half is the negative id. */
    if (id < 0)
        return;
    run_void(ND_PATH_DB_SMS_INBOX, "UPDATE inbox SET is_read = 1 WHERE id = ?", id);
}

void nd_msg_delete_inbox(int64_t id)
{
    if (id < 0)
        return;
    run_void(ND_PATH_DB_SMS_INBOX, "DELETE FROM inbox WHERE id = ?", id);
}

void nd_msg_delete_outbox(int64_t id)
{
    if (id < 0)
        return;
    run_void(ND_PATH_DB_SMS_OUTBOX, "DELETE FROM outbox WHERE id = ?", id);
}

nd_err nd_msg_save_outbox(const char *text)
{
    return nd_msg_save_outbox_to(text, NULL);
}

/* ------------------------------------------------------------------ *
 * The recipient column -- NOT a port
 * ------------------------------------------------------------------ */

/* The Python's outbox is (id, message, timestamp): a sent message has no
 * record of who it went to, because _send_message_flow() asks for a number
 * and then throws it away. Chat style cannot attribute a sent message to a
 * conversation without one.
 *
 * sqlite has no ADD COLUMN IF NOT EXISTS, so the ALTER is simply run every
 * time and its "duplicate column name" is swallowed. Reading sqlite_master
 * back to decide first would be more code arriving at the same place, and
 * PRAGMA table_info would be more still.
 *
 * The column is nullable and NOT backfilled: rows written before it existed
 * genuinely have no recipient, and inventing one would be worse than showing
 * them under ND_MSG_PEER_UNKNOWN, which is what nd_msg_threads() does. */
static void ensure_recipient_column(sqlite3 *db)
{
    char *err = NULL;

    if (sqlite3_exec(db, "ALTER TABLE outbox ADD COLUMN recipient TEXT", NULL, NULL, &err) !=
        SQLITE_OK) {
        /* Already there is the expected case, not a failure. Anything else is
         * worth a line in the log, but never worth refusing to save a
         * message the user has already written. */
        if (err != NULL && strstr(err, "duplicate column") == NULL)
            nd_log(ND_LOG_MESSAGES, "outbox ALTER: %s", err);
    }
    sqlite3_free(err);
}

nd_err nd_msg_save_outbox_to(const char *text, const char *recipient)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    nd_err rc;
    int64_t now;

    if (text == NULL)
        return ND_ERR_INVAL;

    /* os.makedirs(DB_DIR, exist_ok=True) */
    rc = nd_mkdir_p(ND_PATH_DB_DIR, 0755u);
    if (rc != ND_OK)
        return rc;

    /* sqlite3.connect() creates the file; nd_db_open() is the same call. */
    rc = nd_db_open(ND_PATH_DB_SMS_OUTBOX, &db);
    if (rc != ND_OK)
        return rc;

    /* Verbatim from _save_outbox_message. Deliberately NOT ND_SCHEMA_OUTBOX:
     * that string is the core's, with its own indentation, and the two are
     * different bytes in sqlite_master on a phone where this ran first. */
    if (sqlite3_exec(db,
                     "CREATE TABLE IF NOT EXISTS outbox\n"
                     "           (id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
                     "            message TEXT,\n"
                     "            timestamp INTEGER)",
                     NULL, NULL, NULL) != SQLITE_OK) {
        rc = ND_ERR_IO;
        goto done;
    }

    ensure_recipient_column(db);

    if (sqlite3_prepare_v2(db,
                           "INSERT INTO outbox (message, timestamp, recipient) VALUES (?, ?, ?)",
                           -1, &st, NULL) != SQLITE_OK) {
        rc = ND_ERR_IO;
        goto done;
    }
    /* int(time.time()) -- truncation toward zero, and the virtual clock under
     * capture so a saved draft has a reproducible timestamp. */
    now = (int64_t)nd_time_now();
    (void)sqlite3_bind_text(st, 1, text, -1, SQLITE_STATIC);
    (void)sqlite3_bind_int64(st, 2, (sqlite3_int64)now);
    /* NULL rather than "" for an absent recipient, so that a row saved by the
     * ported nd_msg_save_outbox() is indistinguishable from one written
     * before the column existed -- both are genuinely "we do not know". */
    if (recipient != NULL && recipient[0] != '\0')
        (void)sqlite3_bind_text(st, 3, recipient, -1, SQLITE_STATIC);
    else
        (void)sqlite3_bind_null(st, 3);
    if (sqlite3_step(st) != SQLITE_DONE)
        rc = ND_ERR_IO;

done:
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);
    return rc;
}

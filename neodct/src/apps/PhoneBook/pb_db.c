/* pb_db.c -- the three statements PhoneBook runs against contacts.
 *
 * System/apps/PhoneBook/main.py opens /NeoDCT/User/db/phonebook.db three
 * times, each with sqlite3.connect / cursor / execute / commit / close. That
 * is exactly nd_db.h's connection policy, so each of these does the same:
 * open, one prepared statement, close. No handle is held between screens.
 *
 * The TABLE IS NOT CREATED HERE. init_databases() in the core made it before
 * any app ran (spec-core-loop.md), and the Python app never issues a CREATE
 * either -- so on a phone whose database was deleted underneath it these
 * statements fail, and that failure is what the caller reports. Adding a
 * defensive CREATE TABLE would be a behaviour change, not a fix; the only
 * place the Python re-creates a table defensively is the incoming-SMS path,
 * and nd_db.c already reproduces that one.
 *
 * The speed_dial column is written as 0 on insert and never touched again --
 * "1-touch dialing" is a menu entry with no branch behind it.
 */

#include <string.h>

#include <sqlite3.h>

#include "nd_db.h"
#include "nd_paths.h"
#include "nd_types.h"

#include "phonebook.h"

static void clear_err(char *err, size_t err_sz)
{
    if (err != NULL && err_sz > 0u)
        err[0] = '\0';
}

/* sqlite3_errmsg() points into the handle, so it has to be copied out before
 * the handle closes. */
static void take_err(sqlite3 *db, char *err, size_t err_sz)
{
    if (err == NULL || err_sz == 0u)
        return;
    (void)nd_strlcpy(err, (db != NULL) ? sqlite3_errmsg(db) : "cannot open phonebook.db", err_sz);
}

/* One INSERT/UPDATE/DELETE, bound and stepped. Every caller here runs a
 * statement with no result rows, so SQLITE_DONE is the only success. */
static nd_err run_stmt(sqlite3 *db, sqlite3_stmt *st, char *err, size_t err_sz)
{
    if (sqlite3_step(st) != SQLITE_DONE) {
        take_err(db, err, err_sz);
        return ND_ERR_IO;
    }
    return ND_OK;
}

nd_err nd_phonebook_insert(const char *name, const char *number, char *err, size_t err_sz)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    nd_err rc;

    clear_err(err, err_sz);
    if (name == NULL || number == NULL)
        return ND_ERR_INVAL;

    rc = nd_db_open(ND_PATH_DB_PHONEBOOK, &db);
    if (rc != ND_OK) {
        take_err(NULL, err, err_sz);
        return rc;
    }

    if (sqlite3_prepare_v2(db, "INSERT INTO contacts (name, number, speed_dial) VALUES (?, ?, ?)",
                           -1, &st, NULL) != SQLITE_OK) {
        take_err(db, err, err_sz);
        rc = ND_ERR_IO;
        goto done;
    }
    (void)sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    (void)sqlite3_bind_text(st, 2, number, -1, SQLITE_STATIC);
    (void)sqlite3_bind_int(st, 3, 0);
    rc = run_stmt(db, st, err, err_sz);

done:
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);
    return rc;
}

nd_err nd_phonebook_update(int64_t id, const char *name, const char *number, char *err,
                           size_t err_sz)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    nd_err rc;

    clear_err(err, err_sz);
    if (name == NULL || number == NULL)
        return ND_ERR_INVAL;

    rc = nd_db_open(ND_PATH_DB_PHONEBOOK, &db);
    if (rc != ND_OK) {
        take_err(NULL, err, err_sz);
        return rc;
    }

    if (sqlite3_prepare_v2(db, "UPDATE contacts SET name=?, number=? WHERE id=?", -1, &st, NULL) !=
        SQLITE_OK) {
        take_err(db, err, err_sz);
        rc = ND_ERR_IO;
        goto done;
    }
    (void)sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    (void)sqlite3_bind_text(st, 2, number, -1, SQLITE_STATIC);
    (void)sqlite3_bind_int64(st, 3, (sqlite3_int64)id);
    rc = run_stmt(db, st, err, err_sz);

done:
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);
    return rc;
}

nd_err nd_phonebook_delete(int64_t id, char *err, size_t err_sz)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    nd_err rc;

    clear_err(err, err_sz);

    rc = nd_db_open(ND_PATH_DB_PHONEBOOK, &db);
    if (rc != ND_OK) {
        take_err(NULL, err, err_sz);
        return rc;
    }

    if (sqlite3_prepare_v2(db, "DELETE FROM contacts WHERE id=?", -1, &st, NULL) != SQLITE_OK) {
        take_err(db, err, err_sz);
        rc = ND_ERR_IO;
        goto done;
    }
    (void)sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
    rc = run_stmt(db, st, err, err_sz);

done:
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);
    return rc;
}

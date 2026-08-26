/* test_db.c -- schemas, the seed contact, and the two queries the core makes.
 *
 * init_databases() has no pytest at all; the schemas are only implicitly
 * covered by the apps that read them. That is the gap spec-storage-settings.md
 * asks the port to close, and these are the assertions it names:
 *
 *   - PRAGMA table_info returns exactly (id, name, number, speed_dial) in that
 *     order, because PhoneBook consumes SELECT * positionally (R-9);
 *   - the three databases init_databases() creates are in WAL and call_log.db
 *     is NOT, because journal_mode is persisted in the file header and the
 *     asymmetry is permanent;
 *   - call_log.db is not created by init at all.
 */

#include <stdio.h>
#include <string.h>

#include <sqlite3.h>

#include "nd_db.h"
#include "nd_paths.h"

#include "platform_test.h"

/* Runs a statement that yields one text column and copies it out. */
static bool one_text(const char *db_path, const char *sql, char *out, size_t out_sz)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    bool ok = false;

    out[0] = '\0';
    if (nd_db_open(db_path, &db) != ND_OK)
        return false;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);

        (void)nd_strlcpy(out, t != NULL ? (const char *)t : "", out_sz);
        ok = true;
    }
    if (st != NULL)
        sqlite3_finalize(st);
    nd_db_close(db);
    return ok;
}

static void test_init_creates_the_three_databases(void)
{
    CHECK_INT(nd_db_init_all(), ND_OK);

    CHECK(nd_path_is_dir(ND_PATH_DB_DIR));
    CHECK(nd_path_is_file(ND_PATH_DB_PHONEBOOK));
    CHECK(nd_path_is_file(ND_PATH_DB_SMS_INBOX));
    CHECK(nd_path_is_file(ND_PATH_DB_SMS_OUTBOX));
    /* Deliberately absent: the CallLog app creates it lazily. */
    CHECK(!nd_path_exists(ND_PATH_DB_CALL_LOG));

    /* Idempotent -- it runs on every boot. */
    CHECK_INT(nd_db_init_all(), ND_OK);
}

/* R-9: the column order is load-bearing because PhoneBook reads rows
 * positionally as (id, name, number, speed_dial). */
static void test_contacts_column_order_is_pinned(void)
{
    static const char *const want[4] = {"id", "name", "number", "speed_dial"};
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    size_t i = 0u;

    CHECK_INT(nd_db_init_all(), ND_OK);
    CHECK_INT(nd_db_open(ND_PATH_DB_PHONEBOOK, &db), ND_OK);
    CHECK_INT(sqlite3_prepare_v2(db, "PRAGMA table_info(contacts)", -1, &st, NULL), SQLITE_OK);

    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 1);

        CHECK(i < 4u);
        if (i < 4u)
            CHECK_STR(name != NULL ? (const char *)name : "", want[i]);
        i++;
    }
    CHECK_INT(i, 4);

    sqlite3_finalize(st);
    nd_db_close(db);
}

/* journal_mode lives in the database header, so this is not a property of the
 * connection -- it is a property of the file, forever. */
static void test_wal_is_set_on_three_databases_and_not_the_call_log(void)
{
    char mode[32];
    sqlite3 *db = NULL;

    CHECK_INT(nd_db_init_all(), ND_OK);

    CHECK(one_text(ND_PATH_DB_PHONEBOOK, "PRAGMA journal_mode", mode, sizeof mode));
    CHECK_STR(mode, "wal");
    CHECK(one_text(ND_PATH_DB_SMS_INBOX, "PRAGMA journal_mode", mode, sizeof mode));
    CHECK_STR(mode, "wal");
    CHECK(one_text(ND_PATH_DB_SMS_OUTBOX, "PRAGMA journal_mode", mode, sizeof mode));
    CHECK_STR(mode, "wal");

    /* Create the call log the way the CallLog app does: schema, no pragma. */
    CHECK_INT(nd_db_open(ND_PATH_DB_CALL_LOG, &db), ND_OK);
    CHECK_INT(sqlite3_exec(db, ND_SCHEMA_CALLS, NULL, NULL, NULL), SQLITE_OK);
    nd_db_close(db);

    CHECK(one_text(ND_PATH_DB_CALL_LOG, "PRAGMA journal_mode", mode, sizeof mode));
    CHECK_STR(mode, "delete");
}

static void test_the_seed_contact(void)
{
    nd_contact rows[8];
    size_t n;

    CHECK_INT(nd_db_init_all(), ND_OK);

    n = nd_contacts_query(NULL, rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 1);
    CHECK_STR(rows[0].name, "NeoDCT Support");
    CHECK_STR(rows[0].number, "555-1234");
    CHECK_INT(rows[0].speed_dial, 2);

    /* Seeded once, not once per boot. */
    CHECK_INT(nd_db_init_all(), ND_OK);
    CHECK_INT(nd_contacts_query(NULL, rows, ND_ARRAY_LEN(rows)), 1);
}

static void insert_contact(const char *name, const char *number)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;

    CHECK_INT(nd_db_open(ND_PATH_DB_PHONEBOOK, &db), ND_OK);
    CHECK_INT(sqlite3_prepare_v2(db,
                                 "INSERT INTO contacts (name, number, speed_dial) VALUES (?, ?, 0)",
                                 -1, &st, NULL),
              SQLITE_OK);
    (void)sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    (void)sqlite3_bind_text(st, 2, number, -1, SQLITE_STATIC);
    CHECK_INT(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);
    nd_db_close(db);
}

static void test_contacts_query_and_caller_id(void)
{
    nd_contact rows[8];
    char name[ND_CONTACT_NAME_MAX];

    CHECK_INT(nd_db_init_all(), ND_OK);
    insert_contact("Ada", "555-1234");
    insert_contact("Bob", "07700 900123");

    /* ORDER BY name ASC. */
    CHECK_INT(nd_contacts_query(NULL, rows, ND_ARRAY_LEN(rows)), 3);
    CHECK_STR(rows[0].name, "Ada");
    CHECK_STR(rows[1].name, "Bob");
    CHECK_STR(rows[2].name, "NeoDCT Support");

    /* LIKE '%q%', bound rather than concatenated -- the whole point of the
     * prepared statement is that this cannot become SQL. */
    CHECK_INT(nd_contacts_query("o", rows, ND_ARRAY_LEN(rows)), 2);
    CHECK_STR(rows[0].name, "Bob");
    CHECK_INT(nd_contacts_query("' OR 1=1 --", rows, ND_ARRAY_LEN(rows)), 0);

    /* Caller ID: digits only, and the last ten digits are enough, so a
     * stored "07700 900123" matches "+447700900123". */
    CHECK(nd_contacts_lookup_name("+447700900123", name, sizeof name));
    CHECK_STR(name, "Bob");
    /* A short stored number still matches itself exactly. */
    CHECK(nd_contacts_lookup_name("5551234", name, sizeof name));
    CHECK_STR(name, "Ada");
    CHECK(!nd_contacts_lookup_name("999", name, sizeof name));
    CHECK(!nd_contacts_lookup_name("", name, sizeof name));
    CHECK(!nd_contacts_lookup_name("no digits here", name, sizeof name));
}

static void test_incoming_sms_is_stored_and_counted(void)
{
    int64_t first;
    int64_t second;

    CHECK_INT(nd_db_init_all(), ND_OK);
    CHECK_INT(nd_db_count_unread_sms(), 0);

    first = nd_db_store_incoming_sms("+15551234", "hello");
    CHECK(first > 0);
    second = nd_db_store_incoming_sms("+15551234", "again");
    CHECK_INT(second, first + 1);

    CHECK_INT(nd_db_count_unread_sms(), 2);

    {
        sqlite3 *db = NULL;

        CHECK_INT(nd_db_open(ND_PATH_DB_SMS_INBOX, &db), ND_OK);
        CHECK_INT(sqlite3_exec(db, "UPDATE inbox SET is_read = 1 WHERE id = 1", NULL, NULL, NULL),
                  SQLITE_OK);
        nd_db_close(db);
    }
    CHECK_INT(nd_db_count_unread_sms(), 1);
}

/* A missing inbox is an empty inbox, not a failure -- and asking must not
 * leave a stray zero-byte database behind on a phone that has never had a
 * text message. */
static void test_counting_without_a_database_is_zero(void)
{
    CHECK_INT(nd_db_count_unread_sms(), 0);
    CHECK(!nd_path_exists(ND_PATH_DB_SMS_INBOX));

    {
        nd_contact rows[2];
        char name[16];

        CHECK_INT(nd_contacts_query(NULL, rows, ND_ARRAY_LEN(rows)), 0);
        CHECK(!nd_contacts_lookup_name("555", name, sizeof name));
        CHECK(!nd_path_exists(ND_PATH_DB_PHONEBOOK));
    }
}

/* The Python re-creates the inbox table before inserting, so a text arriving
 * after the database was wiped is not lost. */
static void test_store_recreates_a_missing_table(void)
{
    char resolved[ND_PATH_MAX];

    /* Wipe the inbox the way the engineering menu does: the db directory
     * survives, the file does not. */
    CHECK_INT(nd_db_init_all(), ND_OK);
    CHECK_INT(nd_path_resolve(resolved, sizeof resolved, ND_PATH_DB_SMS_INBOX), ND_OK);
    CHECK_INT(remove(resolved), 0);
    CHECK(!nd_path_exists(ND_PATH_DB_SMS_INBOX));

    CHECK(nd_db_store_incoming_sms("+15550000", "first ever") > 0);
    CHECK_INT(nd_db_count_unread_sms(), 1);
}

int main(void)
{
    RUN(test_init_creates_the_three_databases);
    RUN(test_contacts_column_order_is_pinned);
    RUN(test_wal_is_set_on_three_databases_and_not_the_call_log);
    RUN(test_the_seed_contact);
    RUN(test_contacts_query_and_caller_id);
    RUN(test_incoming_sms_is_stored_and_counted);
    RUN(test_counting_without_a_database_is_zero);
    RUN(test_store_recreates_a_missing_table);
    return pt_report("test_db");
}

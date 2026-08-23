/* nd_db.h -- the four sqlite databases.
 *
 * Contacts, SMS inbox, SMS outbox, call log. The schemas below are BYTE-EXACT
 * copies of the Python's CREATE TABLE strings, because an existing phone's
 * databases have to keep opening after the upgrade.
 *
 * ============ THREE THINGS THAT LOOK LIKE BUGS AND ARE NOT ============
 *
 *   1. call_log.db has NO "PRAGMA journal_mode=WAL" while the other three do.
 *      journal_mode is persisted in the database header, so the call log stays
 *      in rollback-journal mode forever. Reproduce the asymmetry; do not add
 *      the pragma.
 *
 *   2. init_databases() does NOT create call_log.db. The CallLog app creates
 *      it lazily on first connect.
 *
 *   3. PhoneBook consumes "SELECT * FROM contacts" POSITIONALLY as
 *      (id, name, number, speed_dial). The column order in CREATE TABLE is
 *      therefore load-bearing. A test asserting PRAGMA table_info returns
 *      exactly those four names in that order belongs with this module.
 *
 * ============ CONNECTION POLICY ============
 *
 * Open, query, close -- per query, as the Python does. That is both 1:1 and
 * correct for the memory budget: a held sqlite connection costs page cache we
 * cannot spare, and no screen queries often enough to notice.
 *
 * ============ SQL ============
 *
 * sqlite3_prepare_v2 with bound parameters, always. Never a concatenated
 * string, not even for an integer, not even from trusted input.
 */

#ifndef ND_DB_H_INCLUDED
#define ND_DB_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward-declared so callers that only want the helpers below need not pull
 * in sqlite3.h. */
struct sqlite3;

/* The schema strings, verbatim. Owned by libneodct. */
extern const char *const ND_SCHEMA_CONTACTS;
extern const char *const ND_SCHEMA_INBOX;
extern const char *const ND_SCHEMA_OUTBOX;
extern const char *const ND_SCHEMA_CALLS;

/* init_databases(): create /NeoDCT/User/db, create the three schemas with
 * WAL, and seed one contact ('NeoDCT Support', '555-1234', 2) when the
 * contacts table is empty. Logs the same three lines the Python printed.
 * Called once, from nd_ui construction, before anything else. */
nd_err nd_db_init_all(void);

/* sqlite3_open_v2 with READWRITE|CREATE and the ND_ROOT prefix applied.
 * *out is owned by the caller; close with nd_db_close(). */
nd_err nd_db_open(const char *path, struct sqlite3 **out);
void nd_db_close(struct sqlite3 *db);

/* ------------------------------------------------------------------ *
 * The two queries the CORE itself makes
 * ------------------------------------------------------------------ */

/* Defensively re-creates the inbox table, inserts, and returns the new rowid.
 * -1 on any failure. Logs "[NOTIFY] SMS stored (id <id>) from <sender>". */
int64_t nd_db_store_incoming_sms(const char *sender, const char *body);

/* SELECT COUNT(*) FROM inbox WHERE is_read = 0. Zero on any error, including
 * a database that does not exist yet -- a missing inbox is an empty inbox,
 * not a failure. */
int32_t nd_db_count_unread_sms(void);

/* ------------------------------------------------------------------ *
 * Contacts -- shared because the CORE dials by name from the home screen
 * ------------------------------------------------------------------ */

#define ND_CONTACT_NAME_MAX   128
#define ND_CONTACT_NUMBER_MAX 64

typedef struct {
    int64_t id;
    char name[ND_CONTACT_NAME_MAX];
    char number[ND_CONTACT_NUMBER_MAX];
    int32_t speed_dial;
} nd_contact;

/* ORDER BY name ASC, optionally filtered with LIKE '%query%'. Pass NULL for
 * all contacts. Fills a caller-owned array; returns how many were written. */
size_t nd_contacts_query(const char *search, nd_contact *out, size_t max);

/* Caller-ID lookup, as incoming_screen does it: compare only the digits, and
 * match when THE LAST TEN DIGITS ARE EQUAL or the full digit strings are.
 * false when nothing matches. */
bool nd_contacts_lookup_name(const char *number, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* ND_DB_H_INCLUDED */

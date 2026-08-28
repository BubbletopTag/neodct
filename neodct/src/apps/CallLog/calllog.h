/* calllog.h -- the parts of the Call log app a unit test can reach.
 *
 * System/apps/CallLog/main.py is 188 lines: five screens over one sqlite
 * table and three settings keys. Nothing here draws; the four `show_*`
 * screens live in main.c and are driven through app_run().
 *
 * test/unit/test_calllog.c dlopen()s the BUILT app.so and dlsym()s these,
 * the way test_tones.c and test_phonebook.c do.
 *
 * ============ TEN ROWS, IN A PagedList ============
 *
 * The Python's SQL said "ORDER BY id DESC LIMIT 20" and its three list screens
 * were VerticalLists -- three small rows at a time, twenty deep. Both are now
 * deliberately different, and this is the redesign AGENTS.md means when it says
 * a golden frame that stops matching because a screen was changed on purpose is
 * the point: the lists show the LAST TEN numbers, one per page in the big type
 * the rest of the phone's top-level menus use, with the call's date and time on
 * the value line under it.
 *
 * Ten because a phone that now actually records calls fills these lists, and
 * twenty pages of paging to reach the bottom of a list nobody scrolls is worse
 * than a shorter list that ends. The number is the SQL's LIMIT as well as the
 * array bound, so the query and the screen cannot disagree.
 *
 * ND_CALLLOG_NUMBER_MAX is 64, which is ND_CONTACT_NUMBER_MAX from nd_db.h; a
 * longer `number` column is truncated for display and recorded in
 * OPEN-QUESTIONS.md CL-2.
 */

#ifndef ND_CALLLOG_H_INCLUDED
#define ND_CALLLOG_H_INCLUDED

#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* APP_ID = 3 -- manifest.json, and the breadcrumb on every screen here. */
#define ND_CALLLOG_APP_ID 3

/* The duration menu's breadcrumb: PagedList(root_id="3-5"). A COMPOSITE id,
 * which is why nd_header's root_id is a string (spec-apps-core.md 0a). */
#define ND_CALLLOG_DURATION_ROOT_ID "3-5"

/* DB_PATH. Absolute and load-bearing; identical to nd_paths.h's
 * ND_PATH_DB_CALL_LOG, and spelled here too so the app reads like the
 * Python it came from. */
#define ND_CALLLOG_DB "/NeoDCT/User/db/call_log.db"

/* The SQL's LIMIT and the array bound, in one place so they cannot drift. */
#define ND_CALLLOG_MAX_CALLS  10
#define ND_CALLLOG_NUMBER_MAX 64

/* "%02d:%02d:%02d" and "%d.%m. %H:%M" both fit in this with room to spare. */
#define ND_CALLLOG_TIME_MAX 32

#define ND_CALLLOG_ROOT_ITEMS     5
#define ND_CALLLOG_CLEAR_ITEMS    4
#define ND_CALLLOG_DURATION_ITEMS 4

extern const char *const nd_calllog_root_items[ND_CALLLOG_ROOT_ITEMS];
extern const char *const nd_calllog_clear_items[ND_CALLLOG_CLEAR_ITEMS];
extern const char *const nd_calllog_duration_items[ND_CALLLOG_DURATION_ITEMS];

/* The three `type` values the table stores, and the label each list screen
 * carries. Index 0/1/2 is the root menu's 0/1/2, so the two arrays are
 * indexed by the same number the PagedList returned. */
extern const char *const nd_calllog_types[3];
extern const char *const nd_calllog_titles[3];

/* `number or "Unknown"`: the Python's `or` is falsy-tested, so a NULL column
 * and an empty string both become this. */
extern const char *const nd_calllog_unknown;

/* TIMER_KEYS, in the dict's insertion order -- which is the order
 * "Clear timers" walks when it zeroes all three. */
typedef enum {
    ND_CALLLOG_TIMER_LAST = 0,
    ND_CALLLOG_TIMER_RECEIVED,
    ND_CALLLOG_TIMER_DIALED,
    ND_CALLLOG_TIMER_COUNT
} nd_calllog_timer;

extern const char *const nd_calllog_timer_keys[ND_CALLLOG_TIMER_COUNT];

/* CLEAR_ITEMS index -> the `type` to delete, NULL meaning "every row".
 * {0: None, 1: "missed", 2: "dialed", 3: "received"} -- which agrees with
 * the labels beside it and NOT with the root menu, where Received is 1 and
 * Dialed is 2. Exposed because a test is the only thing that can notice the
 * two orders drifting apart. */
extern const char *const nd_calllog_clear_targets[ND_CALLLOG_CLEAR_ITEMS];

/* ------------------------------------------------------------------ *
 * Storage
 * ------------------------------------------------------------------ */

/* One row of `SELECT number, timestamp, duration FROM calls`.
 *
 * `duration` is the seconds the call was connected, and is 0 for every row a
 * phone recorded before nd_db_record_call() existed as well as for every
 * missed call. The lists do not show it; the detail screen does. */
typedef struct {
    char number[ND_CALLLOG_NUMBER_MAX];
    int64_t timestamp;
    int64_t duration;
} nd_call_rec;

/* _connect(): makedirs(/NeoDCT/User/db), open, CREATE TABLE IF NOT EXISTS.
 * Exposed because the test wants to prove the schema is nd_db.h's ND_SCHEMA_
 * CALLS byte for byte and that no journal_mode pragma is issued. Returns
 * ND_OK and stores an owned sqlite3 * in *out; close with nd_db_close(). */
struct sqlite3;
nd_err nd_calllog_connect(struct sqlite3 **out);

/* fetch_calls(call_type): the ten newest rows of that type, newest first.
 * Zero on any failure, having logged "[CallLog] DB read failed: ...". */
size_t nd_calllog_fetch(const char *call_type, nd_call_rec *out, size_t max);

/* clear_calls(call_type): NULL clears every row. false on any failure,
 * having logged "[CallLog] DB clear failed: ...". */
bool nd_calllog_clear(const char *call_type);

/* _get_timer_seconds / _set_timer_seconds. The read is
 * `int(get_setting(key, "0") or 0)` with every failure folded to 0. */
int64_t nd_calllog_timer_get(nd_calllog_timer which);
bool nd_calllog_timer_set(nd_calllog_timer which, int64_t seconds);

/* ------------------------------------------------------------------ *
 * Formatting
 * ------------------------------------------------------------------ */

/* format_duration(): max(0, s) as "%02d:%02d:%02d" of hours, minutes,
 * seconds. Hours are NOT capped at 99 -- neither is the Python's. Returns
 * out. */
const char *nd_calllog_format_duration(int64_t seconds, char *out, size_t out_sz);

/* format_call_time(): strftime("%d.%m. %H:%M", localtime(t)), and the empty
 * string when that cannot be done. Returns out. */
const char *nd_calllog_format_call_time(int64_t timestamp, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* ND_CALLLOG_H_INCLUDED */

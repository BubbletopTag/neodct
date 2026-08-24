/* apps/CallLog/main.c -- the Nokia 5190-style Call log, app id 3.
 *
 * A one-to-one port of System/apps/CallLog/main.py (188 lines). Five screens:
 * a root PagedList (3-1 .. 3-5), three call lists, a "Clear call lists"
 * VerticalList and a "Call duration" PagedList over three settings keys.
 *
 * golden/app-calllog.png is the root PagedList's first page: "Call log" in
 * 24 px type, "Missed calls" centred under it, the "3-1" breadcrumb and a
 * "Select" softkey.
 *
 * ============ THE APP OWNS ITS DATABASE, AND CREATES IT ON EVERY READ =====
 *
 * call_log.db is the one of the four databases nd_db_init_all() does NOT
 * create, and this is why: `_connect()` runs `os.makedirs` and
 * `CREATE TABLE IF NOT EXISTS` on every single call, read or write, so the
 * app brings the file into existence the first time somebody opens the menu.
 * It also issues NO `PRAGMA journal_mode=WAL`, unlike the three the core
 * creates -- and journal_mode is persisted in the file header, so the call
 * log stays in rollback-journal mode forever on every phone shipped so far.
 * nd_db.h says to reproduce the asymmetry rather than tidy it away; this is
 * the code that has to.
 *
 * A consequence worth naming: OPENING THE MISSED-CALLS LIST ON A PHONE THAT
 * HAS NEVER TAKEN A CALL CREATES AN EMPTY DATABASE. Messages deliberately
 * does the opposite (msg_db.c guards every read with os.path.exists), and
 * the difference between the two apps is real, not an oversight in one of
 * them. Ported as found.
 *
 * ============ WIDGET LIFETIME DECIDES WHETHER A MENU REMEMBERS ============
 *
 * spec-apps-core.md section 0b tabulates this and it is observable. The two
 * PagedLists here are built ONCE, before their loop, so the page survives a
 * trip into a submenu; the two VerticalLists are rebuilt INSIDE their loop,
 * so the selection resets after every detail screen. Keep both.
 *
 * ============ THE CLEAR MENU'S INDEX MAP IS NOT IN LIST ORDER ============
 *
 * CLEAR_ITEMS reads "All, Missed, Dialed, Received" and the map is
 * {0: None, 1: "missed", 2: "dialed", 3: "received"}. Index 2 is "Dialed"
 * and index 3 is "Received", which agrees with the labels -- but it does NOT
 * agree with the ROOT menu, where Received is 1 and Dialed is 2. The two
 * orders really are different and swapping either to match the other would
 * clear the wrong list.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

#include "nd_app.h"
#include "nd_db.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_settings.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

#include "calllog.h"

/* ------------------------------------------------------------------ *
 * The strings
 * ------------------------------------------------------------------ */

const char *const nd_calllog_root_items[ND_CALLLOG_ROOT_ITEMS] = {
    "Missed calls",       /* 3-1 */
    "Received calls",     /* 3-2 */
    "Dialed calls",       /* 3-3 */
    "Clear call lists",   /* 3-4 */
    "Show call duration", /* 3-5 */
};

const char *const nd_calllog_clear_items[ND_CALLLOG_CLEAR_ITEMS] = {"All", "Missed", "Dialed",
                                                                    "Received"};

const char *const nd_calllog_duration_items[ND_CALLLOG_DURATION_ITEMS] = {
    "Last call duration",
    "Received calls' duration",
    "Dialed calls' duration",
    "Clear timers",
};

const char *const nd_calllog_types[3] = {"missed", "received", "dialed"};
const char *const nd_calllog_titles[3] = {"Missed calls", "Received calls", "Dialed calls"};

const char *const nd_calllog_unknown = "Unknown";

const char *const nd_calllog_timer_keys[ND_CALLLOG_TIMER_COUNT] = {
    ND_SET_CALLLOG_DUR_LAST,
    ND_SET_CALLLOG_DUR_RECEIVED,
    ND_SET_CALLLOG_DUR_DIALED,
};

/* CLEAR_ITEMS index -> `type`. NULL is the Python's None: "DELETE FROM
 * calls" with no WHERE clause. See the header comment; the order is not the
 * root menu's and that is deliberate. */
const char *const nd_calllog_clear_targets[ND_CALLLOG_CLEAR_ITEMS] = {NULL, "missed", "dialed",
                                                                      "received"};

/* ------------------------------------------------------------------ *
 * Storage
 * ------------------------------------------------------------------ */

nd_err nd_calllog_connect(struct sqlite3 **out)
{
    sqlite3 *db = NULL;
    nd_err rc;

    if (out == NULL)
        return ND_ERR_INVAL;
    *out = NULL;

    /* os.makedirs(os.path.dirname(DB_PATH), exist_ok=True) */
    rc = nd_mkdir_p(ND_PATH_DB_DIR, 0755u);
    if (rc != ND_OK)
        return rc;

    rc = nd_db_open(ND_CALLLOG_DB, &db);
    if (rc != ND_OK)
        return rc;

    /* conn.execute(CREATE TABLE IF NOT EXISTS calls ...) -- and nothing
     * else. No journal_mode pragma; see the header comment. */
    if (sqlite3_exec(db, ND_SCHEMA_CALLS, NULL, NULL, NULL) != SQLITE_OK) {
        nd_db_close(db);
        return ND_ERR_IO;
    }

    *out = db;
    return ND_OK;
}

size_t nd_calllog_fetch(const char *call_type, nd_call_rec *out, size_t max)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    size_t n = 0u;

    if (call_type == NULL || out == NULL || max == 0u)
        return 0u;

    if (nd_calllog_connect(&db) != ND_OK) {
        nd_log(ND_LOG_CALLLOG, "DB read failed: cannot open %s", ND_CALLLOG_DB);
        return 0u;
    }

    if (sqlite3_prepare_v2(db,
                           "SELECT number, timestamp FROM calls WHERE type=? "
                           "ORDER BY id DESC LIMIT 20",
                           -1, &st, NULL) != SQLITE_OK) {
        nd_log(ND_LOG_CALLLOG, "DB read failed: %s", sqlite3_errmsg(db));
        nd_db_close(db);
        return 0u;
    }
    (void)sqlite3_bind_text(st, 1, call_type, -1, SQLITE_STATIC);

    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *num = sqlite3_column_text(st, 0);

        memset(&out[n], 0, sizeof out[n]);
        /* The column is nullable: the Python schema declares no NOT NULL,
         * and `number or "Unknown"` is what copes with it. */
        (void)nd_strlcpy(out[n].number, (num != NULL) ? (const char *)num : "",
                         sizeof out[n].number);
        out[n].timestamp = (int64_t)sqlite3_column_int64(st, 1);
        n++;
    }

    (void)sqlite3_finalize(st);
    nd_db_close(db);
    return n;
}

bool nd_calllog_clear(const char *call_type)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    bool ok;

    if (nd_calllog_connect(&db) != ND_OK) {
        nd_log(ND_LOG_CALLLOG, "DB clear failed: cannot open %s", ND_CALLLOG_DB);
        return false;
    }

    if (sqlite3_prepare_v2(
            db, (call_type == NULL) ? "DELETE FROM calls" : "DELETE FROM calls WHERE type=?", -1,
            &st, NULL) != SQLITE_OK) {
        nd_log(ND_LOG_CALLLOG, "DB clear failed: %s", sqlite3_errmsg(db));
        nd_db_close(db);
        return false;
    }
    if (call_type != NULL)
        (void)sqlite3_bind_text(st, 1, call_type, -1, SQLITE_STATIC);

    ok = (sqlite3_step(st) == SQLITE_DONE);
    if (!ok)
        nd_log(ND_LOG_CALLLOG, "DB clear failed: %s", sqlite3_errmsg(db));

    (void)sqlite3_finalize(st);
    /* conn.commit() -- implicit here: sqlite autocommits a statement run
     * outside an explicit transaction, which is the only kind this app
     * opens. */
    nd_db_close(db);
    return ok;
}

/* Python's int(str). Whitespace either side and one optional sign are
 * accepted; ANYTHING ELSE raises, and every raise in this app is caught and
 * folded to the default. Underscore separators (int("1_0") == 10 since 3.6)
 * and non-ASCII digits are NOT accepted here -- the same deviation
 * OPEN-QUESTIONS.md M-10 records for the modem, and recorded again as CL-3
 * because a settings value is more plausibly hand-edited than an AT reply. */
static bool py_int(const char *s, int64_t *out)
{
    const char *p = s;
    bool neg = false;
    bool any = false;
    int64_t v = 0;

    if (s == NULL)
        return false;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
        p++;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        int64_t d = (int64_t)(*p - '0');

        /* int() is unbounded in Python; int64 is not. A value this large is
         * not a call duration, so saturate rather than wrap. */
        if (v > (INT64_MAX - d) / 10)
            v = INT64_MAX / 10;
        v = v * 10 + d;
        any = true;
        p++;
    }
    while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
        p++;
    if (!any || *p != '\0')
        return false;

    *out = neg ? -v : v;
    return true;
}

int64_t nd_calllog_timer_get(nd_calllog_timer which)
{
    char buf[32];
    int64_t v = 0;

    if ((int)which < 0 || (int)which >= (int)ND_CALLLOG_TIMER_COUNT)
        return 0;
    /* get_setting(key, "0") -- and `or 0` turns an empty answer into 0 too. */
    if (nd_settings_get_copy(nd_calllog_timer_keys[which], "0", buf, sizeof buf) != ND_OK)
        return 0;
    if (buf[0] == '\0')
        return 0;
    if (!py_int(buf, &v))
        return 0;
    return v;
}

bool nd_calllog_timer_set(nd_calllog_timer which, int64_t seconds)
{
    char buf[32];

    if ((int)which < 0 || (int)which >= (int)ND_CALLLOG_TIMER_COUNT)
        return false;
    if (nd_snprintf(buf, sizeof buf, "%lld", (long long)seconds) != ND_OK)
        return false;
    if (nd_settings_set(nd_calllog_timer_keys[which], buf) != ND_OK) {
        nd_log(ND_LOG_CALLLOG, "Timer write failed: cannot store %s", nd_calllog_timer_keys[which]);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * Formatting
 * ------------------------------------------------------------------ */

const char *nd_calllog_format_duration(int64_t seconds, char *out, size_t out_sz)
{
    int64_t s = (seconds > 0) ? seconds : 0; /* max(0, int(seconds)) */

    if (out == NULL || out_sz == 0u)
        return out;
    /* "%02d:%02d:%02d". Python does not cap the hours field either, so a
     * timer past 99:59:59 simply grows a column. */
    if (nd_snprintf(out, out_sz, "%02lld:%02lld:%02lld", (long long)(s / 3600),
                    (long long)((s % 3600) / 60), (long long)(s % 60)) != ND_OK)
        out[0] = '\0';
    return out;
}

const char *nd_calllog_format_call_time(int64_t timestamp, char *out, size_t out_sz)
{
    struct tm tm;

    if (out == NULL || out_sz == 0u)
        return out;
    out[0] = '\0';

    /* time.localtime(int(timestamp)) -- through nd_time_localtime(), which
     * is gmtime under capture so a frame rendered in Dublin matches one
     * rendered in CI (nd_vclock.h). */
    memset(&tm, 0, sizeof tm);
    nd_time_localtime((double)timestamp, &tm);

    /* strftime returns 0 when the result did not fit, which is the Python's
     * "any failure -> empty string" branch. */
    if (strftime(out, out_sz, "%d.%m. %H:%M", &tm) == 0u)
        out[0] = '\0';
    return out;
}

/* ------------------------------------------------------------------ *
 * The screens
 * ------------------------------------------------------------------ */

/* show_call_list(ui, title, call_type).
 *
 * fetch_calls() runs ONCE, outside the loop: viewing a detail and coming back
 * does not re-read the table, so a call arriving while the list is up is not
 * shown until the screen is left and re-entered. The VerticalList, by
 * contrast, is rebuilt every iteration, so the cursor returns to the top. */
static void show_call_list(nd_ui *ui, const char *title, const char *call_type)
{
    nd_call_rec calls[ND_CALLLOG_MAX_CALLS];
    const char *items[ND_CALLLOG_MAX_CALLS];
    size_t n;
    size_t i;

    n = nd_calllog_fetch(call_type, calls, ND_ARRAY_LEN(calls));
    if (n == 0u) {
        (void)nd_infoscreen_show(ui, title, "No numbers", "Back");
        return;
    }

    for (i = 0u; i < n; i++)
        items[i] = (calls[i].number[0] != '\0') ? calls[i].number : nd_calllog_unknown;

    for (;;) {
        nd_vlist menu;
        nd_softkey bar;
        int32_t choice;
        char when[ND_CALLLOG_TIME_MAX];

        nd_vlist_init(&menu, ui, title, items, n, ND_CALLLOG_APP_ID);
        nd_softkey_init(&bar, ui, false);
        nd_softkey_update(&bar, "Details", false);

        choice = nd_vlist_show(&menu);
        if (choice == ND_WIDGET_BACK)
            return;
        if (choice < 0 || (size_t)choice >= n)
            return;

        (void)nd_infoscreen_show(
            ui, items[choice],
            nd_calllog_format_call_time(calls[choice].timestamp, when, sizeof when), "Back");

        /* Not in the Python, which had IncomingCall to unwind it. nd_app.h:
         * a loop that outlives a frame polls this. */
        if (nd_app_should_exit())
            return;
    }
}

static void show_clear_menu(nd_ui *ui)
{
    nd_vlist menu;
    nd_softkey bar;
    int32_t choice;

    nd_vlist_init(&menu, ui, "Clear call lists", nd_calllog_clear_items, ND_CALLLOG_CLEAR_ITEMS,
                  ND_CALLLOG_APP_ID);
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "OK", false);

    choice = nd_vlist_show(&menu);
    if (choice == ND_WIDGET_BACK)
        return;
    if (choice < 0 || choice >= ND_CALLLOG_CLEAR_ITEMS)
        return;

    if (nd_calllog_clear(nd_calllog_clear_targets[choice]))
        (void)nd_infoscreen_show(ui, "List cleared", NULL, "OK");
}

/* show_duration_menu(ui). The PagedList is built ONCE, so the page the user
 * was on survives a trip into a reading. */
static void show_duration_menu(nd_ui *ui)
{
    nd_pagedlist menu;

    nd_pagedlist_init(&menu, ui, "Call duration", nd_calllog_duration_items,
                      ND_CALLLOG_DURATION_ITEMS, ND_CALLLOG_DURATION_ROOT_ID, true);

    for (;;) {
        int32_t choice = nd_pagedlist_show(&menu);
        char text[ND_CALLLOG_TIME_MAX];
        size_t k;

        if (choice == ND_WIDGET_BACK)
            return;

        switch (choice) {
        case 0:
        case 1:
        case 2:
            /* The InfoScreen title is the menu entry's own label, verbatim
             * -- apostrophe and all. */
            (void)nd_infoscreen_show(
                ui, nd_calllog_duration_items[choice],
                nd_calllog_format_duration(nd_calllog_timer_get((nd_calllog_timer)choice), text,
                                           sizeof text),
                "Back");
            break;
        case 3:
            /* `for kind in TIMER_KEYS` -- dict insertion order, which is
             * last, received, dialed. */
            for (k = 0u; k < (size_t)ND_CALLLOG_TIMER_COUNT; k++)
                (void)nd_calllog_timer_set((nd_calllog_timer)k, 0);
            (void)nd_infoscreen_show(ui, "Timers cleared", NULL, "OK");
            break;
        default:
            break;
        }

        if (nd_app_should_exit())
            return;
    }
}

/* ------------------------------------------------------------------ *
 * app_run
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    nd_pagedlist menu;
    char root_id[16];

    if (ui == NULL)
        return 1;

    /* PagedList's root_id is a string because callers pass composite ids
     * like "3-5"; this one is the plain integer 3. Built ONCE, outside the
     * loop, so the page survives a trip into a submenu. */
    (void)nd_snprintf(root_id, sizeof root_id, "%d", ND_CALLLOG_APP_ID);
    nd_pagedlist_init(&menu, ui, "Call log", nd_calllog_root_items, ND_CALLLOG_ROOT_ITEMS, root_id,
                      true);

    for (;;) {
        int32_t choice = nd_pagedlist_show(&menu);

        if (choice == ND_WIDGET_BACK)
            return 0;

        switch (choice) {
        case 0:
        case 1:
        case 2:
            show_call_list(ui, nd_calllog_titles[choice], nd_calllog_types[choice]);
            break;
        case 3:
            show_clear_menu(ui);
            break;
        case 4:
            show_duration_menu(ui);
            break;
        default:
            break;
        }

        if (nd_app_should_exit())
            return 0;
    }
}

/* Nothing is held: no sound card, no child process, no open database between
 * screens. nd_app.h requires the symbol anyway, so that "missing" always
 * means "the author forgot" and never "nothing to do". */
void app_shutdown(void) {}

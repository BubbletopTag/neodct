/* apps/Messages/threads.c -- Messages Style, and conversations.
 *
 * NOT A PORT. The Python has an Inbox list and an Outbox list and no notion
 * of a correspondent at all; messages.h explains why the outbox needed a
 * column before any of this was possible. This file turns those two tables
 * into conversations, and answers the one question the Settings row asks.
 *
 * ============ WHY IT GROUPS IN C AND NOT IN SQL ============
 *
 * The obvious implementation is one query with a UNION ALL and a GROUP BY.
 * It was not written that way for two reasons, and the second is the one
 * that decided it:
 *
 *   1. The two tables live in SEPARATE DATABASE FILES -- sms_inbox.db and
 *      sms_outbox.db. Joining them means ATTACH, which means a second path
 *      to resolve, a second failure mode when one file does not exist yet,
 *      and a query that reads differently from every other one in the app.
 *
 *   2. The grouping key is not a column. "555-1234" and "5551234" are one
 *      conversation, and deciding that needs nd_msg_peer_key(), which SQL
 *      cannot call. A GROUP BY on the raw text would put the same person in
 *      two rows, which is precisely the bug the key exists to prevent.
 *
 * So both tables are read with the statements that already exist, capped as
 * they already are, and folded together here. The cost is bounded by
 * ND_MSG_LIST_MAX rather than by the database.
 */

#include <stdlib.h>
#include <string.h>

#include "nd_contacts.h"
#include "nd_db.h"
#include "nd_paths.h"
#include "nd_settings.h"
#include "nd_types.h"

#include "messages.h"

/* ------------------------------------------------------------------ *
 * Messages Style
 * ------------------------------------------------------------------ */

const char *const nd_msg_style_options[ND_MSG_STYLE_ITEMS] = {"Classic", "Chat"};

static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* Tolerant in ONE direction. Anything that is not recognisably "chat" is
 * CLASSIC -- a missing setting, an empty one, a typo, and a settings.prop
 * written before this feature existed all have to mean "behave as before",
 * or upgrading a phone silently redesigns an app on it. */
nd_msg_style nd_msg_style_parse(const char *raw)
{
    char norm[16];
    size_t n = 0u;
    const char *p;

    if (raw == NULL)
        return ND_MSG_STYLE_CLASSIC;

    /* Leading and trailing space is stripped because settings.prop is a file
     * people edit by hand. */
    for (p = raw; *p == ' ' || *p == '\t'; p++) {
    }
    while (*p != '\0' && *p != ' ' && *p != '\t' && n + 1u < sizeof norm)
        norm[n++] = lower_ascii(*p++);
    norm[n] = '\0';

    return (strcmp(norm, "chat") == 0) ? ND_MSG_STYLE_CHAT : ND_MSG_STYLE_CLASSIC;
}

nd_msg_style nd_msg_style_current(void)
{
    return nd_msg_style_parse(nd_settings_get(ND_MSG_STYLE_SETTING, ND_MSG_STYLE_DEFAULT));
}

/* ------------------------------------------------------------------ *
 * The thread key
 * ------------------------------------------------------------------ */

void nd_msg_peer_key(char *dst, size_t dst_sz, const char *number)
{
    size_t n = 0u;
    const char *p;

    if (dst == NULL || dst_sz == 0u)
        return;
    dst[0] = '\0';
    if (number == NULL)
        return;

    for (p = number; *p != '\0' && n + 1u < dst_sz; p++) {
        if ((*p >= '0' && *p <= '9') || *p == '+' || *p == '*' || *p == '#')
            dst[n++] = *p;
    }
    dst[n] = '\0';

    /* An alphanumeric originating address -- "Vodafone", "GOOGLE" -- has no
     * digits at all and would key to the empty string, which would fold every
     * such sender into one conversation. Falling back to the raw text keeps
     * them apart and keeps the thread reachable. */
    if (n == 0u)
        (void)nd_strlcpy(dst, number, dst_sz);
}

/* ------------------------------------------------------------------ *
 * Building the list
 * ------------------------------------------------------------------ */

/* Finds the thread for `key`, or starts one. NULL when the table is full,
 * which drops the OLDEST correspondents rather than the newest -- see the
 * sort note in nd_msg_threads(). */
static nd_msg_thread *find_or_add(nd_msg_thread *list, size_t *n, size_t max, const char *key,
                                  const char *raw)
{
    size_t i;

    for (i = 0u; i < *n; i++) {
        if (strcmp(list[i].peer, key) == 0)
            return &list[i];
    }
    if (*n >= max)
        return NULL;

    memset(&list[*n], 0, sizeof list[*n]);
    (void)nd_strlcpy(list[*n].peer, key, sizeof list[*n].peer);
    /* The raw spelling is kept from the FIRST row seen, and fetch_inbox is
     * newest-first -- so a number that has been written two ways shows the
     * way it was written most recently. */
    (void)nd_strlcpy(list[*n].number, (raw != NULL && raw[0] != '\0') ? raw : key,
                     sizeof list[*n].number);
    return &list[(*n)++];
}

/* The preview is one list row, so it is the message with its newlines turned
 * into spaces -- a raw body would draw its second line over the row below. */
static void set_preview(nd_msg_thread *t, const char *body)
{
    size_t i;

    (void)nd_strlcpy(t->preview, (body != NULL) ? body : "", sizeof t->preview);
    for (i = 0u; t->preview[i] != '\0'; i++) {
        if (t->preview[i] == '\n' || t->preview[i] == '\r' || t->preview[i] == '\t')
            t->preview[i] = ' ';
    }
}

/* Newest first. Insertion sort rather than qsort: the array is at most
 * ND_MSG_THREADS_MAX and this keeps the comparison next to the tie-break,
 * which is the peer key -- so two conversations whose last message shares a
 * timestamp have a stable order rather than qsort's. */
static void sort_by_recency(nd_msg_thread *list, size_t n)
{
    size_t i;

    for (i = 1u; i < n; i++) {
        nd_msg_thread key = list[i];
        size_t j = i;

        while (j > 0u && (list[j - 1u].last_ts < key.last_ts ||
                          (list[j - 1u].last_ts == key.last_ts &&
                           strcmp(list[j - 1u].peer, key.peer) > 0))) {
            list[j] = list[j - 1u];
            j--;
        }
        list[j] = key;
    }
}

size_t nd_msg_threads(nd_msg_thread *out, size_t max)
{
    nd_msg_rec *rows;
    size_t n_threads = 0u;
    size_t n_rows;
    size_t i;

    if (out == NULL || max == 0u)
        return 0u;

    /* One scratch array reused for both tables. ND_MSG_LIST_MAX rows is
     * ~139 kB and is the same ceiling the two list screens already use. */
    rows = calloc((size_t)ND_MSG_LIST_MAX, sizeof *rows);
    if (rows == NULL)
        return 0u;

    /* Threads are gathered into a table sized for the CAP, not for `max`, so
     * that a caller asking for one thread still gets the most recent one and
     * not merely the first the inbox happened to mention. It is sorted and
     * then truncated. */
    {
        nd_msg_thread *all = calloc((size_t)ND_MSG_THREADS_MAX, sizeof *all);

        if (all == NULL) {
            free(rows);
            return 0u;
        }

        n_rows = nd_msg_fetch_inbox(rows, (size_t)ND_MSG_LIST_MAX);
        for (i = 0u; i < n_rows; i++) {
            char key[ND_MSG_SENDER_MAX];
            nd_msg_thread *t;

            nd_msg_peer_key(key, sizeof key, rows[i].sender);
            if (key[0] == '\0')
                (void)nd_strlcpy(key, ND_MSG_PEER_UNKNOWN, sizeof key);

            t = find_or_add(all, &n_threads, (size_t)ND_MSG_THREADS_MAX, key, rows[i].sender);
            if (t == NULL)
                continue;
            t->n_messages++;
            if (rows[i].is_read == 0)
                t->unread++;
            /* fetch_inbox is DESC, so the first row of a thread is its newest
             * -- but the outbox pass below can still beat it, so the
             * comparison is explicit rather than relying on arrival order. */
            if (t->last_ts == 0 || rows[i].timestamp > t->last_ts) {
                t->last_ts = rows[i].timestamp;
                t->last_outgoing = false;
                set_preview(t, rows[i].message);
            }
        }

        n_rows = nd_msg_fetch_outbox(rows, (size_t)ND_MSG_LIST_MAX);
        for (i = 0u; i < n_rows; i++) {
            char key[ND_MSG_SENDER_MAX];
            nd_msg_thread *t;

            nd_msg_peer_key(key, sizeof key, rows[i].recipient);
            if (key[0] == '\0')
                (void)nd_strlcpy(key, ND_MSG_PEER_UNKNOWN, sizeof key);

            t = find_or_add(all, &n_threads, (size_t)ND_MSG_THREADS_MAX, key, rows[i].recipient);
            if (t == NULL)
                continue;
            t->n_messages++;
            if (t->last_ts == 0 || rows[i].timestamp > t->last_ts) {
                t->last_ts = rows[i].timestamp;
                t->last_outgoing = true;
                set_preview(t, rows[i].message);
            }
        }

        /* The display name last, so the lookup runs once per CONVERSATION
         * rather than once per message -- it is a query against a third
         * database. */
        for (i = 0u; i < n_threads; i++) {
            /* Looked up by the RAW number, because that is the spelling the
             * phone book holds -- a contact saved as "555-1234" is not found
             * by "5551234". */
            if (!nd_contacts_lookup_name(all[i].number, all[i].display, sizeof all[i].display) ||
                all[i].display[0] == '\0')
                (void)nd_strlcpy(all[i].display, all[i].number, sizeof all[i].display);
        }

        sort_by_recency(all, n_threads);
        if (n_threads > max)
            n_threads = max;
        memcpy(out, all, n_threads * sizeof *out);
        free(all);
    }

    free(rows);
    return n_threads;
}

/* ------------------------------------------------------------------ *
 * One conversation
 * ------------------------------------------------------------------ */

/* Oldest first, which is the opposite of every other read in this app -- a
 * list is newest-first because that is what you want to see, and a transcript
 * is oldest-first because that is the order it was said in. */
static void sort_by_time(nd_msg_bubble *b, size_t n)
{
    size_t i;

    for (i = 1u; i < n; i++) {
        nd_msg_bubble key = b[i];
        size_t j = i;

        /* The tie-break is `outgoing` and then the id: two messages in the
         * same second must not swap places between two visits to the same
         * conversation. */
        while (j > 0u && (b[j - 1u].timestamp > key.timestamp ||
                          (b[j - 1u].timestamp == key.timestamp &&
                           b[j - 1u].outgoing == key.outgoing && b[j - 1u].id > key.id))) {
            b[j] = b[j - 1u];
            j--;
        }
        b[j] = key;
    }
}

size_t nd_msg_thread_messages(const char *peer, nd_msg_bubble *out, size_t max)
{
    char want[ND_MSG_SENDER_MAX];
    nd_msg_rec *rows;
    size_t n = 0u;
    size_t n_rows;
    size_t i;

    if (peer == NULL || out == NULL || max == 0u)
        return 0u;
    /* The caller may pass a key or a raw number; normalising both here means
     * a screen never has to remember which it is holding.
     *
     * An empty key is the SAME substitution the grouping makes below, and it
     * has to be: a row written before the outbox grew its recipient column
     * has no recipient at all, so nd_msg_threads() files it under
     * (unknown) and the conversation list hands that thread's empty `number`
     * straight back here. Returning nothing for it would make the one
     * conversation an upgraded phone is guaranteed to have the one
     * conversation it cannot open. */
    nd_msg_peer_key(want, sizeof want, peer);
    if (want[0] == '\0')
        (void)nd_strlcpy(want, ND_MSG_PEER_UNKNOWN, sizeof want);

    rows = calloc((size_t)ND_MSG_LIST_MAX, sizeof *rows);
    if (rows == NULL)
        return 0u;

    n_rows = nd_msg_fetch_inbox(rows, (size_t)ND_MSG_LIST_MAX);
    for (i = 0u; i < n_rows && n < max; i++) {
        char key[ND_MSG_SENDER_MAX];

        nd_msg_peer_key(key, sizeof key, rows[i].sender);
        if (key[0] == '\0')
            (void)nd_strlcpy(key, ND_MSG_PEER_UNKNOWN, sizeof key);
        if (strcmp(key, want) != 0)
            continue;
        memset(&out[n], 0, sizeof out[n]);
        out[n].id = rows[i].id;
        (void)nd_strlcpy(out[n].text, rows[i].message, sizeof out[n].text);
        out[n].timestamp = rows[i].timestamp;
        out[n].outgoing = false;
        n++;
    }

    n_rows = nd_msg_fetch_outbox(rows, (size_t)ND_MSG_LIST_MAX);
    for (i = 0u; i < n_rows && n < max; i++) {
        char key[ND_MSG_SENDER_MAX];

        nd_msg_peer_key(key, sizeof key, rows[i].recipient);
        if (key[0] == '\0')
            (void)nd_strlcpy(key, ND_MSG_PEER_UNKNOWN, sizeof key);
        if (strcmp(key, want) != 0)
            continue;
        memset(&out[n], 0, sizeof out[n]);
        out[n].id = rows[i].id;
        (void)nd_strlcpy(out[n].text, rows[i].message, sizeof out[n].text);
        out[n].timestamp = rows[i].timestamp;
        out[n].outgoing = true;
        n++;
    }

    free(rows);
    sort_by_time(out, n);
    return n;
}

void nd_msg_thread_mark_read(const char *peer)
{
    char want[ND_MSG_SENDER_MAX];
    nd_msg_rec *rows;
    size_t n_rows;
    size_t i;

    if (peer == NULL)
        return;
    nd_msg_peer_key(want, sizeof want, peer);
    if (want[0] == '\0')
        (void)nd_strlcpy(want, ND_MSG_PEER_UNKNOWN, sizeof want); /* see above */

    rows = calloc((size_t)ND_MSG_LIST_MAX, sizeof *rows);
    if (rows == NULL)
        return;

    /* One UPDATE per unread row rather than one statement with a LIKE: the
     * rows that belong to this thread are decided by nd_msg_peer_key(), which
     * SQL cannot call, and nd_msg_mark_read() already exists. A conversation
     * has tens of messages, not thousands. */
    n_rows = nd_msg_fetch_inbox(rows, (size_t)ND_MSG_LIST_MAX);
    for (i = 0u; i < n_rows; i++) {
        char key[ND_MSG_SENDER_MAX];

        if (rows[i].is_read != 0)
            continue;
        nd_msg_peer_key(key, sizeof key, rows[i].sender);
        if (key[0] == '\0')
            (void)nd_strlcpy(key, ND_MSG_PEER_UNKNOWN, sizeof key);
        if (strcmp(key, want) == 0)
            nd_msg_mark_read(rows[i].id);
    }
    free(rows);
}

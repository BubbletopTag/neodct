/* nd_modem.c -- ModemService: the SIM7600 state machine, on its own thread.
 *
 * A port of System/core/ModemService/__init__.py (1084 lines). The Python is
 * polled from read_keypress(); this is not. OPEN-QUESTIONS.md decision 1 gave
 * the modem its own thread so that a call interrupts whatever is running,
 * including an app that never asks for a key. Everything else -- every
 * timeout, every cadence, every magic number, every log line's meaning -- is
 * one to one with the Python, including the parts that look like bugs.
 *
 * ============ THE THREAD, AND WHY IT IS SHAPED LIKE THIS ============
 *
 * One thread owns the serial descriptor. Nothing else may touch it, because
 * two writers on one AT port interleave a dial with a CMGS body and the modem
 * answers neither.
 *
 *   - The thread runs poll() about ten times a second, which is exactly the
 *     rate NeoDCT_UI._modem_tick() used to call it at. All the rate limiting
 *     is still inside poll(), untouched.
 *   - dial / answer / hangup / send_sms / fetch_sms / read_stored_sms /
 *     send_at are called from the UI thread. Each posts a request into a
 *     single slot, wakes the thread and blocks on a condvar until the thread
 *     has run it. The UI blocks for exactly as long as the Python's UI did.
 *   - The readouts -- signal_level, operator_display, state, caller_id,
 *     status_snapshot -- take a short mutex and copy. They never wait for a
 *     transaction, so a 30-second SMS ack does not freeze the status bar.
 *
 * The state mutex is never held across a syscall, a sleep or a transaction.
 * That is the whole rule; the small set/get helpers below exist to make it
 * impossible to break by accident.
 *
 * ============ THINGS THAT LOOK WRONG AND ARE LOAD-BEARING ============
 *
 *  - Port discovery sorts with strcmp, so ttyUSB10 comes before ttyUSB2 and
 *    card10 before card2. That is what os.listdir() + sorted() does and the
 *    order decides which port is adopted. Do not "fix" it.
 *  - bInterfaceNumber is parsed as HEXADECIMAL. It is a two-digit hex field
 *    in sysfs, so interface 16 reads "10".
 *  - Interface 0 is dropped entirely: it is the Qualcomm DIAG port, it is
 *    binary, and it never answers AT.
 *  - dial() logs the number AFTER filtering but BEFORE the empty check, so an
 *    all-junk number logs an empty dial and then fails.
 *  - MISSED_CALL has no `state != IDLE` guard where VOICE CALL: END does.
 *  - A quote-less +COPS: 0 reply sets the operator to None, it does not leave
 *    the previous carrier on screen.
 *  - _drop_hardware clears the signal and the carrier but NOT caller_id or
 *    the IMEI.
 *
 * ============ AND FOUR PLACES IT DELIBERATELY IS NOT 1:1 ============
 *
 * Each of these was a freeze or a dead call on the real phone, and each is
 * pinned by a test in test_modem.c section 11:
 *
 *  - RING, +CLIP and MISSED_CALL are IGNORED while a call is being placed or
 *    is up. The Python let a waiting call's RING reset the state to RINGING
 *    -- after which the core's ring tick owned every read_keypress() and the
 *    End key stopped working -- and let its MISSED_CALL end the live call.
 *  - One empty AT+CLCC does not end a call; ND_CLCC_EMPTY_TO_END in a row do.
 *    The first CLCC after ATD is a full poll interval out, not immediate.
 *  - dial() and answer() carry ATD or ATA and nothing else, because the UI
 *    thread is blocked on them. CPCMFRM, CPCMREG=1 and the audio pipes are
 *    owed to the modem thread's next tick; see nd_modem_audio.c's header.
 *  - _drop_hardware DOES stop the audio pipes. A pipe recorded as live with
 *    no modem behind it made every later call start silent.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nd_clock.h"
#include "nd_log.h"
#include "nd_modem_priv.h"
#include "nd_paths.h"
#include "nd_settings.h"
#include "nd_types.h"

const int ND_BAR_THRESHOLDS[4] = {2, 8, 14, 20};

/* AT+CLCC <stat> names, for the console. */
static const char *clcc_name(int32_t stat)
{
    switch (stat) {
    case ND_CLCC_CONNECTED:
        return "CONNECTED";
    case ND_CLCC_HELD:
        return "HELD";
    case ND_CLCC_CALLING:
        return "CALLING";
    case ND_CLCC_RINGING:
        return "RINGING";
    case ND_CLCC_INCOMING:
        return "INCOMING";
    case ND_CLCC_WAITING:
        return "WAITING";
    default:
        return NULL;
    }
}

static const char *state_name(nd_call_state s)
{
    switch (s) {
    case ND_CALL_CALLING:
        return "CALLING";
    case ND_CALL_RINGING:
        return "RINGING";
    case ND_CALL_CONNECTED:
        return "CONNECTED";
    case ND_CALL_IDLE:
    default:
        return "IDLE";
    }
}

/* ------------------------------------------------------------------ *
 * Little string helpers, each matching one Python expression exactly
 * ------------------------------------------------------------------ */

static bool py_space(char c)
{
    uint8_t u = (uint8_t)c;

    return u == ' ' || u == '\t' || u == '\n' || u == '\v' || u == '\f' || u == '\r' ||
           u == 0x1cu || u == 0x1du || u == 0x1eu || u == 0x1fu;
}

/* str.strip(), in place. */
static void py_strip(char *s)
{
    size_t start = 0u;
    size_t end;

    if (s == NULL)
        return;
    end = strlen(s);
    while (start < end && py_space(s[start]))
        start++;
    while (end > start && py_space(s[end - 1u]))
        end--;
    memmove(s, &s[start], end - start);
    s[end - start] = '\0';
}

/* Everything after the first ':'; NULL when there is none. Python's
 * line.split(":", 1)[1]. */
static const char *after_colon(const char *line)
{
    const char *c = strchr(line, ':');

    return (c != NULL) ? c + 1 : NULL;
}

/* Python's s.split(",")[idx]. False when there are not that many fields. */
static bool comma_field(const char *s, size_t idx, char *out, size_t out_sz)
{
    size_t field = 0u;
    const char *start = s;

    for (;;) {
        const char *comma = strchr(start, ',');
        size_t len = (comma != NULL) ? (size_t)(comma - start) : strlen(start);

        if (field == idx) {
            if (len >= out_sz)
                len = out_sz - 1u;
            memcpy(out, start, len);
            out[len] = '\0';
            return true;
        }
        if (comma == NULL)
            return false;
        start = comma + 1;
        field++;
    }
}

/* Python's line.split('"')[k]. Part k is the text between quote k and quote
 * k+1 (1-based quotes), or the tail after quote k when there is no k+1th --
 * which is why 'a"b'.split('"')[1] is "b" with only ONE quote present.
 * False when the line has fewer than k quotes. */
static bool split_quote(const char *line, size_t k, char *out, size_t out_sz)
{
    const char *p = line;
    size_t seen = 0u;
    const char *start;
    const char *end;
    size_t len;

    while (seen < k) {
        p = strchr(p, '"');
        if (p == NULL)
            return false;
        p++;
        seen++;
    }
    start = p;
    end = strchr(start, '"');
    len = (end != NULL) ? (size_t)(end - start) : strlen(start);
    if (len >= out_sz)
        len = out_sz - 1u;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

/* str.isdigit() for a byte string: non-empty and every character '0'..'9'. */
static bool all_digits(const char *s)
{
    size_t i;

    if (s == NULL || s[0] == '\0')
        return false;
    for (i = 0u; s[i] != '\0'; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

/* One UTF-8 code point. Returns the byte length; an invalid lead byte counts
 * as one character, which is the closest a byte string gets to what Python's
 * str already guarantees. */
static size_t utf8_step(const char *s, uint32_t *cp)
{
    uint8_t c = (uint8_t)s[0];
    size_t need;
    uint32_t v;
    size_t i;

    if (c < 0x80u) {
        *cp = c;
        return 1u;
    }
    if ((c & 0xe0u) == 0xc0u) {
        need = 2u;
        v = c & 0x1fu;
    } else if ((c & 0xf0u) == 0xe0u) {
        need = 3u;
        v = c & 0x0fu;
    } else if ((c & 0xf8u) == 0xf0u) {
        need = 4u;
        v = c & 0x07u;
    } else {
        *cp = 0xfffdu;
        return 1u;
    }
    for (i = 1u; i < need; i++) {
        if (((uint8_t)s[i] & 0xc0u) != 0x80u) {
            *cp = 0xfffdu;
            return 1u;
        }
        v = (v << 6) | ((uint8_t)s[i] & 0x3fu);
    }
    *cp = v;
    return need;
}

static size_t utf8_chars(const char *s)
{
    size_t n = 0u;
    size_t i = 0u;

    while (s[i] != '\0') {
        uint32_t cp;

        i += utf8_step(&s[i], &cp);
        n++;
    }
    return n;
}

/* str.encode("ascii", "replace"): one '?' per code point over U+007F. */
static size_t ascii_replace(const char *s, char *out, size_t out_sz)
{
    size_t w = 0u;
    size_t i = 0u;

    while (s[i] != '\0' && w + 1u < out_sz) {
        uint32_t cp;

        i += utf8_step(&s[i], &cp);
        out[w++] = (cp < 0x80u) ? (char)cp : '?';
    }
    out[w] = '\0';
    return w;
}

size_t nd_modem__filter_number(const char *in, char *out, size_t out_sz)
{
    size_t w = 0u;
    size_t i;

    if (out == NULL || out_sz == 0u)
        return 0u;
    out[0] = '\0';
    if (in == NULL)
        return 0u;

    for (i = 0u; in[i] != '\0' && w + 1u < out_sz; i++) {
        if (strchr("0123456789*#+", in[i]) != NULL)
            out[w++] = in[i];
    }
    out[w] = '\0';
    return w;
}

/* ------------------------------------------------------------------ *
 * State, all of it behind st_mu
 * ------------------------------------------------------------------ */

/* Exported so nd_modem_audio.c and nd_modem_sim.c can touch the same shared
 * fields under the same mutex. Never held across a syscall or a sleep. */
void nd_modem__lock(nd_modem *m)
{
    (void)pthread_mutex_lock(&m->st_mu);
}

void nd_modem__unlock(nd_modem *m)
{
    (void)pthread_mutex_unlock(&m->st_mu);
}

static void lock_state(nd_modem *m)
{
    nd_modem__lock(m);
}

static void unlock_state(nd_modem *m)
{
    nd_modem__unlock(m);
}

static nd_call_state get_state(nd_modem *m)
{
    nd_call_state s;

    lock_state(m);
    s = m->state;
    unlock_state(m);
    return s;
}

static void set_state(nd_modem *m, nd_call_state s)
{
    lock_state(m);
    m->state = s;
    unlock_state(m);
}

/* Did the last probe see a candidate AT port? The one input to
 * nd_modem__may_simulate() that is not the link state, and the reason a dial
 * on a desktop is still faked while a dial on a phone with an unopenable
 * SIM7600 is refused. */
static bool saw_radio(nd_modem *m)
{
    bool v;

    lock_state(m);
    v = m->saw_candidates;
    unlock_state(m);
    return v;
}

/* One short phrase for "why can this phone not use its radio", for the two
 * refusals the user actually sees. The probe's own per-port reason is the
 * best answer there is -- "/dev/ttyUSB2: Permission denied" names the fault
 * exactly -- and the fault reason is the fallback for a modem that was
 * adopted and then died. */
static void no_modem_reason(nd_modem *m, char *out, size_t out_sz)
{
    lock_state(m);
    if (m->last_probe_why[0] != '\0')
        (void)nd_strlcpy(out, m->last_probe_why, out_sz);
    else if (m->fault_why[0] != '\0')
        (void)nd_strlcpy(out, m->fault_why, out_sz);
    else
        (void)nd_strlcpy(out, "no modem is answering", out_sz);
    unlock_state(m);
}

/* The call coming up, from whichever of VOICE CALL: BEGIN, CLCC <stat> 0,
 * ATA or the simulated dial reports it first. The timer starts on the first
 * report and the audio work is armed on the first report, and a second report
 * of the same call is neither -- re-asserting PCM under a running pipe was
 * the static. Returns true on that first report. */
static bool mark_connected(nd_modem *m)
{
    bool edge = false;

    lock_state(m);
    m->state = ND_CALL_CONNECTED;
    if (!m->call_connected) {
        m->call_connected = true;
        m->call_connected_at = nd_modem__now();
        edge = true;
    }
    unlock_state(m);
    /* hardware and allow_calls are written by this thread only. A pretend
     * call, with or without a modem present, has no pipes to bring up. */
    if (edge && m->hardware && m->allow_calls) {
        m->audio_connect_pending = true;
        m->next_pcm_try = 0.0;
    }
    return edge;
}

/* ------------------------------------------------------------------ *
 * The event ring -- collections.deque(maxlen=8)
 * ------------------------------------------------------------------ */

/* append(): a full deque silently drops the LEFTMOST (oldest). */
void nd_modem__queue(nd_modem *m, const nd_mev *e)
{
    size_t slot;

    lock_state(m);
    if (m->ev_count == ND_MODEM_EVENT_QUEUE_MAX) {
        m->ev_head = (m->ev_head + 1u) % ND_MODEM_EVENT_QUEUE_MAX;
        m->ev_count--;
    }
    slot = (m->ev_head + m->ev_count) % ND_MODEM_EVENT_QUEUE_MAX;
    m->ev[slot] = *e;
    m->ev_count++;
    unlock_state(m);
}

/* appendleft(): a full deque silently drops the RIGHTMOST (newest). */
void nd_modem__queue_front(nd_modem *m, const nd_mev *e)
{
    lock_state(m);
    if (m->ev_count == ND_MODEM_EVENT_QUEUE_MAX)
        m->ev_count--; /* the newest falls off the right */
    m->ev_head = (m->ev_head + ND_MODEM_EVENT_QUEUE_MAX - 1u) % ND_MODEM_EVENT_QUEUE_MAX;
    m->ev[m->ev_head] = *e;
    m->ev_count++;
    unlock_state(m);
}

/* popleft(). */
bool nd_modem__take(nd_modem *m, nd_mev *out)
{
    bool got = false;

    lock_state(m);
    if (m->ev_count > 0u) {
        *out = m->ev[m->ev_head];
        m->ev_head = (m->ev_head + 1u) % ND_MODEM_EVENT_QUEUE_MAX;
        m->ev_count--;
        got = true;
    }
    unlock_state(m);
    return got;
}

static void mev_init(nd_mev *e, nd_mev_kind kind)
{
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->index = -1;
}

static void queue_simple(nd_modem *m, nd_mev_kind kind, const char *detail)
{
    nd_mev e;

    mev_init(&e, kind);
    if (detail != NULL) {
        e.has_detail = true;
        (void)nd_strlcpy(e.text, detail, sizeof e.text);
    }
    nd_modem__queue(m, &e);
}

/* ------------------------------------------------------------------ *
 * URC dispatch -- _handle_urc, line 374
 * ------------------------------------------------------------------ */

static bool starts_with(const char *s, const char *pfx)
{
    return strncmp(s, pfx, strlen(pfx)) == 0;
}

void nd_modem__parse_reg(nd_modem *m, const char *line)
{
    const char *rest = after_colon(line);
    char f0[32];
    char f1[32];
    int32_t v;

    if (rest == NULL)
        return;
    /* Query form is "<n>,<stat>", unsolicited form is "<stat>[,...]". */
    if (comma_field(rest, 1u, f1, sizeof f1)) {
        if (!nd_modem__parse_int(f1, &v))
            return;
    } else if (comma_field(rest, 0u, f0, sizeof f0)) {
        if (!nd_modem__parse_int(f0, &v))
            return;
    } else {
        return;
    }
    lock_state(m);
    m->reg_stat = v;
    unlock_state(m);
}

void nd_modem__handle_urc(nd_modem *m, const char *line)
{
    if (line == NULL)
        return;

    /* Call-flow URCs go to the console: bring-up needs eyes. Note
     * "MISSED_CALL" has no colon here where the prefix tuple has one. */
    if (starts_with(line, "RING") || starts_with(line, "VOICE CALL:") ||
        starts_with(line, "NO CARRIER") || starts_with(line, "MISSED_CALL") ||
        starts_with(line, "+CLIP:") || starts_with(line, "BUSY") || starts_with(line, "NO ANSWER"))
        nd_log(ND_LOG_MODEM, "%s", line);

    if (strcmp(line, "RING") == 0) {
        nd_call_state st = get_state(m);

        if (st == ND_CALL_CALLING || st == ND_CALL_CONNECTED) {
            /* Call waiting, which this phone does not do. Taking it would
             * put the live call's screen into the ring loop. */
            nd_log(ND_LOG_MODEM, "RING with a call up: ignored.");
            return;
        }
        if (st != ND_CALL_RINGING) {
            lock_state(m);
            m->state = ND_CALL_RINGING;
            m->caller_id[0] = '\0';
            m->caller_id_known = false;
            unlock_state(m);
            queue_simple(m, ND_MEV_INCOMING, NULL);
        }
        return;
    }
    if (starts_with(line, "+CLIP:")) {
        char number[ND_MODEM_NUMBER_MAX];
        bool changed = false;
        nd_call_state st = get_state(m);

        if (st == ND_CALL_CALLING || st == ND_CALL_CONNECTED)
            return; /* the waiting call's number is not the live call's */
        if (!split_quote(line, 1u, number, sizeof number))
            return; /* IndexError -> number = None */
        if (number[0] == '\0')
            return; /* `if number and ...` */
        lock_state(m);
        if (!m->caller_id_known || strcmp(m->caller_id, number) != 0) {
            (void)nd_strlcpy(m->caller_id, number, sizeof m->caller_id);
            m->caller_id_known = true;
            changed = true;
        }
        unlock_state(m);
        if (changed) {
            nd_mev e;

            mev_init(&e, ND_MEV_INCOMING);
            e.has_detail = true;
            (void)nd_strlcpy(e.text, number, sizeof e.text);
            nd_modem__queue(m, &e);
        }
        return;
    }
    if (starts_with(line, "VOICE CALL: BEGIN")) {
        nd_mev e;

        (void)mark_connected(m);
        lock_state(m);
        mev_init(&e, ND_MEV_CONNECTED);
        if (m->caller_id_known) {
            e.has_detail = true;
            (void)nd_strlcpy(e.text, m->caller_id, sizeof e.text);
        }
        unlock_state(m);
        nd_modem__queue(m, &e);
        return;
    }
    if (starts_with(line, "VOICE CALL: END") || strcmp(line, "NO CARRIER") == 0) {
        if (get_state(m) != ND_CALL_IDLE) {
            set_state(m, ND_CALL_IDLE);
            nd_modem__stop_call_audio(m);
            queue_simple(m, ND_MEV_ENDED, line);
        }
        return;
    }
    if (starts_with(line, "MISSED_CALL:")) {
        char text[ND_MODEM_TEXT_MAX];
        const char *rest = after_colon(line);
        nd_call_state st = get_state(m);

        if (st == ND_CALL_CALLING || st == ND_CALL_CONNECTED) {
            /* A waiting call that rang out. The Python ended the LIVE call
             * here, leaving the line up on the modem with the phone back on
             * the home screen and nothing to press. */
            nd_log(ND_LOG_MODEM, "MISSED_CALL with a call up: ignored.");
            return;
        }
        /* No `state != IDLE` guard here, unlike VOICE CALL: END. */
        set_state(m, ND_CALL_IDLE);
        nd_modem__stop_call_audio(m);
        (void)nd_strlcpy(text, rest != NULL ? rest : "", sizeof text);
        py_strip(text);
        queue_simple(m, ND_MEV_MISSED, text);
        return;
    }
    if (starts_with(line, "+CMTI:")) {
        /* +CMTI: "SM",3 -- rsplit(",", 1)[1]; no comma at all is silent. */
        const char *comma = strrchr(line, ',');
        nd_mev e;
        int32_t index;

        if (comma == NULL || !nd_modem__parse_int(comma + 1, &index))
            return;
        mev_init(&e, ND_MEV_SMS_RECEIVED);
        e.has_detail = true;
        e.index = index;
        nd_modem__queue(m, &e);
        return;
    }
    if (starts_with(line, "+CEREG:") || starts_with(line, "+CREG:"))
        nd_modem__parse_reg(m, line);
}

/* ------------------------------------------------------------------ *
 * Reply parsers
 * ------------------------------------------------------------------ */

void nd_modem__parse_csq(nd_modem *m, const nd_lines *lines)
{
    size_t i;

    for (i = 0u; i < lines->n; i++) {
        const char *line = nd_modem__lines_get(lines, i);
        const char *rest;
        const char *second;
        char seg[64];
        char field[64];
        size_t len;
        int32_t v;

        if (!starts_with(line, "+CSQ:"))
            continue;
        /* split(":")[1] -- no maxsplit, so it stops at a second colon. */
        rest = after_colon(line);
        if (rest == NULL)
            continue;
        second = strchr(rest, ':');
        len = (second != NULL) ? (size_t)(second - rest) : strlen(rest);
        if (len >= sizeof seg)
            len = sizeof seg - 1u;
        memcpy(seg, rest, len);
        seg[len] = '\0';
        if (!comma_field(seg, 0u, field, sizeof field))
            continue;
        if (!nd_modem__parse_int(field, &v))
            continue; /* parse failure leaves _csq unchanged */
        lock_state(m);
        m->csq = v; /* the LAST matching line wins */
        unlock_state(m);
    }
}

void nd_modem__parse_cops(nd_modem *m, const nd_lines *lines)
{
    size_t i;

    for (i = 0u; i < lines->n; i++) {
        const char *line = nd_modem__lines_get(lines, i);
        char name[32];

        if (!starts_with(line, "+COPS:"))
            continue;
        lock_state(m);
        if (strchr(line, '"') != NULL && split_quote(line, 1u, name, sizeof name)) {
            (void)nd_strlcpy(m->operator_name, name, sizeof m->operator_name);
            m->operator_known = true;
        } else {
            /* A quote-less "+COPS: 0" sets the operator to None. */
            m->operator_name[0] = '\0';
            m->operator_known = false;
        }
        unlock_state(m);
    }
}

int32_t nd_modem__bars(int32_t csq)
{
    int32_t bars = 0;
    size_t i;

    if (csq < 0 || csq == 99)
        return 0;
    for (i = 0u; i < ND_ARRAY_LEN(ND_BAR_THRESHOLDS); i++) {
        if (csq >= ND_BAR_THRESHOLDS[i])
            bars++;
    }
    return bars;
}

/* ------------------------------------------------------------------ *
 * _poll_clcc, line 489
 * ------------------------------------------------------------------ */

void nd_modem__poll_clcc(nd_modem *m)
{
    char final[64];
    nd_lines *lines = &m->collected;
    bool have_stat = false;
    int32_t stat = 0;
    size_t i;

    if (!nd_modem__transact(m, "AT+CLCC", ND_CLCC_POLL_S, final, sizeof final, lines))
        return;
    if (strcmp(final, "OK") != 0)
        return;

    for (i = 0u; i < lines->n; i++) {
        const char *line = nd_modem__lines_get(lines, i);
        const char *rest;
        char field[32];

        if (!starts_with(line, "+CLCC:"))
            continue;
        rest = after_colon(line);
        if (rest == NULL)
            continue;
        if (!comma_field(rest, 2u, field, sizeof field))
            continue; /* IndexError -> try the next line */
        if (!nd_modem__parse_int(field, &stat))
            continue; /* ValueError -> try the next line */
        have_stat = true;
        break;
    }

    if (!have_stat) {
        if (get_state(m) != ND_CALL_IDLE) {
            /* One empty list is also what the modem answers in the beat
             * between ATD returning OK and the call entering its table, and
             * between RING and the same. Ending the call on one tore down
             * calls that were still being placed. */
            m->clcc_empty++;
            if (m->clcc_empty < ND_CLCC_EMPTY_TO_END) {
                nd_log(ND_LOG_MODEM, "CLCC: no call in the list (%d of %d); asking again.",
                       (int)m->clcc_empty, (int)ND_CLCC_EMPTY_TO_END);
                return;
            }
            nd_log(ND_LOG_MODEM, "CLCC: no call in the list; ending call state.");
            set_state(m, ND_CALL_IDLE);
            nd_modem__stop_call_audio(m);
            queue_simple(m, ND_MEV_ENDED, "CLCC empty");
        }
        return;
    }
    m->clcc_empty = 0;

    if (stat != m->call_stat) {
        const char *name = clcc_name(stat);

        lock_state(m);
        m->call_stat = stat;
        unlock_state(m);
        if (name != NULL)
            nd_log(ND_LOG_MODEM, "Call progress: %s", name);
        else
            nd_log(ND_LOG_MODEM, "Call progress: %d", (int)stat);
    }
    if (stat == ND_CLCC_CONNECTED)
        (void)mark_connected(m);
}

/* ------------------------------------------------------------------ *
 * Port discovery -- _candidate_ports, line 178
 * ------------------------------------------------------------------ */

typedef struct {
    int32_t iface;
    char dev[ND_MODEM_PORT_MAX];
} cand;

static int cmp_str(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static int cmp_pref(const void *a, const void *b)
{
    const cand *x = a;
    const cand *y = b;

    /* sorted(preferred) on (iface, dev) tuples. */
    if (x->iface != y->iface)
        return (x->iface < y->iface) ? -1 : 1;
    return strcmp(x->dev, y->dev);
}

/* Read /sys/class/tty/<name>/device/../bInterfaceNumber as HEX. */
static bool read_iface(const char *name, int32_t *out)
{
    char virt[ND_PATH_MAX];
    char resolved[ND_PATH_MAX];
    char text[64];
    FILE *f;
    size_t n;

    if (nd_snprintf(virt, sizeof virt, "%s/%s/device/../bInterfaceNumber", ND_MODEM_TTY_DIR,
                    name) != ND_OK)
        return false;
    if (nd_path_resolve(resolved, sizeof resolved, virt) != ND_OK)
        return false;
    f = fopen(resolved, "rb");
    if (f == NULL)
        return false;
    n = fread(text, 1u, sizeof text - 1u, f);
    (void)fclose(f);
    text[n] = '\0';
    py_strip(text);
    return nd_modem__parse_hex(text, out);
}

/* Byte-sorted listing of the entries of one directory whose names start with
 * `prefix`. Returns the count written.
 *
 * THE FILTER IS INSIDE THE LOOP, AND THAT IS THE WHOLE POINT.
 *
 * This used to take every name until the array was full and filter
 * afterwards, with `max` = ND_MODEM_CAND_MAX = 32 -- a number sized for "a
 * SIM7600 enumerates five AT ports", applied to a directory listing. The
 * directory is /sys/class/tty, which on the phone's CONFIG_VT kernel holds
 * tty0..tty63, console, ptmx, tty and ttyFIQ0 as well as the modem's nodes:
 * about seventy-five entries, of which the caller wants five.
 *
 * readdir on sysfs returns kernfs order -- an rbtree keyed on a name hash --
 * so it is neither alphabetical nor creation order, and which 32 of the 75
 * names survived the truncation was an arbitrary, deterministic function of
 * the kernel build. On a kernel where "ttyUSB2" and "ttyUSB3" hashed past the
 * cut, candidate_ports() returned nothing at all and the phone reported "no
 * candidate AT ports (no ttyUSB* in /sys/class/tty)" for ever, with the modem
 * sitting there enumerated. Sorting afterwards cannot undo a name that was
 * never read.
 *
 * Filtering first makes the cap mean what its name says. A NULL prefix keeps
 * the old take-everything behaviour for callers that want it. */
static size_t sorted_listdir(const char *dir, const char *prefix,
                             char names[][ND_MODEM_PORT_MAX], size_t max)
{
    char resolved[ND_PATH_MAX];
    size_t plen = (prefix != NULL) ? strlen(prefix) : 0u;
    DIR *d;
    struct dirent *ent;
    size_t n = 0u;

    if (nd_path_resolve(resolved, sizeof resolved, dir) != ND_OK)
        return 0u;
    d = opendir(resolved);
    if (d == NULL)
        return 0u;
    while ((ent = readdir(d)) != NULL && n < max) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (plen > 0u && strncmp(ent->d_name, prefix, plen) != 0)
            continue;
        (void)nd_strlcpy(names[n], ent->d_name, ND_MODEM_PORT_MAX);
        n++;
    }
    (void)closedir(d);
    /* sorted() is a plain byte-wise sort. ttyUSB10 therefore comes before
     * ttyUSB2, and that is deliberate -- see the file header. */
    qsort(names, n, ND_MODEM_PORT_MAX, cmp_str);
    return n;
}

size_t nd_modem__candidate_ports(nd_modem *m, char ports[][ND_MODEM_PORT_MAX], size_t max)
{
    char names[ND_MODEM_CAND_MAX][ND_MODEM_PORT_MAX];
    cand pref[ND_MODEM_CAND_MAX];
    char rest[ND_MODEM_CAND_MAX][ND_MODEM_PORT_MAX];
    size_t n_pref = 0u;
    size_t n_rest = 0u;
    size_t n_names;
    size_t out = 0u;
    size_t i;

    if (max == 0u)
        return 0u;

    if (m->configured_port[0] != '\0' && strcmp(m->configured_port, ND_MODEM_DEFAULT_PORT) != 0) {
        (void)nd_strlcpy(ports[0], m->configured_port, ND_MODEM_PORT_MAX);
        return 1u;
    }

    /* The "ttyUSB" filter now runs inside the listing rather than here, so
     * the 32-name array bounds the modem's ports and not the tty class. */
    n_names = sorted_listdir(ND_MODEM_TTY_DIR, "ttyUSB", names, ND_ARRAY_LEN(names));
    for (i = 0u; i < n_names; i++) {
        int32_t iface;
        bool have_iface;
        char dev[ND_MODEM_PORT_MAX];

        if (nd_snprintf(dev, sizeof dev, "/dev/%s", names[i]) != ND_OK)
            continue;
        have_iface = read_iface(names[i], &iface);
        if (have_iface && (iface == 2 || iface == 3)) {
            if (n_pref < ND_ARRAY_LEN(pref)) {
                pref[n_pref].iface = iface;
                (void)nd_strlcpy(pref[n_pref].dev, dev, ND_MODEM_PORT_MAX);
                n_pref++;
            }
        } else if (!have_iface || iface != 0) {
            /* iface == 0 is the Qualcomm DIAG port; it never answers AT.
             * An unreadable bInterfaceNumber (Python's None) is kept. */
            if (n_rest < ND_ARRAY_LEN(rest))
                (void)nd_strlcpy(rest[n_rest++], dev, ND_MODEM_PORT_MAX);
        }
    }

    qsort(pref, n_pref, sizeof pref[0], cmp_pref);
    for (i = 0u; i < n_pref && out < max; i++)
        (void)nd_strlcpy(ports[out++], pref[i].dev, ND_MODEM_PORT_MAX);
    for (i = 0u; i < n_rest && out < max; i++)
        (void)nd_strlcpy(ports[out++], rest[i], ND_MODEM_PORT_MAX);
    return out;
}

/* ------------------------------------------------------------------ *
 * _init_modem, line 252
 * ------------------------------------------------------------------ */

void nd_modem__adopt(nd_modem *m, int fd, const char *dev)
{
    m->fd = fd;
    lock_state(m);
    (void)nd_strlcpy(m->port, dev, sizeof m->port);
    unlock_state(m);
    m->rx_len = 0u;
    m->rx_overflow_logged = false;
}

void nd_modem__init_modem(nd_modem *m)
{
    static const char *const INIT_SEQUENCE[] = {
        "ATE0",              /* echo off                                     */
        "AT+CMEE=2",         /* verbose errors                               */
        "AT+CLIP=1",         /* caller ID URCs                               */
        "AT+CVHU=0",         /* make hangup commands actually hang up        */
        "AT+COPS=3,1",       /* short operator names ("T-Mobile", "Tello")   */
        "AT+CMGF=1",         /* text-mode SMS everywhere                     */
        "AT+CNMI=2,1,0,0,0", /* push +CMTI URC on new SMS                    */
    };
    char final[64];
    nd_lines *lines = &m->collected;
    char port[ND_MODEM_PORT_MAX];
    size_t i;

    lock_state(m);
    m->hardware = true;
    /* Adoption is the ONLY way out of a fault. Clearing fault_pending too
     * matters: a modem that dies and recovers before the UI ever drains the
     * latch should not pop a notice about a fault that is already over. */
    m->faulted = false;
    m->fault_pending = false;
    m->fault_why[0] = '\0';
    /* And the same for "unreachable": we just reached it. A modem that goes
     * unreachable again after this must be free to say so again. */
    m->unreachable_announced = false;
    /* Stamped here, not left at 0.0, so the watchdog measures "quiet since we
     * adopted it" rather than "quiet since the epoch" -- a modem that goes
     * silent the instant it is adopted still gets its full grace period. */
    m->last_ok_at = nd_modem__now();
    unlock_state(m);

    for (i = 0u; i < ND_ARRAY_LEN(INIT_SEQUENCE); i++)
        (void)nd_modem__transact(m, INIT_SEQUENCE[i], 2.0, NULL, 0u, NULL);

    if (nd_modem__transact(m, "AT+CGSN", 2.0, final, sizeof final, lines) &&
        strcmp(final, "OK") == 0) {
        bool found = false;

        lock_state(m);
        m->imei[0] = '\0';
        m->imei_known = false;
        unlock_state(m);
        for (i = 0u; i < lines->n && !found; i++) {
            char cand_imei[64];

            (void)nd_strlcpy(cand_imei, nd_modem__lines_get(lines, i), sizeof cand_imei);
            py_strip(cand_imei);
            if (!all_digits(cand_imei))
                continue;
            lock_state(m);
            (void)nd_strlcpy(m->imei, cand_imei, sizeof m->imei);
            m->imei_known = true;
            unlock_state(m);
            found = true;
        }
    }

    {
        char imei[sizeof m->imei];

        lock_state(m);
        (void)nd_strlcpy(port, m->port, sizeof port);
        (void)nd_strlcpy(imei, m->imei_known ? m->imei : "unknown", sizeof imei);
        unlock_state(m);
        nd_log(ND_LOG_MODEM, "SIM7600 on %s (IMEI %s). Using REAL modem.", port, imei);
    }

    /* Catch messages that arrived while the phone was off. */
    queue_simple(m, ND_MEV_SMS_STORED_CHECK, NULL);

    if (!m->allow_calls)
        nd_log(ND_LOG_MODEM, "Real call placement DISABLED "
                             "(system.modem.allow_calls=OFF); dial/answer will simulate.");
}

/* ------------------------------------------------------------------ *
 * _probe_ports / _probe_hardware, lines 199 and 209
 * ------------------------------------------------------------------ */

/* Append "sep<text>" to a bounded buffer, doing nothing once it is full. The
 * reason line is a diagnostic; losing the tail of a very long candidate list
 * is better than any of the alternatives. */
static void why_add(char *why, size_t why_sz, const char *sep, const char *text)
{
    size_t len;

    if (why == NULL || why_sz == 0u)
        return;
    len = strlen(why);
    if (len + 1u >= why_sz)
        return;
    (void)nd_strlcpy(why + len, (len > 0u) ? sep : "", why_sz - len);
    len = strlen(why);
    if (len + 1u < why_sz)
        (void)nd_strlcpy(why + len, text, why_sz - len);
}

/* The candidate list is gathered by the caller now, so that the count is
 * known even on the paths that never get as far as opening a port -- the
 * AT-port lock being held is the important one. "Are there candidates" is
 * the difference between Simulation and ND_MODEM_LINK_UNREACHABLE, and it
 * must not depend on how far into the probe we got. */
static bool probe_ports(nd_modem *m, char ports[][ND_MODEM_PORT_MAX], size_t n, char *why,
                        size_t why_sz)
{
    size_t i;

    if (n == 0u) {
        /* Nothing even enumerated. On the phone that is a USB, kernel or
         * power question, not a modem question -- and it is the single most
         * useful thing this function can say. */
        (void)nd_strlcpy(why, "no candidate AT ports (no ttyUSB* in /sys/class/tty)", why_sz);
        return false;
    }

    for (i = 0u; i < n; i++) {
        char note[ND_MODEM_PORT_MAX + 64];
        char final[64];
        int fd;

        if (!nd_path_exists(ports[i])) {
            /* Enumerated in sysfs but no device node: mdev/udev has not
             * caught up, or /dev is not the one being listed. */
            (void)nd_snprintf(note, sizeof note, "%s: no device node", ports[i]);
            why_add(why, why_sz, "; ", note);
            continue;
        }
        fd = nd_modem__open_port(ports[i]);
        if (fd < 0) {
            /* Almost always EACCES (permissions) or EBUSY/ENODEV (something
             * else holds it, or the modem re-enumerated mid-probe). The errno
             * is the whole diagnosis and it used to be thrown away here. */
            (void)nd_snprintf(note, sizeof note, "%s: %s", ports[i], strerror(errno));
            why_add(why, why_sz, "; ", note);
            continue;
        }
        /* The Python assigns self.fd/self.port BEFORE testing, so the
         * transaction and any teardown inside it see a real port. */
        nd_modem__adopt(m, fd, ports[i]);
        if (nd_modem__transact(m, "AT", 1.0, final, sizeof final, NULL) &&
            strcmp(final, "OK") == 0) {
            nd_modem__init_modem(m);
            return true;
        }
        /* Opened, but did not answer "OK" in a second. A DIAG port, a port
         * another process is mid-transaction on, or a modem still booting. */
        (void)nd_snprintf(note, sizeof note, "%s: AT -> %s", ports[i],
                          (final[0] != '\0') ? final : "no reply");
        why_add(why, why_sz, "; ", note);
        /* The Python closes the local `fd` here even when _drop_hardware has
         * already closed it -- a double close it gets away with because the
         * exception is swallowed. In C the number may have been recycled by
         * another thread, so close it only if we still own it. The visible
         * behaviour (skip this candidate, keep probing) is unchanged. */
        {
            bool still_ours = (m->fd == fd);

            m->fd = -1;
            lock_state(m);
            m->port[0] = '\0';
            unlock_state(m);
            if (still_ours)
                (void)close(fd);
        }
    }
    return false;
}

/* Print `why`, but only when it is not what was printed last time.
 *
 * The probe runs every PROBE_RETRY_S forever while there is no modem, so an
 * unconditional print would put the same line on the serial console six times
 * a minute for the life of the phone. Printing on CHANGE gives the one line
 * that matters at boot, and a new line the moment the situation changes --
 * ports appearing, permissions changing, S45modem letting go. */
static void probe_note(nd_modem *m, const char *why)
{
    if (why == NULL || why[0] == '\0')
        return;
    if (strcmp(m->last_probe_why, why) == 0)
        return;
    /* Read by the UI thread through nd_modem_status_snapshot(), so it is
     * st_mu state like every other field the snapshot copies. */
    lock_state(m);
    (void)nd_strlcpy(m->last_probe_why, why, sizeof m->last_probe_why);
    unlock_state(m);
    nd_log(ND_LOG_MODEM, "No AT port: %s", why);
}

/* Record what the kernel is showing us, and hold the boot grace open while
 * there is nothing there yet.
 *
 * Two jobs, both about the same mistake: latching a verdict on a bus that has
 * not settled. `saw_candidates` decides Simulation vs UNREACHABLE and must be
 * set from the CANDIDATE LIST, not from how far the probe got -- a lock held
 * by S45modem stops the probe dead and says nothing at all about whether the
 * phone has a radio. And the grace is pushed out whenever the candidate list
 * is empty, or the first time it stops being empty, so that the window the
 * header promises is measured against the modem appearing rather than against
 * the UI starting. ND_MODEM_LATE_GRACE_MAX_S caps the whole thing. */
static void note_candidates(nd_modem *m, size_t n)
{
    double now = nd_modem__now();
    bool appeared;

    lock_state(m);
    appeared = (n > 0u) && !m->saw_candidates;
    m->saw_candidates = (n > 0u);
    if (n == 0u) {
        /* No radio in the phone at all now. Whatever we announced about an
         * unreachable one is stale, and a modem plugged back in later must
         * be able to announce itself again. */
        m->unreachable_announced = false;
    }
    if (m->boot_grace > 0.0 && (n == 0u || appeared)) {
        double want = now + m->boot_grace;

        if (want > m->late_grace_deadline)
            want = m->late_grace_deadline;
        if (want > m->boot_deadline)
            m->boot_deadline = want;
    }
    unlock_state(m);
}

/* Arm the one-shot notice for "there is a radio here and I cannot reach it".
 *
 * The same latch nd_modem__drop_hardware() uses for a modem that died, and
 * armed on the EDGE for the same reason: the probe repeats for the life of
 * the phone, and a modal every ten seconds is a denial of service rather
 * than a diagnosis. `faulted` is deliberately NOT set -- that flag means "we
 * had one and lost it" and clearing it is nd_modem__init_modem()'s job, so
 * borrowing it here would make an unreachable modem indistinguishable from a
 * dropped one in every other place that reads it. */
static void announce_unreachable(nd_modem *m, const char *why)
{
    bool first;

    lock_state(m);
    first = !m->unreachable_announced && !m->faulted;
    if (first) {
        m->unreachable_announced = true;
        m->fault_pending = true;
        (void)nd_strlcpy(m->fault_why, (why != NULL) ? why : "", sizeof m->fault_why);
    }
    unlock_state(m);
    if (first)
        nd_log_err(ND_LOG_MODEM, "MODEM UNREACHABLE: a modem is enumerated and none of its "
                                 "ports could be used (%s).",
                   (why != NULL && why[0] != '\0') ? why : "no reason recorded");
}

bool nd_modem__probe_hardware(nd_modem *m)
{
    char ports[ND_MODEM_CAND_MAX][ND_MODEM_PORT_MAX];
    char why[ND_MODEM_PROBE_WHY_MAX];
    size_t n_cand;
    bool ok;

    /* Armed BEFORE the attempt, so a port held by S45modem still costs a full
     * PROBE_RETRY_S before the next try -- or a BOOT_PROBE_S during the boot
     * grace, when the modem is expected any moment and ten seconds of
     * "No Service" past the moment it answers is ten seconds too many. */
    m->next_probe = nd_modem__now() +
                    (nd_modem__now() < m->boot_deadline ? ND_MODEM_BOOT_PROBE_S : ND_PROBE_RETRY_S);

    n_cand = nd_modem__candidate_ports(m, ports, ND_ARRAY_LEN(ports));
    note_candidates(m, n_cand);

    if (!nd_modem__acquire(m)) {
        /* Silent until now, and it is a real cause on a phone that has one:
         * S45modem's background redial takes the lock per transaction, and a
         * probe that lands inside one looks exactly like having no modem.
         *
         * The two failures are told apart because they are opposite facts.
         * "Held" means another process is mid-transaction with the modem,
         * which is proof the modem is there; try again shortly. "Unusable"
         * means there is no lock file this process can open -- root's atcmd
         * created /tmp/neodct-modem.lock 0644 root:root before we got there
         * and we are ndusr -- and the service will not drive a shared tty
         * unserialised, because that is the corruption the lock exists to
         * prevent and it is what "works sometimes" is made of. */
        char note[ND_MODEM_PROBE_WHY_MAX];

        if (m->lock_unusable) {
            (void)nd_snprintf(note, sizeof note,
                              "AT port lock unusable (%s); refusing to share the tty "
                              "unserialised",
                              m->lock_why[0] != '\0' ? m->lock_why : "unknown");
        } else {
            (void)nd_strlcpy(note, "the AT port lock is held (S45modem or atcmd is mid-session)",
                             sizeof note);
        }
        probe_note(m, note);
        /* Only the unusable case is a verdict. A lock somebody else is
         * holding is a modem being talked to, and saying "unreachable" for
         * that would fire a notice at every phone whose data connection is
         * coming up normally. */
        if (m->lock_unusable && n_cand > 0u && nd_modem__now() >= m->boot_deadline)
            announce_unreachable(m, note);
        return false;
    }
    why[0] = '\0';
    ok = probe_ports(m, ports, n_cand, why, sizeof why);
    nd_modem__release(m);
    if (ok) {
        lock_state(m);
        m->last_probe_why[0] = '\0'; /* so losing it later says why again */
        unlock_state(m);
    } else {
        probe_note(m, why);
        /* Candidates existed and not one of them worked. Past the grace that
         * is not Simulation and never was; it is a radio this phone cannot
         * reach, and it says so out loud exactly once. */
        if (n_cand > 0u && nd_modem__now() >= m->boot_deadline)
            announce_unreachable(m, why);
    }
    return ok;
}

/* ------------------------------------------------------------------ *
 * _drop_hardware, line 355
 * ------------------------------------------------------------------ */

void nd_modem__drop_hardware(nd_modem *m, const char *why)
{
    /* Inside the boot grace a modem that answered once and then went away is
     * a SIM7600 re-enumerating while its firmware boots, which it does. That
     * is not a fault; it is probed for again. */
    bool booting = nd_modem__now() < m->boot_deadline;

    /* NOT "back to Simulation Mode" any more, because it never was: getting
     * here means a modem we had ALREADY ADOPTED stopped working. Every call
     * site is a hard errno on that port, or the watchdog in nd_modem_poll().
     * A phone that has never seen a modem does not come through here at all
     * -- it simply never leaves ND_MODEM_LINK_SIM. See nd_modem.h. */
    if (booting)
        nd_log(ND_LOG_MODEM, "Lost the modem during boot (%s); probing again.", why);
    else
        nd_log_err(ND_LOG_MODEM, "MODEM FAULT: lost the modem (%s).", why);
    if (m->fd >= 0)
        (void)close(m->fd);
    m->fd = -1;
    m->rx_len = 0u;
    m->rx_overflow_logged = false;

    lock_state(m);
    m->port[0] = '\0';
    m->hardware = false;
    m->state = ND_CALL_IDLE;
    m->csq = -1;
    m->reg_stat = -1;
    m->operator_name[0] = '\0';
    m->operator_known = false;
    /* The latch is armed only on the EDGE into fault. A modem that fails, is
     * re-probed, fails again and again would otherwise put a modal in front
     * of the user every ten seconds for ever, which is not a diagnosis, it is
     * a denial of service. `faulted` staying true is what suppresses the
     * repeat; nd_modem__init_modem() clears both on a successful adoption, so
     * a modem that really does come back can fault again later and be
     * reported again. */
    if (!booting) {
        if (!m->faulted)
            m->fault_pending = true;
        m->faulted = true;
        (void)nd_strlcpy(m->fault_why, why != NULL ? why : "", sizeof m->fault_why);
    }
    unlock_state(m);

    /* caller_id and imei are deliberately NOT cleared here -- the Python does
     * not clear them either. The audio pipes ARE, and the Python did not:
     * with the modem gone they are two processes holding the sound card and
     * a dead tty, and while they were recorded as live the next call
     * refused to start audio at all. hardware is already false above, so
     * this queues no CPCMREG=0 for a port that is not there. */
    nd_modem__stop_call_audio(m);
    queue_simple(m, ND_MEV_MODEM_LOST, why);
}

/* ------------------------------------------------------------------ *
 * The PCM path, on the modem thread with the port lock already held
 * ------------------------------------------------------------------ */

/* After ATD: pick the stream rate and ask for PCM over USB. CPCMFRM picks
 * the rate (1 = 16 kHz). If the modem accepts CPCMREG=1 now, the speaker
 * alone is started so ringback is audible; the mic waits for the call to
 * come up, because nothing is listening on the uplink until then. The
 * Python did all of this INSIDE dial(), with the UI thread waiting. */
static void pcm_setup(nd_modem *m)
{
    char cmd[32];
    char final_frm[64];
    char final_reg[64];
    const char *frm = (m->pcm_rate == 16000) ? "1" : "0";

    if (nd_snprintf(cmd, sizeof cmd, "AT+CPCMFRM=%s", frm) != ND_OK)
        return;
    if (!nd_modem__transact(m, cmd, 2.0, final_frm, sizeof final_frm, NULL))
        (void)nd_strlcpy(final_frm, "None", sizeof final_frm);
    if (!nd_modem__transact(m, "AT+CPCMREG=1", 3.0, final_reg, sizeof final_reg, NULL))
        (void)nd_strlcpy(final_reg, "None", sizeof final_reg);
    nd_log(ND_LOG_MODEM, "USB audio setup: CPCMFRM=%s -> %s, CPCMREG=1 -> %s", frm, final_frm,
           final_reg);
    if (strcmp(final_reg, "OK") != 0)
        return; /* the call coming up asks again */
    m->pcm_reg_ok = true;
    /* Either transact can carry a NO CARRIER through the URC handler and end
     * the call under us; a pipe for a call that is over is a leak. */
    if (get_state(m) != ND_CALL_IDLE)
        nd_modem__start_speaker(m);
}

/* The call is up: assert PCM again while nothing is reading the port, then
 * bring the pipes up from a clean buffer. false means the modem would not
 * enable PCM and the caller should try again after the holdoff -- without
 * PCM the pipes would read nothing and push 32 KB/s at a port that is not
 * in PCM mode, so they are not started. */
static bool call_audio_up(nd_modem *m)
{
    char final[64];
    bool got;

    got = nd_modem__transact(m, "AT+CPCMREG=1", 3.0, final, sizeof final, NULL);
    nd_log(ND_LOG_MODEM, "CPCMREG=1 (call up) -> %s", got ? final : "None");
    if (!got || strcmp(final, "OK") != 0)
        return false;
    m->pcm_reg_ok = true;
    if (get_state(m) == ND_CALL_IDLE)
        return true; /* collapsed during the command; nothing to bring up */
    nd_modem__restart_speaker(m);
    nd_modem__start_mic(m);
    return true;
}

/* ------------------------------------------------------------------ *
 * poll() -- line 429
 * ------------------------------------------------------------------ */

void nd_modem_poll(nd_modem *m)
{
    double now;
    size_t i;

    if (m == NULL)
        return;
    now = nd_modem__now();

    /* Pretend-dial completion: Simulation Mode AND real hardware with
     * system.modem.allow_calls OFF. */
    if (m->sim_connect_armed && get_state(m) == ND_CALL_CALLING && now >= m->sim_connect_at) {
        m->sim_connect_armed = false;
        (void)mark_connected(m);
        queue_simple(m, ND_MEV_CONNECTED, NULL);
    }

    /* THE SIMULATED FAULT, and it is checked BEFORE the no-hardware early
     * return below rather than after it. That ordering is the whole point:
     * this hook exists so the fault screen can be seen on QEMU, and QEMU
     * NEVER HAS HARDWARE -- so a check on the far side of `if (!m->hardware)
     * return;` could only ever fire on the one platform it was written to
     * avoid touching. It was on the wrong side of that return until a
     * screenshot of the fault notice turned out to be a screenshot of the
     * ordinary home screen.
     *
     * The `!m->faulted` guard is what stops it re-dropping, and re-logging,
     * on every one of the ten ticks a second. */
    if (nd_path_exists(ND_MODEM_SIM_FAULT)) {
        if (!m->faulted) {
            nd_modem__drop_hardware(m, "simulated fault (" ND_MODEM_SIM_FAULT ")");
            lock_state(m);
            m->fault_from_hook = true;
            unlock_state(m);
        }
        return;
    }
    if (m->fault_from_hook) {
        /* The file is gone, so undo what it did. A REAL fault is not cleared
         * here and must not be: it is cleared only by nd_modem__init_modem(),
         * i.e. by actually adopting a modem again. This branch exists purely
         * so `touch` then `rm` is a loop a person can run twice. */
        lock_state(m);
        m->fault_from_hook = false;
        m->faulted = false;
        m->fault_pending = false;
        m->fault_why[0] = '\0';
        unlock_state(m);
        nd_log(ND_LOG_MODEM, "Simulated fault cleared; back to Simulation Mode.");
    }

    if (!m->hardware) {
        nd_modem__poll_sim(m, now); /* NOT rate limited: full tick rate */
        return;
    }

    /* THE WATCHDOG, and why it is here rather than at the failure sites.
     *
     * Every AT timeout in this service returns a bare `false` that nobody
     * looks at, on purpose -- one missed AT+CSQ is normal on a modem that is
     * busy registering, and the whole design treats it as nothing. That is
     * right, and it is also how a modem could die completely and keep its
     * bars on screen for ever: read() returning 0 or EAGAIN is a `break`, not
     * an errno, so drop_hardware never fires and the last good CSQ just sits
     * there.
     *
     * So the question is not "did that one fail" but "has ANYTHING succeeded
     * lately", which is one comparison against one timestamp, fed from the
     * single choke point every transaction passes through. No counter, no
     * per-command state, and nothing to reset in five places.
     *
     * The simulated fault hook is handled above, before the no-hardware
     * return, for the reason given there. */
    {
        double quiet_since;

        lock_state(m);
        quiet_since = m->last_ok_at;
        unlock_state(m);
        if (quiet_since > 0.0 && now - quiet_since > ND_MODEM_FAULT_AFTER_S) {
            char why[64];

            (void)nd_snprintf(why, sizeof why, "no reply for %.0fs", now - quiet_since);
            nd_modem__drop_hardware(m, why);
            return;
        }
    }

    if (now < m->next_urc)
        return;
    m->next_urc = now + ND_POLL_URC_S;

    if (!nd_modem__acquire(m)) {
        /* CONTENTION IS NOT SILENCE, AND THE WATCHDOG ABOVE CANNOT TELL.
         *
         * This early return sits BELOW the ND_MODEM_FAULT_AFTER_S check, and
         * last_ok_at is fed only by a transaction that completed -- so every
         * tick spent locked out was being counted as a tick the modem failed
         * to answer. S45modem holds the port in long stretches during its
         * data bring-up (`at 'AT+CGATT=1' 40` is one atcmd invocation
         * holding the flock for up to forty seconds, and dial_rounds walks
         * four rounds of an APN matrix), so two of those plus the gaps is
         * ninety seconds of "silence" from a modem that was answering
         * somebody else the whole time. The phone then put a MODEM FAULT
         * modal on screen and blanked the carrier line for a healthy radio.
         *
         * Another process holding this lock is POSITIVE evidence that the
         * modem is there and being talked to, so the clock is stamped
         * forward. A genuinely dead modem is still caught: nothing holds the
         * lock for it, so the ticks that time out do count.
         *
         * The lock being unusable is the other failure and is deliberately
         * NOT stamped -- see nd_modem__acquire(). We are not talking to the
         * modem and neither is anything we can prove, so the watchdog is
         * left to do its job. */
        if (!m->lock_unusable) {
            lock_state(m);
            m->last_ok_at = now;
            unlock_state(m);
        }
        return; /* boot script or atcmd session in progress */
    }
    m->lock_taken_at = now;

    (void)nd_modem__read_pending(m, &m->rx_lines);
    for (i = 0u; i < m->rx_lines.n; i++)
        nd_modem__handle_urc(m, nd_modem__lines_get(&m->rx_lines, i));

    /* Deferred CPCMREG=0: the teardown happened inside a URC handler, where
     * the lock was already held. */
    if (m->pcm_cleanup) {
        m->pcm_cleanup = false;
        (void)nd_modem__transact(m, "AT+CPCMREG=0", 2.0, NULL, 0u, NULL);
    }

    if (get_state(m) != ND_CALL_IDLE) {
        /* The work dial() and answer() left for this thread. Both flags are
         * cleared by nd_modem__stop_call_audio() if the call collapses
         * first, so neither runs for a call that is already over. */
        if (m->pcm_setup_pending) {
            m->pcm_setup_pending = false;
            pcm_setup(m);
        }
        if (m->audio_connect_pending && now >= m->next_pcm_try) {
            m->next_pcm_try = now + ND_AUDIO_RESTART_HOLDOFF_S;
            if (call_audio_up(m))
                m->audio_connect_pending = false;
        }
        if (now >= m->next_clcc) {
            m->next_clcc = now + ND_CLCC_POLL_S;
            nd_modem__poll_clcc(m);
        }
        nd_modem__watch_audio_proc(m, now);
    }

    /* if / elif / elif: at most ONE query per tick, deliberately staggered.
     * All three timers start at 0.0, so the first three ticks fire CSQ, then
     * CEREG?, then COPS?. */
    if (now >= m->next_csq) {
        char final[64];

        m->next_csq = now + ND_POLL_SIGNAL_S;
        if (nd_modem__transact(m, "AT+CSQ", 1.5, final, sizeof final, &m->collected) &&
            strcmp(final, "OK") == 0)
            nd_modem__parse_csq(m, &m->collected);
    } else if (now >= m->next_net) {
        m->next_net = now + ND_POLL_NET_S;
        /* The reply is parsed by _handle_urc, which is why it is not read
         * back here. */
        (void)nd_modem__transact(m, "AT+CEREG?", 1.5, NULL, 0u, NULL);
    } else if (now >= m->next_cops) {
        char final[64];

        m->next_cops = now + ND_POLL_OPERATOR_S;
        if (nd_modem__transact(m, "AT+COPS?", 3.0, final, sizeof final, &m->collected) &&
            strcmp(final, "OK") == 0)
            nd_modem__parse_cops(m, &m->collected);
    }

    nd_modem__release(m);

    /* FAIR SHARE OF THE PORT WITH S45modem.
     *
     * The two sides of this flock are asymmetric. This one takes it
     * non-blocking every ND_POLL_URC_S = 0.5 s and, having taken it, may sit
     * on it for 3.0 s waiting out an AT+COPS? on a modem that is busy
     * registering. The other side is atcmd, which retries `flock -x -n` once
     * a second and gives up after $TIMEOUT tries with exit 3; S45modem's
     * bring-up counts that as a failed AT and calls fail() after five of
     * them, so the phone ends the boot with no data connection and the log
     * line "modem not answering AT" about a modem that answered us sixty
     * times in the same minute.
     *
     * A tick that held the port longer than the tick interval therefore owes
     * the rest of the system an equal window with the lock free. This never
     * delays anything the user is waiting on -- dial(), hangup() and
     * send_sms() take the lock themselves on the UI thread's request and are
     * not gated by next_urc -- it only slows the background CSQ/CEREG/COPS
     * polling down to at most half the port's time, which is what the
     * cadence assumed it was doing all along. */
    {
        double released = nd_modem__now();
        double held = released - m->lock_taken_at;

        if (held > ND_POLL_URC_S) {
            double resume = released + held;

            if (resume > m->next_urc)
                m->next_urc = resume;
        }
    }
}

/* ------------------------------------------------------------------ *
 * Call control -- lines 770, 802, 814
 * ------------------------------------------------------------------ */

static bool do_dial(nd_modem *m, const char *raw)
{
    char number[ND_MODEM_NUMBER_MAX];
    char cmd[ND_MODEM_NUMBER_MAX + 8];
    char final[64];
    bool got;

    (void)nd_modem__filter_number(raw, number, sizeof number);
    /* After the filter, before the empty check: an all-junk number logs an
     * empty dial and then fails. */
    nd_log(ND_LOG_MODEM, "Requesting Dial: %s", number);
    if (number[0] == '\0')
        return false;

    if (!m->hardware || !m->allow_calls) {
        if (m->hardware) {
            /* A live modem with system.modem.allow_calls=OFF. A deliberate
             * development switch, and the log line has always said so. */
            nd_log(ND_LOG_MODEM, "Calls not enabled yet; simulating this dial.");
        } else if (!nd_modem__may_simulate(nd_modem_link_state(m), saw_radio(m))) {
            char reason[ND_MODEM_PROBE_WHY_MAX];

            /* THE PHONE MUST NOT PRETEND IT PLACED A CALL.
             *
             * This branch used to be reached for every kind of "no modem",
             * including the common one on real hardware: a SIM7600 sitting
             * in the phone whose ports the service could not open. The owner
             * then watched ND_CALL_CALLING become ND_CALL_CONNECTED two
             * seconds later, with a call timer running and an End key to
             * press, for a call that was never dialled -- and no audio,
             * because mark_connected() gates the pipes on m->hardware. A
             * failed dial the dialer can report is not a worse outcome than
             * that; it is the only honest one. */
            no_modem_reason(m, reason, sizeof reason);
            nd_log_err(ND_LOG_MODEM, "Dial refused: no usable modem (%s).", reason);
            return false;
        }
        set_state(m, ND_CALL_CALLING);
        m->sim_connect_at = nd_modem__now() + 2.0;
        m->sim_connect_armed = true;
        return true;
    }

    if (nd_snprintf(cmd, sizeof cmd, "ATD%s;", number) != ND_OK)
        return false;

    /* CALLING from before the ATD goes out, so a RING that lands during it
     * is a collision to ignore rather than a call to take. A NO CARRIER
     * final is routed through the URC handler and puts this back to IDLE;
     * a plain ERROR is put back below. */
    set_state(m, ND_CALL_CALLING);
    lock_state(m);
    m->call_stat = -1;
    m->call_connected = false;
    m->call_connected_at = 0.0;
    unlock_state(m);
    m->clcc_empty = 0;
    m->pcm_reg_ok = false;
    m->audio_connect_pending = false;

    got = nd_modem__command(m, cmd, 8.0, final, sizeof final, NULL);
    if (!got || strcmp(final, "OK") != 0) {
        nd_log(ND_LOG_MODEM, "Dial failed (final=%s)", got ? final : "None");
        if (get_state(m) == ND_CALL_CALLING)
            set_state(m, ND_CALL_IDLE);
        return false;
    }

    /* That is all dial() does with the UI waiting on it. The PCM setup and
     * the ringback pipe are the next tick's, and the tick is told to run at
     * once rather than at its half-second cadence. CLCC waits a full poll
     * interval: the modem has not entered the call in its table yet, and an
     * empty list would count against it. */
    m->pcm_setup_pending = true;
    m->next_clcc = nd_modem__now() + ND_CLCC_POLL_S;
    m->next_urc = 0.0;
    return true;
}

static bool do_answer(nd_modem *m)
{
    char final[64];

    if (!m->hardware || !m->allow_calls) {
        (void)mark_connected(m);
        return true;
    }
    if (nd_modem__command(m, "ATA", 8.0, final, sizeof final, NULL) && strcmp(final, "OK") == 0) {
        /* ATA is all of it; the PCM and the pipes are the tick's, at once. */
        (void)mark_connected(m);
        m->next_urc = 0.0;
        return true;
    }
    return false; /* state is deliberately left alone */
}

static bool do_hangup(nd_modem *m)
{
    char final[64];
    bool got;

    nd_log(ND_LOG_MODEM, "Requesting Hangup");
    m->sim_connect_armed = false;
    nd_modem__stop_call_audio(m);
    if (!m->hardware) {
        set_state(m, ND_CALL_IDLE);
        return true;
    }
    /* AT+CHUP is safe with no call up, and it also rejects a live incoming
     * RING even while allow_calls is OFF. */
    got = nd_modem__command(m, "AT+CHUP", 5.0, final, sizeof final, NULL);
    if (!got || strcmp(final, "OK") != 0)
        got = nd_modem__command(m, "ATH", 5.0, final, sizeof final, NULL);
    set_state(m, ND_CALL_IDLE);
    return got && strcmp(final, "OK") == 0;
}

/* ------------------------------------------------------------------ *
 * SMS -- lines 831, 897, 923, 951
 * ------------------------------------------------------------------ */

size_t nd_modem__parse_sms_records(const nd_lines *lines, const char *header, nd_sms_rec *out,
                                   size_t max)
{
    size_t n = 0u;
    bool open_rec = false;
    size_t body_len = 0u;
    size_t i;

    for (i = 0u; i < lines->n; i++) {
        const char *line = nd_modem__lines_get(lines, i);

        if (starts_with(line, header)) {
            char sender[ND_MODEM_NUMBER_MAX];

            if (open_rec)
                n++; /* the previous record closes here */
            if (n >= max) {
                /* The Python's list is unbounded; see OPEN-QUESTIONS.md M-5. */
                open_rec = false;
                break;
            }
            memset(&out[n], 0, sizeof out[n]);
            out[n].index = -1;
            out[n].unread = true;
            /* quoted = line.split('"')[1::2]; sender = quoted[1] if there are
             * at least two quoted fields, i.e. at least three quotes. */
            if (split_quote(line, 3u, sender, sizeof sender))
                (void)nd_strlcpy(out[n].sender, sender, sizeof out[n].sender);
            else
                (void)nd_strlcpy(out[n].sender, "unknown", sizeof out[n].sender);
            if (strcmp(header, "+CMGL:") == 0) {
                const char *rest = after_colon(line);
                char field[32];
                int32_t idx;

                if (rest != NULL && comma_field(rest, 0u, field, sizeof field) &&
                    nd_modem__parse_int(field, &idx))
                    out[n].index = idx;
            }
            open_rec = true;
            body_len = 0u;
        } else if (open_rec) {
            size_t len = strlen(line);

            /* "\n".join(body) */
            if (body_len > 0u && body_len + 1u < sizeof out[n].text)
                out[n].text[body_len++] = '\n';
            if (body_len + len >= sizeof out[n].text)
                len = sizeof out[n].text - 1u - body_len;
            memcpy(&out[n].text[body_len], line, len);
            body_len += len;
            out[n].text[body_len] = '\0';
        }
        /* Lines before the first header are discarded. */
    }
    if (open_rec)
        n++;

    for (i = 0u; i < n; i++)
        py_strip(out[i].text);
    return n;
}

static bool do_send_sms(nd_modem *m, const char *raw_number, const char *raw_text, char *detail,
                        size_t detail_sz)
{
    char number[ND_MODEM_NUMBER_MAX];
    char text[ND_MODEM_TEXT_MAX];
    char wire[ND_MODEM_TEXT_MAX + 2];
    char cmd[ND_MODEM_NUMBER_MAX + 16];
    char final[64];
    char ref[64];
    bool have_ref = false;
    double deadline;
    size_t w = 0u;
    size_t i;
    bool ok = false;

    if (detail != NULL && detail_sz > 0u)
        detail[0] = '\0';

    (void)nd_modem__filter_number(raw_number, number, sizeof number);
    /* Ctrl-Z and ESC are stripped and NOTHING else -- newlines are kept. */
    for (i = 0u; raw_text != NULL && raw_text[i] != '\0' && w + 1u < sizeof text; i++) {
        if (raw_text[i] == '\x1a' || raw_text[i] == '\x1b')
            continue;
        text[w++] = raw_text[i];
    }
    text[w] = '\0';

    if (number[0] == '\0') {
        (void)nd_strlcpy(detail, "no number", detail_sz);
        return false;
    }
    if (text[0] == '\0') {
        (void)nd_strlcpy(detail, "empty message", detail_sz);
        return false;
    }
    nd_log(ND_LOG_MODEM, "Sending SMS to %s (%u chars)", number, (unsigned)utf8_chars(text));

    if (!m->hardware) {
        if (!nd_modem__may_simulate(nd_modem_link_state(m), saw_radio(m))) {
            char reason[ND_MODEM_PROBE_WHY_MAX];

            /* Reported as sent, never transmitted, no way for the owner to
             * tell -- the same lie as a faked dial and quieter. `detail` is
             * rendered verbatim by Messages as "Send failed: <detail>"
             * (nd_modem.h), so the probe's own reason goes on the screen:
             * "Send failed: /dev/ttyUSB2: Permission denied" is a sentence
             * somebody can act on. */
            no_modem_reason(m, reason, sizeof reason);
            (void)nd_snprintf(detail, detail_sz, "no modem: %s", reason);
            nd_log_err(ND_LOG_MODEM, "SMS refused: %s", detail);
            return false;
        }
        nd_log(ND_LOG_MODEM, "(Simulation Mode: pretending the SMS went out.)");
        queue_simple(m, ND_MEV_SMS_SENT, number);
        (void)nd_strlcpy(detail, "simulated", detail_sz);
        return true;
    }

    if (!nd_modem__acquire(m)) {
        (void)nd_strlcpy(detail, "modem port busy", detail_sz);
        return false;
    }

    if (!nd_modem__transact(m, "AT+CMGF=1", 2.0, final, sizeof final, NULL) ||
        strcmp(final, "OK") != 0) {
        (void)snprintf(detail, detail_sz, "text mode rejected (%s)",
                       final[0] != '\0' ? final : "None");
        goto done;
    }

    if (nd_snprintf(cmd, sizeof cmd, "AT+CMGS=\"%s\"\r", number) != ND_OK) {
        (void)nd_strlcpy(detail, "no number", detail_sz);
        goto done;
    }
    if (write(m->fd, cmd, strlen(cmd)) < 0) {
        char why[ND_MODEM_WHY_MAX];

        (void)snprintf(why, sizeof why, "port write failed: %s", strerror(errno));
        nd_modem__drop_hardware(m, why);
        (void)nd_strlcpy(detail, "modem lost", detail_sz);
        goto done;
    }

    if (!nd_modem__wait_sms_prompt(m, ND_SMS_PROMPT_TIMEOUT_S)) {
        /* ESC backs out of a half-open CMGS so the port is not left eating
         * everything we send next as message body. */
        {
            /* Errors ignored, as in the Python: the caller is already being
             * told the prompt never came. */
            ssize_t esc = write(m->fd, "\x1b", 1u);

            ND_UNUSED(esc);
        }
        (void)nd_strlcpy(detail, "no > prompt from modem", detail_sz);
        goto done;
    }

    w = ascii_replace(text, wire, sizeof wire - 1u);
    wire[w++] = '\x1a';
    if (write(m->fd, wire, w) < 0) {
        char why[ND_MODEM_WHY_MAX];

        (void)snprintf(why, sizeof why, "port write failed: %s", strerror(errno));
        nd_modem__drop_hardware(m, why);
        (void)nd_strlcpy(detail, "modem lost", detail_sz);
        goto done;
    }

    ref[0] = '\0';
    deadline = nd_modem__now() + ND_SMS_SEND_TIMEOUT_S;
    while (nd_modem__now() < deadline) {
        (void)nd_modem__read_pending(m, &m->rx_lines);
        for (i = 0u; i < m->rx_lines.n; i++) {
            const char *line = nd_modem__lines_get(&m->rx_lines, i);

            if (starts_with(line, "+CMGS:")) {
                const char *rest = after_colon(line);

                (void)nd_strlcpy(ref, rest != NULL ? rest : "", sizeof ref);
                py_strip(ref);
                have_ref = (ref[0] != '\0');
            } else if (strcmp(line, "OK") == 0) {
                nd_log(ND_LOG_MODEM, "SMS accepted by network (ref %s).", have_ref ? ref : "None");
                queue_simple(m, ND_MEV_SMS_SENT, number);
                (void)nd_strlcpy(detail, have_ref ? ref : "sent", detail_sz);
                ok = true;
                goto done;
            } else if (strcmp(line, "ERROR") == 0 || starts_with(line, "+CMS ERROR") ||
                       starts_with(line, "+CME ERROR")) {
                nd_log(ND_LOG_MODEM, "SMS rejected: %s", line);
                (void)nd_strlcpy(detail, line, detail_sz);
                goto done;
            } else if (nd_modem__is_urc(line)) {
                nd_modem__handle_urc(m, line);
            }
        }
        if (m->fd < 0) {
            (void)nd_strlcpy(detail, "modem lost", detail_sz);
            goto done;
        }
        nd_modem__nap(ND_SMS_WAIT_SLEEP_S);
    }
    (void)nd_strlcpy(detail, "timeout waiting for network", detail_sz);

done:
    nd_modem__release(m);
    return ok;
}

static nd_sms_st do_fetch_sms(nd_modem *m, int32_t index, nd_sms_rec *out)
{
    char cmd[32];
    char final[64];
    nd_sms_rec rec;
    nd_sms_st st = ND_SMS_ERROR;

    /* Two "indices" that are not SIM slots; see OPEN-QUESTIONS.md M-1. */
    if (index == ND_MODEM_SMS_IDX_SIM) {
        bool got = false;

        lock_state(m);
        if (m->sim_sms_pending) {
            memset(out, 0, sizeof *out);
            out->index = ND_MODEM_SMS_IDX_SIM;
            out->unread = true;
            (void)nd_strlcpy(out->sender, m->sim_sms_sender, sizeof out->sender);
            (void)nd_strlcpy(out->text, m->sim_sms_body, sizeof out->text);
            m->sim_sms_pending = false;
            got = true;
        }
        unlock_state(m);
        return got ? ND_SMS_OK : ND_SMS_ERROR;
    }
    if (index == ND_MODEM_SMS_IDX_STORED)
        return ND_SMS_ERROR; /* the sweep is nd_modem_read_stored_sms() */

    if (!m->hardware)
        return ND_SMS_ERROR;
    if (!nd_modem__acquire(m))
        return ND_SMS_BUSY;

    (void)nd_modem__transact(m, "AT+CMGF=1", 2.0, NULL, 0u, NULL);
    (void)nd_snprintf(cmd, sizeof cmd, "AT+CMGR=%d", (int)index);
    if (!nd_modem__transact(m, cmd, 5.0, final, sizeof final, &m->collected) ||
        strcmp(final, "OK") != 0) {
        nd_log(ND_LOG_MODEM, "CMGR %d failed (%s)", (int)index, final[0] != '\0' ? final : "None");
        goto done;
    }
    if (nd_modem__parse_sms_records(&m->collected, "+CMGR:", &rec, 1u) == 0u)
        goto done;

    (void)nd_snprintf(cmd, sizeof cmd, "AT+CMGD=%d", (int)index);
    (void)nd_modem__transact(m, cmd, 5.0, NULL, 0u, NULL);

    rec.index = index;
    *out = rec;
    nd_log(ND_LOG_MODEM, "SMS received from %s (%u chars)", rec.sender,
           (unsigned)utf8_chars(rec.text));
    st = ND_SMS_OK;

done:
    nd_modem__release(m);
    return st;
}

static nd_sms_st do_read_stored(nd_modem *m, nd_sms_rec *out, size_t max, size_t *n_out)
{
    char final[64];
    nd_sms_st st = ND_SMS_ERROR;
    size_t n = 0u;
    size_t i;

    *n_out = 0u;
    if (!m->hardware)
        return ND_SMS_ERROR;
    if (!nd_modem__acquire(m))
        return ND_SMS_BUSY;

    (void)nd_modem__transact(m, "AT+CMGF=1", 2.0, NULL, 0u, NULL);
    if (!nd_modem__transact(m, "AT+CMGL=\"REC UNREAD\"", 8.0, final, sizeof final, &m->collected) ||
        strcmp(final, "OK") != 0)
        goto done;

    n = nd_modem__parse_sms_records(&m->collected, "+CMGL:", out, max);
    for (i = 0u; i < n; i++) {
        char cmd[32];

        if (out[i].index < 0)
            continue;
        (void)nd_snprintf(cmd, sizeof cmd, "AT+CMGD=%d", (int)out[i].index);
        (void)nd_modem__transact(m, cmd, 5.0, NULL, 0u, NULL);
    }
    if (n > 0u)
        nd_log(ND_LOG_MODEM, "Imported %u stored SMS from the SIM.", (unsigned)n);
    *n_out = n;
    st = ND_SMS_OK;

done:
    nd_modem__release(m);
    return st;
}

/* ------------------------------------------------------------------ *
 * The request slot
 * ------------------------------------------------------------------ */

static void run_request(nd_modem *m, nd_modem_req *r)
{
    switch (r->kind) {
    case ND_REQ_DIAL:
        r->ok = do_dial(m, r->s1);
        break;
    case ND_REQ_ANSWER:
        r->ok = do_answer(m);
        break;
    case ND_REQ_HANGUP:
        r->ok = do_hangup(m);
        break;
    case ND_REQ_SEND_SMS:
        r->ok = do_send_sms(m, r->s1, r->s2, r->detail, sizeof r->detail);
        break;
    case ND_REQ_FETCH_SMS:
        r->sms_st = do_fetch_sms(m, r->i1, r->rec_out);
        break;
    case ND_REQ_READ_STORED:
        r->sms_st = do_read_stored(m, r->rec_out, r->rec_max, &r->rec_n);
        break;
    case ND_REQ_SEND_AT:
    default:
        if (!m->hardware) {
            r->err = ND_ERR_HARDWARE;
        } else if (nd_modem__command(m, r->s1, r->timeout, r->final_out, r->final_sz,
                                     r->lines_out)) {
            r->err = ND_OK;
        } else {
            r->err = ND_ERR_TIMEOUT;
        }
        break;
    }
}

/* Post one request and block until the modem thread has run it. With no
 * thread -- construction failed, or a unit test drove nd_modem__create()
 * directly -- it runs inline on the caller. */
static void submit(nd_modem *m, nd_modem_req *r)
{
    r->done = false;

    if (!m->thread_started) {
        run_request(m, r);
        r->done = true;
        return;
    }

    (void)pthread_mutex_lock(&m->req_mu);
    while (m->pending != NULL && !m->quit)
        (void)pthread_cond_wait(&m->done_cv, &m->req_mu);
    if (m->quit) {
        (void)pthread_mutex_unlock(&m->req_mu);
        r->done = true;
        return;
    }
    m->pending = r;
    (void)pthread_cond_signal(&m->req_cv);
    while (!r->done)
        (void)pthread_cond_wait(&m->done_cv, &m->req_mu);
    (void)pthread_mutex_unlock(&m->req_mu);
}

static void *modem_thread(void *arg)
{
    nd_modem *m = arg;

    for (;;) {
        nd_modem_req *r;
        struct timespec ts;

        (void)pthread_mutex_lock(&m->req_mu);
        if (m->quit) {
            /* A request posted in the instant before quit was set would
             * otherwise leave its caller blocked on done_cv for ever. */
            if (m->pending != NULL) {
                m->pending->done = true;
                m->pending = NULL;
            }
            (void)pthread_cond_broadcast(&m->done_cv);
            (void)pthread_mutex_unlock(&m->req_mu);
            break;
        }
        r = m->pending;
        (void)pthread_mutex_unlock(&m->req_mu);

        if (r != NULL) {
            run_request(m, r);
            (void)pthread_mutex_lock(&m->req_mu);
            m->pending = NULL;
            r->done = true;
            (void)pthread_cond_broadcast(&m->done_cv);
            (void)pthread_mutex_unlock(&m->req_mu);
            continue; /* service a queued caller before ticking again */
        }

        nd_modem_poll(m);

        /* One UI tick. read_keypress() called poll() at this rate and every
         * cadence inside poll() was measured against it. */
        (void)clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100L * 1000L * 1000L;
        if (ts.tv_nsec >= 1000L * 1000L * 1000L) {
            ts.tv_nsec -= 1000L * 1000L * 1000L;
            ts.tv_sec += 1;
        }
        (void)pthread_mutex_lock(&m->req_mu);
        if (m->pending == NULL && !m->quit)
            (void)pthread_cond_timedwait(&m->req_cv, &m->req_mu, &ts);
        (void)pthread_mutex_unlock(&m->req_mu);
    }
    return NULL;
}

/* ------------------------------------------------------------------ *
 * Construction and teardown
 * ------------------------------------------------------------------ */

static int32_t pcm_rate_setting(void)
{
    const char *v = nd_settings_get(ND_SET_HW_MODEM_PCM_RATE, "16000");
    int32_t rate;

    if (v == NULL || !nd_modem__parse_int(v, &rate))
        return ND_MODEM_PCM_RATE_DEFAULT;
    return rate;
}

static void port_from_settings(char *out, size_t out_sz)
{
    const char *v = nd_settings_get(ND_SET_HW_MODEM_AT_PORT, ND_MODEM_DEFAULT_PORT);

    (void)nd_strlcpy(out, v != NULL ? v : ND_MODEM_DEFAULT_PORT, out_sz);
}

static double boot_grace_setting(void)
{
    const char *v = nd_settings_get(ND_SET_MODEM_BOOT_GRACE, "30");
    int32_t secs;

    if (v == NULL || !nd_modem__parse_int(v, &secs) || secs < 0)
        return ND_MODEM_BOOT_GRACE_DEFAULT_S;
    return (double)secs;
}

static bool calls_enabled_setting(void)
{
    /* Default flipped to ON for the 0.3.0a call bring-up. In the Python an
     * EXCEPTION while reading returns True and an unrecognised value returns
     * False; nd_settings_get() cannot raise, so only the second path is
     * reachable and the default handles the first. */
    return nd_setting_modem_truthy(nd_settings_get(ND_SET_MODEM_ALLOW_CALLS, "ON"));
}

/* Open (creating if need be) the AT-port lock file. `why` gets
 * "<path>: <strerror>" on failure and "" on success.
 *
 * ============ WHY THE READ-ONLY RETRY IS THE WHOLE FIX ============
 *
 * /tmp is a tmpfs, so this file does not exist until somebody makes it, and
 * two different principals race to be that somebody every boot. nd-core is
 * ndusr (since 0.5.0b) under run_neodct.sh's umask 0027, so when it wins the
 * file is 0640 ndusr:ndusr and root can still open it -- root ignores modes.
 * atcmd is root out of S45modem under init's umask 0022, so when IT wins the
 * file is 0644 root:root, and this open, asking for O_RDWR, gets EACCES. /tmp
 * is sticky 1777, so ndusr cannot replace it either.
 *
 * That was the end of the lock for the rest of the session: lock_fd stayed
 * -1 and nd_modem__acquire() returned true anyway, so the core drove
 * /dev/ttyUSB2 while S45modem's APN walk was driving the same port -- each
 * side eating the other's reply lines, both reporting failures, nothing
 * logged. It is decided per boot, which is exactly the shape of "the modem
 * rarely works, though once it showed Tello".
 *
 * flock(2) locks the open file DESCRIPTION and does not care what access
 * mode it was opened with, so LOCK_EX on an O_RDONLY descriptor is a real
 * exclusive lock against busybox flock in S45modem. The write permission was
 * never needed -- nothing is ever written to this file. Retrying read-only
 * therefore makes the create order stop mattering, from this side alone,
 * with no cooperation from a script this module does not own. */
static int open_lock_file(char *why, size_t why_sz)
{
    char resolved[ND_PATH_MAX];
    char dir[ND_PATH_MAX];
    const char *slash;
    int fd;
    int saved;

    if (why != NULL && why_sz > 0u)
        why[0] = '\0';

    /* ND_ROOT-resolved so a host test locks its own scratch file rather than
     * fighting a real phone's. mkdir -p of the parent is a C-only addition:
     * /tmp always exists in production, but <root>/tmp does not. */
    (void)nd_strlcpy(dir, ND_MODEM_LOCK_FILE, sizeof dir);
    slash = strrchr(dir, '/');
    if (slash != NULL && slash != dir) {
        dir[(size_t)(slash - dir)] = '\0';
        (void)nd_mkdir_p(dir, 0755u);
    }
    if (nd_path_resolve(resolved, sizeof resolved, ND_MODEM_LOCK_FILE) != ND_OK) {
        if (why != NULL)
            (void)nd_snprintf(why, why_sz, "%s: path too long", ND_MODEM_LOCK_FILE);
        return -1;
    }
    fd = open(resolved, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (fd >= 0)
        return fd;
    saved = errno;
    fd = open(resolved, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        /* Worth a line: it means another user owns the file, which is the
         * normal state on a phone where root's atcmd ran first, and it is
         * the fingerprint of the bug above if it ever comes back. */
        nd_log(ND_LOG_MODEM, "%s belongs to another user (%s); locking it read-only.", resolved,
               strerror(saved));
        return fd;
    }
    if (errno == ENOENT || errno == EACCES)
        errno = saved; /* the create attempt is the more informative failure */
    if (why != NULL)
        (void)nd_snprintf(why, why_sz, "%s: %s", resolved, strerror(errno));
    return -1;
}

void nd_modem__lock_reopen(nd_modem *m)
{
    double now;
    char why[ND_MODEM_WHY_MAX];
    int fd;

    if (m == NULL || m->lock_fd >= 0)
        return;
    now = nd_modem__now();
    if (now < m->next_lock_try)
        return;
    m->next_lock_try = now + ND_MODEM_LOCK_RETRY_S;

    fd = open_lock_file(why, sizeof why);
    if (fd >= 0) {
        m->lock_fd = fd;
        m->lock_why[0] = '\0';
        return;
    }
    /* Printed on CHANGE only, like probe_note(): this is retried every couple
     * of seconds for the life of the phone and one line per attempt would
     * bury the boot log. */
    if (strcmp(m->lock_why, why) != 0) {
        (void)nd_strlcpy(m->lock_why, why, sizeof m->lock_why);
        nd_log_err(ND_LOG_MODEM, "cannot open the AT port lock: %s", why);
    }
}

nd_err nd_modem__create(nd_modem **out)
{
    nd_modem *m;

    /* owned by the caller; free with nd_modem__destroy() or nd_modem_close() */
    m = calloc(1u, sizeof *m);
    if (m == NULL)
        return ND_ERR_NOMEM;

    m->fd = -1;
    m->lock_fd = -1;
    m->state = ND_CALL_IDLE;
    m->csq = -1;
    m->reg_stat = -1;
    m->call_stat = -1;
    m->audio_pid = -1;
    m->mic_pid = -1;
    /* All six timers start at 0.0, so the first poll() fires CSQ at once. */

    if (pthread_mutex_init(&m->st_mu, NULL) != 0) {
        free(m);
        return ND_ERR_IO;
    }
    if (pthread_mutex_init(&m->req_mu, NULL) != 0) {
        (void)pthread_mutex_destroy(&m->st_mu);
        free(m);
        return ND_ERR_IO;
    }
    if (pthread_cond_init(&m->req_cv, NULL) != 0 || pthread_cond_init(&m->done_cv, NULL) != 0) {
        (void)pthread_mutex_destroy(&m->req_mu);
        (void)pthread_mutex_destroy(&m->st_mu);
        free(m);
        return ND_ERR_IO;
    }

    m->pcm_rate = pcm_rate_setting();
    port_from_settings(m->configured_port, sizeof m->configured_port);
    m->allow_calls = calls_enabled_setting();
    m->boot_grace = boot_grace_setting();
    m->boot_deadline = nd_modem__now() + m->boot_grace;
    m->late_grace_deadline = nd_modem__now() + ND_MODEM_LATE_GRACE_MAX_S;
    m->lock_fd = open_lock_file(m->lock_why, sizeof m->lock_why);
    if (m->lock_fd < 0)
        nd_log_err(ND_LOG_MODEM, "cannot open the AT port lock: %s", m->lock_why);

    *out = m;
    return ND_OK;
}

void nd_modem__destroy(nd_modem *m)
{
    if (m == NULL)
        return;
    if (m->fd >= 0)
        (void)close(m->fd);
    if (m->lock_fd >= 0)
        (void)close(m->lock_fd);
    (void)pthread_cond_destroy(&m->done_cv);
    (void)pthread_cond_destroy(&m->req_cv);
    (void)pthread_mutex_destroy(&m->req_mu);
    (void)pthread_mutex_destroy(&m->st_mu);
    free(m);
}

nd_err nd_modem_open(nd_modem **out)
{
    pthread_attr_t attr;
    nd_modem *m = NULL;
    nd_err rc;

    if (out == NULL)
        return ND_ERR_INVAL;
    *out = NULL;

    nd_log(ND_LOG_MODEM, "Initializing ModemService...");
    rc = nd_modem__create(&m);
    if (rc != ND_OK)
        return rc;

    if (!nd_modem__probe_hardware(m)) {
        double grace = m->boot_deadline - nd_modem__now();

        if (grace > 0.0) {
            /* Not "Simulation" yet: the modem may simply not be up. The
             * announcement, if it comes to that, is nd_modem__poll_sim()'s. */
            nd_log(ND_LOG_MODEM, "No modem has answered yet; probing every %ds for up to %.0fs.",
                   (int)ND_MODEM_BOOT_PROBE_S, grace);
        } else if (saw_radio(m)) {
            /* Candidates exist and none of them worked. announce_unreachable()
             * has already put the reason on the console and armed the notice;
             * saying "Simulation Mode" as well would be the lie this release
             * exists to stop. sim_announced is set so poll_sim() does not say
             * it later either. */
            m->sim_announced = true;
        } else {
            nd_log(ND_LOG_MODEM, "HARDWARE NOT FOUND: Running in Simulation Mode.");
            m->sim_announced = true;
        }
        nd_log(ND_LOG_MODEM, "Will re-probe every %ds; sim hooks: %s / %s.", (int)ND_PROBE_RETRY_S,
               ND_MODEM_SIM_CSQ, ND_MODEM_SIM_RING);
    }

    if (pthread_attr_init(&attr) == 0) {
        (void)pthread_attr_setstacksize(&attr, ND_MODEM_STACK_BYTES);
        if (pthread_create(&m->thread, &attr, modem_thread, m) == 0)
            m->thread_started = true;
        else
            nd_log_err(ND_LOG_MODEM, "cannot start the modem thread: %s", strerror(errno));
        (void)pthread_attr_destroy(&attr);
    }

    *out = m;
    return ND_OK;
}

void nd_modem_close(nd_modem *m)
{
    if (m == NULL)
        return;

    if (m->thread_started) {
        (void)pthread_mutex_lock(&m->req_mu);
        m->quit = true;
        (void)pthread_cond_broadcast(&m->req_cv);
        (void)pthread_cond_broadcast(&m->done_cv);
        (void)pthread_mutex_unlock(&m->req_mu);
        (void)pthread_join(m->thread, NULL);
        m->thread_started = false;
    }

    nd_modem__stop_call_audio(m);
    nd_modem__destroy(m);
}

/* ------------------------------------------------------------------ *
 * The public, UI-thread surface
 * ------------------------------------------------------------------ */

bool nd_modem_dial(nd_modem *m, const char *number)
{
    nd_modem_req r;

    if (m == NULL || number == NULL)
        return false;
    memset(&r, 0, sizeof r);
    r.kind = ND_REQ_DIAL;
    r.s1 = number;
    submit(m, &r);
    return r.ok;
}

bool nd_modem_answer(nd_modem *m)
{
    nd_modem_req r;

    if (m == NULL)
        return false;
    memset(&r, 0, sizeof r);
    r.kind = ND_REQ_ANSWER;
    submit(m, &r);
    return r.ok;
}

bool nd_modem_hangup(nd_modem *m)
{
    nd_modem_req r;

    if (m == NULL)
        return false;
    memset(&r, 0, sizeof r);
    r.kind = ND_REQ_HANGUP;
    submit(m, &r);
    return r.ok;
}

bool nd_modem_send_sms(nd_modem *m, const char *number, const char *text, char *detail,
                       size_t detail_sz)
{
    nd_modem_req r;

    if (detail != NULL && detail_sz > 0u)
        detail[0] = '\0';
    if (m == NULL || number == NULL || text == NULL)
        return false;
    memset(&r, 0, sizeof r);
    r.kind = ND_REQ_SEND_SMS;
    r.s1 = number;
    r.s2 = text;
    submit(m, &r);
    if (detail != NULL && detail_sz > 0u)
        (void)nd_strlcpy(detail, r.detail, detail_sz);
    return r.ok;
}

nd_sms_st nd_modem_fetch_sms(nd_modem *m, int32_t index, nd_sms_rec *out)
{
    nd_modem_req r;

    if (m == NULL || out == NULL)
        return ND_SMS_ERROR;
    memset(out, 0, sizeof *out);
    memset(&r, 0, sizeof r);
    r.kind = ND_REQ_FETCH_SMS;
    r.i1 = index;
    r.rec_out = out;
    r.sms_st = ND_SMS_ERROR;
    submit(m, &r);
    return r.sms_st;
}

nd_sms_st nd_modem_read_stored_sms(nd_modem *m, nd_sms_rec *out, size_t max, size_t *n_out)
{
    nd_modem_req r;
    size_t discard = 0u;

    if (n_out == NULL)
        n_out = &discard;
    *n_out = 0u;
    if (m == NULL || out == NULL || max == 0u)
        return ND_SMS_ERROR;
    memset(&r, 0, sizeof r);
    r.kind = ND_REQ_READ_STORED;
    r.rec_out = out;
    r.rec_max = max;
    r.sms_st = ND_SMS_ERROR;
    submit(m, &r);
    *n_out = r.rec_n;
    return r.sms_st;
}

nd_err nd_modem_send_at(nd_modem *m, const char *cmd, double timeout, char *final_out,
                        size_t final_sz, struct nd_lines *lines_out)
{
    nd_modem_req r;

    if (final_out != NULL && final_sz > 0u)
        final_out[0] = '\0';
    nd_modem__lines_reset(lines_out);
    if (m == NULL || cmd == NULL)
        return ND_ERR_INVAL;
    memset(&r, 0, sizeof r);
    r.kind = ND_REQ_SEND_AT;
    r.s1 = cmd;
    r.timeout = (timeout > 0.0) ? timeout : 5.0;
    r.final_out = final_out;
    r.final_sz = final_sz;
    r.lines_out = lines_out;
    r.err = ND_ERR_TIMEOUT;
    submit(m, &r);
    return r.err;
}

/* ------------------------------------------------------------------ *
 * The event queue, mapped onto the frozen four-value public enum
 * ------------------------------------------------------------------ */

bool nd_modem_take_pending_event(nd_modem *m, nd_modem_event *out)
{
    nd_mev e;

    if (m == NULL || out == NULL)
        return false;

    while (nd_modem__take(m, &e)) {
        memset(out, 0, sizeof *out);
        out->index = -1;
        switch (e.kind) {
        case ND_MEV_INCOMING:
            out->kind = ND_MODEM_EV_RING;
            if (e.has_detail)
                (void)nd_strlcpy(out->number, e.text, sizeof out->number);
            return true;
        case ND_MEV_CONNECTED:
            out->kind = ND_MODEM_EV_CONNECTED;
            if (e.has_detail)
                (void)nd_strlcpy(out->number, e.text, sizeof out->number);
            return true;
        case ND_MEV_ENDED:
        case ND_MEV_MISSED:
            out->kind = ND_MODEM_EV_HANGUP;
            return true;
        case ND_MEV_SMS_RECEIVED:
            out->kind = ND_MODEM_EV_SMS;
            out->index = e.index;
            return true;
        case ND_MEV_SMS_SIM:
            out->kind = ND_MODEM_EV_SMS;
            out->index = ND_MODEM_SMS_IDX_SIM;
            (void)nd_strlcpy(out->number, e.sender, sizeof out->number);
            return true;
        case ND_MEV_SMS_STORED_CHECK:
            out->kind = ND_MODEM_EV_SMS;
            out->index = ND_MODEM_SMS_IDX_STORED;
            return true;
        case ND_MEV_SMS_SENT:
        case ND_MEV_MODEM_LOST:
        case ND_MEV_MODEM_FOUND:
        default:
            /* No spelling in the frozen enum. Dropped rather than surfaced as
             * something it is not; see OPEN-QUESTIONS.md M-1. */
            break;
        }
    }
    out->kind = ND_MODEM_EV_NONE;
    return false;
}

void nd_modem_requeue_event(nd_modem *m, const nd_modem_event *ev)
{
    nd_mev e;

    if (m == NULL || ev == NULL)
        return;
    mev_init(&e, ND_MEV_SMS_RECEIVED);
    switch (ev->kind) {
    case ND_MODEM_EV_RING:
        e.kind = ND_MEV_INCOMING;
        break;
    case ND_MODEM_EV_CONNECTED:
        e.kind = ND_MEV_CONNECTED;
        break;
    case ND_MODEM_EV_HANGUP:
        e.kind = ND_MEV_ENDED;
        break;
    case ND_MODEM_EV_SMS:
        if (ev->index == ND_MODEM_SMS_IDX_SIM)
            e.kind = ND_MEV_SMS_SIM;
        else if (ev->index == ND_MODEM_SMS_IDX_STORED)
            e.kind = ND_MEV_SMS_STORED_CHECK;
        else
            e.kind = ND_MEV_SMS_RECEIVED;
        break;
    case ND_MODEM_EV_NONE:
    default:
        return;
    }
    e.index = ev->index;
    if (ev->number[0] != '\0') {
        e.has_detail = true;
        (void)nd_strlcpy(e.text, ev->number, sizeof e.text);
        (void)nd_strlcpy(e.sender, ev->number, sizeof e.sender);
    }
    nd_modem__queue_front(m, &e);
}

/* ------------------------------------------------------------------ *
 * Readouts -- line 1003 onwards
 * ------------------------------------------------------------------ */

bool nd_modem_registered(nd_modem *m)
{
    int32_t reg;

    if (m == NULL)
        return false;
    lock_state(m);
    reg = m->reg_stat;
    unlock_state(m);
    return reg == 1 || reg == 5; /* 1 home, 5 roaming */
}

/* ------------------------------------------------------------------ *
 * Simulation: how many bars, and what the carrier is called
 * ------------------------------------------------------------------ */

/* Cached, because nd_modem_signal_level() is called once per rendered frame
 * -- ten times a second -- and nd_clock_has_route() opens and reads two files
 * in /proc each time. Two seconds is far shorter than anyone can plug an
 * Ethernet cable in and look at the screen, and it takes the syscall rate
 * from 20/s to 1/s.
 *
 * A plain file-static with no lock, following g_sim in nd_ui.c: the readouts
 * are called from the UI thread, and the worst a race could do is serve a
 * two-second-old answer to a question about a cable. */
static double g_route_checked_at;
static bool g_route_present;
static bool g_route_known;

static int32_t sim_route_bars(void)
{
    double now = nd_modem__now();

    if (!g_route_known || now - g_route_checked_at > ND_MODEM_SIM_ROUTE_TTL_S) {
        g_route_present = nd_clock_has_route();
        g_route_checked_at = now;
        g_route_known = true;
    }
    return g_route_present ? ND_MODEM_SIM_BARS_ONLINE : ND_MODEM_SIM_BARS_OFFLINE;
}

void nd_modem__sim_route_forget(void)
{
    g_route_known = false;
}

nd_modem_link nd_modem_link_state(nd_modem *m)
{
    bool hw;
    bool bad;
    bool radio;
    bool booting;

    /* A core with no ModemService is not a core with a broken one. */
    if (m == NULL)
        return ND_MODEM_LINK_SIM;

    lock_state(m);
    hw = m->hardware;
    bad = m->faulted;
    radio = m->saw_candidates;
    booting = nd_modem__now() < m->boot_deadline;
    unlock_state(m);

    if (hw)
        return ND_MODEM_LINK_LIVE;
    if (bad)
        return ND_MODEM_LINK_FAULT;
    if (booting)
        return ND_MODEM_LINK_PROBING;
    /* THE LINE THIS WHOLE ENUM EXISTS FOR. `radio` is "the last probe found
     * at least one candidate AT port", i.e. the kernel says there is a
     * serial device in this phone that could be the modem. Getting here with
     * it true means every one of those ports failed -- permissions, EBUSY, a
     * held lock, no OK inside a second -- and reporting that as Simulation
     * is a phone telling its owner that a broken radio is fine. */
    return radio ? ND_MODEM_LINK_UNREACHABLE : ND_MODEM_LINK_SIM;
}

/* The simulate-or-refuse policy, as a table, because it is the decision and
 * not the plumbing that was wrong. See nd_modem.h above nd_modem_dial(). */
bool nd_modem__may_simulate(nd_modem_link link, bool has_radio)
{
    switch (link) {
    case ND_MODEM_LINK_LIVE:
        /* A live modem places real calls. The one exception --
         * system.modem.allow_calls=OFF -- is a development switch and is
         * handled at the call site, where the log line already says so. */
        return false;
    case ND_MODEM_LINK_SIM:
    case ND_MODEM_LINK_PROBING:
        /* Simulation is honest only on a device with no radio in it. During
         * the boot grace on a phone whose ports HAVE appeared, "not yet" is
         * the truthful answer to a dial, not a two-second fake connect. */
        return !has_radio;
    case ND_MODEM_LINK_FAULT:
    case ND_MODEM_LINK_UNREACHABLE:
    default:
        return false;
    }
}

const char *nd_modem_take_pending_fault(nd_modem *m)
{
    bool pending;

    if (m == NULL)
        return NULL;

    lock_state(m);
    pending = m->fault_pending;
    m->fault_pending = false;
    /* Copied into the same kind of snapshot buffer the other two const char *
     * readouts use, and for the same reason: the caller reads it immediately
     * and the lock is not held across the draw. */
    if (pending)
        (void)nd_strlcpy(m->fault_display, m->fault_why, sizeof m->fault_display);
    unlock_state(m);

    return pending ? m->fault_display : NULL;
}

int32_t nd_modem_signal_level(nd_modem *m)
{
    int32_t csq;
    int32_t reg;
    bool hw;

    if (m == NULL)
        return -1;

    lock_state(m);
    hw = m->hardware;
    unlock_state(m);

    if (!hw) {
        int32_t sim;
        nd_modem_link link = nd_modem_link_state(m);

        /* A FAULTED modem has no signal, and says so with an empty meter
         * rather than with the full one the layout's sim_val would draw. This
         * is checked before the hook so that a stale /tmp/neodct_sim_csq
         * cannot paint bars onto a broken phone.
         *
         * UNREACHABLE is the same answer for the same reason, and it is the
         * one that was actually being got wrong on the phone: with the
         * carrier line reading "Simulation", the fall-through at the bottom
         * of this function drew FOUR FULL BARS next to it whenever
         * /proc/net/route had a default route -- which S45modem's data call
         * puts there. A full meter on a phone whose radio the UI cannot open
         * is worse than no meter at all. */
        if (link == ND_MODEM_LINK_FAULT || link == ND_MODEM_LINK_UNREACHABLE)
            return 0;

        /* Read outside the state lock: one open/read/close per rendered
         * frame is how the hook stays live, but it must never be a thing the
         * modem thread can be blocked behind. */
        if (nd_modem__sim_read_int(ND_MODEM_SIM_CSQ, &sim))
            return nd_modem__bars(sim);

        /* Still waiting for the modem to come up: an empty meter, which is
         * what a phone whose radio is booting shows. After the hook, so a
         * developer's csq file works from the first second on QEMU. */
        if (link == ND_MODEM_LINK_PROBING)
            return 0;

        /* NO HOOK, NO HARDWARE: SIMULATION. This used to return -1, which the
         * home layout renders as sim_val -- a FULL meter, four bars, next to
         * the words "No Service". That pair is nonsense in both directions
         * and it is what this branch exists to stop.
         *
         * What a simulated modem can actually do decides the number. Calls
         * and texts are simulated inside this service and work with no
         * network at all, so the meter is never empty. But the browser, the
         * clock sync and every download need a real route, so a box with no
         * network is meaningfully worse off than one with -- and the meter is
         * the only place on the home screen that can say so.
         *
         *   a default route   4 -- full, and honest: everything works
         *   none              1 -- one bar: you can still fake a call or a
         *                          text, and nothing else will load
         *
         * nd_clock_has_route() is the same /proc/net/route reader ClockService
         * waits on before its first SNTP sync, so "has a route" means exactly
         * what it means everywhere else in this tree. */
        return sim_route_bars();
    }

    lock_state(m);
    csq = m->csq;
    reg = m->reg_stat;
    unlock_state(m);

    if (reg >= 0 && !(reg == 1 || reg == 5))
        return 0;
    return nd_modem__bars(csq);
}

const char *nd_modem_operator_display(nd_modem *m)
{
    bool hw;

    if (m == NULL)
        return NULL;

    lock_state(m);
    hw = m->hardware;
    unlock_state(m);

    if (!hw) {
        char text[32];
        nd_modem_link link = nd_modem_link_state(m);

        /* A faulted modem gets NO carrier name at all -- not the operator,
         * not "Simulation", and not the "No Service" placeholder either. The
         * home screen drops the line entirely (nd_ui_render_home), because a
         * broken radio has nothing truthful to put there. */
        if (link == ND_MODEM_LINK_FAULT)
            return NULL;

        /* An UNREACHABLE modem gets a NAME rather than a blank, and the
         * difference from FAULT is deliberate. FAULT is a radio that was
         * working a moment ago, so the empty line plus the modal reads as
         * "something just broke". UNREACHABLE is the steady state of a phone
         * that has never managed to open its own modem, and it is reached
         * through code paths -- a udev group that arrived late, a lock file
         * owned by root -- where the owner may never see a modal at all.
         * Naming it in the carrier slot is the only thing on the home screen
         * that can tell them apart from a phone in a tunnel.
         *
         * Checked BEFORE the /tmp/neodct_sim_operator hook for the same
         * reason the meter is: a leftover hook file must not be able to
         * paint a carrier name onto a radio nothing can reach. */
        if (link == ND_MODEM_LINK_UNREACHABLE) {
            lock_state(m);
            (void)nd_strlcpy(m->op_display, ND_MODEM_UNREACHABLE_CARRIER, sizeof m->op_display);
            unlock_state(m);
            return m->op_display;
        }

        if (nd_modem__sim_read_text(ND_MODEM_SIM_OPS, text, sizeof text)) {
            py_strip(text);
            if (text[0] != '\0') {
                lock_state(m);
                (void)nd_strlcpy(m->op_display, text, sizeof m->op_display);
                unlock_state(m);
                return m->op_display;
            }
        }

        /* Still waiting for the modem: nothing, and the layout keeps its own
         * "No Service" -- which for once is the truth. */
        if (link == ND_MODEM_LINK_PROBING)
            return NULL;

        /* The hook is unset or blank, so this is plain Simulation Mode and it
         * says so. It used to return NULL, which the layout leaves as its
         * authored placeholder -- the literal words "No Service" -- on a
         * phone that will happily place a call and send a text. There IS a
         * service here; it is a pretend one, and naming it is both more
         * honest and the only way a tester can tell simulated bars from real
         * ones at a glance. */
        lock_state(m);
        (void)nd_strlcpy(m->op_display, ND_MODEM_SIM_CARRIER, sizeof m->op_display);
        unlock_state(m);
        return m->op_display;
    }
    if (!nd_modem_registered(m))
        return NULL;

    lock_state(m);
    if (!m->operator_known) {
        unlock_state(m);
        return NULL;
    }
    (void)nd_strlcpy(m->op_display, m->operator_name, sizeof m->op_display);
    unlock_state(m);
    return m->op_display;
}

void nd_modem_call_status(nd_modem *m, const char **label, int32_t *secs)
{
    nd_call_state st;
    int32_t stat;
    bool connected;
    double at;

    if (label != NULL)
        *label = "IDLE";
    if (secs != NULL)
        *secs = -1;
    if (m == NULL)
        return;

    lock_state(m);
    st = m->state;
    stat = m->call_stat;
    connected = m->call_connected;
    at = m->call_connected_at;
    unlock_state(m);

    if (st == ND_CALL_IDLE)
        return;
    if (stat == ND_CLCC_RINGING) {
        if (label != NULL)
            *label = "RINGING";
        return;
    }
    if (stat == ND_CLCC_CALLING) {
        if (label != NULL)
            *label = "CALLING";
        return;
    }
    if (st == ND_CALL_CONNECTED || stat == ND_CLCC_CONNECTED) {
        if (label != NULL)
            *label = "CONNECTED";
        if (secs != NULL && connected)
            *secs = (int32_t)(nd_modem__now() - at);
        return;
    }
    if (label != NULL)
        *label = state_name(st);
}

nd_call_state nd_modem_state(nd_modem *m)
{
    return (m != NULL) ? get_state(m) : ND_CALL_IDLE;
}

const char *nd_modem_caller_id(nd_modem *m)
{
    bool known;

    if (m == NULL)
        return NULL;
    lock_state(m);
    known = m->caller_id_known;
    if (known)
        (void)nd_strlcpy(m->cid_display, m->caller_id, sizeof m->cid_display);
    unlock_state(m);
    return known ? m->cid_display : NULL;
}

bool nd_modem_has_hardware(nd_modem *m)
{
    bool hw;

    if (m == NULL)
        return false;
    lock_state(m);
    hw = m->hardware;
    unlock_state(m);
    return hw;
}

void nd_modem_status_snapshot(nd_modem *m, nd_modem_status *out)
{
    const char *op;

    if (out == NULL)
        return;
    memset(out, 0, sizeof *out);
    out->signal_level = -1;
    out->csq_rssi = -1;
    out->reg_stat = -1; /* Python's None; REG_NAMES[None] is "--" */
    out->call_secs = -1;
    out->state = ND_CALL_IDLE;
    if (m == NULL)
        return;

    /* "bars" re-enters signal_level() in the Python, so in Simulation Mode it
     * hits /tmp/neodct_sim_csq a second time. Kept. */
    out->signal_level = nd_modem_signal_level(m);
    op = nd_modem_operator_display(m);
    out->registered = nd_modem_registered(m);
    nd_modem_call_status(m, NULL, &out->call_secs);

    lock_state(m);
    out->hardware = m->hardware;
    (void)nd_strlcpy(out->port, m->port, sizeof out->port);
    (void)nd_strlcpy(out->imei, m->imei_known ? m->imei : "", sizeof out->imei);
    out->csq_rssi = m->csq;
    out->reg_stat = m->reg_stat;
    out->state = m->state;
    (void)nd_strlcpy(out->probe_why, m->last_probe_why, sizeof out->probe_why);
    (void)nd_strlcpy(out->caller_id, m->caller_id_known ? m->caller_id : "", sizeof out->caller_id);
    unlock_state(m);

    (void)nd_strlcpy(out->operator_name, op != NULL ? op : "", sizeof out->operator_name);
}

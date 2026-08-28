/* test_modem.c -- ModemService's first tests.
 *
 * The Python has none: 1084 lines with an AT state machine, an SMS parser and
 * a call flow, completely uncovered. So this is written against the C from
 * scratch, and every expected value in it was read out of
 * System/core/ModemService/__init__.py by hand and is cited by line.
 *
 * ============ THE FAKE MODEM ============
 *
 * A pty. The test holds the master, the code under test opens the slave and
 * cannot tell it from a SIM7600. A small thread on the master matches whole
 * command lines against a rule table and writes the scripted reply, with two
 * behaviours a table cannot express:
 *
 *   - AT+CMGS="..." answers with a bare "> " and NO newline, then waits for a
 *     body terminated by Ctrl-Z. That is the whole reason _wait_sms_prompt
 *     exists, so a fake that sent a normal line would test nothing.
 *   - A rule whose reply is NULL says nothing at all, which is how the
 *     timeout path is reached without waiting for a real modem to hang.
 *
 * The fixture keeps a spare slave descriptor open for its lifetime. Without
 * it the pty hangs up the moment the modem closes its side -- which the probe
 * does on every candidate that fails -- and the master starts returning EIO.
 *
 * The slave is reached as /dev/modem, a symlink inside the scratch root, so
 * the code under test goes through the ordinary
 * system.hw.modem_at_port setting and nd_path_resolve() with no test hooks in
 * it at all.
 *
 * ============ WHY THIS INCLUDES A PRIVATE HEADER ============
 *
 * nd_modem_open() starts a thread and probes. Half of what is worth testing
 * -- the parsers, the ring's two overflow directions, the poll scheduler's
 * staggering -- wants a modem that is NOT running, driven a tick at a time.
 * lib/nd_modem_priv.h is included by relative path for exactly that, and for
 * nothing else.
 */

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "nd_log.h"
#include "nd_modem.h"
#include "nd_settings.h"

#include "../../lib/nd_modem_priv.h"

#include "platform_test.h"

#define SETTINGS_PATH "/User/settings.prop"
#define VERSION_PATH  "/System/version.prop"
#define MODEM_LINK    "/dev/modem"

/* ------------------------------------------------------------------ *
 * The fake modem
 * ------------------------------------------------------------------ */

typedef struct {
    const char *cmd;
    const char *reply; /* NULL means "say nothing", for the timeout path */
} fake_rule;

typedef struct {
    int master;
    int keepalive;
    char slave[64];
    const fake_rule *rules;
    size_t n_rules;
    const char *cmgs_reply;

    pthread_mutex_t mu;
    pthread_t th;
    bool running;
    bool stop;
    bool awaiting_body;
    char in[4096];
    size_t in_len;
    int commands;
    char last_cmd[256];
    char last_body[512];
} fake_modem;

static void fake_say(fake_modem *fm, const char *text)
{
    size_t left = strlen(text);
    const char *p = text;

    while (left > 0u) {
        ssize_t n = write(fm->master, p, left);

        if (n <= 0)
            return;
        p += (size_t)n;
        left -= (size_t)n;
    }
}

static void fake_command(fake_modem *fm, const char *line)
{
    size_t i;

    fm->commands++;
    (void)nd_strlcpy(fm->last_cmd, line, sizeof fm->last_cmd);

    if (strncmp(line, "AT+CMGS=", 8u) == 0) {
        fm->awaiting_body = true;
        fake_say(fm, "\r\n> ");
        return;
    }
    for (i = 0u; i < fm->n_rules; i++) {
        if (strcmp(fm->rules[i].cmd, line) == 0) {
            if (fm->rules[i].reply != NULL)
                fake_say(fm, fm->rules[i].reply);
            return;
        }
    }
    fake_say(fm, "\r\nERROR\r\n");
}

static void fake_process(fake_modem *fm)
{
    for (;;) {
        if (fm->awaiting_body) {
            char *ctrlz = memchr(fm->in, '\x1a', fm->in_len);
            size_t len;

            if (ctrlz == NULL)
                return;
            len = (size_t)(ctrlz - fm->in);
            if (len >= sizeof fm->last_body)
                len = sizeof fm->last_body - 1u;
            memcpy(fm->last_body, fm->in, len);
            fm->last_body[len] = '\0';
            len = (size_t)(ctrlz - fm->in) + 1u;
            memmove(fm->in, &fm->in[len], fm->in_len - len);
            fm->in_len -= len;
            fm->awaiting_body = false;
            if (fm->cmgs_reply != NULL)
                fake_say(fm, fm->cmgs_reply);
            continue;
        }
        {
            char *cr = memchr(fm->in, '\r', fm->in_len);
            char line[256];
            size_t len;

            if (cr == NULL)
                return;
            len = (size_t)(cr - fm->in);
            if (len >= sizeof line)
                len = sizeof line - 1u;
            memcpy(line, fm->in, len);
            line[len] = '\0';
            len = (size_t)(cr - fm->in) + 1u;
            memmove(fm->in, &fm->in[len], fm->in_len - len);
            fm->in_len -= len;
            if (line[0] != '\0' && line[0] != '\n')
                fake_command(fm, line);
        }
    }
}

static void *fake_thread(void *arg)
{
    fake_modem *fm = arg;

    for (;;) {
        struct pollfd p;
        char tmp[512];
        ssize_t n;

        (void)pthread_mutex_lock(&fm->mu);
        if (fm->stop) {
            (void)pthread_mutex_unlock(&fm->mu);
            break;
        }
        (void)pthread_mutex_unlock(&fm->mu);

        p.fd = fm->master;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1u, 10) <= 0)
            continue;
        if ((p.revents & POLLIN) == 0)
            continue;

        n = read(fm->master, tmp, sizeof tmp);
        if (n <= 0)
            continue;

        (void)pthread_mutex_lock(&fm->mu);
        if (fm->in_len + (size_t)n <= sizeof fm->in) {
            memcpy(&fm->in[fm->in_len], tmp, (size_t)n);
            fm->in_len += (size_t)n;
        }
        fake_process(fm);
        (void)pthread_mutex_unlock(&fm->mu);
    }
    return NULL;
}

/* Bring up the pty, publish it as <root>/dev/modem and point the setting at
 * it. Returns false when the host has no ptys, which no CI runner does. */
static bool fake_start(fake_modem *fm, const fake_rule *rules, size_t n_rules)
{
    char link[ND_PATH_MAX];

    memset(fm, 0, sizeof *fm);
    fm->rules = rules;
    fm->n_rules = n_rules;
    fm->cmgs_reply = "\r\n+CMGS: 42\r\n\r\nOK\r\n";
    fm->master = -1;
    fm->keepalive = -1;
    (void)pthread_mutex_init(&fm->mu, NULL);

    fm->master = posix_openpt(O_RDWR | O_NOCTTY);
    if (fm->master < 0)
        return false;
    if (grantpt(fm->master) != 0 || unlockpt(fm->master) != 0)
        return false;
    if (ptsname_r(fm->master, fm->slave, sizeof fm->slave) != 0)
        return false;

    /* The spare slave keeps the pty from hanging up between probes. */
    fm->keepalive = open(fm->slave, O_RDWR | O_NOCTTY);
    if (fm->keepalive < 0)
        return false;

    pt_mkdir("/dev");
    if (nd_path_resolve(link, sizeof link, MODEM_LINK) != ND_OK)
        return false;
    (void)unlink(link);
    if (symlink(fm->slave, link) != 0)
        return false;

    if (pthread_create(&fm->th, NULL, fake_thread, fm) != 0)
        return false;
    fm->running = true;
    return true;
}

static void fake_stop(fake_modem *fm)
{
    if (fm->running) {
        (void)pthread_mutex_lock(&fm->mu);
        fm->stop = true;
        (void)pthread_mutex_unlock(&fm->mu);
        (void)pthread_join(fm->th, NULL);
        fm->running = false;
    }
    if (fm->keepalive >= 0)
        (void)close(fm->keepalive);
    if (fm->master >= 0)
        (void)close(fm->master);
    (void)pthread_mutex_destroy(&fm->mu);
}

/* Push an unsolicited line at the modem, the way a real one would. */
static void fake_inject(fake_modem *fm, const char *text)
{
    (void)pthread_mutex_lock(&fm->mu);
    fake_say(fm, text);
    (void)pthread_mutex_unlock(&fm->mu);
}

static int fake_commands(fake_modem *fm)
{
    int n;

    (void)pthread_mutex_lock(&fm->mu);
    n = fm->commands;
    (void)pthread_mutex_unlock(&fm->mu);
    return n;
}

static void fake_body(fake_modem *fm, char *out, size_t out_sz)
{
    (void)pthread_mutex_lock(&fm->mu);
    (void)nd_strlcpy(out, fm->last_body, out_sz);
    (void)pthread_mutex_unlock(&fm->mu);
}

/* ------------------------------------------------------------------ *
 * Fixture helpers
 * ------------------------------------------------------------------ */

static void use_scratch_settings(const char *body)
{
    nd_settings_set_paths(SETTINGS_PATH, VERSION_PATH);
    pt_write_text(SETTINGS_PATH, body != NULL ? body : "");
}

static nd_modem *make_modem(void)
{
    nd_modem *m = NULL;

    if (nd_modem__create(&m) != ND_OK)
        return NULL;
    return m;
}

/* Waiting for the modem's own line reader to see something the fake wrote.
 * Real time, because the AT engine deliberately runs on CLOCK_MONOTONIC. */
static void settle(double seconds)
{
    nd_modem__nap(seconds);
}

/* poll() rate-limits itself to 2 Hz. A test that wants the next scheduled
 * query does not want to sleep half a second for it, and the timer is the
 * modem thread's own field, so it is reset here rather than waited out. */
static void poll_now(nd_modem *m)
{
    m->next_urc = 0.0;
    nd_modem_poll(m);
}

/* ------------------------------------------------------------------ *
 * 1. The line decoder
 * ------------------------------------------------------------------ */

static void test_decode_line(void)
{
    char out[64];
    static const uint8_t high[] = {'A', 0x80u, 'B'};
    static const uint8_t spaces[] = {' ', '\t', 'O', 'K', '\r', ' '};
    static const uint8_t only_ws[] = {' ', '\r', '\t'};

    CHECK_INT(nd_modem__decode_line((const uint8_t *)"OK", 2u, out, sizeof out), 2);
    CHECK_STR(out, "OK");

    /* strip() removes the \r the modem ends every line with. */
    (void)nd_modem__decode_line(spaces, sizeof spaces, out, sizeof out);
    CHECK_STR(out, "OK");

    /* An all-whitespace line decodes empty and _read_pending drops it. */
    CHECK_INT(nd_modem__decode_line(only_ws, sizeof only_ws, out, sizeof out), 0);

    /* decode("ascii","replace"): one U+FFFD per byte over 0x7F. */
    CHECK_INT(nd_modem__decode_line(high, sizeof high, out, sizeof out), 5);
    CHECK_INT((uint8_t)out[1], 0xef);
    CHECK_INT((uint8_t)out[2], 0xbf);
    CHECK_INT((uint8_t)out[3], 0xbd);
    CHECK_INT(out[4], 'B');

    /* 0x1c..0x1f are whitespace to Python's str.strip(); 0x1b is not. */
    {
        static const uint8_t fs[] = {0x1cu, 'O', 'K', 0x1fu};
        static const uint8_t esc[] = {0x1bu, 'O', 'K'};

        (void)nd_modem__decode_line(fs, sizeof fs, out, sizeof out);
        CHECK_STR(out, "OK");
        CHECK_INT(nd_modem__decode_line(esc, sizeof esc, out, sizeof out), 3);
        CHECK_INT((uint8_t)out[0], 0x1b);
    }
}

static void test_int_and_hex(void)
{
    int32_t v = -12345;

    CHECK(nd_modem__parse_int("  20 ", &v) && v == 20);
    CHECK(nd_modem__parse_int("-3", &v) && v == -3);
    CHECK(nd_modem__parse_int("+7", &v) && v == 7);
    CHECK(!nd_modem__parse_int("", &v));
    CHECK(!nd_modem__parse_int("2a", &v));
    CHECK(!nd_modem__parse_int("0x10", &v));
    CHECK(!nd_modem__parse_int("7.5", &v));

    /* bInterfaceNumber is a two-digit HEX field: "10" is sixteen. */
    CHECK(nd_modem__parse_hex("02", &v) && v == 2);
    CHECK(nd_modem__parse_hex("10", &v) && v == 16);
    CHECK(nd_modem__parse_hex(" 0a\n", &v) && v == 10);
    CHECK(nd_modem__parse_hex("0x04", &v) && v == 4);
    CHECK(!nd_modem__parse_hex("zz", &v));
    CHECK(!nd_modem__parse_hex("", &v));
}

static void test_final_and_urc_classification(void)
{
    CHECK(nd_modem__is_final("OK"));
    CHECK(nd_modem__is_final("ERROR"));
    CHECK(nd_modem__is_final("NO CARRIER"));
    CHECK(nd_modem__is_final("NO DIALTONE"));
    CHECK(nd_modem__is_final("BUSY"));
    CHECK(nd_modem__is_final("NO ANSWER"));
    CHECK(nd_modem__is_final("+CME ERROR: 10"));
    CHECK(nd_modem__is_final("+CMS ERROR: 500"));
    CHECK(!nd_modem__is_final("OKAY"));
    CHECK(!nd_modem__is_final("+CSQ: 20,99"));

    CHECK(nd_modem__is_urc("RING"));
    CHECK(nd_modem__is_urc("+CLIP: \"+15551234\",145"));
    CHECK(nd_modem__is_urc("VOICE CALL: BEGIN"));
    CHECK(nd_modem__is_urc("MISSED_CALL: 11:02AM 15551234"));
    CHECK(nd_modem__is_urc("NO CARRIER"));
    CHECK(nd_modem__is_urc("+CMTI: \"SM\",3"));
    CHECK(nd_modem__is_urc("+CEREG: 1"));
    CHECK(nd_modem__is_urc("+CREG: 0,5"));
    CHECK(nd_modem__is_urc("+CPIN: READY"));
    CHECK(nd_modem__is_urc("+SIMCARD: NOT AVAILABLE"));
    CHECK(!nd_modem__is_urc("+CSQ: 20,99"));
    /* "RING" is a PREFIX in the tuple, so RINGXYZ matches it too. */
    CHECK(nd_modem__is_urc("RINGXYZ"));
}

static void test_bars_table(void)
{
    /* 0-1 -> 0, 2-7 -> 1, 8-13 -> 2, 14-19 -> 3, 20-31 -> 4, 99 -> 0. */
    CHECK_INT(nd_modem__bars(-1), 0); /* Python's None */
    CHECK_INT(nd_modem__bars(0), 0);
    CHECK_INT(nd_modem__bars(1), 0);
    CHECK_INT(nd_modem__bars(2), 1);
    CHECK_INT(nd_modem__bars(7), 1);
    CHECK_INT(nd_modem__bars(8), 2);
    CHECK_INT(nd_modem__bars(13), 2);
    CHECK_INT(nd_modem__bars(14), 3);
    CHECK_INT(nd_modem__bars(19), 3);
    CHECK_INT(nd_modem__bars(20), 4);
    CHECK_INT(nd_modem__bars(31), 4);
    CHECK_INT(nd_modem__bars(99), 0);
    CHECK_INT(ND_BAR_THRESHOLDS[0], 2);
    CHECK_INT(ND_BAR_THRESHOLDS[3], 20);
}

/* ------------------------------------------------------------------ *
 * 2. The event ring, both overflow directions
 * ------------------------------------------------------------------ */

static void test_event_ring_drops_the_oldest_on_append(void)
{
    nd_modem *m = make_modem();
    nd_mev e;
    int i;

    CHECK(m != NULL);
    if (m == NULL)
        return;

    /* deque(maxlen=8).append() silently drops the LEFTMOST. */
    for (i = 0; i < 12; i++) {
        memset(&e, 0, sizeof e);
        e.kind = ND_MEV_SMS_RECEIVED;
        e.index = i;
        nd_modem__queue(m, &e);
    }
    for (i = 4; i < 12; i++) {
        CHECK(nd_modem__take(m, &e));
        CHECK_INT(e.index, i);
    }
    CHECK(!nd_modem__take(m, &e));

    nd_modem__destroy(m);
}

static void test_event_ring_drops_the_newest_on_requeue(void)
{
    nd_modem *m = make_modem();
    nd_mev e;
    int i;

    CHECK(m != NULL);
    if (m == NULL)
        return;

    for (i = 0; i < 8; i++) {
        memset(&e, 0, sizeof e);
        e.kind = ND_MEV_SMS_RECEIVED;
        e.index = i;
        nd_modem__queue(m, &e);
    }
    /* appendleft() on a full deque drops the RIGHTMOST -- index 7 goes. */
    memset(&e, 0, sizeof e);
    e.kind = ND_MEV_SMS_RECEIVED;
    e.index = 99;
    nd_modem__queue_front(m, &e);

    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.index, 99);
    for (i = 0; i < 6; i++) {
        CHECK(nd_modem__take(m, &e));
        CHECK_INT(e.index, i);
    }
    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.index, 6);
    CHECK(!nd_modem__take(m, &e));

    nd_modem__destroy(m);
}

/* ------------------------------------------------------------------ *
 * 3. URC dispatch
 * ------------------------------------------------------------------ */

static void test_urc_ring_and_clip(void)
{
    nd_modem *m = make_modem();
    nd_mev e;

    CHECK(m != NULL);
    if (m == NULL)
        return;

    nd_modem__handle_urc(m, "RING");
    CHECK_INT(nd_modem_state(m), ND_CALL_RINGING);
    CHECK(nd_modem_caller_id(m) == NULL);
    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.kind, ND_MEV_INCOMING);
    CHECK(!e.has_detail); /* ("incoming", None) */

    /* A second RING while already RINGING queues nothing. */
    nd_modem__handle_urc(m, "RING");
    CHECK(!nd_modem__take(m, &e));

    nd_modem__handle_urc(m, "+CLIP: \"+15551234\",145,\"\",0,\"\",0");
    CHECK_STR(nd_modem_caller_id(m), "+15551234");
    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.kind, ND_MEV_INCOMING);
    CHECK(e.has_detail);
    CHECK_STR(e.text, "+15551234");

    /* The same number again is not a new event. */
    nd_modem__handle_urc(m, "+CLIP: \"+15551234\",145");
    CHECK(!nd_modem__take(m, &e));

    /* No quote at all is an IndexError in the Python: no number, no event. */
    nd_modem__handle_urc(m, "+CLIP: 15551234,145");
    CHECK(!nd_modem__take(m, &e));
    CHECK_STR(nd_modem_caller_id(m), "+15551234");

    nd_modem__destroy(m);
}

static void test_urc_call_lifecycle(void)
{
    nd_modem *m = make_modem();
    nd_mev e;
    const char *label = NULL;
    int32_t secs = -1;

    CHECK(m != NULL);
    if (m == NULL)
        return;

    nd_modem__handle_urc(m, "RING");
    (void)nd_modem__take(m, &e);
    nd_modem__handle_urc(m, "+CLIP: \"5550000\",129");
    (void)nd_modem__take(m, &e);

    nd_modem__handle_urc(m, "VOICE CALL: BEGIN");
    CHECK_INT(nd_modem_state(m), ND_CALL_CONNECTED);
    CHECK(m->pcm_retry); /* re-assert CPCMREG=1 on the next free tick */
    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.kind, ND_MEV_CONNECTED);
    CHECK_STR(e.text, "5550000"); /* ("connected", caller_id) */

    nd_modem_call_status(m, &label, &secs);
    CHECK_STR(label, "CONNECTED");
    CHECK(secs >= 0);

    nd_modem__handle_urc(m, "VOICE CALL: END");
    CHECK_INT(nd_modem_state(m), ND_CALL_IDLE);
    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.kind, ND_MEV_ENDED);
    CHECK_STR(e.text, "VOICE CALL: END");

    /* END again with the call already down queues nothing. */
    nd_modem__handle_urc(m, "VOICE CALL: END");
    CHECK(!nd_modem__take(m, &e));

    nd_modem_call_status(m, &label, &secs);
    CHECK_STR(label, "IDLE");
    CHECK_INT(secs, -1);

    /* MISSED_CALL has NO state != IDLE guard, so it fires from IDLE. */
    nd_modem__handle_urc(m, "MISSED_CALL: 11:02AM 15551234");
    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.kind, ND_MEV_MISSED);
    CHECK_STR(e.text, "11:02AM 15551234");

    nd_modem__destroy(m);
}

static void test_urc_cmti_and_registration(void)
{
    nd_modem *m = make_modem();
    nd_mev e;

    CHECK(m != NULL);
    if (m == NULL)
        return;

    nd_modem__handle_urc(m, "+CMTI: \"SM\",3");
    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.kind, ND_MEV_SMS_RECEIVED);
    CHECK_INT(e.index, 3);

    /* rsplit(",", 1)[1] with no comma at all is an IndexError: silent. */
    nd_modem__handle_urc(m, "+CMTI: SM");
    CHECK(!nd_modem__take(m, &e));
    /* A non-numeric slot is a ValueError: also silent. */
    nd_modem__handle_urc(m, "+CMTI: \"SM\",x");
    CHECK(!nd_modem__take(m, &e));

    /* Query form "<n>,<stat>" takes field 1. */
    nd_modem__handle_urc(m, "+CEREG: 0,1");
    CHECK(nd_modem_registered(m));
    CHECK_INT(m->reg_stat, 1);

    /* Unsolicited form "<stat>" takes field 0. */
    nd_modem__handle_urc(m, "+CEREG: 2");
    CHECK(!nd_modem_registered(m));
    CHECK_INT(m->reg_stat, 2);

    /* Roaming counts as registered. */
    nd_modem__handle_urc(m, "+CREG: 0,5");
    CHECK(nd_modem_registered(m));

    /* The long query form still uses field 1. */
    nd_modem__handle_urc(m, "+CEREG: 0,1,\"1A2B\",\"01234567\",7");
    CHECK_INT(m->reg_stat, 1);

    /* A parse failure leaves _reg_stat alone. */
    nd_modem__handle_urc(m, "+CEREG: 0,zz");
    CHECK_INT(m->reg_stat, 1);

    nd_modem__destroy(m);
}

static void test_csq_and_cops_parsers(void)
{
    nd_modem *m = make_modem();
    nd_lines lines;

    CHECK(m != NULL);
    if (m == NULL)
        return;

    nd_modem__lines_reset(&lines);
    nd_modem__lines_add(&lines, "+CSQ: 23,99");
    nd_modem__parse_csq(m, &lines);
    CHECK_INT(m->csq, 23);

    /* The LAST matching line wins. */
    nd_modem__lines_reset(&lines);
    nd_modem__lines_add(&lines, "+CSQ: 5,99");
    nd_modem__lines_add(&lines, "+CSQ: 31,99");
    nd_modem__parse_csq(m, &lines);
    CHECK_INT(m->csq, 31);

    /* A parse failure leaves _csq unchanged. */
    nd_modem__lines_reset(&lines);
    nd_modem__lines_add(&lines, "+CSQ: ,99");
    nd_modem__parse_csq(m, &lines);
    CHECK_INT(m->csq, 31);

    nd_modem__lines_reset(&lines);
    nd_modem__lines_add(&lines, "+COPS: 0,0,\"Tello\",7");
    nd_modem__parse_cops(m, &lines);
    CHECK(m->operator_known);
    CHECK_STR(m->operator_name, "Tello");

    /* A quote-less reply sets the operator to None -- it does not keep the
     * previous carrier on the home screen. */
    nd_modem__lines_reset(&lines);
    nd_modem__lines_add(&lines, "+COPS: 0");
    nd_modem__parse_cops(m, &lines);
    CHECK(!m->operator_known);

    nd_modem__destroy(m);
}

static void test_sms_record_parsers(void)
{
    nd_lines lines;
    nd_sms_rec recs[4];
    size_t n;

    /* +CMGR: the sender is the SECOND quoted field. */
    nd_modem__lines_reset(&lines);
    nd_modem__lines_add(&lines,
                        "+CMGR: \"REC UNREAD\",\"+15551234\",\"\",\"24/08/23,10:11:12+04\"");
    nd_modem__lines_add(&lines, "hello there");
    nd_modem__lines_add(&lines, "second line");
    n = nd_modem__parse_sms_records(&lines, "+CMGR:", recs, ND_ARRAY_LEN(recs));
    CHECK_INT(n, 1);
    CHECK_STR(recs[0].sender, "+15551234");
    CHECK_STR(recs[0].text, "hello there\nsecond line");
    CHECK_INT(recs[0].index, -1); /* patched in by the caller */

    /* Lines before the first header are discarded. */
    nd_modem__lines_reset(&lines);
    nd_modem__lines_add(&lines, "junk before the header");
    nd_modem__lines_add(&lines, "+CMGR: \"REC READ\",\"+1555\",\"\"");
    nd_modem__lines_add(&lines, "body");
    n = nd_modem__parse_sms_records(&lines, "+CMGR:", recs, ND_ARRAY_LEN(recs));
    CHECK_INT(n, 1);
    CHECK_STR(recs[0].text, "body");

    /* Fewer than two quoted fields -> "unknown". */
    nd_modem__lines_reset(&lines);
    nd_modem__lines_add(&lines, "+CMGR: \"REC UNREAD\"");
    nd_modem__lines_add(&lines, "x");
    n = nd_modem__parse_sms_records(&lines, "+CMGR:", recs, ND_ARRAY_LEN(recs));
    CHECK_INT(n, 1);
    CHECK_STR(recs[0].sender, "unknown");

    /* +CMGL: multi-record, and the index comes from before the first comma. */
    nd_modem__lines_reset(&lines);
    nd_modem__lines_add(&lines, "+CMGL: 1,\"REC UNREAD\",\"+15550001\",\"\",\"24/08/23\"");
    nd_modem__lines_add(&lines, "first");
    nd_modem__lines_add(&lines, "+CMGL: 2,\"REC UNREAD\",\"+15550002\",\"\",\"24/08/23\"");
    nd_modem__lines_add(&lines, "second");
    nd_modem__lines_add(&lines, "and more");
    n = nd_modem__parse_sms_records(&lines, "+CMGL:", recs, ND_ARRAY_LEN(recs));
    CHECK_INT(n, 2);
    CHECK_INT(recs[0].index, 1);
    CHECK_STR(recs[0].sender, "+15550001");
    CHECK_STR(recs[0].text, "first");
    CHECK_INT(recs[1].index, 2);
    CHECK_STR(recs[1].sender, "+15550002");
    CHECK_STR(recs[1].text, "second\nand more");

    /* A header with no body at all yields an empty body, not a dropped
     * record. */
    nd_modem__lines_reset(&lines);
    nd_modem__lines_add(&lines, "+CMGL: 7,\"REC UNREAD\",\"+1555\",\"\"");
    n = nd_modem__parse_sms_records(&lines, "+CMGL:", recs, ND_ARRAY_LEN(recs));
    CHECK_INT(n, 1);
    CHECK_STR(recs[0].text, "");
    CHECK_INT(recs[0].index, 7);
}

static void test_number_filter(void)
{
    char out[ND_MODEM_NUMBER_MAX];

    CHECK_INT(nd_modem__filter_number("+1 (555) 123-4567", out, sizeof out), 12);
    CHECK_STR(out, "+15551234567");
    CHECK_INT(nd_modem__filter_number("*#06#", out, sizeof out), 5);
    CHECK_STR(out, "*#06#");
    CHECK_INT(nd_modem__filter_number("abc", out, sizeof out), 0);
    CHECK_STR(out, "");
}

/* ------------------------------------------------------------------ *
 * 4. Port discovery -- the ordering that looks like a bug
 * ------------------------------------------------------------------ */

static void write_tty(const char *name, const char *iface_hex)
{
    char path[ND_PATH_MAX];

    (void)snprintf(path, sizeof path, "/sys/class/tty/%s/device", name);
    pt_mkdir(path);
    if (iface_hex != NULL) {
        (void)snprintf(path, sizeof path, "/sys/class/tty/%s/bInterfaceNumber", name);
        pt_write_text(path, iface_hex);
    }
}

static void test_candidate_port_ordering(void)
{
    nd_modem *m;
    char ports[ND_MODEM_CAND_MAX][ND_MODEM_PORT_MAX];
    size_t n;

    use_scratch_settings("");
    write_tty("ttyUSB0", "00\n");  /* DIAG: dropped entirely            */
    write_tty("ttyUSB1", "01\n");  /* rest                              */
    write_tty("ttyUSB2", "02\n");  /* preferred                         */
    write_tty("ttyUSB3", "03\n");  /* preferred                         */
    write_tty("ttyUSB10", "04\n"); /* rest, and sorts BEFORE ttyUSB2    */
    write_tty("ttyUSB4", NULL);    /* unreadable iface == Python's None */
    write_tty("ttyS0", "02\n");    /* not a ttyUSB: never considered    */

    m = make_modem();
    CHECK(m != NULL);
    if (m == NULL)
        return;

    n = nd_modem__candidate_ports(m, ports, ND_ARRAY_LEN(ports));
    CHECK_INT(n, 5);
    if (n == 5u) {
        CHECK_STR(ports[0], "/dev/ttyUSB2");
        CHECK_STR(ports[1], "/dev/ttyUSB3");
        /* rest, in the byte-sorted listing's own order. ttyUSB10 before
         * ttyUSB2 is what sorted(os.listdir()) does and it is deliberate. */
        CHECK_STR(ports[2], "/dev/ttyUSB1");
        CHECK_STR(ports[3], "/dev/ttyUSB10");
        CHECK_STR(ports[4], "/dev/ttyUSB4");
    }
    nd_modem__destroy(m);
}

static void test_configured_port_is_the_only_candidate(void)
{
    nd_modem *m;
    char ports[ND_MODEM_CAND_MAX][ND_MODEM_PORT_MAX];

    use_scratch_settings("system.hw.modem_at_port=/dev/ttyACM0\n");
    write_tty("ttyUSB2", "02\n");

    m = make_modem();
    CHECK(m != NULL);
    if (m == NULL)
        return;
    CHECK_INT(nd_modem__candidate_ports(m, ports, ND_ARRAY_LEN(ports)), 1);
    CHECK_STR(ports[0], "/dev/ttyACM0");
    nd_modem__destroy(m);
}

static void test_hex_interface_16_is_not_interface_10(void)
{
    nd_modem *m;
    char ports[ND_MODEM_CAND_MAX][ND_MODEM_PORT_MAX];

    use_scratch_settings("");
    /* "10" parsed as hex is 16, so it is neither preferred nor dropped. */
    write_tty("ttyUSB0", "10\n");

    m = make_modem();
    CHECK(m != NULL);
    if (m == NULL)
        return;
    CHECK_INT(nd_modem__candidate_ports(m, ports, ND_ARRAY_LEN(ports)), 1);
    CHECK_STR(ports[0], "/dev/ttyUSB0");
    nd_modem__destroy(m);
}

/* ------------------------------------------------------------------ *
 * 5. The ALSA capture scan
 * ------------------------------------------------------------------ */

static void test_capture_device_scan(void)
{
    char dev[64];

    pt_mkdir("/proc/asound/card0");
    pt_write_text("/proc/asound/card0/pcm0p", ""); /* playback only */
    CHECK(!nd_modem__find_capture_device(dev, sizeof dev));

    pt_mkdir("/proc/asound/card1");
    pt_write_text("/proc/asound/card1/pcm0c", "");
    CHECK(nd_modem__find_capture_device(dev, sizeof dev));
    CHECK_STR(dev, "plughw:1,0");
}

static void test_capture_scan_is_byte_sorted(void)
{
    char dev[64];

    /* card10 sorts before card2, which is what sorted(os.listdir()) does. */
    pt_mkdir("/proc/asound/card10");
    pt_write_text("/proc/asound/card10/pcm0c", "");
    pt_mkdir("/proc/asound/card2");
    pt_write_text("/proc/asound/card2/pcm0c", "");
    CHECK(nd_modem__find_capture_device(dev, sizeof dev));
    CHECK_STR(dev, "plughw:10,0");
}

/* ------------------------------------------------------------------ *
 * 6. The flock
 * ------------------------------------------------------------------ */

static void test_the_lock_keeps_two_holders_apart(void)
{
    nd_modem *m;
    char resolved[ND_PATH_MAX];
    int other;

    use_scratch_settings("");
    m = make_modem();
    CHECK(m != NULL);
    if (m == NULL)
        return;
    CHECK(m->lock_fd >= 0);

    CHECK(nd_modem__acquire(m));
    nd_modem__release(m);

    /* A second open file description -- what busybox `flock` in S45modem is
     * from our point of view -- must lock us out. */
    CHECK_INT(nd_path_resolve(resolved, sizeof resolved, ND_MODEM_LOCK_FILE), ND_OK);
    other = open(resolved, O_RDWR);
    CHECK(other >= 0);
    if (other >= 0) {
        CHECK_INT(flock(other, LOCK_EX | LOCK_NB), 0);
        CHECK(!nd_modem__acquire(m));
        CHECK_INT(flock(other, LOCK_UN), 0);
        CHECK(nd_modem__acquire(m));
        nd_modem__release(m);
        (void)close(other);
    }
    nd_modem__destroy(m);
}

/* ------------------------------------------------------------------ *
 * 7. The pty: probe, init sequence, the staggered poll
 * ------------------------------------------------------------------ */

static const fake_rule SIM7600[] = {
    {"AT", "\r\nOK\r\n"},
    {"ATE0", "\r\nOK\r\n"},
    {"AT+CMEE=2", "\r\nOK\r\n"},
    {"AT+CLIP=1", "\r\nOK\r\n"},
    {"AT+CVHU=0", "\r\nOK\r\n"},
    {"AT+COPS=3,1", "\r\nOK\r\n"},
    {"AT+CMGF=1", "\r\nOK\r\n"},
    {"AT+CNMI=2,1,0,0,0", "\r\nOK\r\n"},
    {"AT+CGSN", "\r\n866758041234567\r\n\r\nOK\r\n"},
    {"AT+CSQ", "\r\n+CSQ: 23,99\r\n\r\nOK\r\n"},
    {"AT+CEREG?", "\r\n+CEREG: 0,1\r\n\r\nOK\r\n"},
    {"AT+COPS?", "\r\n+COPS: 0,0,\"Tello\",7\r\n\r\nOK\r\n"},
    {"AT+CLCC", "\r\n+CLCC: 1,0,0,0,0,\"+15551234\",129\r\n\r\nOK\r\n"},
    {"AT+CHUP", "\r\nOK\r\n"},
    {"AT+CPCMFRM=1", "\r\nOK\r\n"},
    {"AT+CPCMREG=1", "\r\nOK\r\n"},
    {"AT+CPCMREG=0", "\r\nOK\r\n"},
    {"ATD5551234;", "\r\nOK\r\n"},
    {"AT+CMGD=3", "\r\nOK\r\n"},
    {"AT+CMGD=1", "\r\nOK\r\n"},
    {"AT+CMGD=2", "\r\nOK\r\n"},
    {"AT+CMGR=3", "\r\n+CMGR: \"REC UNREAD\",\"+15551234\",\"\",\"24/08/23,10:11:12+04\"\r\n"
                  "hello there\r\n\r\nOK\r\n"},
    {"AT+CMGL=\"REC UNREAD\"",
     "\r\n+CMGL: 1,\"REC UNREAD\",\"+15550001\",\"\",\"24/08/23,01:02:03+04\"\r\n"
     "first message\r\n"
     "+CMGL: 2,\"REC UNREAD\",\"+15550002\",\"\",\"24/08/23,01:02:04+04\"\r\n"
     "second message\r\n\r\nOK\r\n"},
    {"AT+SILENT", NULL}, /* says nothing: the timeout path */
};

/* Bring a modem up on the fake. Returns NULL when the host has no ptys. */
static nd_modem *attach(fake_modem *fm)
{
    nd_modem *m;

    use_scratch_settings("system.hw.modem_at_port=" MODEM_LINK "\n");
    if (!fake_start(fm, SIM7600, ND_ARRAY_LEN(SIM7600)))
        return NULL;
    m = make_modem();
    if (m == NULL)
        return NULL;
    if (!nd_modem__probe_hardware(m)) {
        nd_modem__destroy(m);
        return NULL;
    }
    return m;
}

static void test_probe_adopts_the_port_and_runs_the_init_sequence(void)
{
    fake_modem fm;
    nd_modem *m = attach(&fm);
    nd_modem_status st;
    nd_mev e;

    CHECK(m != NULL);
    if (m == NULL) {
        fake_stop(&fm);
        return;
    }

    CHECK(nd_modem_has_hardware(m));
    /* AT plus the seven configuration commands plus AT+CGSN. */
    CHECK_INT(fake_commands(&fm), 9);

    nd_modem_status_snapshot(m, &st);
    CHECK(st.hardware);
    CHECK_STR(st.port, MODEM_LINK);
    CHECK_STR(st.imei, "866758041234567");

    /* _init_modem queues ("sms_stored_check", None) so messages that landed
     * while the phone was off get swept. */
    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.kind, ND_MEV_SMS_STORED_CHECK);

    nd_modem__destroy(m);
    fake_stop(&fm);
}

static void test_poll_staggers_csq_then_cereg_then_cops(void)
{
    fake_modem fm;
    nd_modem *m = attach(&fm);
    int before;

    CHECK(m != NULL);
    if (m == NULL) {
        fake_stop(&fm);
        return;
    }

    before = fake_commands(&fm);
    poll_now(m); /* tick 1: AT+CSQ */
    CHECK_INT(fake_commands(&fm) - before, 1);
    CHECK_STR(fm.last_cmd, "AT+CSQ");
    CHECK_INT(m->csq, 23);
    CHECK_INT(nd_modem_signal_level(m), 4);

    poll_now(m); /* tick 2: AT+CEREG? */
    CHECK_STR(fm.last_cmd, "AT+CEREG?");
    /* The reply is routed through _handle_urc, not read back. */
    CHECK(nd_modem_registered(m));

    poll_now(m); /* tick 3: AT+COPS? */
    CHECK_STR(fm.last_cmd, "AT+COPS?");
    CHECK_STR(nd_modem_operator_display(m), "Tello");

    /* Nothing is due now, so a fourth tick sends nothing at all. */
    before = fake_commands(&fm);
    poll_now(m);
    CHECK_INT(fake_commands(&fm) - before, 0);

    nd_modem__destroy(m);
    fake_stop(&fm);
}

static void test_poll_routes_an_unsolicited_ring(void)
{
    fake_modem fm;
    nd_modem *m = attach(&fm);
    nd_mev e;

    CHECK(m != NULL);
    if (m == NULL) {
        fake_stop(&fm);
        return;
    }
    while (nd_modem__take(m, &e))
        ; /* drop sms_stored_check */

    fake_inject(&fm, "\r\nRING\r\n\r\n+CLIP: \"+15551234\",145,\"\",0,\"\",0\r\n");
    settle(0.05);
    /* poll() also runs the in-call CLCC query once the state leaves IDLE, and
     * this modem's CLCC says the call is already up. Hold that off for one
     * tick so what is being checked here is only the URC routing. */
    m->next_clcc = nd_modem__now() + 100.0;
    poll_now(m);

    CHECK_INT(nd_modem_state(m), ND_CALL_RINGING);
    CHECK_STR(nd_modem_caller_id(m), "+15551234");
    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.kind, ND_MEV_INCOMING);
    CHECK(!e.has_detail);
    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.kind, ND_MEV_INCOMING);
    CHECK_STR(e.text, "+15551234");

    /* Let the CLCC through on the next tick: <stat> 0 promotes the call to
     * CONNECTED without any VOICE CALL: BEGIN ever arriving. */
    m->next_clcc = 0.0;
    poll_now(m);
    CHECK_INT(nd_modem_state(m), ND_CALL_CONNECTED);
    CHECK_INT(m->call_stat, 0);

    nd_modem__destroy(m);
    fake_stop(&fm);
}

static void test_transact_times_out_on_a_silent_modem(void)
{
    fake_modem fm;
    nd_modem *m = attach(&fm);
    char final[64];
    double t0;
    double elapsed;

    CHECK(m != NULL);
    if (m == NULL) {
        fake_stop(&fm);
        return;
    }

    t0 = nd_modem__now();
    CHECK(!nd_modem__transact(m, "AT+SILENT", 0.3, final, sizeof final, NULL));
    elapsed = nd_modem__now() - t0;
    CHECK_STR(final, "");
    CHECK(elapsed >= 0.3);
    CHECK(elapsed < 2.0);
    /* A timeout is not a lost modem. */
    CHECK(nd_modem_has_hardware(m));

    /* An unknown command still gets a final code back. */
    CHECK(nd_modem__transact(m, "AT+NOPE", 1.0, final, sizeof final, NULL));
    CHECK_STR(final, "ERROR");

    nd_modem__destroy(m);
    fake_stop(&fm);
}

static void test_a_mid_command_urc_is_handled_and_still_collected(void)
{
    fake_modem fm;
    nd_modem *m = attach(&fm);
    nd_lines lines;
    char final[64];
    nd_mev e;

    CHECK(m != NULL);
    if (m == NULL) {
        fake_stop(&fm);
        return;
    }
    while (nd_modem__take(m, &e))
        ;

    /* AT+CEREG?'s own reply looks exactly like the URC, which is why
     * _transact routes it AND keeps it. */
    CHECK(nd_modem__transact(m, "AT+CEREG?", 1.5, final, sizeof final, &lines));
    CHECK_STR(final, "OK");
    CHECK_INT(lines.n, 1);
    CHECK_STR(nd_modem__lines_get(&lines, 0u), "+CEREG: 0,1");
    CHECK_INT(m->reg_stat, 1);

    nd_modem__destroy(m);
    fake_stop(&fm);
}

static void test_the_rx_buffer_is_bounded(void)
{
    fake_modem fm;
    nd_modem *m = attach(&fm);
    char junk[1024];
    nd_lines lines;
    int i;

    CHECK(m != NULL);
    if (m == NULL) {
        fake_stop(&fm);
        return;
    }

    /* R-7: a babbling port with no newline must not grow the buffer without
     * bound. Sixteen kilobytes of it, then a real line. */
    memset(junk, 'x', sizeof junk);
    junk[sizeof junk - 1u] = '\0';
    for (i = 0; i < 16; i++) {
        fake_inject(&fm, junk);
        settle(0.01);
        (void)nd_modem__read_pending(m, &lines);
        CHECK(m->rx_len <= ND_MODEM_RXBUF_MAX);
    }
    CHECK(m->rx_overflow_logged);

    /* And it resynchronises at the next newline. */
    fake_inject(&fm, "\r\n+CSQ: 7,99\r\n");
    settle(0.05);
    (void)nd_modem__read_pending(m, &lines);
    CHECK(lines.n >= 1u);
    if (lines.n >= 1u)
        CHECK_STR(nd_modem__lines_get(&lines, lines.n - 1u), "+CSQ: 7,99");

    nd_modem__destroy(m);
    fake_stop(&fm);
}

/* ------------------------------------------------------------------ *
 * 8. SMS over the pty
 * ------------------------------------------------------------------ */

static void test_send_sms_walks_the_prompt_and_the_ack(void)
{
    fake_modem fm;
    nd_modem *m = attach(&fm);
    char detail[ND_MODEM_DETAIL_MAX];
    char body[512];
    nd_mev e;

    CHECK(m != NULL);
    if (m == NULL) {
        fake_stop(&fm);
        return;
    }
    while (nd_modem__take(m, &e))
        ;

    CHECK(nd_modem_send_sms(m, "+1 (555) 123-4567", "hello\nthere", detail, sizeof detail));
    CHECK_STR(detail, "42"); /* the +CMGS reference */
    fake_body(&fm, body, sizeof body);
    CHECK_STR(body, "hello\nthere"); /* newlines are kept */
    CHECK_STR(fm.last_cmd, "AT+CMGS=\"+15551234567\"");

    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.kind, ND_MEV_SMS_SENT);
    CHECK_STR(e.text, "+15551234567");

    nd_modem__destroy(m);
    fake_stop(&fm);
}

static void test_send_sms_reports_a_rejection_verbatim(void)
{
    fake_modem fm;
    nd_modem *m = attach(&fm);
    char detail[ND_MODEM_DETAIL_MAX];

    CHECK(m != NULL);
    if (m == NULL) {
        fake_stop(&fm);
        return;
    }

    /* Messages renders the detail as "Send failed: <detail>". */
    fm.cmgs_reply = "\r\n+CMS ERROR: 500\r\n";
    CHECK(!nd_modem_send_sms(m, "5551234", "nope", detail, sizeof detail));
    CHECK_STR(detail, "+CMS ERROR: 500");

    nd_modem__destroy(m);
    fake_stop(&fm);
}

static void test_send_sms_rejects_an_empty_number_and_body(void)
{
    fake_modem fm;
    nd_modem *m = attach(&fm);
    char detail[ND_MODEM_DETAIL_MAX];

    CHECK(m != NULL);
    if (m == NULL) {
        fake_stop(&fm);
        return;
    }

    CHECK(!nd_modem_send_sms(m, "abc", "hi", detail, sizeof detail));
    CHECK_STR(detail, "no number");
    /* Ctrl-Z and ESC are stripped, so a body of nothing else is empty. */
    CHECK(!nd_modem_send_sms(m, "5551234", "\x1a\x1b", detail, sizeof detail));
    CHECK_STR(detail, "empty message");

    nd_modem__destroy(m);
    fake_stop(&fm);
}

static void test_fetch_sms_reads_then_deletes(void)
{
    fake_modem fm;
    nd_modem *m = attach(&fm);
    nd_sms_rec rec;

    CHECK(m != NULL);
    if (m == NULL) {
        fake_stop(&fm);
        return;
    }

    CHECK_INT(nd_modem_fetch_sms(m, 3, &rec), ND_SMS_OK);
    CHECK_INT(rec.index, 3);
    CHECK_STR(rec.sender, "+15551234");
    CHECK_STR(rec.text, "hello there");
    /* The message is deleted so the SIM's ~30 slots never fill up. */
    CHECK_STR(fm.last_cmd, "AT+CMGD=3");

    /* A slot the modem does not know about is an error, not a crash. */
    CHECK_INT(nd_modem_fetch_sms(m, 9, &rec), ND_SMS_ERROR);

    nd_modem__destroy(m);
    fake_stop(&fm);
}

static void test_read_stored_sms_sweeps_and_deletes_each(void)
{
    fake_modem fm;
    nd_modem *m = attach(&fm);
    nd_sms_rec recs[4];
    size_t n = 0u;

    CHECK(m != NULL);
    if (m == NULL) {
        fake_stop(&fm);
        return;
    }

    CHECK_INT(nd_modem_read_stored_sms(m, recs, ND_ARRAY_LEN(recs), &n), ND_SMS_OK);
    CHECK_INT(n, 2);
    if (n == 2u) {
        CHECK_INT(recs[0].index, 1);
        CHECK_STR(recs[0].sender, "+15550001");
        CHECK_STR(recs[0].text, "first message");
        CHECK_INT(recs[1].index, 2);
        CHECK_STR(recs[1].text, "second message");
    }
    CHECK_STR(fm.last_cmd, "AT+CMGD=2");

    nd_modem__destroy(m);
    fake_stop(&fm);
}

/* ------------------------------------------------------------------ *
 * 9. Call control over the pty
 * ------------------------------------------------------------------ */

static void test_dial_and_hangup_on_real_hardware(void)
{
    fake_modem fm;
    nd_modem *m = attach(&fm);

    CHECK(m != NULL);
    if (m == NULL) {
        fake_stop(&fm);
        return;
    }

    CHECK(nd_modem_dial(m, "555-1234"));
    CHECK_INT(nd_modem_state(m), ND_CALL_CALLING);
    /* There is no PCM port under the scratch root, so no child was spawned
     * and the call is silent -- which is the documented degradation. */
    CHECK(!m->audio_live);
    CHECK(!m->mic_live);

    /* CPCMREG=1 answered OK, so no in-call retry is armed. */
    CHECK(!m->pcm_retry);

    CHECK(nd_modem_hangup(m));
    CHECK_INT(nd_modem_state(m), ND_CALL_IDLE);
    CHECK(m->pcm_cleanup); /* AT+CPCMREG=0 goes out on the next free tick */

    poll_now(m);
    CHECK(!m->pcm_cleanup);

    nd_modem__destroy(m);
    fake_stop(&fm);
}

static void test_a_junk_number_logs_an_empty_dial_and_fails(void)
{
    fake_modem fm;
    nd_modem *m = attach(&fm);
    int before;

    CHECK(m != NULL);
    if (m == NULL) {
        fake_stop(&fm);
        return;
    }

    before = fake_commands(&fm);
    CHECK(!nd_modem_dial(m, "hello"));
    CHECK_INT(fake_commands(&fm) - before, 0);
    CHECK_INT(nd_modem_state(m), ND_CALL_IDLE);

    nd_modem__destroy(m);
    fake_stop(&fm);
}

static void test_clcc_ends_a_call_the_modem_forgot(void)
{
    fake_modem fm;
    nd_modem *m = attach(&fm);
    nd_mev e;

    CHECK(m != NULL);
    if (m == NULL) {
        fake_stop(&fm);
        return;
    }

    /* +CLCC: ...,0,... is <stat> CONNECTED. */
    nd_modem__handle_urc(m, "RING");
    nd_modem__poll_clcc(m);
    CHECK_INT(nd_modem_state(m), ND_CALL_CONNECTED);
    CHECK_INT(m->call_stat, 0);
    while (nd_modem__take(m, &e))
        ;

    /* An empty call list with a call still up ends it cleanly -- this is the
     * missed "VOICE CALL: END" recovery. */
    fm.rules = NULL;
    fm.n_rules = 0u; /* every command now answers ERROR */
    nd_modem__poll_clcc(m);
    CHECK_INT(nd_modem_state(m), ND_CALL_CONNECTED); /* not OK: no change */

    fm.rules = SIM7600;
    fm.n_rules = ND_ARRAY_LEN(SIM7600);
    {
        static const fake_rule EMPTY_LIST[] = {{"AT+CLCC", "\r\nOK\r\n"}};

        fm.rules = EMPTY_LIST;
        fm.n_rules = ND_ARRAY_LEN(EMPTY_LIST);
        nd_modem__poll_clcc(m);
        CHECK_INT(nd_modem_state(m), ND_CALL_IDLE);
        CHECK(nd_modem__take(m, &e));
        CHECK_INT(e.kind, ND_MEV_ENDED);
        CHECK_STR(e.text, "CLCC empty");
    }

    nd_modem__destroy(m);
    fake_stop(&fm);
}

static void test_a_dead_port_drops_back_to_simulation(void)
{
    fake_modem fm;
    nd_modem *m = attach(&fm);
    nd_mev e;
    char final[64];

    CHECK(m != NULL);
    if (m == NULL) {
        fake_stop(&fm);
        return;
    }
    while (nd_modem__take(m, &e))
        ;

    /* Yank the descriptor out from under the engine the way a USB unplug
     * does, then transact: the write fails and the modem is dropped. */
    (void)close(m->fd);
    m->fd = open("/dev/null", O_RDONLY);
    CHECK(m->fd >= 0);
    (void)nd_modem__transact(m, "AT", 0.2, final, sizeof final, NULL);

    CHECK(!nd_modem_has_hardware(m));
    CHECK_INT(m->fd, -1);
    CHECK_INT(m->csq, -1);
    CHECK(!m->operator_known);
    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.kind, ND_MEV_MODEM_LOST);

    nd_modem__destroy(m);
    fake_stop(&fm);
}

/* ------------------------------------------------------------------ *
 * 10. Simulation Mode
 * ------------------------------------------------------------------ */

static void test_sim_signal_and_operator_hooks(void)
{
    nd_modem *m;

    use_scratch_settings("");
    m = make_modem();
    CHECK(m != NULL);
    if (m == NULL)
        return;

    /* No hook at all is None, which the home screen renders as the layout's
     * sim_val -- NOT as zero bars. */
    CHECK_INT(nd_modem_signal_level(m), -1);
    CHECK(nd_modem_operator_display(m) == NULL);

    pt_write_text(ND_MODEM_SIM_CSQ, "23\n");
    CHECK_INT(nd_modem_signal_level(m), 4);
    pt_write_text(ND_MODEM_SIM_CSQ, "3");
    CHECK_INT(nd_modem_signal_level(m), 1);
    pt_write_text(ND_MODEM_SIM_CSQ, "99");
    CHECK_INT(nd_modem_signal_level(m), 0);
    pt_write_text(ND_MODEM_SIM_CSQ, "not a number");
    CHECK_INT(nd_modem_signal_level(m), -1);

    pt_write_text(ND_MODEM_SIM_OPS, "  Tello \n");
    CHECK_STR(nd_modem_operator_display(m), "Tello");
    pt_write_text(ND_MODEM_SIM_OPS, "   ");
    CHECK(nd_modem_operator_display(m) == NULL);

    nd_modem__destroy(m);
}

static void test_sim_ring_hook_is_edge_triggered(void)
{
    nd_modem *m;
    char resolved[ND_PATH_MAX];
    nd_mev e;

    use_scratch_settings("");
    m = make_modem();
    CHECK(m != NULL);
    if (m == NULL)
        return;

    pt_write_text(ND_MODEM_SIM_RING, "5551234\n");
    nd_modem_poll(m);
    CHECK_INT(nd_modem_state(m), ND_CALL_RINGING);
    CHECK_STR(nd_modem_caller_id(m), "5551234");
    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.kind, ND_MEV_INCOMING);
    CHECK_STR(e.text, "5551234");

    /* One ring per write: a second tick with the same mtime is silent. */
    nd_modem_poll(m);
    CHECK(!nd_modem__take(m, &e));

    /* rm while it rings = the caller gave up. */
    CHECK_INT(nd_path_resolve(resolved, sizeof resolved, ND_MODEM_SIM_RING), ND_OK);
    CHECK_INT(unlink(resolved), 0);
    nd_modem_poll(m);
    CHECK_INT(nd_modem_state(m), ND_CALL_IDLE);
    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.kind, ND_MEV_ENDED);
    CHECK_STR(e.text, "sim caller gave up");

    /* An empty file rings from the default number. */
    pt_write_text(ND_MODEM_SIM_RING, "\n");
    nd_modem_poll(m);
    CHECK_STR(nd_modem_caller_id(m), "5550000");

    nd_modem__destroy(m);
}

static void test_sim_sms_hook_delivers_and_consumes(void)
{
    nd_modem *m;
    nd_mev e;
    nd_sms_rec rec;
    nd_modem_event pub;

    use_scratch_settings("");
    m = make_modem();
    CHECK(m != NULL);
    if (m == NULL)
        return;

    pt_write_text(ND_MODEM_SIM_SMS, "5551234|hey there\n");
    nd_modem_poll(m);
    /* The hook file is removed so the message arrives exactly once. */
    CHECK(!nd_path_exists(ND_MODEM_SIM_SMS));
    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.kind, ND_MEV_SMS_SIM);
    CHECK_STR(e.sender, "5551234");
    CHECK_STR(e.text, "hey there");

    /* No bar at all: the whole string is the body and the sender is the
     * default. */
    pt_write_text(ND_MODEM_SIM_SMS, "just a body");
    nd_modem_poll(m);
    CHECK(nd_modem__take(m, &e));
    CHECK_STR(e.sender, "5550000");
    CHECK_STR(e.text, "just a body");

    /* And the whole way through the public surface: the event comes out as
     * ND_MODEM_EV_SMS and fetch_sms() hands back the stashed record. */
    pt_write_text(ND_MODEM_SIM_SMS, "5559999|round trip");
    nd_modem_poll(m);
    CHECK(nd_modem_take_pending_event(m, &pub));
    CHECK_INT(pub.kind, ND_MODEM_EV_SMS);
    CHECK_INT(pub.index, ND_MODEM_SMS_IDX_SIM);
    CHECK_INT(nd_modem_fetch_sms(m, pub.index, &rec), ND_SMS_OK);
    CHECK_STR(rec.sender, "5559999");
    CHECK_STR(rec.text, "round trip");
    /* Only once. */
    CHECK_INT(nd_modem_fetch_sms(m, pub.index, &rec), ND_SMS_ERROR);

    nd_modem__destroy(m);
}

static void test_sim_dial_pretends_after_two_seconds(void)
{
    nd_modem *m;
    nd_mev e;

    use_scratch_settings("");
    m = make_modem();
    CHECK(m != NULL);
    if (m == NULL)
        return;

    CHECK(nd_modem_dial(m, "5551234"));
    CHECK_INT(nd_modem_state(m), ND_CALL_CALLING);
    CHECK(m->sim_connect_armed);

    /* Rather than sleep for the two seconds, bring the deadline forward --
     * it is the modem thread's own field and this test IS that thread. */
    m->sim_connect_at = nd_modem__now() - 0.001;
    nd_modem_poll(m);
    CHECK_INT(nd_modem_state(m), ND_CALL_CONNECTED);
    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.kind, ND_MEV_CONNECTED);
    CHECK(!e.has_detail);

    CHECK(nd_modem_hangup(m));
    CHECK_INT(nd_modem_state(m), ND_CALL_IDLE);
    CHECK(!m->sim_connect_armed);
    /* No hardware, so no deferred CPCMREG=0. */
    CHECK(!m->pcm_cleanup);

    nd_modem__destroy(m);
}

static void test_sim_send_sms_pretends(void)
{
    nd_modem *m;
    char detail[ND_MODEM_DETAIL_MAX];
    nd_mev e;

    use_scratch_settings("");
    m = make_modem();
    CHECK(m != NULL);
    if (m == NULL)
        return;

    CHECK(nd_modem_send_sms(m, "555 1234", "hi", detail, sizeof detail));
    CHECK_STR(detail, "simulated");
    CHECK(nd_modem__take(m, &e));
    CHECK_INT(e.kind, ND_MEV_SMS_SENT);
    CHECK_STR(e.text, "5551234");

    nd_modem__destroy(m);
}

/* ------------------------------------------------------------------ *
 * 10b. The probe says WHY there is no modem
 * ------------------------------------------------------------------ *
 *
 * "HARDWARE NOT FOUND: Running in Simulation Mode." on a phone that visibly
 * HAS a modem plugged into it is the least useful sentence this service can
 * print, and until now it was the only one -- probe_ports() dropped every
 * errno and every refusal on the floor. These pin the three answers it can
 * give, because a diagnostic nobody checked is a diagnostic that rots.
 */

static void test_probe_reports_no_candidates(void)
{
    nd_modem *m;

    use_scratch_settings("");
    /* An empty /sys/class/tty: nothing enumerated at all. */
    m = make_modem();
    CHECK(m != NULL);
    if (m == NULL)
        return;
    CHECK(!nd_modem__probe_hardware(m));
    CHECK(strstr(m->last_probe_why, "no candidate AT ports") != NULL);
    nd_modem__destroy(m);
}

static void test_probe_reports_a_missing_device_node(void)
{
    nd_modem *m;

    /* Configured to a path that is not there: sysfs is bypassed entirely, so
     * this is the "the node the setting names does not exist" answer. */
    use_scratch_settings("system.hw.modem_at_port=/dev/nope-not-here\n");
    m = make_modem();
    CHECK(m != NULL);
    if (m == NULL)
        return;
    CHECK(!nd_modem__probe_hardware(m));
    CHECK(strstr(m->last_probe_why, "/dev/nope-not-here") != NULL);
    CHECK(strstr(m->last_probe_why, "no device node") != NULL);
    nd_modem__destroy(m);
}

static void test_probe_reports_the_errno_when_the_port_will_not_open(void)
{
    nd_modem *m;
    char resolved[ND_PATH_MAX];

    /* A directory exists at the port's path. open(O_RDWR) on a directory is
     * EISDIR, which is a real errno reaching a real strerror() -- the point
     * being that the REASON travels, not that this particular one does. */
    use_scratch_settings("system.hw.modem_at_port=/notaport\n");
    CHECK_INT(nd_mkdir_p("/notaport", 0755u), ND_OK);
    CHECK_INT(nd_path_resolve(resolved, sizeof resolved, "/notaport"), ND_OK);

    m = make_modem();
    CHECK(m != NULL);
    if (m == NULL)
        return;
    CHECK(!nd_modem__probe_hardware(m));
    CHECK(strstr(m->last_probe_why, "/notaport") != NULL);
    CHECK(strstr(m->last_probe_why, strerror(EISDIR)) != NULL);
    nd_modem__destroy(m);
}

/* The lock is the cause with no symptom: S45modem's background redial takes
 * it per transaction, and a probe landing inside one is indistinguishable
 * from having no modem at all -- it used to `return false` in silence. */
static void test_probe_reports_the_held_lock(void)
{
    nd_modem *m;
    nd_modem *other;

    use_scratch_settings("");
    m = make_modem();
    other = make_modem();
    CHECK(m != NULL && other != NULL);
    if (m == NULL || other == NULL) {
        nd_modem__destroy(m);
        nd_modem__destroy(other);
        return;
    }
    if (m->lock_fd < 0 || other->lock_fd < 0) {
        /* No lock file on this host: the probe serialises with nobody and
         * this case cannot arise. */
        nd_modem__destroy(m);
        nd_modem__destroy(other);
        return;
    }

    /* `other` stands in for S45modem holding the AT port. */
    CHECK(nd_modem__acquire(other));
    CHECK(!nd_modem__probe_hardware(m));
    CHECK(strstr(m->last_probe_why, "lock is held") != NULL);
    nd_modem__release(other);

    nd_modem__destroy(other);
    nd_modem__destroy(m);
}

/* Printed once, not six times a minute for the life of the phone. */
static void test_the_same_reason_is_not_reprinted(void)
{
    nd_modem *m;
    char first[ND_MODEM_PROBE_WHY_MAX];

    use_scratch_settings("");
    m = make_modem();
    CHECK(m != NULL);
    if (m == NULL)
        return;

    CHECK(!nd_modem__probe_hardware(m));
    (void)nd_strlcpy(first, m->last_probe_why, sizeof first);
    CHECK(first[0] != '\0');

    /* Same failure again: last_probe_why is unchanged, which is exactly what
     * probe_note() compares against before printing. */
    m->next_probe = 0.0;
    CHECK(!nd_modem__probe_hardware(m));
    CHECK_STR(m->last_probe_why, first);

    nd_modem__destroy(m);
}

static void test_sim_reprobe_finds_a_modem_plugged_in_later(void)
{
    fake_modem fm;
    nd_modem *m;
    nd_mev e;
    bool found = false;

    /* No /dev/modem yet: the probe fails and the service starts simulated. */
    use_scratch_settings("system.hw.modem_at_port=" MODEM_LINK "\n");
    m = make_modem();
    CHECK(m != NULL);
    if (m == NULL)
        return;
    CHECK(!nd_modem__probe_hardware(m));
    CHECK(!nd_modem_has_hardware(m));

    /* Now plug it in. The re-probe timer is armed ten seconds out, so it is
     * brought forward rather than waited for. */
    if (!fake_start(&fm, SIM7600, ND_ARRAY_LEN(SIM7600))) {
        nd_modem__destroy(m);
        fake_stop(&fm);
        return;
    }
    m->next_probe = 0.0;
    nd_modem_poll(m);
    CHECK(nd_modem_has_hardware(m));
    while (nd_modem__take(m, &e)) {
        if (e.kind == ND_MEV_MODEM_FOUND) {
            found = true;
            CHECK_STR(e.text, MODEM_LINK);
        }
    }
    CHECK(found);

    nd_modem__destroy(m);
    fake_stop(&fm);
}

/* ------------------------------------------------------------------ *
 * 11. allow_calls=OFF
 * ------------------------------------------------------------------ */

static void test_allow_calls_off_simulates_on_real_hardware(void)
{
    fake_modem fm;
    nd_modem *m;
    int before;

    use_scratch_settings("system.hw.modem_at_port=" MODEM_LINK "\n"
                         "system.modem.allow_calls=OFF\n");
    if (!fake_start(&fm, SIM7600, ND_ARRAY_LEN(SIM7600))) {
        fake_stop(&fm);
        return;
    }
    m = make_modem();
    CHECK(m != NULL);
    if (m == NULL) {
        fake_stop(&fm);
        return;
    }
    CHECK(!m->allow_calls);
    CHECK(nd_modem__probe_hardware(m));

    before = fake_commands(&fm);
    CHECK(nd_modem_dial(m, "5551234"));
    /* No ATD went out: the dial is pretended even with a modem attached. */
    CHECK_INT(fake_commands(&fm) - before, 0);
    CHECK_INT(nd_modem_state(m), ND_CALL_CALLING);
    CHECK(m->sim_connect_armed);

    /* answer() is pretended too. */
    CHECK(nd_modem_answer(m));
    CHECK_INT(nd_modem_state(m), ND_CALL_CONNECTED);

    nd_modem__destroy(m);
    fake_stop(&fm);
}

static void test_allow_calls_parsing(void)
{
    nd_modem *m;

    use_scratch_settings("system.modem.allow_calls=yes\n");
    m = make_modem();
    CHECK(m != NULL);
    if (m != NULL) {
        CHECK(m->allow_calls);
        nd_modem__destroy(m);
    }

    /* Anything unrecognised is FALSE -- there is no default to fall back to
     * once the setting has been read. */
    use_scratch_settings("system.modem.allow_calls=maybe\n");
    m = make_modem();
    CHECK(m != NULL);
    if (m != NULL) {
        CHECK(!m->allow_calls);
        nd_modem__destroy(m);
    }

    /* Absent means the caller's default, which is ON. */
    use_scratch_settings("");
    m = make_modem();
    CHECK(m != NULL);
    if (m != NULL) {
        CHECK(m->allow_calls);
        nd_modem__destroy(m);
    }
}

/* ------------------------------------------------------------------ *
 * 12. The threaded public surface
 * ------------------------------------------------------------------ */

static void test_the_thread_runs_and_stops_cleanly(void)
{
    nd_modem *m = NULL;
    char detail[ND_MODEM_DETAIL_MAX];
    nd_modem_status st;

    use_scratch_settings("");
    CHECK_INT(nd_modem_open(&m), ND_OK);
    CHECK(m != NULL);
    if (m == NULL)
        return;
    CHECK(m->thread_started);
    CHECK(!nd_modem_has_hardware(m)); /* no ports under the scratch root */

    /* Every one of these crosses the request slot and comes back. */
    CHECK(nd_modem_dial(m, "5551234"));
    CHECK_INT(nd_modem_state(m), ND_CALL_CALLING);
    CHECK(nd_modem_hangup(m));
    CHECK_INT(nd_modem_state(m), ND_CALL_IDLE);
    CHECK(nd_modem_send_sms(m, "5551234", "threaded", detail, sizeof detail));
    CHECK_STR(detail, "simulated");

    nd_modem_status_snapshot(m, &st);
    CHECK(!st.hardware);
    CHECK_INT(st.signal_level, -1);
    CHECK_INT(st.state, ND_CALL_IDLE);

    /* The ring hook is serviced by the thread, with nobody polling -- which
     * is the whole point of decision 1: a call arrives even when the app in
     * front never asks for a key. The thread ticks at 10 Hz, so this waits a
     * few ticks rather than assuming one. */
    pt_write_text(ND_MODEM_SIM_RING, "5559876\n");
    {
        int spins;

        for (spins = 0; spins < 40 && nd_modem_state(m) != ND_CALL_RINGING; spins++)
            settle(0.05);
    }
    CHECK_INT(nd_modem_state(m), ND_CALL_RINGING);
    CHECK_STR(nd_modem_caller_id(m), "5559876");

    nd_modem_close(m);
}

/* nd_modem_last_call_secs(): the reading that OUTLIVES the call.
 *
 * The core writes the call log after the hangup, and by then
 * nd_modem_call_status() reports -1 because the modem is IDLE again. So the
 * length is latched on the way to IDLE and has to survive until the next call
 * replaces it. Three claims: a live call has no length yet, a finished one
 * does, and a call that never connected has zero rather than the previous
 * call's answer.
 *
 * Real seconds, deliberately: nd_modem__now() is CLOCK_MONOTONIC and is not
 * on the virtual clock, so a simulated call has to actually last a second for
 * this to mean anything. Simulation connects two seconds after the dial. */
static void test_the_last_call_length_outlives_the_call(void)
{
    nd_modem *m = NULL;
    int spins;

    use_scratch_settings("");
    CHECK_INT(nd_modem_open(&m), ND_OK);
    CHECK(m != NULL);
    if (m == NULL)
        return;

    CHECK_INT(nd_modem_last_call_secs(m), 0);

    CHECK(nd_modem_dial(m, "5551234"));
    for (spins = 0; spins < 100 && nd_modem_state(m) != ND_CALL_CONNECTED; spins++)
        settle(0.05);
    CHECK_INT(nd_modem_state(m), ND_CALL_CONNECTED);
    /* Still up: there is no LAST call yet. */
    CHECK_INT(nd_modem_last_call_secs(m), 0);

    settle(1.1);
    CHECK(nd_modem_hangup(m));
    CHECK_INT(nd_modem_state(m), ND_CALL_IDLE);
    CHECK(nd_modem_last_call_secs(m) >= 1);
    CHECK(nd_modem_last_call_secs(m) < 60); /* and it is seconds, not something else */

    /* A second hangup does not restart the clock on a call already counted. */
    {
        int32_t first = nd_modem_last_call_secs(m);

        CHECK(nd_modem_hangup(m));
        CHECK_INT(nd_modem_last_call_secs(m), first);
    }

    /* A new dial clears it, and a call that is ended before it connects
     * leaves zero rather than the previous call's length. */
    CHECK(nd_modem_dial(m, "5551234"));
    CHECK_INT(nd_modem_last_call_secs(m), 0);
    CHECK(nd_modem_hangup(m));
    CHECK_INT(nd_modem_last_call_secs(m), 0);

    CHECK_INT(nd_modem_last_call_secs(NULL), 0);

    nd_modem_close(m);
}

/* An ANSWERED call in simulation is stamped too. Nothing sends
 * "VOICE CALL: BEGIN" when there is no modem, so do_answer() is the only
 * thing that can mark a pretend call connected -- without it the in-call
 * screen shows no timer and the call log records a call that lasted no time,
 * while a simulated DIALLED call shows both. */
static void test_a_simulated_answer_starts_the_clock(void)
{
    nd_modem *m = NULL;
    const char *label = NULL;
    int32_t secs = -1;
    int spins;

    use_scratch_settings("");
    CHECK_INT(nd_modem_open(&m), ND_OK);
    CHECK(m != NULL);
    if (m == NULL)
        return;

    pt_write_text(ND_MODEM_SIM_RING, "5559876\n");
    for (spins = 0; spins < 40 && nd_modem_state(m) != ND_CALL_RINGING; spins++)
        settle(0.05);
    CHECK_INT(nd_modem_state(m), ND_CALL_RINGING);

    CHECK(nd_modem_answer(m));
    CHECK_INT(nd_modem_state(m), ND_CALL_CONNECTED);

    nd_modem_call_status(m, &label, &secs);
    CHECK_STR(label, "CONNECTED");
    CHECK(secs >= 0); /* -1 would be "connected, but no timer" */

    settle(1.1);
    CHECK(nd_modem_hangup(m));
    CHECK(nd_modem_last_call_secs(m) >= 1);

    /* The hook file goes, so the next test does not inherit a ringing phone. */
    {
        char resolved[ND_PATH_MAX];

        if (nd_path_resolve(resolved, sizeof resolved, ND_MODEM_SIM_RING) == ND_OK)
            (void)unlink(resolved);
    }
    nd_modem_close(m);
}

static void test_requeue_puts_an_event_back_at_the_front(void)
{
    nd_modem *m;
    nd_modem_event a;
    nd_modem_event b;
    nd_mev e;

    use_scratch_settings("");
    m = make_modem();
    CHECK(m != NULL);
    if (m == NULL)
        return;

    memset(&e, 0, sizeof e);
    e.kind = ND_MEV_SMS_RECEIVED;
    e.index = 1;
    e.has_detail = true;
    nd_modem__queue(m, &e);
    e.index = 2;
    nd_modem__queue(m, &e);

    CHECK(nd_modem_take_pending_event(m, &a));
    CHECK_INT(a.index, 1);
    /* main.py requeues when the port was busy, and the retry must come back
     * BEFORE the message that landed after it. */
    nd_modem_requeue_event(m, &a);
    CHECK(nd_modem_take_pending_event(m, &b));
    CHECK_INT(b.index, 1);
    CHECK(nd_modem_take_pending_event(m, &b));
    CHECK_INT(b.index, 2);
    CHECK(!nd_modem_take_pending_event(m, &b));

    nd_modem__destroy(m);
}

static void test_unrepresentable_events_are_skipped_not_surfaced(void)
{
    nd_modem *m;
    nd_modem_event pub;
    nd_mev e;

    use_scratch_settings("");
    m = make_modem();
    CHECK(m != NULL);
    if (m == NULL)
        return;

    /* sms_sent and modem_lost have no spelling in the frozen public enum;
     * the SMS behind them must still come out. */
    memset(&e, 0, sizeof e);
    e.kind = ND_MEV_SMS_SENT;
    nd_modem__queue(m, &e);
    e.kind = ND_MEV_MODEM_LOST;
    nd_modem__queue(m, &e);
    e.kind = ND_MEV_SMS_RECEIVED;
    e.index = 5;
    nd_modem__queue(m, &e);

    CHECK(nd_modem_take_pending_event(m, &pub));
    CHECK_INT(pub.kind, ND_MODEM_EV_SMS);
    CHECK_INT(pub.index, 5);
    CHECK(!nd_modem_take_pending_event(m, &pub));
    CHECK_INT(pub.kind, ND_MODEM_EV_NONE);

    nd_modem__destroy(m);
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    (void)nd_settings_init();
    nd_log_set_colour(false);

    RUN(test_decode_line);
    RUN(test_int_and_hex);
    RUN(test_final_and_urc_classification);
    RUN(test_bars_table);

    RUN(test_event_ring_drops_the_oldest_on_append);
    RUN(test_event_ring_drops_the_newest_on_requeue);

    RUN(test_urc_ring_and_clip);
    RUN(test_urc_call_lifecycle);
    RUN(test_urc_cmti_and_registration);
    RUN(test_csq_and_cops_parsers);
    RUN(test_sms_record_parsers);
    RUN(test_number_filter);

    RUN(test_candidate_port_ordering);
    RUN(test_configured_port_is_the_only_candidate);
    RUN(test_hex_interface_16_is_not_interface_10);
    RUN(test_capture_device_scan);
    RUN(test_capture_scan_is_byte_sorted);
    RUN(test_the_lock_keeps_two_holders_apart);

    RUN(test_probe_adopts_the_port_and_runs_the_init_sequence);
    RUN(test_poll_staggers_csq_then_cereg_then_cops);
    RUN(test_poll_routes_an_unsolicited_ring);
    RUN(test_transact_times_out_on_a_silent_modem);
    RUN(test_a_mid_command_urc_is_handled_and_still_collected);
    RUN(test_the_rx_buffer_is_bounded);

    RUN(test_send_sms_walks_the_prompt_and_the_ack);
    RUN(test_send_sms_reports_a_rejection_verbatim);
    RUN(test_send_sms_rejects_an_empty_number_and_body);
    RUN(test_fetch_sms_reads_then_deletes);
    RUN(test_read_stored_sms_sweeps_and_deletes_each);

    RUN(test_dial_and_hangup_on_real_hardware);
    RUN(test_a_junk_number_logs_an_empty_dial_and_fails);
    RUN(test_clcc_ends_a_call_the_modem_forgot);
    RUN(test_a_dead_port_drops_back_to_simulation);

    RUN(test_sim_signal_and_operator_hooks);
    RUN(test_sim_ring_hook_is_edge_triggered);
    RUN(test_sim_sms_hook_delivers_and_consumes);
    RUN(test_sim_dial_pretends_after_two_seconds);
    RUN(test_sim_send_sms_pretends);
    RUN(test_probe_reports_no_candidates);
    RUN(test_probe_reports_a_missing_device_node);
    RUN(test_probe_reports_the_errno_when_the_port_will_not_open);
    RUN(test_probe_reports_the_held_lock);
    RUN(test_the_same_reason_is_not_reprinted);
    RUN(test_sim_reprobe_finds_a_modem_plugged_in_later);

    RUN(test_allow_calls_off_simulates_on_real_hardware);
    RUN(test_allow_calls_parsing);

    RUN(test_the_thread_runs_and_stops_cleanly);
    RUN(test_the_last_call_length_outlives_the_call);
    RUN(test_a_simulated_answer_starts_the_clock);
    RUN(test_requeue_puts_an_event_back_at_the_front);
    RUN(test_unrepresentable_events_are_skipped_not_surfaced);

    return pt_report("test_modem");
}

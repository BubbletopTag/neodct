/* test_modem_app.c -- the engineering Modem app's RADIO page, app id 9005.
 *
 * modem_app.h says the row builders are "kept drawing-free so they can be
 * bench-tested" and nothing was testing them. These are the rows somebody
 * reads off a screen while deciding whether to take a phone apart, so what
 * they say is asserted as strings.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. RF names CFUN 0, 1 and 4 and prints anything else. Those three are the
 *     ones that change what the rest of the page means -- a radio in flight
 *     mode reports CSQ 99 and CEREG 4 with a perfect antenna on it, which is
 *     the screen this row exists to stop being ambiguous.
 *
 *  2. RSRP prints tenths of a dBm, keeps the mode beside it, falls back to
 *     the bare mode when the reply carried no measurement ("NO SERVICE" is an
 *     answer, and a more useful one than "--"), and KEEPS THE SIGN on a value
 *     whose whole part is zero. No modem reports -0.4 dBm; a formatter that
 *     is only right for the inputs somebody expected is how this page ended
 *     up with one ambiguous row in the first place.
 *
 *  3. The two rows appear ONLY with hardware, and the order is
 *     OPER/REG/RF/CSQ/RSRP/CALL -- RF above CSQ because it decides what CSQ
 *     means, RSRP below it because it is the measurement CSQ 99 hides, and no
 *     BARS, which is a coarser view of CSQ and is on the home screen anyway.
 *
 *  4. Without hardware the page is OPER/REG/CSQ/BARS/PORTS/WHY: no RF or RSRP
 *     (questions for a module that is not there), no CALL (there can be no
 *     call without a modem), and THE WHY ROW IS REACHABLE, which it was not
 *     while CALL held the slot.
 *
 *  5. Six is the ceiling, and the geometry says so: content_bottom is 145,
 *     not 175. A seventh row draws through the bottom line -- which is how
 *     this was found, so the check is here rather than in a picture.
 *
 *  5. Every builder still honours a short `max`, at every one of its new
 *     early returns.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set.
 */

#include <stdio.h>
#include <string.h>

#include "nd_app.h"
#include "nd_modem.h"

#include "smallapp_test.h"

#include "../../apps/Modem/modem_app.h"

static struct {
    size_t (*radio_rows)(const nd_modem_status *, int32_t, nd_modemapp_row *, size_t);
    size_t (*sim_rows_absent)(nd_modemapp_row *, size_t);
    const char *(*rf_text)(int32_t, char *, size_t);
    const char *(*rsrp_text)(const nd_modem_status *, char *, size_t);
    int32_t (*line_h)(int32_t, int32_t, size_t);
    const char *const *reset_hint;
    const char *const *ask_reset;
} api;

static bool api_open(void *h)
{
    *(void **)&api.radio_rows = sa_sym(h, "nd_modemapp_radio_rows");
    *(void **)&api.sim_rows_absent = sa_sym(h, "nd_modemapp_sim_rows_absent");
    *(void **)&api.rf_text = sa_sym(h, "nd_modemapp_rf_text");
    *(void **)&api.rsrp_text = sa_sym(h, "nd_modemapp_rsrp_text");
    *(void **)&api.line_h = sa_sym(h, "nd_modemapp_line_h");
    api.reset_hint = sa_sym(h, "nd_modemapp_reset_hint");
    api.ask_reset = sa_sym(h, "nd_modemapp_ask_reset");
    return api.radio_rows != NULL && api.sim_rows_absent != NULL && api.rf_text != NULL &&
           api.rsrp_text != NULL && api.line_h != NULL && api.reset_hint != NULL &&
           api.ask_reset != NULL;
}

/* A snapshot with a modem on it, registered and idle. */
static void live(nd_modem_status *st)
{
    memset(st, 0, sizeof *st);
    st->hardware = true;
    (void)snprintf(st->port, sizeof st->port, "%s", "/dev/ttyUSB2");
    (void)snprintf(st->operator_name, sizeof st->operator_name, "%s", "Tello");
    st->signal_level = 3;
    st->csq_rssi = 17;
    st->reg_stat = 1;
    st->registered = true;
    st->state = ND_CALL_IDLE;
    st->call_secs = -1;
    st->cfun = 1;
    (void)snprintf(st->cell_mode, sizeof st->cell_mode, "%s", "LTE");
    st->rsrp_dbm10 = -902;
}

/* The screen from the bug report: a modem that answers, a radio that says
 * nothing. */
static void no_signal(nd_modem_status *st)
{
    live(st);
    st->operator_name[0] = '\0';
    st->signal_level = 0;
    st->csq_rssi = 99;
    st->reg_stat = 4;
    st->registered = false;
    (void)snprintf(st->cell_mode, sizeof st->cell_mode, "%s", "NO SERVICE");
    st->rsrp_dbm10 = ND_MODEM_RSRP_NONE;
}

static const char *value_of(const nd_modemapp_row *rows, size_t n, const char *label)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        if (strcmp(rows[i].label, label) == 0)
            return rows[i].value;
    }
    return NULL;
}

static void test_rf_text(void)
{
    char buf[ND_MODEMAPP_VALUE_MAX];

    CHECK_STR(api.rf_text(-1, buf, sizeof buf), "--", "CFUN unknown");
    CHECK_STR(api.rf_text(0, buf, sizeof buf), "OFF (CFUN 0)", "CFUN 0");
    CHECK_STR(api.rf_text(1, buf, sizeof buf), "ON (CFUN 1)", "CFUN 1");
    CHECK_STR(api.rf_text(4, buf, sizeof buf), "FLIGHT (CFUN 4)", "CFUN 4");
    /* Not guessed at. The SIM7600 has 5, 6 and 7 and they are not worth
     * inventing names for. */
    CHECK_STR(api.rf_text(7, buf, sizeof buf), "CFUN 7", "CFUN 7");
}

static void test_rsrp_text(void)
{
    nd_modem_status st;
    char buf[ND_MODEMAPP_VALUE_MAX];

    live(&st);
    CHECK_STR(api.rsrp_text(&st, buf, sizeof buf), "-90.2 dBm LTE", "a working cell");

    /* The reading CSQ 99 hides, and the reason this row exists. */
    st.rsrp_dbm10 = -1134;
    CHECK_STR(api.rsrp_text(&st, buf, sizeof buf), "-113.4 dBm LTE", "cell edge");

    /* Nothing has answered at all. */
    st.cell_mode[0] = '\0';
    CHECK_STR(api.rsrp_text(&st, buf, sizeof buf), "--", "no CPSI yet");

    /* A mode with no measurement in it is its own answer. */
    no_signal(&st);
    CHECK_STR(api.rsrp_text(&st, buf, sizeof buf), "NO SERVICE", "CPSI said NO SERVICE");

    (void)snprintf(st.cell_mode, sizeof st.cell_mode, "%s", "GSM");
    CHECK_STR(api.rsrp_text(&st, buf, sizeof buf), "GSM", "GSM has no RSRP");

    /* THE SIGN. -0.4 has a whole part of 0, and "%d.%d" would print "0.4". */
    live(&st);
    st.rsrp_dbm10 = -4;
    CHECK_STR(api.rsrp_text(&st, buf, sizeof buf), "-0.4 dBm LTE", "a zero whole part keeps -");
    st.rsrp_dbm10 = 0;
    CHECK_STR(api.rsrp_text(&st, buf, sizeof buf), "0.0 dBm LTE", "and zero itself has no sign");

    CHECK_STR(api.rsrp_text(NULL, buf, sizeof buf), "--", "NULL snapshot");
}

static void test_radio_rows_with_hardware(void)
{
    nd_modem_status st;
    nd_modemapp_row rows[ND_MODEMAPP_MAX_ROWS];
    size_t n;

    live(&st);
    n = api.radio_rows(&st, 3, rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 6, "six rows with a modem");

    /* The ORDER is the argument. RF is read before CSQ because it says
     * whether CSQ means anything; RSRP is read after it because it is what
     * CSQ could not measure. */
    CHECK_STR(rows[0].label, "OPER", "row 0");
    CHECK_STR(rows[1].label, "REG", "row 1");
    CHECK_STR(rows[2].label, "RF", "row 2");
    CHECK_STR(rows[3].label, "CSQ", "row 3");
    CHECK_STR(rows[4].label, "RSRP", "row 4");
    CHECK_STR(rows[5].label, "CALL", "row 5");

    CHECK_STR(rows[0].value, "Tello", "operator");
    CHECK_STR(rows[1].value, "HOME  (CEREG 1)", "registration");
    CHECK_STR(rows[2].value, "ON (CFUN 1)", "radio on");
    CHECK_STR(rows[3].value, "17/31  -79 dBm", "csq");
    CHECK_STR(rows[4].value, "-90.2 dBm LTE", "rsrp");
    CHECK_STR(rows[5].value, "IDLE", "call");

    /* BARS is the row that paid for them: it is a four-level rounding of the
     * CSQ two rows above it, and the home screen draws it anyway. */
    CHECK(value_of(rows, n, "BARS") == NULL, "no BARS row with hardware");
}

/* The photograph this all started from: OPER --, REG UNKNOWN (CEREG 4),
 * CSQ 99, BARS 0/4. The old page stopped there and could not say which of
 * three very different faults it was looking at. */
static void test_the_no_signal_screen_is_now_diagnosable(void)
{
    nd_modem_status st;
    nd_modemapp_row rows[ND_MODEMAPP_MAX_ROWS];
    size_t n;

    no_signal(&st);
    n = api.radio_rows(&st, 0, rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 6, "six rows");
    CHECK_STR(value_of(rows, n, "OPER"), "--", "no operator");
    CHECK_STR(value_of(rows, n, "REG"), "UNKNOWN  (CEREG 4)", "the reported registration");
    CHECK_STR(value_of(rows, n, "CSQ"), "99 (no signal)", "the ambiguous row");
    /* The two that resolve it: the radio IS on, and the modem DID look. */
    CHECK_STR(value_of(rows, n, "RF"), "ON (CFUN 1)", "so it is not flight mode");
    CHECK_STR(value_of(rows, n, "RSRP"), "NO SERVICE", "and the RF path found nothing");

    /* The other half of the same screen: a radio switched off. Same CSQ, same
     * CEREG, and now it says so. */
    st.cfun = 4;
    n = api.radio_rows(&st, 0, rows, ND_ARRAY_LEN(rows));
    CHECK_STR(value_of(rows, n, "CSQ"), "99 (no signal)", "identical CSQ");
    CHECK_STR(value_of(rows, n, "RF"), "FLIGHT (CFUN 4)", "and a different diagnosis");
}

static void test_radio_rows_without_hardware(void)
{
    nd_modem_status st;
    nd_modemapp_row rows[ND_MODEMAPP_MAX_ROWS];
    size_t n;

    memset(&st, 0, sizeof st);
    st.hardware = false;
    st.signal_level = -1;
    st.csq_rssi = -1;
    st.reg_stat = -1;
    st.cfun = -1;
    st.rsrp_dbm10 = ND_MODEM_RSRP_NONE;
    st.call_secs = -1;
    (void)snprintf(st.probe_why, sizeof st.probe_why, "%s",
                   "no candidate AT ports (no ttyUSB* in /sys/class/tty)");

    n = api.radio_rows(&st, -1, rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 6, "six rows with no modem");

    /* RF and RSRP are absent: with no module there is nothing to have asked,
     * and two "--" rows would push PORTS and WHY off the page. */
    CHECK(value_of(rows, n, "RF") == NULL, "no RF row without hardware");
    CHECK(value_of(rows, n, "RSRP") == NULL, "no RSRP row without hardware");
    /* CALL is what paid for WHY here: there can be no call without a modem. */
    CHECK(value_of(rows, n, "CALL") == NULL, "no CALL row without hardware");
    CHECK(value_of(rows, n, "BARS") != NULL, "BARS survives without hardware");
    CHECK(value_of(rows, n, "PORTS") != NULL, "PORTS appears without hardware");

    /* THE ROW THAT WAS UNREACHABLE. PORTS filled the sixth slot and the
     * builder returned before it ever wrote this one.
     *
     * Shortened, because a probe reason runs to ND_MODEM_PROBE_WHY_MAX and the
     * full string ran off the right edge of the frame the moment the row
     * became reachable. */
    CHECK_STR(value_of(rows, n, "WHY"), "no candi..ass/tty)", "WHY is reachable now");
}

/* An empty probe_why still yields no WHY row -- the builder only writes one
 * when it has a reason, so the count drops to six rather than drawing "". */
static void test_no_why_row_without_a_reason(void)
{
    nd_modem_status st;
    nd_modemapp_row rows[ND_MODEMAPP_MAX_ROWS];
    size_t n;

    memset(&st, 0, sizeof st);
    st.signal_level = -1;
    st.csq_rssi = -1;
    st.reg_stat = -1;
    st.call_secs = -1;

    n = api.radio_rows(&st, -1, rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 5, "five rows when the probe gave no reason");
    CHECK(value_of(rows, n, "WHY") == NULL, "no empty WHY row");
}

/* Every early return honours `max`, including the two the new rows added. */
static void test_max_is_honoured_at_every_step(void)
{
    nd_modem_status st;
    nd_modemapp_row rows[ND_MODEMAPP_MAX_ROWS];
    size_t cap;

    live(&st);
    for (cap = 1u; cap <= ND_ARRAY_LEN(rows); cap++) {
        size_t n = api.radio_rows(&st, 3, rows, cap);

        CHECK(n <= cap, "never writes past max");
    }
    CHECK_INT(api.radio_rows(&st, 3, rows, 0u), 0, "max 0 writes nothing");
    CHECK_INT(api.radio_rows(NULL, 3, rows, ND_ARRAY_LEN(rows)), 0, "NULL status");
    CHECK_INT(api.radio_rows(&st, 3, NULL, ND_ARRAY_LEN(rows)), 0, "NULL rows");

    /* Truncated at three, RF is still the third row and CSQ has not been
     * reached -- the order holds under a short cap too. */
    CHECK_INT(api.radio_rows(&st, 3, rows, 3u), 3, "three rows");
    CHECK_STR(rows[2].label, "RF", "RF survives truncation at 3");
}

/* WHY SIX. content_bottom is ND_UI_H - ND_SOFTKEY_H = 145, not 175 -- the
 * softkey strip lives inside the 240x175 frame, not below it -- so the rows
 * own y=36..131 and six is the last count whose final row clears the
 * port/page line at bottom-14. */
static void test_six_rows_is_the_ceiling(void)
{
    const int32_t bottom = ND_UI_H - ND_SOFTKEY_H;
    int32_t line_h;

    CHECK_INT(bottom, 145, "content_bottom");

    line_h = api.line_h(bottom, 36, 6u);
    CHECK_INT(line_h, 15, "six rows over the 93 px available, exactly at the floor");
    CHECK(36 + 5 * line_h < bottom - 14, "the sixth row clears the bottom line");

    /* A seventh cannot buy itself room: the pitch is ALREADY at its floor, so
     * the extra row is drawn 15 px lower with nothing to give it. Its top
     * lands 5 px above the bottom line -- less than the line of type it is
     * about to draw -- so the two overlap. That is the check that would have
     * caught a seven-row layout without rendering it. */
    CHECK_INT(api.line_h(bottom, 36, 7u), 15, "a seventh row cannot buy pitch");
    CHECK_INT(36 + 6 * api.line_h(bottom, 36, 7u), 126, "and starts at y=126");
    CHECK(126 + line_h > bottom - 14, "which runs through the bottom line at 131");
}

static void test_reset_strings(void)
{
    /* Short: it shares the bottom line with the port and the page counter. */
    CHECK_STR(*api.reset_hint, "* = reset", "the hint");
    /* Says what it costs. The module comes back by itself. */
    CHECK(strstr(*api.ask_reset, "Reboot the modem?") != NULL, "the question");
    CHECK(strstr(*api.ask_reset, "20 s") != NULL, "and what it costs");
}

int main(void)
{
    void *h = sa_begin("Modem", "ndmodemapp");

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        fprintf(stderr, "test_modem_app: the app.so is missing a symbol\n");
        (void)dlclose(h);
        return 1;
    }

    RUN(test_rf_text);
    RUN(test_rsrp_text);
    RUN(test_radio_rows_with_hardware);
    RUN(test_the_no_signal_screen_is_now_diagnosable);
    RUN(test_radio_rows_without_hardware);
    RUN(test_no_why_row_without_a_reason);
    RUN(test_max_is_honoured_at_every_step);
    RUN(test_six_rows_is_the_ceiling);
    RUN(test_reset_strings);

    return sa_end(h, "test_modem_app");
}

/* test_modemapp.c -- the engineering Modem app's RADIO page, app id 9005.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. A REGISTERED PHONE IS UNTOUCHED. Its five rows come out character for
 *     character as before, including the DOUBLE space in "HOME  (CEREG 1)"
 *     and the 3GPP 27.007 dBm the CSQ row derives. Everything else here only
 *     happens on a phone that is not on the network.
 *
 *  2. SIX ROWS IS A CEILING, NOT A COUNT. The pitch floors at 15 px against a
 *     95 px content area, so a seventh row lands on the port line. Every one
 *     of the three shapes this page has -- registered, unregistered, no modem
 *     -- has to come in at six or under, and that is asserted for each.
 *
 *  3. THE WHY ROW FINALLY APPEARS. It was built seventh, behind five fixed
 *     rows and PORTS, so `n >= max` returned before it every time: the probe
 *     reason crosses the service wire to be drawn here and never was. It fits
 *     now because CALL -- a forced, unchangeable "IDLE" with no modem -- gives
 *     up its place.
 *
 *  4. SIM and CAUSE appear only with a modem present and not registered,
 *     where BARS gives up its place for the same reason: signal_level() is a
 *     hard 0 for any known non-registered stat, so the row is a constant.
 *     Each is skipped individually while its answer is still unknown, so a
 *     half-filled snapshot draws half of them and never a blank one.
 *
 *  5. +CPSI?'s RSRP LANDS IN THE CSQ ROW. "99 (no signal)" is what a SIM7600
 *     says about LTE it is measuring perfectly well, so a serving cell
 *     replaces it -- in the row already asking the question, because there is
 *     no room for another one.
 *
 *  6. The RSRP tenths. -1134 is "-113.4 dBm" and not "-113.-4 dBm", which is
 *     what integer division and remainder give for a negative number if the
 *     sign is not taken off the tenths by hand.
 */

#include <stdio.h>
#include <string.h>

#include "nd_app.h"
#include "nd_modem.h"

#include "smallapp_test.h"

#include "../../apps/Modem/modem_app.h"

static struct {
    size_t (*radio_rows)(const nd_modem_status *, int32_t, nd_modemapp_row *, size_t);
    const char *(*rsrp_text)(int32_t, char *, size_t);
    int32_t (*line_h)(int32_t, int32_t, size_t);
} api;

static bool api_open(void *h)
{
    *(void **)&api.radio_rows = sa_sym(h, "nd_modemapp_radio_rows");
    *(void **)&api.rsrp_text = sa_sym(h, "nd_modemapp_rsrp_text");
    *(void **)&api.line_h = sa_sym(h, "nd_modemapp_line_h");

    return api.radio_rows != NULL && api.rsrp_text != NULL && api.line_h != NULL;
}

/* ------------------------------------------------------------------ *
 * Snapshots
 * ------------------------------------------------------------------ */

/* A phone on the network: the five-row page, and the one every other case
 * here is a departure from. */
static nd_modem_status registered_snap(void)
{
    nd_modem_status st;

    memset(&st, 0, sizeof st);
    st.hardware = true;
    st.registered = true;
    st.reg_stat = 1;
    st.csq_rssi = 23;
    st.signal_level = 4;
    st.state = ND_CALL_IDLE;
    st.rsrp_dbm10 = ND_MODEM_RSRP_UNKNOWN;
    (void)nd_strlcpy(st.port, "/dev/ttyUSB2", sizeof st.port);
    (void)nd_strlcpy(st.operator_name, "Tello", sizeof st.operator_name);
    return st;
}

/* The phone this app was reported for: a modem that is plugged in, awake and
 * refused by the network. CEREG 3, CSQ 99, and every reason the modem gave
 * for it. */
static nd_modem_status denied_snap(void)
{
    nd_modem_status st = registered_snap();

    st.registered = false;
    st.reg_stat = 3;
    st.csq_rssi = 99;
    st.signal_level = 0;
    st.operator_name[0] = '\0';
    (void)nd_strlcpy(st.sim_state, "READY", sizeof st.sim_state);
    (void)nd_strlcpy(st.cell_mode, "LTE", sizeof st.cell_mode);
    (void)nd_strlcpy(st.reg_cause, "PLMN not allowed", sizeof st.reg_cause);
    st.rsrp_dbm10 = -1134;
    return st;
}

/* ------------------------------------------------------------------ *
 * 1. The registered page, unchanged
 * ------------------------------------------------------------------ */

static void test_registered_page_is_five_rows(void)
{
    nd_modem_status st = registered_snap();
    nd_modemapp_row rows[ND_MODEMAPP_MAX_ROWS];
    size_t n;

    memset(rows, 0, sizeof rows);
    n = api.radio_rows(&st, 4, rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 5, "a registered phone still draws exactly five rows");

    CHECK_STR(rows[0].label, "OPER", "row 0 label");
    CHECK_STR(rows[0].value, "Tello", "the carrier");
    CHECK_STR(rows[1].label, "REG", "row 1 label");
    CHECK_STR(rows[1].value, "HOME  (CEREG 1)", "REG keeps its double space");
    CHECK_STR(rows[2].label, "CSQ", "row 2 label");
    CHECK_STR(rows[2].value, "23/31  -67 dBm", "CSQ keeps the 27.007 mapping");
    CHECK_STR(rows[3].label, "BARS", "row 3 label");
    CHECK_STR(rows[3].value, "4/4", "bars come from the patched signal_level()");
    CHECK_STR(rows[4].label, "CALL", "row 4 label");
    CHECK_STR(rows[4].value, "IDLE", "the call state");
}

/* A registered phone never draws SIM or CAUSE, even with every field filled
 * in -- +CEER reports the LAST failure and would otherwise stand under a
 * healthy REG row accusing a working network of having refused the phone. */
static void test_registered_hides_the_diagnostics(void)
{
    nd_modem_status st = denied_snap();
    nd_modemapp_row rows[ND_MODEMAPP_MAX_ROWS];

    st.registered = true;
    st.reg_stat = 5;
    st.csq_rssi = 17;
    memset(rows, 0, sizeof rows);
    CHECK_INT(api.radio_rows(&st, 2, rows, ND_ARRAY_LEN(rows)), 5,
              "registration hides the diagnostic rows");
    CHECK_STR(rows[1].value, "ROAMING  (CEREG 5)", "REG names roaming");
    CHECK_STR(rows[3].label, "BARS", "and BARS keeps its place");
}

/* ------------------------------------------------------------------ *
 * 4. The rows that only exist when it is broken
 * ------------------------------------------------------------------ */

static void test_denied_page_says_why(void)
{
    nd_modem_status st = denied_snap();
    nd_modemapp_row rows[ND_MODEMAPP_MAX_ROWS];
    size_t n;

    memset(rows, 0, sizeof rows);
    n = api.radio_rows(&st, 0, rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 6, "four rows plus SIM and CAUSE, and not one more");

    CHECK_STR(rows[0].value, "--", "no carrier while the network says no");
    CHECK_STR(rows[1].value, "DENIED  (CEREG 3)", "REG names the refusal");
    CHECK_STR(rows[2].label, "CSQ", "the signal row");
    CHECK_STR(rows[2].value, "99  LTE -113.4 dBm", "+CPSI?'s cell instead of \"(no signal)\"");
    CHECK_STR(rows[3].label, "CALL", "BARS is gone; CALL moves up into its place");
    CHECK_STR(rows[4].label, "SIM", "then the SIM state");
    CHECK_STR(rows[4].value, "READY", "+CPIN?'s answer, so the SIM is not the problem");
    CHECK_STR(rows[5].label, "CAUSE", "and the network's own reason");
    CHECK_STR(rows[5].value, "PLMN not allowed", "+CEER, the answer to a CEREG 3");

    /* The prose rows are elided in the middle at 20 characters, the width of
     * the value column: what a longer +CEER or +CPIN answer carries at its
     * end is the half that names the fault. */
    (void)nd_strlcpy(st.reg_cause, "service option not subscribed", sizeof st.reg_cause);
    (void)nd_strlcpy(st.sim_state, "SIM not inserted -- check the tray", sizeof st.sim_state);
    memset(rows, 0, sizeof rows);
    (void)api.radio_rows(&st, 0, rows, ND_ARRAY_LEN(rows));
    CHECK_STR(rows[4].value, "SIM not i.. the tray", "a long SIM state keeps both ends");
    CHECK_STR(rows[5].value, "service o..ubscribed", "and so does a long cause");
}

/* An unregistered phone that has answered none of the diagnostic queries yet
 * draws the four rows and stops. */
static void test_unregistered_but_silent_adds_nothing(void)
{
    nd_modem_status st = denied_snap();
    nd_modemapp_row rows[ND_MODEMAPP_MAX_ROWS];

    st.sim_state[0] = '\0';
    st.cell_mode[0] = '\0';
    st.reg_cause[0] = '\0';
    st.rsrp_dbm10 = ND_MODEM_RSRP_UNKNOWN;

    memset(rows, 0, sizeof rows);
    CHECK_INT(api.radio_rows(&st, 0, rows, ND_ARRAY_LEN(rows)), 4,
              "no diagnostics answered yet, so no diagnostic rows");
    CHECK_STR(rows[2].value, "99 (no signal)", "and CSQ falls back to the old wording");
    CHECK_STR(rows[3].label, "CALL", "the last row is CALL");
}

static void test_each_diagnostic_row_is_optional(void)
{
    nd_modem_status st = denied_snap();
    nd_modemapp_row rows[ND_MODEMAPP_MAX_ROWS];

    /* No SIM answer yet: CAUSE still draws, and it moves up. */
    st.sim_state[0] = '\0';
    memset(rows, 0, sizeof rows);
    CHECK_INT(api.radio_rows(&st, 0, rows, ND_ARRAY_LEN(rows)), 5, "SIM row skipped");
    CHECK_STR(rows[4].label, "CAUSE", "CAUSE takes the empty SIM row's place");

    /* And +CEER before the modem has a failure to report. */
    st = denied_snap();
    st.reg_cause[0] = '\0';
    memset(rows, 0, sizeof rows);
    CHECK_INT(api.radio_rows(&st, 0, rows, ND_ARRAY_LEN(rows)), 5, "CAUSE row skipped");
    CHECK_STR(rows[4].label, "SIM", "the last row is SIM");
}

/* ------------------------------------------------------------------ *
 * 5. The CSQ row and the cell behind it
 * ------------------------------------------------------------------ */

static void test_csq_row_prefers_the_serving_cell(void)
{
    nd_modem_status st = denied_snap();
    nd_modemapp_row rows[ND_MODEMAPP_MAX_ROWS];

    /* A cell heard but not measured prints the mode alone rather than a mode
     * with an empty number after it. "NO SERVICE" is the reading. */
    (void)nd_strlcpy(st.cell_mode, "NO SERVICE", sizeof st.cell_mode);
    st.rsrp_dbm10 = ND_MODEM_RSRP_UNKNOWN;
    memset(rows, 0, sizeof rows);
    (void)api.radio_rows(&st, 0, rows, ND_ARRAY_LEN(rows));
    CHECK_STR(rows[2].value, "99  NO SERVICE", "no RSRP means no trailing number");

    /* An RSRP with no mode is not enough either: the number without the
     * technology it was measured on is not a reading anyone can use. */
    st = denied_snap();
    st.cell_mode[0] = '\0';
    memset(rows, 0, sizeof rows);
    (void)api.radio_rows(&st, 0, rows, ND_ARRAY_LEN(rows));
    CHECK_STR(rows[2].value, "99 (no signal)", "an RSRP with no mode falls back");

    /* A real RSSI is never overwritten -- +CSQ answering at all means it has
     * something to say, and 27.007's mapping is what this row has always
     * shown. */
    st = denied_snap();
    st.csq_rssi = 12;
    memset(rows, 0, sizeof rows);
    (void)api.radio_rows(&st, 0, rows, ND_ARRAY_LEN(rows));
    CHECK_STR(rows[2].value, "12/31  -89 dBm", "a real CSQ keeps the 27.007 mapping");

    /* And a registered phone gets the cell too: +CSQ answers 99 on LTE for
     * whole firmware families, which is the case this started from. */
    st = registered_snap();
    st.csq_rssi = 99;
    (void)nd_strlcpy(st.cell_mode, "LTE", sizeof st.cell_mode);
    st.rsrp_dbm10 = -856;
    memset(rows, 0, sizeof rows);
    (void)api.radio_rows(&st, 4, rows, ND_ARRAY_LEN(rows));
    CHECK_STR(rows[2].value, "99  LTE -85.6 dBm", "a registered phone shows its cell too");
}

/* ------------------------------------------------------------------ *
 * 3. The WHY row the seventh place used to eat
 * ------------------------------------------------------------------ */

static void test_no_hardware_page_reaches_the_why_row(void)
{
    nd_modem_status st;
    nd_modemapp_row rows[ND_MODEMAPP_MAX_ROWS];
    size_t n;

    memset(&st, 0, sizeof st);
    st.hardware = false;
    st.signal_level = -1;
    st.csq_rssi = -1;
    st.reg_stat = -1;
    st.call_secs = -1;
    (void)nd_strlcpy(st.probe_why, "/dev/ttyUSB2: Permission denied", sizeof st.probe_why);

    memset(rows, 0, sizeof rows);
    n = api.radio_rows(&st, -1, rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 6, "OPER, REG, CSQ, BARS, PORTS and -- at last -- WHY");
    CHECK_STR(rows[0].value, "--", "no carrier is known");
    CHECK_STR(rows[1].value, "--", "nor a registration");
    CHECK_STR(rows[2].value, "--", "nor a CSQ");
    CHECK_STR(rows[3].label, "BARS", "BARS stays: the capture hook still overrides it");
    CHECK_STR(rows[4].label, "PORTS", "CALL is gone; PORTS moves up into its place");
    CHECK_STR(rows[5].label, "WHY", "and the reason, which used to be truncated away");
    /* Elided in the middle, so the errno on the tail survives: it is the half
     * that names the fault, and a clip at the screen edge eats exactly it. */
    CHECK_STR(rows[5].value, "/dev/ttyU..on denied", "the port at one end, the errno at the other");

    /* A reason that fits is not touched. */
    (void)nd_strlcpy(st.probe_why, "no ttyUSB nodes", sizeof st.probe_why);
    memset(rows, 0, sizeof rows);
    (void)api.radio_rows(&st, -1, rows, ND_ARRAY_LEN(rows));
    CHECK_STR(rows[5].value, "no ttyUSB nodes", "a short reason is left alone");
    (void)nd_strlcpy(st.probe_why, "/dev/ttyUSB2: Permission denied", sizeof st.probe_why);

    /* Nothing to say about why: five rows, no empty sixth. */
    st.probe_why[0] = '\0';
    memset(rows, 0, sizeof rows);
    CHECK_INT(api.radio_rows(&st, -1, rows, ND_ARRAY_LEN(rows)), 5, "no reason, no WHY row");

    /* SIM and CAUSE describe a radio, and without hardware there isn't one --
     * a snapshot left over from before the modem was lost adds no rows. */
    (void)nd_strlcpy(st.sim_state, "READY", sizeof st.sim_state);
    (void)nd_strlcpy(st.reg_cause, "PLMN not allowed", sizeof st.reg_cause);
    memset(rows, 0, sizeof rows);
    CHECK_INT(api.radio_rows(&st, -1, rows, ND_ARRAY_LEN(rows)), 5,
              "a stale snapshot adds no rows without hardware");
}

/* ------------------------------------------------------------------ *
 * 2. Six is a ceiling
 * ------------------------------------------------------------------ */

static void test_no_shape_of_the_page_exceeds_six(void)
{
    nd_modemapp_row rows[ND_MODEMAPP_MAX_ROWS];
    nd_modem_status st;
    size_t reg_stat;

    /* Every reachable combination of the three axes the row set branches on
     * -- hardware, registration and which diagnostics have answered -- with
     * a live call in each, since CS voice on 2G/3G runs with CEREG at 4. */
    for (reg_stat = 0u; reg_stat < 7u; reg_stat++) {
        size_t fields;

        for (fields = 0u; fields < 8u; fields++) {
            size_t hw;

            for (hw = 0u; hw < 2u; hw++) {
                st = denied_snap();
                st.hardware = (hw != 0u);
                st.reg_stat = (int32_t)reg_stat;
                st.registered = (reg_stat == 1u || reg_stat == 5u);
                st.state = ND_CALL_CONNECTED;
                if ((fields & 1u) == 0u)
                    st.sim_state[0] = '\0';
                if ((fields & 2u) == 0u)
                    st.reg_cause[0] = '\0';
                if ((fields & 4u) == 0u)
                    st.cell_mode[0] = '\0';
                CHECK(api.radio_rows(&st, 0, rows, ND_ARRAY_LEN(rows)) <= ND_MODEMAPP_MAX_ROWS,
                      "no combination of state builds a seventh row");
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 * 6. The tenths
 * ------------------------------------------------------------------ */

static void test_rsrp_text(void)
{
    char buf[32];

    CHECK_STR(api.rsrp_text(-1134, buf, sizeof buf), "-113.4 dBm",
              "the tenths do not get their own minus sign");
    CHECK_STR(api.rsrp_text(-1130, buf, sizeof buf), "-113.0 dBm", "a whole number keeps its .0");
    CHECK_STR(api.rsrp_text(-856, buf, sizeof buf), "-85.6 dBm", "a good cell");
    CHECK_STR(api.rsrp_text(ND_MODEM_RSRP_UNKNOWN, buf, sizeof buf), "",
              "no reading prints nothing at all");

    /* Defensive: the app hands it a real buffer every time, but a zero-sized
     * one must not be written to. */
    CHECK_STR(api.rsrp_text(-1134, NULL, 0u), "", "no buffer, no write");
}

/* ------------------------------------------------------------------ *
 * The row pitch that decides all of it
 * ------------------------------------------------------------------ */

static void test_line_h_is_why_six(void)
{
    /* The real geometry: content bottom 145 on the 240x175 panel, rows from
     * y = 36. Five rows get 18 px; six are already on the 15 px floor, and a
     * seventh would be held there too -- 36 + 6 * 15 is 126, and the port
     * line is drawn at 131. That is the whole argument for the cap. */
    CHECK_INT(api.line_h(145, 36, 5u), 18, "five rows breathe");
    CHECK_INT(api.line_h(145, 36, 6u), 15, "six sit exactly on the floor");
    CHECK_INT(api.line_h(145, 36, 7u), 15, "and a seventh cannot be squeezed in under it");
    CHECK(36 + 5 * api.line_h(145, 36, 6u) + 14 <= 145 - 14, "the sixth row clears the port line");
    CHECK(36 + 6 * api.line_h(145, 36, 7u) + 14 > 145 - 14,
          "a seventh would not, which is what the cap is for");
}

static void test_null_safety(void)
{
    nd_modemapp_row rows[1];
    nd_modem_status st = registered_snap();

    CHECK_INT(api.radio_rows(NULL, 0, rows, ND_ARRAY_LEN(rows)), 0, "no status, no rows");
    CHECK_INT(api.radio_rows(&st, 0, NULL, 4u), 0, "no output buffer, no rows");
    CHECK_INT(api.radio_rows(&st, 0, rows, 0u), 0, "a zero cap writes nothing");
}

int main(void)
{
    void *h = sa_begin("Modem", "ndmodemapp");

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }

    RUN(test_registered_page_is_five_rows);
    RUN(test_registered_hides_the_diagnostics);
    RUN(test_denied_page_says_why);
    RUN(test_unregistered_but_silent_adds_nothing);
    RUN(test_each_diagnostic_row_is_optional);
    RUN(test_csq_row_prefers_the_serving_cell);
    RUN(test_no_hardware_page_reaches_the_why_row);
    RUN(test_no_shape_of_the_page_exceeds_six);
    RUN(test_rsrp_text);
    RUN(test_line_h_is_why_six);
    RUN(test_null_safety);

    return sa_end(h, "test_modemapp");
}

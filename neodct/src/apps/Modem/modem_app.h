/* modem_app.h -- what apps/Modem/main.c shows its unit test.
 *
 * The three row builders and the page furniture. main.py's own comment on the
 * split -- "kept drawing-free so they can be bench-tested" -- is the reason
 * these are not static.
 */

#ifndef ND_MODEM_APP_H_INCLUDED
#define ND_MODEM_APP_H_INCLUDED

#include "nd_modem.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* main.py: REFRESH_S = 1.0, PAGES = ("RADIO", "SIM", "DATA"). */
#define ND_MODEMAPP_REFRESH_S 1.0
#define ND_MODEMAPP_N_PAGES   3

typedef enum {
    ND_MODEMAPP_PAGE_RADIO = 0,
    ND_MODEMAPP_PAGE_SIM,
    ND_MODEMAPP_PAGE_DATA
} nd_modemapp_page;

extern const char *const nd_modemapp_pages[ND_MODEMAPP_N_PAGES];

/* SIX, AND SIX IS THE HARD CEILING -- the geometry decides it, not taste.
 *
 * nd_modemapp_line_h() floors the pitch at 15 px, rows start at y=36, and the
 * port/page line is drawn at content_bottom-14. content_bottom is
 * ND_UI_H - ND_SOFTKEY_H = 175 - 30 = 145, NOT 175 -- the softkey strip is
 * inside the 240x175 frame, not below it. So the rows own y=36..131 and the
 * pitch for six is exactly (145-36-16)/6 = 15. A seventh row lands at y=126
 * and draws straight through the bottom line; that is not arithmetic, it is
 * what golden/eng-modem.png showed the moment it was tried.
 *
 * So each of the two RADIO pages spends its six on what it can actually
 * answer, and each drops the one row that is dead in its own case:
 *
 *   with a modem     OPER REG RF CSQ RSRP CALL   -- no BARS: it is
 *                    nd_modem__bars(csq), a coarser view of a row already on
 *                    the page, and it is on the home screen anyway.
 *   with none        OPER REG CSQ BARS PORTS WHY -- no CALL: there can be no
 *                    call without a modem, while BARS is the sim override and
 *                    the only signal readout left.
 *
 * SIM has six; DATA has five.
 *
 * ============ AND THAT IS HOW THE WHY ROW BECAME REACHABLE ============
 *
 * The no-hardware path has always written PORTS and then WHY. PORTS took the
 * sixth slot, the `n >= max` under it returned, and WHY -- the row whose own
 * comment says it exists because "a phone in a pocket has no console" -- could
 * not be reached by any input. Dropping CALL is what makes room for it, which
 * is why golden/eng-modem.png trades one for the other. */
#define ND_MODEMAPP_MAX_ROWS  6
#define ND_MODEMAPP_LABEL_MAX 8
/* "PORTS" can list every ttyUSB node on the phone and the Python does not
 * shorten it, so this is the one value that can be long. */
#define ND_MODEMAPP_VALUE_MAX 128

typedef struct {
    char label[ND_MODEMAPP_LABEL_MAX];
    char value[ND_MODEMAPP_VALUE_MAX];
} nd_modemapp_row;

/* REG_NAMES. Index by +CEREG <stat>; anything outside 0..5 is str(stat), and
 * None (-1) is "--". */
#define ND_MODEMAPP_N_REG_NAMES 6
extern const char *const nd_modemapp_reg_names[ND_MODEMAPP_N_REG_NAMES];

/* snap["state"], which is ModemService's own string and NOT the label
 * nd_modem_call_status() computes -- that one re-derives RINGING/CALLING from
 * +CLCC and would print a different word for the same state. */
const char *nd_modemapp_state_name(nd_call_state st);

/* _radio_rows(modem). `bars` is what ui.modem.signal_level() answers, which
 * the capture harness overrides -- -1 for unknown, as everywhere else. */
size_t nd_modemapp_radio_rows(const nd_modem_status *st, int32_t bars, nd_modemapp_row *out,
                              size_t max);

/* _sim_rows(modem) for a phone with no modem: six "n/a (sim)" rows. The
 * hardware path lives in main.c because it is six AT transactions. */
size_t nd_modemapp_sim_rows_absent(nd_modemapp_row *out, size_t max);

/* _data_rows(). Reads the filesystem through modem_probe.h. */
size_t nd_modemapp_data_rows(nd_modemapp_row *out, size_t max);

/* max(15, (bottom - y - 16) // max(1, n_rows)). */
int32_t nd_modemapp_line_h(int32_t bottom, int32_t y, size_t n_rows);

/* ------------------------------------------------------------------ *
 * The two rows CSQ could not give
 * ------------------------------------------------------------------ *
 *
 * Split out and exported for the same reason the row builders are: they are
 * the whole diagnostic value of this screen and they are pure functions of a
 * snapshot, so the test can state every case in a table.
 */

/* +CFUN <fun> as a row value: "--", "ON (CFUN 1)", "OFF (CFUN 0)",
 * "FLIGHT (CFUN 4)", or "CFUN <n>" for anything else. The named three are the
 * ones that change what the screen means; the rest are printed rather than
 * guessed at. */
const char *nd_modemapp_rf_text(int32_t cfun, char *out, size_t out_sz);

/* +CPSI as a row value: "--" when nothing answered, the bare system mode when
 * the reply carried no RSRP ("NO SERVICE", "GSM"), else "-113.4 dBm LTE".
 *
 * The tenths are printed by hand because a value between -0.9 and 0 has a
 * whole part of 0 and would lose its sign to "%d.%d". No modem reports one --
 * LTE RSRP runs about -44 to -140 dBm -- but a formatter that is only correct
 * for the inputs somebody expected is how "99 (no signal)" became the only
 * thing this page could say. */
const char *nd_modemapp_rsrp_text(const nd_modem_status *st, char *out, size_t out_sz);

/* ------------------------------------------------------------------ *
 * The reset
 * ------------------------------------------------------------------ */

/* The WHY row's own shorten limit, NARROWER than the DATA page's 24.
 *
 * Values are drawn at x=70 on a 240 px screen, so the column is 170 px, and
 * at font_s that is about twenty characters -- 24 ran off the right edge the
 * first time the row was rendered. Shortening keeps the head AND the tail
 * precisely so the tail survives, so a limit that lets the tail clip anyway
 * defeats the point: "/sys/class/tty)" is the half of a probe reason that
 * says where to look. */
#define ND_MODEMAPP_WHY_LIMIT 18

/* main.c's dwell after a reset was sent, matching FuelGauge's quick-start. */
#define ND_MODEMAPP_FLASH_S 2.0

/* The bottom-line note on the RADIO page, and the two the reset replaces it
 * with. Short because it shares that line with the port and "1/3". */
extern const char *const nd_modemapp_reset_hint;
extern const char *const nd_modemapp_reset_sent;
extern const char *const nd_modemapp_reset_refused;

/* The confirmation. Same shape as Power's, for the same reason: it is a
 * reboot, it drops calls and data, and it must not happen on one keypress. */
extern const char *const nd_modemapp_ask_reset;

/* The bottom-left string: the port when there is hardware, "SIMULATION"
 * otherwise. */
#define ND_MODEMAPP_SIMULATION "SIMULATION"

/* The core did not answer. NOT the same thing as "there is no modem", and
 * drawing SIMULATION for it -- which is what this app used to do, because it
 * threw away nd_svc_modem_status()'s return value -- reports a working modem
 * as a missing one. See nd_modemapp_draw_page(). */
#define ND_MODEMAPP_NO_LINK "NO LINK TO CORE"

/* The dialog shown when the core has no ModemService at all. */
extern const char *const nd_modemapp_no_service_msg;

/* Draw one page. Exported so the test can render without the key loop.
 *
 * `linked` is nd_svc_modem_status()'s return value: false means the question
 * never reached the core, so `st` is the "nothing is known" snapshot and NOT
 * evidence about the modem. */
/* `note` is drawn CENTRED on the bottom line, between the port on the left and
 * the page counter on the right -- the reset hint, or what the last reset
 * did. NULL draws nothing, which is what every page but RADIO passes. */
void nd_modemapp_draw_page(nd_ui *ui, const nd_modem_status *st, bool linked, int32_t page,
                           const nd_modemapp_row *rows, size_t n_rows, const char *note);

#ifdef __cplusplus
}
#endif

#endif /* ND_MODEM_APP_H_INCLUDED */

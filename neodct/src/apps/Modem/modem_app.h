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

/* RADIO has six rows with no modem attached and five with one; SIM has six;
 * DATA has five. */
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
void nd_modemapp_draw_page(nd_ui *ui, const nd_modem_status *st, bool linked, int32_t page,
                           const nd_modemapp_row *rows, size_t n_rows);

#ifdef __cplusplus
}
#endif

#endif /* ND_MODEM_APP_H_INCLUDED */

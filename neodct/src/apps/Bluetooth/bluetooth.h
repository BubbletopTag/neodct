/* bluetooth.h -- what apps/Bluetooth/main.c shows its unit test.
 *
 * Everything that decides WHAT is on the screen is here and is pure; main.c
 * keeps only the key loop and the drawing. That is the same split
 * fuelgauge.h and modem's main.py describe, and it exists for the same
 * reason: a row builder can be bench-tested against a controller that is not
 * plugged in, and a draw call cannot.
 *
 * ============ THE ROW IS `left`/`right`, NOT `label`/`value` ============
 *
 * FuelGauge's row is a label at x=8 and a value at x=70. Two of the three
 * pages here are that shape; the scan list is not, and calling its fields
 * label and value would be a lie about which column is which.
 *
 *   Adapter and Self test   left  = the field name, drawn at x=8
 *                           right = the reading,    drawn at x=70
 *   Scan                    left  = the ADDRESS,    drawn at x=8
 *                           right = the class,      drawn at x=168
 *
 * The scan list is that way round because an address is 157 px in font_s and
 * a two-column layout at x=70 would put its last octet past the right edge.
 * At x=8 it ends at 165 and the class name follows it; a long class ("Audio/
 * Video", "Miscellaneous") is clipped by the canvas, and clipping the class
 * is fine where clipping the address would not be.
 */

#ifndef ND_BLUETOOTH_APP_H_INCLUDED
#define ND_BLUETOOTH_APP_H_INCLUDED

#include "nd_bt.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* App id 9007. 9002 went to MicTest; 9007 is the next free engineering
 * slot and sits after Downgrade (9006). */
#define ND_BTAPP_ID 9007

/* "4294967295 / 4294967295" is 23 characters and is the widest thing a cell
 * can hold; 32 leaves room without making the row struct large. */
#define ND_BTAPP_CELL_MAX 32

/* Six rows is what 93 px of content area holds at the 15 px floor the
 * FuelGauge pitch rule imposes, and the adapter page uses all six. */
#define ND_BTAPP_MAX_ROWS 6

/* The adapter readout refreshes at 1 Hz, as FuelGauge does. Anything faster
 * is an ioctl per frame for a reading that changes when a human presses a
 * key. */
#define ND_BTAPP_REFRESH_S 1.0

typedef struct {
    char left[ND_BTAPP_CELL_MAX];
    char right[ND_BTAPP_CELL_MAX];
} nd_btapp_row;

/* ------------------------------------------------------------------ *
 * The menu
 * ------------------------------------------------------------------ */

typedef enum {
    ND_BTAPP_MENU_ADAPTER = 0,
    ND_BTAPP_MENU_SCAN,
    ND_BTAPP_MENU_SELFTEST,
    ND_BTAPP_MENU_N
} nd_btapp_menu_item;

extern const char *const nd_btapp_menu[ND_BTAPP_MENU_N];

/* The two refusals. They are separate strings because they are separate
 * faults: no CONFIG_BT is a kernel that has to be rebuilt and reflashed,
 * no adapter is a dongle that is not plugged in or whose firmware did not
 * load. Telling a technician the wrong one costs an afternoon. */
extern const char *const nd_btapp_no_kernel_msg;
extern const char *const nd_btapp_no_adapter_msg;

/* ------------------------------------------------------------------ *
 * The adapter page
 * ------------------------------------------------------------------ */

/* PSCAN and ISCAN, spelled separately from the rest of the flags.
 *
 * They are pulled out of the STATE row because "UP RUNNING PSCAN ISCAN" is
 * 217 px in font_s and the value column has 170. Splitting them is not a
 * layout dodge though -- page scan and inquiry scan answer "can something
 * else find this phone", and the other flags answer "is the radio alive",
 * which are the two questions a bring-up screen is for.
 *
 * "none" when neither is set, because an empty cell reads as a bug. */
void nd_btapp_disc_str(uint32_t flags, char *out, size_t out_sz);

/* The six rows of the adapter readout. `have` false writes the single row a
 * screen with no controller shows, so the page never draws a blank grid. */
size_t nd_btapp_adapter_rows(const nd_bt_adapter *a, bool have, nd_btapp_row *out, size_t max);

/* The softkey text: what pressing it will DO, not what the radio is. An
 * adapter that is down offers "Radio On". */
const char *nd_btapp_power_softkey(uint32_t flags);

/* ------------------------------------------------------------------ *
 * The scan page
 * ------------------------------------------------------------------ */

/* One row per device found, address left and class right. `ok` false writes
 * one row carrying `error` instead -- strerror(errno) from the ioctl, the way
 * FuelGauge reports a failed i2c read. Zero devices with ok true is NOT an
 * error and gets its own row: the radio transmitted and nobody answered. */
size_t nd_btapp_scan_rows(const nd_bt_device *devs, size_t n, bool ok, const char *error,
                          nd_btapp_row *out, size_t max);

/* "5.1 s window" -- the bottom line, from the unit count the scan was run
 * with, so the screen says how long it actually listened rather than how long
 * it was meant to. */
void nd_btapp_window_str(uint8_t units, char *out, size_t out_sz);

/* ------------------------------------------------------------------ *
 * The self test
 * ------------------------------------------------------------------ */

typedef enum {
    ND_BTAPP_STEP_KERNEL = 0, /* socket(AF_BLUETOOTH) -- is CONFIG_BT on      */
    ND_BTAPP_STEP_ADAPTER,    /* HCIGETDEVLIST -- is a controller registered  */
    ND_BTAPP_STEP_ADDRESS,    /* BD_ADDR != 00:00:.. -- did the firmware load */
    ND_BTAPP_STEP_RADIO,      /* HCIDEVUP -- does the controller answer       */
    ND_BTAPP_STEP_SCAN,       /* HCIINQUIRY -- does it transmit and receive   */
    ND_BTAPP_STEP_N
} nd_btapp_step;

typedef enum { ND_BTAPP_PASS = 0, ND_BTAPP_FAIL, ND_BTAPP_SKIP } nd_btapp_verdict;

typedef struct {
    nd_btapp_step step;
    nd_btapp_verdict verdict;
    char detail[ND_BTAPP_CELL_MAX];
} nd_btapp_check;

/* "KERNEL", "ADAPTER", ... Never NULL. */
const char *nd_btapp_step_name(nd_btapp_step step);

/* "PASS" / "FAIL" / "--". Never NULL. */
const char *nd_btapp_verdict_str(nd_btapp_verdict v);

/* Runs the five checks in order and STOPS AT THE FIRST FAILURE, marking the
 * rest SKIP. A cascade of five failures all caused by one missing dongle
 * tells a technician less than one failure and four dashes.
 *
 * `do_scan` false stops after RADIO and marks SCAN skipped. That exists for
 * the unit test: the scan step brings the radio up and then blocks for five
 * seconds, and `make test` may not do either.
 *
 * Returns how many checks were written -- always ND_BTAPP_STEP_N unless the
 * caller's buffer is smaller. */
size_t nd_btapp_selftest(nd_btapp_check *out, size_t max, bool do_scan);

/* One row per check: the step name left, the verdict right. */
size_t nd_btapp_check_rows(const nd_btapp_check *checks, size_t n, nd_btapp_row *out, size_t max);

/* The detail of the first FAIL, for the bottom line -- "Operation not
 * permitted" is 215 px and has no column to live in. "" when everything
 * passed. Never NULL. */
const char *nd_btapp_first_failure(const nd_btapp_check *checks, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* ND_BLUETOOTH_APP_H_INCLUDED */

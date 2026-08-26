/* fuelgauge.h -- what apps/FuelGauge/main.c shows its unit test.
 *
 * The row builder is separated from the drawing for the reason main.py gives
 * for the same split in the Modem app: "kept drawing-free so they can be
 * bench-tested". A row is a label and a value; the app puts the label at
 * x=8 and the value at x=70 and does nothing else with either.
 */

#ifndef ND_FUELGAUGE_H_INCLUDED
#define ND_FUELGAUGE_H_INCLUDED

#include "nd_battery.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* main.py: REFRESH_S = 1.0. */
#define ND_FG_REFRESH_S 1.0

/* The flash line's dwell, `flash_until = time.monotonic() + 2.0`. */
#define ND_FG_FLASH_S 2.0

/* Six rows on the hardware path, one on either bail-out path. */
#define ND_FG_MAX_ROWS 6

/* "%.4f V  (0x%04X)" is 26 characters at four digits of volts; 48 leaves room
 * for a gauge that reads implausibly high without truncating the hex. */
#define ND_FG_VALUE_MAX 48
#define ND_FG_LABEL_MAX 8

typedef struct {
    char label[ND_FG_LABEL_MAX];
    char value[ND_FG_VALUE_MAX];
} nd_fg_row;

extern const char *const nd_fg_hw_required_msg;

/* The string _rows_from_snapshot() clips to 24 characters on the error path.
 * See the note above its use in main.c -- it is a CPython TypeError message,
 * and it is on golden/eng-fuelgauge.png. */
extern const char *const nd_fg_forced_hw_error;

/* _rows_from_snapshot(snap). `ok` is nd_battery_debug_snapshot()'s return:
 * false means the four register reads did not happen and the one ERROR row is
 * built from `error` instead. `smoothed_v` is the GAUGE row's average, and
 * `have_smoothed` false is Python's None, drawn as "--".
 *
 * Returns how many rows were written, never more than max. */
size_t nd_fg_rows(const nd_battery_snap *snap, bool ok, const char *error, bool have_smoothed,
                  double smoothed_v, nd_fg_row *out, size_t max);

/* The Python clips the error to 24 characters BY CHARACTER, not by byte. Every
 * message that can reach it is ASCII, so the two agree; exported so the test
 * can say so. */
#define ND_FG_ERROR_CLIP 24

/* max(15, (bottom - y - 16) // max(1, n_rows)) -- the row pitch. */
int32_t nd_fg_line_h(int32_t bottom, int32_t y, size_t n_rows);

#ifdef __cplusplus
}
#endif

#endif /* ND_FUELGAUGE_H_INCLUDED */

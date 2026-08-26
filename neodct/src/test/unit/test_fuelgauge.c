/* test_fuelgauge.c -- the FuelGauge engineering app, app id 9004.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. _rows_from_snapshot's six format strings are Python's, character for
 *     character -- including the DOUBLE space in "%.4f V  (0x%04X)" and in
 *     "%d/4  avg %s", the "%%" that becomes one per cent sign, and the
 *     leading sign "%+.2f" puts on a positive C-rate. Every one of them is
 *     white text at x=70 on a frame somebody reads numbers off, so they are
 *     asserted as strings rather than inferred from a digest.
 *
 *  2. A part with no CRATE register (0xFFFF, i.e. a 17043 or 17044) prints
 *     "n/a (17043/44)" and not a number. nd_battery.c turns that 0xFFFF into
 *     NaN, so this is also the test that NaN survives the struct.
 *
 *  3. The error row clips to 24 CHARACTERS, and the string it clips is the
 *     CPython TypeError text -- see the header comment in main.c and
 *     OPEN-QUESTIONS.md B-2. Those 24 characters are on the reference frame.
 *
 *  4. The row pitch: max(15, (bottom - y - 16) // n). Six rows over the 93 px
 *     available comes out at exactly 15, so the floor is doing nothing today
 *     and is the only thing that keeps a seventh row legible.
 *
 *  5. THE GOLDEN FRAME. eng-fuelgauge is the app's FIRST and only draw, with
 *     a battery that reports hardware (the capture hook) and has no fd (the
 *     truth). One frame, judged by the SHA-256 goldenframe.py compares.
 *
 *  6. WITHOUT the hook the app refuses to run, which is the behaviour that
 *     makes 5 interesting: the same binary, the same absent gauge, and a
 *     MessageDialog instead of a readout.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nd_app.h"
#include "nd_battery.h"
#include "nd_ui_sim.h"

#include "smallapp_test.h"

#include "../../apps/FuelGauge/fuelgauge.h"

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    size_t (*rows)(const nd_battery_snap *, bool, const char *, bool, double, nd_fg_row *, size_t);
    int32_t (*line_h)(int32_t, int32_t, size_t);
    const char *const *hw_required;
    const char *const *forced_hw_error;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&api.rows = sa_sym(h, "nd_fg_rows");
    *(void **)&api.line_h = sa_sym(h, "nd_fg_line_h");
    api.hw_required = dlsym(h, "nd_fg_hw_required_msg");
    api.forced_hw_error = dlsym(h, "nd_fg_forced_hw_error");

    return api.run != NULL && api.shutdown != NULL && api.rows != NULL && api.line_h != NULL &&
           api.hw_required != NULL && api.forced_hw_error != NULL;
}

/* ------------------------------------------------------------------ *
 * 1 and 2. The rows
 * ------------------------------------------------------------------ */

/* The values a MAX17048 on a healthy cell would actually return. */
static nd_battery_snap sample_snap(void)
{
    nd_battery_snap s;

    memset(&s, 0, sizeof s);
    s.raw_vcell = 0xC000u;
    s.vcell = (double)s.raw_vcell * ND_VCELL_LSB;
    s.raw_soc = 0x6100u;
    s.soc_percent = (double)s.raw_soc / 256.0;
    s.crate = 10.0 * ND_CRATE_LSB;
    s.level = 3;
    s.ic_version = 0x0012;
    s.raw_config = 0x971Cu;
    s.bus = 3;
    s.addr = 0x36;
    return s;
}

static void test_rows_hardware(void)
{
    nd_battery_snap s = sample_snap();
    nd_fg_row rows[ND_FG_MAX_ROWS];
    size_t n;

    memset(rows, 0, sizeof rows);
    n = api.rows(&s, true, "", true, 3.851, rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 6, "six rows on the hardware path");

    CHECK_STR(rows[0].label, "VCELL", "row 0 label");
    CHECK_STR(rows[0].value, "3.8400 V  (0xC000)", "VCELL keeps the double space");
    CHECK_STR(rows[1].label, "SOC", "row 1 label");
    CHECK_STR(rows[1].value, "97.00 %  (0x6100)", "SOC");
    CHECK_STR(rows[2].label, "CRATE", "row 2 label");
    CHECK_STR(rows[2].value, "+2.08 %/hr", "a positive C-rate keeps its sign");
    CHECK_STR(rows[3].label, "GAUGE", "row 3 label");
    CHECK_STR(rows[3].value, "3/4  avg 3.851 V", "GAUGE keeps the double space too");
    CHECK_STR(rows[4].label, "VER", "row 4 label");
    CHECK_STR(rows[4].value, "0x0012", "VER");
    CHECK_STR(rows[5].label, "CFG", "row 5 label");
    CHECK_STR(rows[5].value, "0x971C", "CFG");
}

static void test_rows_edge_values(void)
{
    nd_battery_snap s = sample_snap();
    nd_fg_row rows[ND_FG_MAX_ROWS];

    /* A discharging cell. -10 LSB is the signed reading of 0xFFF6. */
    s.crate = -10.0 * ND_CRATE_LSB;
    (void)api.rows(&s, true, "", true, 3.851, rows, ND_ARRAY_LEN(rows));
    CHECK_STR(rows[2].value, "-2.08 %/hr", "a negative C-rate");

    /* A 17043/44: no CRATE register at all. nd_battery.c writes NaN for the
     * 0xFFFF those parts return, and the app must print the note rather than
     * "nan %/hr". */
    s.crate = NAN;
    (void)api.rows(&s, true, "", true, 3.851, rows, ND_ARRAY_LEN(rows));
    CHECK_STR(rows[2].value, "n/a (17043/44)", "no CRATE register");

    /* Nothing has polled yet: _smoothed is None. */
    (void)api.rows(&s, true, "", false, 0.0, rows, ND_ARRAY_LEN(rows));
    CHECK_STR(rows[3].value, "3/4  avg --", "no average yet");
}

/* ------------------------------------------------------------------ *
 * 3. The error row
 * ------------------------------------------------------------------ */

static void test_error_row(void)
{
    nd_battery_snap s = sample_snap();
    nd_fg_row rows[ND_FG_MAX_ROWS];
    size_t n;

    CHECK_STR(*api.forced_hw_error, "no fuel gauge on the bus",
              "the CPython TypeError the capture reproduces");

    memset(rows, 0, sizeof rows);
    n = api.rows(&s, false, *api.forced_hw_error, true, 3.851, rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 1, "one row when the registers could not be read");
    CHECK_STR(rows[0].label, "ERROR", "the error row's label");
    /* Exactly str(exc)[:24]. This string is ON golden/eng-fuelgauge.png. */
    CHECK_STR(rows[0].value, "no fuel gauge on the bus", "fits inside the 24-char clip");
    CHECK_INT(strlen(rows[0].value), ND_FG_ERROR_CLIP, "24 characters, not 23 or 25");

    /* A reason shorter than the clip is not padded. */
    n = api.rows(&s, false, "No such device", true, 3.851, rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 1, "still one row");
    CHECK_STR(rows[0].value, "No such device", "a short reason survives whole");

    /* `if snap is None: rows = [("MODE", "SIMULATION")]` -- unreachable from
     * app_run(), which refuses to start without a battery, and ported anyway
     * because _draw_readout tests for it independently. */
    n = api.rows(NULL, false, "", false, 0.0, rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 1, "no snapshot at all is one row");
    CHECK_STR(rows[0].label, "MODE", "the MODE row");
    CHECK_STR(rows[0].value, "SIMULATION", "SIMULATION");
}

/* ------------------------------------------------------------------ *
 * 4. The pitch
 * ------------------------------------------------------------------ */

static void test_line_h(void)
{
    CHECK_INT(api.line_h(145, 36, 6), 15, "six rows over 93 px is 15 exactly");
    CHECK_INT(api.line_h(145, 36, 1), 93, "one row takes the whole space");
    CHECK_INT(api.line_h(145, 36, 7), 15, "seven rows would be 13, and the floor wins");
    CHECK_INT(api.line_h(145, 36, 0), 93, "max(1, len(rows)) -- no division by zero");
}

/* ------------------------------------------------------------------ *
 * 5 and 6. Running it
 * ------------------------------------------------------------------ */

static char saved_root[ND_PATH_MAX];

static bool root_to_overlay(void)
{
    char overlay[ND_PATH_MAX];

    (void)nd_strlcpy(saved_root, nd_path_root(), sizeof saved_root);
    if (!sa_overlay_root(overlay, sizeof overlay))
        return false;
    return nd_path_set_root(overlay) == ND_OK;
}

static void root_restore(void)
{
    (void)nd_path_set_root(saved_root[0] != '\0' ? saved_root : NULL);
}

static void test_golden_frame(void)
{
    sa_fixture fx;
    nd_battery *batt = NULL;
    int rc;

    if (!root_to_overlay()) {
        CHECK(false, "found the overlay");
        return;
    }
    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        root_restore();
        return;
    }
    /* A REAL BatteryService, in simulation mode -- exactly what nd_ui_init
     * hands the app inside nd-shoot. bus and addr fall back to the header's
     * defaults because no settings.prop names others, which is where
     * "i2c-3 @ 0x36" along the bottom of the frame comes from. */
    if (nd_battery_open(&batt, -1, -1) != ND_OK) {
        CHECK(false, "nd_battery_open");
        sa_fx_free(&fx);
        root_restore();
        return;
    }
    fx.ui.battery = batt;
    CHECK(!nd_battery_has_hardware(batt), "there is genuinely no gauge on this host");

    /* uistub.StubUI.simulate_status(battery=4, signal=4, carrier="Tello"),
     * whose battery.hardware = True is the ONLY reason this app draws a
     * readout instead of its refusal dialog. */
    nd_ui_sim_status(&fx.ui, 4, 4, "Tello");

    if (!sa_hold(&fx, ND_KEY_CLEAR)) {
        CHECK(false, "held key");
        nd_battery_close(batt);
        sa_fx_free(&fx);
        root_restore();
        return;
    }

    nd_vclock_enable();
    rc = api.run(&fx.ui);

    CHECK_INT(rc, 0, "Back returns 0");
    /* One draw and one only: the loop redraws on a 1.0 s timer and the
     * virtual clock advances 0.1 s per COMMITTED frame, so the second
     * iteration is never due -- which is why the Python's frames[-1] is its
     * first frame too. */
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 1, "one frame; the 1 Hz refresh never falls due");
    sa_expect_golden(&fx, nd_capture_recent(fx.cap, 0u), "eng-fuelgauge");

    nd_vclock_disable();
    nd_ui_sim_clear(&fx.ui);
    nd_battery_close(batt);
    sa_fx_free(&fx);
    root_restore();
}

static void test_refuses_without_the_hook(void)
{
    sa_fixture fx;
    nd_battery *batt = NULL;

    if (!root_to_overlay()) {
        CHECK(false, "found the overlay for the warning icon");
        return;
    }
    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        root_restore();
        return;
    }
    if (nd_battery_open(&batt, -1, -1) != ND_OK) {
        CHECK(false, "nd_battery_open");
        sa_fx_free(&fx);
        root_restore();
        return;
    }
    fx.ui.battery = batt;
    nd_ui_sim_clear(&fx.ui); /* no simulate_status this time */

    /* MessageDialog drains the channel first, so the key has to be held. */
    if (!sa_hold(&fx, ND_KEY_ENTER)) {
        CHECK(false, "held key");
        nd_battery_close(batt);
        sa_fx_free(&fx);
        root_restore();
        return;
    }

    nd_vclock_enable();
    CHECK_INT(api.run(&fx.ui), 0, "the refusal path still returns 0");
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 1, "one frame: the dialog, and no readout");
    /* The dialog fills every row, including the softkey strip a readout would
     * have left as "QStart". Row 0 of a readout is the "FuelGauge" title's
     * band; here the top-left corner is the warning triangle's transparent
     * margin, i.e. black. */
    CHECK(nd_image_get_px(fx.canvas, 120, 0).r == 0u, "no title bar was drawn");
    nd_vclock_disable();

    /* And with no battery at all -- an app process, where nd_ui.h says the
     * service handles are NULL. */
    fx.ui.battery = NULL;
    nd_vclock_enable();
    CHECK_INT(api.run(&fx.ui), 0, "a NULL battery refuses the same way");
    nd_vclock_disable();

    nd_battery_close(batt);
    sa_fx_free(&fx);
    root_restore();
}

static void test_strings(void)
{
    CHECK_STR(*api.hw_required,
              "No MAX1704x fuel gauge found, so BatteryService is running its QEMU "
              "simulation stub. This app needs real hardware.",
              "HW_REQUIRED_MSG");
    CHECK_DBL(ND_FG_REFRESH_S, 1.0, "REFRESH_S");
    CHECK_DBL(ND_FG_FLASH_S, 2.0, "the two-second flash dwell");
}

static void test_null_safety(void)
{
    nd_fg_row rows[1];

    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown();
    CHECK_INT(api.rows(NULL, true, NULL, false, 0.0, NULL, 0u), 0, "rows with no output buffer");
    CHECK_INT(api.rows(NULL, true, NULL, false, 0.0, rows, 0u), 0, "rows with a zero cap");
    sa_checks++;
}

int main(void)
{
    void *h = sa_begin("FuelGauge", "ndfuelgauge");

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }

    RUN(test_strings);
    RUN(test_rows_hardware);
    RUN(test_rows_edge_values);
    RUN(test_error_row);
    RUN(test_line_h);
    RUN(test_golden_frame);
    RUN(test_refuses_without_the_hook);
    RUN(test_null_safety);

    return sa_end(h, "test_fuelgauge");
}

/* apps/FuelGauge/main.c -- the MAX1704x's registers, once a second.
 *
 * A one-to-one port of System/engineering/apps/FuelGauge/main.py. App id
 * 9004, engineering menu, manifest name "FuelGauge". It prints what the gauge
 * chip actually said -- raw hex beside the scaled value -- so a bench supply
 * sweep can be read off the phone's own screen instead of a logic analyser.
 * The softkey issues a quick-start, which makes the chip re-seed its
 * state-of-charge estimate from VCELL.
 *
 * It reads through the system BatteryService rather than opening /dev/i2c-3
 * itself, "so it shares the already-open i2c handle and never fights the
 * OS-level battery polling" -- the Python's own comment, and the reason this
 * app takes ui->battery instead of a bus number.
 *
 * ============ WHY THE HARDWARE GATE IS THE SIM-AWARE READOUT ============
 *
 * main.py gates on `battery.hardware`, an ATTRIBUTE of the live service
 * object. uistub.StubUI.simulate_status() sets that attribute to True, which
 * is how golden/eng-fuelgauge.png exists at all: the capture host has no
 * gauge, and without the patch this app would draw its refusal dialog and the
 * reference frame would be a copy of widget-messagedialog.
 *
 * C cannot patch a field, so the override lives beside the UI and
 * nd_ui_status_battery_hardware() is the one place that consults it
 * (nd_ui_sim.h). Reading it here is not "the app using a test hook" -- it is
 * the C spelling of `battery.hardware`, and nd_battery_has_hardware() is the
 * C spelling of "is there really a chip", which the Python has no way to ask
 * separately. Below, the app needs BOTH, and that is the only place the two
 * can disagree.
 *
 * ============ THE ERROR ROW ON THE REFERENCE FRAME ============
 *
 * Follow the Python through the capture and it ends somewhere no C program
 * arrives on its own. hardware is True (patched) but `self.fd` is still None,
 * so debug_snapshot() gets past its own guard, builds the dict, and then
 * `os.write(None, bytes([REG_VCELL]))` raises
 *
 *     TypeError: 'NoneType' object cannot be interpreted as an integer
 *
 * which `except Exception as exc: snap["error"] = str(exc)` catches and
 * _rows_from_snapshot clips to 24 characters. Those 24 characters are on the
 * STORED reference frame, in white, at x=70.
 *
 * They are NOT reproduced here, and "port the bug too" (CODING-STANDARDS.md
 * 9.4) does not ask for them. That rule covers quirks the phone really has --
 * a rounding, an off-by-one, an odd sort order -- not a crash the capture
 * harness manufactured by forcing the Python into a state the phone cannot
 * reach. A Python traceback is a fact about the harness, not about this
 * device; compiling one into a C binary would make the OS lie about what it
 * is, and a technician reading it off a real handset would go looking for an
 * interpreter that is not running.
 *
 * So eng-fuelgauge is a `recut` frame: the reference records what the phone
 * does, not what the old harness made the Python say. Every other error path
 * in this file is unchanged and still reports strerror(errno), which is what
 * the Python shows for a real gauge that fails a read -- and what a
 * technician actually needs. OPEN-QUESTIONS.md B-2.
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nd_app.h"
#include "nd_battery.h"
#include "nd_draw.h"
#include "nd_keycodes.h"
#include "nd_svc.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_ui_sim.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

#include "fuelgauge.h"

#define FG_KEY_NAV  ND_KEY_ENTER
#define FG_KEY_BACK ND_KEY_BACK

const char *const nd_fg_hw_required_msg =
    "No MAX1704x fuel gauge found, so BatteryService is running its QEMU "
    "simulation stub. This app needs real hardware.";

/* Shown when the app has been told there is a gauge and the driver says there
 * is not. On the phone that cannot happen -- nd_battery_debug_snapshot()
 * refuses without hardware and the app draws its refusal dialog instead, never
 * reaching this row. Only the capture harness can arrange it, by forcing
 * `hardware` true over a driver that never opened a bus.
 *
 * The Python reaches the same impossible state and reports
 * "'NoneType' object cannot be interpreted as an integer" -- the text of a
 * TypeError from os.write(None, ...), caught by a bare `except Exception` and
 * clipped to 24 characters. Those characters are on the stored reference
 * frame, which is why this string was originally reproduced verbatim.
 *
 * That was wrong and it is deliberately not done any more. A Python traceback
 * is not a fact about this phone; it is a fact about a harness that put the
 * Python somewhere it cannot otherwise go. Compiling one into a C binary makes
 * the OS lie about what it is, and a technician reading it off a real handset
 * would be chasing an interpreter that is not running. eng-fuelgauge is a
 * `recut` frame instead: the reference now records what the phone does.
 *
 * Every OTHER error path here is unchanged and still reports strerror(errno),
 * which is what the Python shows for a real gauge that fails a read. */
const char *const nd_fg_forced_hw_error = "no fuel gauge on the bus";

/* ------------------------------------------------------------------ *
 * The rows -- _rows_from_snapshot(), kept drawing-free
 * ------------------------------------------------------------------ */

static void row_set(nd_fg_row *r, const char *label, const char *value)
{
    (void)nd_strlcpy(r->label, label, sizeof r->label);
    (void)nd_strlcpy(r->value, value, sizeof r->value);
}

size_t nd_fg_rows(const nd_battery_snap *snap, bool ok, const char *error, bool have_smoothed,
                  double smoothed_v, nd_fg_row *out, size_t max)
{
    char buf[ND_FG_VALUE_MAX];
    char avg[24];
    size_t n = 0u;

    if (out == NULL || max == 0u)
        return 0u;

    /* `if snap is None: rows = [("MODE", "SIMULATION")]`. Unreachable through
     * app_run(), which has already refused to start without a battery, and
     * kept because _draw_readout tests for it independently of run(). */
    if (snap == NULL) {
        row_set(&out[n++], "MODE", "SIMULATION");
        return n;
    }

    if (!ok) {
        /* snap["error"][:24]. Python slices CHARACTERS; every string that can
         * arrive here is ASCII, so a byte clip is the same clip. */
        char clipped[ND_FG_ERROR_CLIP + 1];

        (void)nd_strlcpy(clipped, error != NULL ? error : "", sizeof clipped);
        row_set(&out[n++], "ERROR", clipped);
        return n;
    }

    (void)snprintf(buf, sizeof buf, "%.4f V  (0x%04X)", snap->vcell, (unsigned)snap->raw_vcell);
    row_set(&out[n++], "VCELL", buf);
    if (n >= max)
        return n;

    (void)snprintf(buf, sizeof buf, "%.2f %%  (0x%04X)", snap->soc_percent, (unsigned)snap->raw_soc);
    row_set(&out[n++], "SOC", buf);
    if (n >= max)
        return n;

    /* `"n/a (17043/44)" if crate is None else "%+.2f %%/hr" % crate`. NaN is
     * this port's None -- the 17043 and 17044 have no CRATE register and
     * nd_battery.c writes NaN for the 0xFFFF they return. */
    if (isnan(snap->crate))
        row_set(&out[n++], "CRATE", "n/a (17043/44)");
    else {
        (void)snprintf(buf, sizeof buf, "%+.2f %%/hr", snap->crate);
        row_set(&out[n++], "CRATE", buf);
    }
    if (n >= max)
        return n;

    if (have_smoothed)
        (void)snprintf(avg, sizeof avg, "%.3f V", smoothed_v);
    else
        (void)nd_strlcpy(avg, "--", sizeof avg);
    (void)snprintf(buf, sizeof buf, "%d/4  avg %s", (int)snap->level, avg);
    row_set(&out[n++], "GAUGE", buf);
    if (n >= max)
        return n;

    (void)snprintf(buf, sizeof buf, "0x%04X", (unsigned)snap->ic_version);
    row_set(&out[n++], "VER", buf);
    if (n >= max)
        return n;

    (void)snprintf(buf, sizeof buf, "0x%04X", (unsigned)snap->raw_config);
    row_set(&out[n++], "CFG", buf);
    return n;
}

int32_t nd_fg_line_h(int32_t bottom, int32_t y, size_t n_rows)
{
    int32_t divisor = n_rows < 1u ? 1 : (int32_t)n_rows;
    int32_t h = (bottom - y - 16) / divisor;

    /* max(15, ...): six rows over 93 px is 15 exactly, so the floor is doing
     * nothing today and is the only thing keeping a seventh row legible. */
    return h < 15 ? 15 : h;
}

/* ------------------------------------------------------------------ *
 * _draw_readout
 * ------------------------------------------------------------------ */

static void draw_readout(nd_ui *ui, const nd_fg_row *rows, size_t n_rows, bool have_snap,
                         const nd_battery_snap *snap, const char *flash)
{
    int32_t screen_w = nd_ui_width(ui);
    int32_t bottom = nd_ui_content_bottom(ui);
    int32_t y = 36;
    int32_t line_h;
    size_t i;

    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, screen_w, bottom), ND_BLACK);
    (void)nd_draw_text(ui->draw, 5, 0, "FuelGauge", ui->font_xl, ND_WHITE);
    (void)nd_draw_line(ui->draw, 0, 30, screen_w, 30, ND_WHITE, 1);

    line_h = nd_fg_line_h(bottom, y, n_rows);
    for (i = 0u; i < n_rows; i++) {
        (void)nd_draw_text(ui->draw, 8, y, rows[i].label, ui->font_s, ND_GRAY);
        (void)nd_draw_text(ui->draw, 70, y, rows[i].value, ui->font_s, ND_WHITE);
        y += line_h;
    }

    /* Bottom status line: quick-start feedback left, static bus info right. */
    if (have_snap) {
        char bus_text[32];
        int32_t bw = 0;
        int32_t bh = 0;

        (void)snprintf(bus_text, sizeof bus_text, "i2c-%d @ 0x%02X", snap->bus,
                       (unsigned)snap->addr);
        nd_ui_text_size(ui, bus_text, ui->font_s, &bw, &bh);
        (void)nd_draw_text(ui->draw, screen_w - 5 - bw, bottom - 14, bus_text, ui->font_s, ND_GRAY);
    }
    if (flash != NULL && flash[0] != '\0')
        (void)nd_draw_text(ui->draw, 8, bottom - 14, flash, ui->font_s, ND_GRAY);
}

/* ------------------------------------------------------------------ *
 * run()
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    nd_softkey softkey;
    nd_fg_row rows[ND_FG_MAX_ROWS];
    const char *flash = "";
    double flash_until = 0.0;
    double last_draw = 0.0;

    if (ui == NULL || ui->draw == NULL || ui->canvas == NULL)
        return 1;

    /* `if battery is None or not battery.hardware`. Both halves, in this
     * order; see the header comment for why the second is the sim-aware one.
     *
     * The first half used to be `ui->battery == NULL`, which in an app
     * process is ALWAYS true (nd_app.h) -- so this app refused to run on a
     * phone with a gauge on the bus. nd_svc_battery_present() asks the core.
     * The second half is deliberately unchanged: nd_ui_status_battery_
     * hardware() is where the capture override lives, and it is what makes
     * golden/eng-fuelgauge.png exist at all. OPEN-QUESTIONS.md MSG-1. */
    if (!nd_svc_battery_present(ui) || !nd_ui_status_battery_hardware(ui)) {
        nd_msgdialog dlg;

        nd_msgdialog_init(&dlg, ui, nd_fg_hw_required_msg);
        (void)nd_msgdialog_show(&dlg);
        return 0;
    }

    nd_softkey_init(&softkey, ui, false);

    for (;;) {
        double now = nd_time_monotonic();
        int32_t key;

        if (flash[0] != '\0' && now >= flash_until) {
            flash = "";
            last_draw = 0.0;
        }
        if (now - last_draw >= ND_FG_REFRESH_S) {
            nd_svc_battery b;
            bool ok;
            int saved_errno;
            char reason[ND_FG_VALUE_MAX];
            size_t n_rows;

            /* main.py makes three separate reads here -- debug_snapshot(),
             * .hardware and .vcell -- and draws them on ONE frame. In the
             * core they are three calls into one live object and cannot
             * disagree; across a process boundary they would be three round
             * trips at three different instants. So they are one request,
             * and the errno rides in it explicitly because errno does not
             * cross a socket and the ERROR row is strerror() of it. The
             * order of the three reads is preserved inside that request --
             * see nd_svc.h. */
            ok = nd_svc_battery_read(ui, &b) && b.ok;
            saved_errno = (int)b.err;
            if (ok)
                reason[0] = '\0';
            else if (!b.hardware)
                (void)nd_strlcpy(reason, nd_fg_forced_hw_error, sizeof reason);
            else
                (void)nd_strlcpy(reason, strerror(saved_errno), sizeof reason);

            n_rows =
                nd_fg_rows(&b.snap, ok, reason, b.have_vcell, b.vcell, rows, ND_ARRAY_LEN(rows));
            draw_readout(ui, rows, n_rows, true, &b.snap, flash);
            nd_softkey_update(&softkey, "QStart", false);
            if (nd_ui_present(ui) != ND_OK)
                return 0;
            last_draw = now;
        }

        key = nd_ui_read_keypress(ui, 0.1);
        if (key == FG_KEY_BACK)
            return 0;
        if (key == FG_KEY_NAV) {
            bool ok = nd_svc_battery_quickstart(ui);

            flash = ok ? "quick-start sent" : "quick-start FAILED";
            /* main.py reads the clock a SECOND time here rather than reusing
             * `now`, so the two-second dwell starts from after the i2c write
             * rather than from before the draw. Kept. */
            flash_until = nd_time_monotonic() + ND_FG_FLASH_S;
            last_draw = 0.0;
        }
        if (nd_app_should_exit())
            return 0;
    }
}

void app_shutdown(void) {}

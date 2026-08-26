/* nd_ui_sim.h -- the five status readouts the home screen paints, and the
 * simulation hook the capture harness drives them with.
 *
 * ADDITION to the frozen header set (like nd_vclock.h and nd_capture.h). It
 * changes no declaration in nd_ui.h; it only names things nd_ui.c already had
 * to have.
 *
 * ============ WHY THE READOUTS ARE FUNCTIONS ============
 *
 * render_element() asks four questions of three services: what the battery
 * level is, whether there is a fuel gauge at all, how many signal bars there
 * are, and what the carrier is called. In Python those are attribute lookups
 * on live service objects. In C the services live only in the CORE -- an app
 * process gets NULL for all three (see nd_ui.h) -- so every read needs one
 * place that knows the fallback. These are that place, and they are what
 * nd_layout.c calls.
 *
 * The fallbacks are not invented. They are what the Python's own services
 * report with no hardware attached, which is the state `home-simulation.png`
 * was captured in:
 *
 *     battery level 3          BatteryService.__init__ sets _level = 3 and
 *                              nothing has polled yet
 *     battery hardware false   _probe_hardware() failed, so the "?" is drawn
 *     signal level -1          ModemService.signal_level() is None, so the
 *                              layout's sim_val (4) applies
 *     carrier ""               operator_display() is None, so the placeholder
 *                              "No Service" stands
 *
 * ============ THE SIMULATION HOOK ============
 *
 * nd_ui_sim_status() is uistub.StubUI.simulate_status(), which is how every
 * golden frame that shows a healthy phone was captured:
 *
 *     ui.stub.simulate_status(battery=4, signal=4, carrier="Tello")
 *
 * The Python does it by replacing two bound methods and one attribute on the
 * live service objects. C cannot monkey-patch, so the override is stored
 * beside the UI and consulted first. It is process-global, because there is
 * exactly one nd_ui per process.
 *
 * This is a TEST AND CAPTURE hook. Nothing on the phone calls it.
 */

#ifndef ND_UI_SIM_H_INCLUDED
#define ND_UI_SIM_H_INCLUDED

#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * The readouts
 * ------------------------------------------------------------------ */

/* battery.level() -- 0..4. */
int32_t nd_ui_status_battery_level(const nd_ui *ui);

/* battery.hardware -- false means the home screen draws the "?" label. */
bool nd_ui_status_battery_hardware(const nd_ui *ui);

/* modem.signal_level() -- -1 for "unknown", which is NOT the same as 0 bars:
 * unknown falls back to the layout's sim_val, zero draws an empty meter. */
int32_t nd_ui_status_signal_level(const nd_ui *ui);

/* modem.operator_display() -- "" when there is no registered network, in
 * which case the layout's "No Service" placeholder is left alone. Never NULL.
 * Owned by libneodct; valid until the next call. */
const char *nd_ui_status_carrier(const nd_ui *ui);

/* notify.active() -- true while an undismissed banner exists. */
bool nd_ui_status_notify_active(const nd_ui *ui);

/* notify.banner_lines() -- 0 or 2 lines into caller-owned buffers, returning
 * how many were written. */
size_t nd_ui_status_banner_lines(const nd_ui *ui, char l1[ND_NOTIFY_LINE_MAX],
                                 char l2[ND_NOTIFY_LINE_MAX]);

/* ------------------------------------------------------------------ *
 * The hook
 * ------------------------------------------------------------------ */

/* uistub.StubUI.simulate_status(). Pass carrier NULL or "" for no network.
 * Sets battery.hardware true, exactly as the stub does. */
void nd_ui_sim_status(nd_ui *ui, int32_t battery_level, int32_t signal_level, const char *carrier);

/* Pretend `count` unread messages arrived and the banner is up -- the state
 * `ui.notify.post_sms(1, tone=False); ui._unread_sms = 1` leaves behind.
 * Used only when no real nd_notify is attached. count 0 clears it. */
void nd_ui_sim_sms_banner(nd_ui *ui, int32_t count);

/* Drop every override. */
void nd_ui_sim_clear(nd_ui *ui);

#ifdef __cplusplus
}
#endif

#endif /* ND_UI_SIM_H_INCLUDED */

/* nd_svc.h -- the route from an app process to the core's services.
 *
 * ============ THE PROBLEM THIS EXISTS FOR ============
 *
 * nd_ui.h owns the modem, the battery and the notify service, and gives all
 * three to the CORE process only:
 *
 *     nd_modem   *modem;      -- NULL in an app
 *     nd_battery *battery;    -- NULL in an app
 *     nd_notify  *notify;     -- NULL in an app
 *
 * and nd_ui_init_app() says why: "those live in the core and an app that
 * needs them asks across the boundary." This header is the asking. Without
 * it Messages could not send a text at all (OPEN-QUESTIONS.md MSG-1), the
 * engineering Modem app always said "ModemService is not running." and
 * FuelGauge could not read the gauge.
 *
 * The full design -- transport, blocking, timeouts, what is validated -- is
 * docs/c-rewrite/spec-app-services.md. What a caller needs is here.
 *
 * ============ A DIRECT HANDLE ALWAYS WINS ============
 *
 * Every call below does the same thing first: if ui->modem (or ui->battery)
 * is non-NULL it calls the service directly and the socket is never touched.
 * That is not an optimisation, it is the compatibility guarantee. The core
 * has the handles; so does nd-shoot, which runs apps IN PROCESS to capture
 * the golden frames. Both therefore execute exactly the code they executed
 * before this header existed, and no reference frame can move.
 *
 * Only an app process -- handle NULL, NEODCT_SERVICE_FD present -- goes to
 * the wire.
 *
 * ============ FIVE OPERATIONS, AND DELIBERATELY NO MORE ============
 *
 * Send an SMS, snapshot the modem, snapshot the battery, quick-start the
 * gauge, reboot the module. nd_modem.h's dial, answer, HANGUP, fetch_sms,
 * read_stored_sms and raw send_at are NOT on this wire and must not be added
 * to it without the same argument being made again: an app must not be able
 * to hang up a live call by accident, and must not be able to type ATH at
 * the modem on purpose. See spec-app-services.md section 4.
 *
 * ============ WHY THE FIFTH ONE IS ALLOWED ============
 *
 * nd_svc_modem_reset() is a WRITE, and the fifth op, so it owes the same
 * argument the battery quick-start owed as the fourth:
 *
 *   - it carries NO ARGUMENT. The app cannot choose a string, so this is not
 *     a passthrough in disguise -- the AT command is a literal in lib/ and
 *     the wire record has nowhere to put an alternative. That is the line
 *     section 4 draws, and it is still drawn;
 *   - the core refuses it mid-call, on the modem thread, so the accident the
 *     section is written against -- an app dropping a live call -- cannot
 *     happen through it;
 *   - it is already what the phone does to itself. /etc/init.d/S45modem runs
 *     AT+CFUN=1,1 unprompted when a dial cycle fails, so this adds no
 *     capability to the system; it moves an existing recovery within reach of
 *     somebody holding the phone instead of a serial console;
 *   - it is reachable only from the engineering menu's Modem app, behind a
 *     confirmation, which is the same shape Power uses for a reboot.
 *
 * What it can do is interrupt data: the module goes away for twenty seconds
 * or so and wwan0 with it. That is the cost of the recovery it performs, it
 * is what the confirmation is asking about, and it is why this is not on a
 * softkey.
 *
 * ============ EVERY CALL CAN FAIL, INCLUDING THE TRANSPORT ============
 *
 * There are three distinguishable outcomes and the callers care about all
 * three:
 *
 *   present, and the operation succeeded   -> true
 *   present, and the operation failed      -> false, with the service's own
 *                                             reason in `detail` where there
 *                                             is one
 *   no route at all                        -> nd_svc_*_present() is false,
 *                                             which is what Messages turns
 *                                             into "ModemService is not
 *                                             running."
 *
 * A dead core is the second kind, not the third: the app has a channel, it
 * just did not get an answer. That keeps "the phone has no modem" and "the
 * send did not happen" as different sentences on screen.
 */

#ifndef ND_SVC_H_INCLUDED
#define ND_SVC_H_INCLUDED

#include "nd_battery.h"
#include "nd_modem.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every call takes a CONST context: none of them writes to the nd_ui. The
 * service handles it reads out of it (ui->modem, ui->battery) are pointers,
 * so a const struct still yields a usable nd_modem * -- which is why this
 * costs nothing and saves a cast at every call site in nd_ui.c.
 *
 * ------------------------------------------------------------------ *
 * Timings. All three are load-bearing; see spec-app-services.md 3.
 * ------------------------------------------------------------------ */

/* An app waiting for a short answer. The core replies in microseconds; this
 * covers a core that has merely been descheduled. */
#define ND_SVC_TIMEOUT_S 5.0

/* An app waiting for a send. It must sit ABOVE the core's own worst case --
 * ND_SMS_PROMPT_TIMEOUT_S (5) + ND_SMS_SEND_TIMEOUT_S (30) plus the AT+CMGF
 * that precedes them -- or it would abort sends that were about to succeed.
 * It must also be finite, so a wedged core cannot wedge the app. */
#define ND_SVC_SMS_TIMEOUT_S 45.0

/* An app waiting for a module reboot. Must sit above the core's own
 * ND_MODEM_RESET_TIMEOUT_S (10) with room for the request to queue behind an
 * AT transaction already on the port, or the app would give up on a reset
 * that is happening and report it as failed. */
#define ND_SVC_RESET_TIMEOUT_S 15.0

/* How long the core waits for its service thread after the app has gone.
 * Past this the thread is detached to finish its in-flight request and free
 * itself: there is no way to abort a CMGS already on the wire, and freezing
 * the phone to watch one finish is the failure this whole design avoids. */
#define ND_SVC_JOIN_S 2.0

/* The core's poll slice while it waits for a request. Only sets how quickly
 * a stopped thread notices; nothing observable depends on it. */
#define ND_SVC_POLL_S 0.2

/* ------------------------------------------------------------------ *
 * What the app calls
 * ------------------------------------------------------------------ */

/* Is there a modem reachable from here at all? True when this process owns
 * one, or when the core owns one and answered. False is the case Messages
 * and the Modem app render as "ModemService is not running." -- and it stays
 * false for a core that has no ModemService of its own, which is the same
 * sentence for the same reason. */
bool nd_svc_modem_present(const nd_ui *ui);

/* modem.send_sms(number, text) -> (ok, detail).
 *
 * detail is written on EVERY path, success included, and is rendered verbatim
 * by Messages as "Send failed: <detail>" -- so its wording is user-visible.
 * Where the modem had an opinion it is the modem's own ("no > prompt from
 * modem", "text mode rejected (...)", "simulated"); where the request never
 * reached a modem it is ours ("core service is gone", "no answer from the
 * core", "the core refused the request"). ND_MODEM_DETAIL_MAX is the width
 * nd_modem.h gives it and nothing here widens it. */
bool nd_svc_send_sms(const nd_ui *ui, const char *number, const char *text, char *detail,
                     size_t detail_sz);

/* modem.status_snapshot(). *out is always written -- on failure it is the
 * zeroed "nothing is known" snapshot nd_modem_status_snapshot() produces for
 * a NULL modem, so a caller that ignores the return still renders "--"
 * rather than uninitialised memory. */
bool nd_svc_modem_status(const nd_ui *ui, nd_modem_status *out);

/* modem.reset() -- AT+CFUN=1,1, the module reboot.
 *
 * True when the command went to the modem. False when there is no modem,
 * when the core has no ModemService, when the request never got an answer --
 * and, deliberately, WHEN A CALL IS UP: see nd_modem_reset(), which is where
 * that refusal is decided and where the call state cannot change under the
 * check. A caller cannot distinguish the refusals from each other and does
 * not need to; all of them mean the module was not rebooted.
 *
 * After a true the modem is GONE for twenty seconds or so and the next status
 * snapshots say SIMULATION until the core's probe re-adopts it -- possibly on
 * a different ttyUSB node. That is the modem re-enumerating, not a fault. */
bool nd_svc_modem_reset(const nd_ui *ui);

/* ------------------------------------------------------------------ *
 * The battery
 * ------------------------------------------------------------------ */

/* ONE round trip for what FuelGauge used to take three calls to get.
 *
 * That is not only cheaper. debug_snapshot(), has_hardware() and vcell() are
 * three different instants, and the app draws them on one frame; asking once
 * makes the frame consistent. errno is carried explicitly because errno does
 * not cross a socket and FuelGauge renders strerror() of it. */
typedef struct {
    bool ok;         /* nd_battery_debug_snapshot()'s return           */
    int32_t err;     /* the errno it left behind; 0 when ok            */
    bool hardware;   /* nd_battery_has_hardware()                      */
    bool have_vcell; /* nd_battery_vcell()'s return                    */
    double vcell;    /* the smoothed volts, valid only when have_vcell */
    int32_t level;   /* nd_battery_level(), 0..4                       */
    nd_battery_snap snap;
} nd_svc_battery;

/* As nd_svc_modem_present(), for the gauge. */
bool nd_svc_battery_present(const nd_ui *ui);

/* *out is zeroed (with snap.crate NaN, as nd_battery_debug_snapshot() leaves
 * it) before anything else, so a false return is still safe to render. */
bool nd_svc_battery_read(const nd_ui *ui, nd_svc_battery *out);

/* battery.quickstart(). The one WRITE on this channel: a single register on
 * one i2c device, which cannot reach the modem and cannot touch a call.
 * See spec-app-services.md section 4 for why it is here at all. */
bool nd_svc_battery_quickstart(const nd_ui *ui);

/* ------------------------------------------------------------------ *
 * The client half -- libneodct plumbing, not for apps
 * ------------------------------------------------------------------ *
 *
 * Process-global, following the precedent nd_ui.c already sets for g_sim and
 * g_ring_seen_at: there is exactly one nd_ui per process, so state with
 * nowhere to live in the frozen struct lives beside it rather than widening
 * it. nd_ui_init_app() opens it; nd_ui_teardown() closes it.
 */

/* Adopt NEODCT_SERVICE_FD if it is set and usable. Absent is not an error --
 * a hand-run nd-apprun, or nd-shoot, simply has no core to ask. Takes
 * ownership of the descriptor. */
void nd_svc_client_open_from_env(void);

/* Idempotent, and safe in a process that never opened one. */
void nd_svc_client_close(void);

/* Whether a channel is open. For tests and for logging; a caller deciding
 * what to draw wants nd_svc_modem_present() instead. */
bool nd_svc_client_active(void);

/* ------------------------------------------------------------------ *
 * The server half -- the core only
 * ------------------------------------------------------------------ *
 *
 * nd_proc_launch_app() drives all four in order and nothing else calls them.
 * The sequence matters and CODING-STANDARDS.md 1.1 is why:
 *
 *   1. nd_svc_server_open()        BEFORE the fork -- the socketpair has to
 *                                  exist to be listed in the descriptor plan
 *   2. fork() + execve()           nothing in between, as always
 *   3. nd_svc_server_start()       AFTER the fork -- so the thread does not
 *                                  exist at the instant the child is made
 *   4. nd_svc_server_stop()        after waitpid()
 *
 * nd_svc_server_free() releases a server that was opened but never started,
 * which is the error path in the launcher.
 */
typedef struct nd_svc_server nd_svc_server;

/* socketpair(AF_UNIX, SOCK_SEQPACKET). Both ends are O_CLOEXEC; the child's
 * has its flag cleared by the descriptor plan, exactly as the key channel
 * and the crash pipe do. Owned by the caller. */
nd_err nd_svc_server_open(nd_svc_server **out);

/* The descriptor to hand the child, or -1. */
int nd_svc_server_child_fd(const nd_svc_server *s);

/* Close our copy of the child's end and start the serving thread. `ui` must
 * outlive the server, which in the launcher it does. ND_ERR_IO when the
 * thread will not start, in which case the app simply runs without a service
 * channel -- a degraded app is better than a launch that failed. */
nd_err nd_svc_server_start(nd_svc_server *s, nd_ui *ui);

/* Stop, join within ND_SVC_JOIN_S, and free. Past the deadline the thread is
 * detached and frees itself; see the header of that constant. */
void nd_svc_server_stop(nd_svc_server *s);

/* Free a server that was never started. */
void nd_svc_server_free(nd_svc_server *s);

#ifdef __cplusplus
}
#endif

#endif /* ND_SVC_H_INCLUDED */

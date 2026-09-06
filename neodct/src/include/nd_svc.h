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
 * ============ FOUR SERVICE OPERATIONS, AND DELIBERATELY NO MORE ============
 *
 * Send an SMS, snapshot the modem, snapshot the battery, quick-start the
 * gauge. nd_modem.h's dial, answer, HANGUP, fetch_sms, read_stored_sms and
 * raw send_at are NOT on this wire and must not be added to it without the
 * same argument being made again: an app must not be able to hang up a live
 * call by accident, and must not be able to type ATH at the modem on
 * purpose. See spec-app-services.md section 4.
 *
 * ============ AND FOUR VERBS THAT ARE NOT SERVICES AT ALL ============
 *
 * nd_svc_reboot(), nd_svc_poweroff(), nd_svc_set_clock() and
 * nd_svc_format_card() are here for the opposite reason to the four above.
 * Those four ADDED something an app could not do; these four TAKE AWAY
 * something five apps could -- run as root.
 *
 * Power, Update and Downgrade resolved a program name along $PATH and
 * fork/exec'd it with the privilege to power-cycle the machine. Clock called
 * settimeofday(). Settings ran the SD-card helper. Those five calls were the
 * whole of ROOT_STOCK_APPS in nd_proc.c, and therefore the whole reason any
 * stock app ran with privilege the rest do not. With these four verbs the
 * list is empty and EVERY stock app runs as ndusr.
 *
 * THREE OF THE FOUR CARRY NO ARGUMENTS AT ALL, including the format -- the
 * core reads the card itself rather than being handed a device name. The
 * fourth carries a single bounded integer. That is not thrift: the moment the
 * app chooses the string, the core is doing something of the app's choosing
 * as root, which is the thing being removed. Section 4's rule -- make the
 * argument again -- applies to each; spec-app-services.md sections 9 and 10
 * are where it was made.
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

#include <time.h>

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

/* An app waiting for a card to be formatted. The old code did not wait a
 * bounded time at all -- Settings called nd_proc_wait(pid, -1.0) and blocked
 * until the helper was done -- so this is the finite version of "forever",
 * picked so it cannot fire while anything is still making progress: two
 * mkfs.vfat runs, a partition-table re-read and a sync(2) on the slowest card
 * the phone will accept. The core gives the helper ND_SVC_FORMAT_WAIT_S and
 * the app gives the core a little more, so that when a format really does
 * wedge, the side that KNOWS WHY is the side that gives up first and the user
 * gets "Formatting failed." rather than a silent timeout. */
#define ND_SVC_FORMAT_WAIT_S    240.0
#define ND_SVC_FORMAT_TIMEOUT_S 250.0

/* An app waiting for the card's layout to be restated after an install. A
 * chown and a chmod per file in apps/, which is metadata and quick on any
 * card; thirty seconds is a card that has stopped answering, not a slow one.
 * Same shape as the format's pair: the core gives up first. */
#define ND_SVC_LAYOUT_WAIT_S    30.0
#define ND_SVC_LAYOUT_TIMEOUT_S 40.0

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
 * Ending the session
 * ------------------------------------------------------------------ *
 *
 * ============ WHY THESE TWO TAKE NO nd_ui * ============
 *
 * Every call above takes a context because A DIRECT HANDLE ALWAYS WINS:
 * ui->modem decides whether the socket is touched at all. There is no ui->
 * handle for a halt -- the phone is not a service the core holds a pointer
 * to -- so the rule is the same sentence with the handle removed:
 *
 *     THE CORE DOES IT; AN APP ASKS THE CORE.
 *
 * and the predicate is "is there a channel". A context that went unused
 * would be a parameter lying about what the function reads.
 *
 * In an app process the channel is open and the request goes out. In any
 * process WITHOUT one -- nd-core itself, a hand-launched nd-apprun,
 * nd-shoot, a unit test -- there is no core to ask, so the resolve-and-spawn
 * happens right here, which is byte for byte what the three apps did before
 * this existed.
 *
 * ============ WHAT false MEANS ============
 *
 * The halt did not start. Three things produce it and Power draws the same
 * sentence for all three, honestly: no candidate exists on this image, the
 * core refused the record, or no answer came back. A true means the core has
 * committed -- it has already resolved the binary, and it syncs and spawns
 * after answering. There is nothing to un-ask.
 *
 * ============ THE ORDER THE CORE WORKS IN, WHICH IS THE DESIGN ============
 *
 *   1. validate           2. resolve the binary       3. SEND THE REPLY
 *   4. sync(2)            5. spawn
 *
 * Everything that can fail AND BE REPORTED happens before the reply;
 * everything that cannot be undone happens after it. That is why no new
 * timeout constant exists: ND_SVC_TIMEOUT_S covers steps 1-3, and step 4 --
 * the only unbounded one -- is on the far side of the answer. Syncing first
 * would let a tired flash time the app out at five seconds, draw "Reboot
 * failed." and then reboot underneath it. spec-app-services.md 9.4.
 */

/* reboot(8) / poweroff(8), performed by the core. */
bool nd_svc_reboot(void);
bool nd_svc_poweroff(void);

/* _HALT_COMMANDS and _REBOOT_COMMANDS, flattened, moved here out of the
 * Power and Update apps -- two shared objects that could not see each other
 * and therefore each carried a copy. Each entry is a NULL-terminated argv.
 *
 * THE ORDER IS THE PYTHON'S AND IS LOAD-BEARING: `poweroff` on the PATH and
 * `/sbin/poweroff` are different programs on an image that carries both. */
#define ND_SVC_HALT_CANDIDATES 3
extern const char *const *const nd_svc_poweroff_commands[ND_SVC_HALT_CANDIDATES];
extern const char *const *const nd_svc_reboot_commands[ND_SVC_HALT_CANDIDATES];

/* What subprocess.Popen(["name", ...]) resolves argv[0] to: execvp's rule,
 * which nd_proc_spawn() deliberately does not implement (it takes a path).
 * A name containing '/' is used as given; anything else is looked up along
 * $PATH, with false standing in for the OSError Python raises when nothing
 * executable is found. `out` is left empty on failure.
 *
 * NOT ND_ROOT-resolved -- it is an executable, and nd_proc.h is explicit
 * that executables are not. */
bool nd_svc_halt_which(const char *name, char *out, size_t out_sz);

/* ------------------------------------------------------------------ *
 * The last two: the clock, and formatting a card
 * ------------------------------------------------------------------ *
 *
 * These paid off the final two names in nd_proc.c's ROOT_STOCK_APPS -- Clock
 * and Settings -- which is what emptied it. They are a different shape from
 * REBOOT and POWEROFF, because those took no arguments and these have
 * something to say; section 4's rule applies to each, so the argument is made
 * again below. The working is spec-app-services.md section 10.
 *
 * ============ SETTING THE CLOCK ============
 *
 * The clock ALREADY moves without anybody asking. nd_clock_start() applies a
 * floor at boot and then syncs over SNTP on a detached thread, so "a process
 * decided what time it is" is the normal case and not a new power. What is
 * new is an app choosing the value.
 *
 * What that can and cannot reach was checked rather than assumed:
 *
 *   THE RELEASE SIGNATURE DOES NOT READ THE CLOCK. nd_signing.c has no
 *   notBefore, no notAfter and no time() call, and neither does the
 *   initramfs gate. So no clock buys an install of anything unsigned, which
 *   is the one outcome that would have settled this the other way.
 *
 *   TLS DOES. nd_clock.h's first paragraph is about exactly this: a clock in
 *   1970 fails every "not valid before" check. Forward is the dangerous
 *   direction -- far enough ahead and an EXPIRED certificate looks current,
 *   so a download could come from a server whose key was revoked. It still
 *   could not be installed: the .ndsw is signature-checked in the initramfs,
 *   after the download and by something the running system cannot rewrite.
 *
 * So the bound is not decoration. Below the build epoch is refused because
 * no legitimate time predates the image asking; ND_SVC_CLOCK_MAX_SKEW_S
 * ahead is refused because that is the direction that ages certificates out.
 * Between them the phone believes its owner, which is the whole point of a
 * clock app.
 *
 * ============ FORMATTING A CARD ============
 *
 * TAKES NO DEVICE, and that is the entire security design rather than an
 * ergonomic choice. Settings used to pass card->device to the helper; a verb
 * shaped that way would let any app name a block device, and the two most
 * interesting ones on this phone are the system partition and the user
 * partition. The core reads nd_storage_card() itself, so there is no string
 * to validate because there is no string.
 *
 * It also refuses a card that is not `removable`, which on QEMU is the
 * virtiofs share -- a directory on the developer's machine, and mkfs on it
 * is not a thing anybody meant to ask for.
 *
 * The argument for allowing it at all is that AN APP CAN ALREADY DESTROY THE
 * CARD'S CONTENTS. It is mounted uid=ndusr, so every file on it is one
 * unlink() away from any app on the phone. The verb adds "and rewrite the
 * partition table", which is the same loss by a faster route, not a new one.
 * What it does NOT add is reach: the format is confined to the one device
 * the core found, and no app can move it.
 */

/* How far ahead of the build epoch a hand-set clock may be. Ten years is far
 * more than a person correcting a date needs and far less than the
 * multi-decade jump that ages a long-lived CA certificate out. Behind the
 * build epoch there is no allowance at all: nothing legitimate predates the
 * image doing the asking. */
#define ND_SVC_CLOCK_MAX_SKEW_S ((time_t)(10 * 365 * 24 * 60 * 60))

/* nd_clock_set(), performed by the core, with the bound above applied first.
 *
 * false is refused-or-failed and the caller cannot tell which. That is
 * deliberate: Clock draws one sentence for both, because "the phone will not
 * believe that date" and "the phone could not write that date" are the same
 * instruction to the person holding it -- try a different one.
 *
 * THE BOUND IS APPLIED ON WHICHEVER SIDE RUNS THE OPERATION, so a process
 * with no channel -- nd-shoot, a hand-launched nd-apprun, a unit test -- gets
 * exactly the same answer as an app on the phone. A rule that only existed on
 * the far side of a socket would be a rule you could get out of by not having
 * one. */
bool nd_svc_set_clock(time_t when);

/* ------------------------------------------------------------------ *
 * Formatting a card: a JOB, not a call
 * ------------------------------------------------------------------ *
 *
 * ============ WHY THERE ARE THREE OF THESE ============
 *
 * There was one, and it blocked until the format finished. On QEMU that is
 * under a second, because the "card" is a file on the developer's disk. On the
 * phone it is a partition table, two mkfs runs, a BLKRRPART and a sync(2) on a
 * class-4 SD card -- tens of seconds to minutes -- and for the whole of it:
 *
 *   the core's serving thread was inside the wait,
 *   the BROKER (a serial, one-request-at-a-time server) was inside the wait,
 *   and the core's broker round-trip mutex was held by both.
 *
 * The core's UI thread takes that mutex on every turn of the loop that SCANS
 * THE I2C KEY MATRIX. The matrix has no kernel queue behind it, so a key
 * pressed while nobody is scanning is not delayed, it is LOST. The owner's
 * report was "trying to format the sdcard froze the system", and it was
 * literally true: no keys, no repaint, no cancel, no way back until the format
 * ended by itself.
 *
 * So: START the job, POLL it, CANCEL it. Every verb answers in microseconds
 * and nothing waits for anything. The screen that asked owns the waiting, and
 * can spend it drawing a progress bar and offering a way out.
 *
 * ============ WHAT A SCREEN DOES WITH THEM ============
 *
 *     if (!nd_svc_format_start()) { "Formatting failed." ; return; }
 *     for (;;) {
 *         double secs = 0.0;
 *         nd_svc_format_state st = nd_svc_format_poll(&secs);
 *
 *         if (st == ND_SVC_FORMAT_DONE_OK)   { ...; break; }
 *         if (st != ND_SVC_FORMAT_RUNNING)   { "Formatting failed."; break; }
 *         draw a bar from secs / ND_SVC_FORMAT_WAIT_S, pump one key;
 *         if (the key was CLEAR) { (void)nd_svc_format_cancel(); break; }
 *     }
 *
 * A poll every ND_SVC_FORMAT_POLL_S is plenty: mkfs reports no progress of its
 * own, so `secs` is the only thing that moves and a bar drawn from it is an
 * honest "how long this usually takes", not a measurement.
 */

/* Where the job is. */
typedef enum {
    ND_SVC_FORMAT_IDLE = 0,     /* nothing running and no verdict waiting */
    ND_SVC_FORMAT_RUNNING = 1,  /* the helper is still going              */
    ND_SVC_FORMAT_DONE_OK = 2,  /* it finished and the card was formatted */
    ND_SVC_FORMAT_DONE_FAIL = 3 /* it failed, was stopped, or wedged      */
} nd_svc_format_state;

/* How often a caller should poll. Below what anybody perceives, far above the
 * cost of the round trip. */
#define ND_SVC_FORMAT_POLL_S 0.2

/* Start one, on WHICHEVER CARD THE CORE ITSELF CAN SEE -- see above for why
 * there is no device parameter.
 *
 * false is "no format is running", and it covers a card that is absent, one
 * that is not removable, and a helper that could not be started. True while
 * one is already running: a second Format press must not lay a second mke2fs
 * over the first, so it joins the one in progress instead. */
bool nd_svc_format_start(void);

/* One non-blocking look. `elapsed_s` (may be NULL) receives how long the job
 * has been running, or how long the finished one took.
 *
 * A DONE verdict is handed out ONCE and the job then reads IDLE. A verdict
 * that latched would outlive the screen that asked for it and be shown to the
 * next one -- which, on a phone where the format outlives the app that started
 * it, is a thing that would actually happen. */
nd_svc_format_state nd_svc_format_poll(double *elapsed_s);

/* Stop it: SIGTERM, then SIGKILL, both through the broker, which is the only
 * process that can signal a root helper it forked. True when nothing is
 * running afterwards, which is also the answer when nothing was.
 *
 * The card is left in whatever state the mkfs had reached, which is why this
 * is a deliberate act by the owner and not something a screen does on its way
 * out. spec-app-services.md 10.3. */
bool nd_svc_format_cancel(void);

/* Start it and wait, in slices, for a caller that has nothing to draw.
 *
 * false is "the card was not formatted", and it covers a card that is absent,
 * one that is not removable, a helper that is missing, and a helper that ran
 * and failed. The waiting happens in THIS process now, so the core, the broker
 * and the UI thread are free the whole time -- but this call still ties up the
 * caller, so a screen with a progress bar to draw wants the three verbs above
 * instead. Aborts and cancels the job if nd_app_should_exit() goes true, which
 * is the incoming-call teardown in nd_app.h. */
bool nd_svc_format_card(void);

/* neodct-sdcard layout, performed by the core: restate the ownership and
 * modes of everything on the card, which is what a mount does and what an
 * install needs done again. Settings asks for this after unpacking a .nap
 * (nd_nap.h), because the one thing ndusr cannot do to the app it just
 * unpacked is make its data/ belong to ndusr_ut.
 *
 * No argument, for the reason the format has none: the helper lays out the
 * card at the phone's own mountpoint and decides for itself whether the card
 * is one of ours. false covers no card, a card that is not a NeoDCT card,
 * and a helper that failed -- and the app it was asked for is installed and
 * in the menu either way; it just cannot save anything until the card is
 * next mounted, which Settings says. */
bool nd_svc_layout_card(void);

/* ------------------------------------------------------------------ *
 * The halt simulation -- TESTS ONLY, in the sense nd_ui_sim.h means it
 * ------------------------------------------------------------------ *
 *
 * power.h already says the honest thing: a test suite that can switch off
 * the machine it is running on is not a test suite. So the CONSEQUENCE is
 * injected out and everything else is real -- validation, resolution, the
 * reply, and the sync(2) all still happen, which is what lets a test prove
 * the ordering above rather than assert it.
 *
 * `spawn` replaces step 5 and nothing else. `poweroff`/`reboot`/`n` replace
 * the tables, which is the only way to reach the "nothing resolved" branch
 * on a host where /sbin/poweroff really does exist. Any of them may be NULL.
 *
 * Set it BEFORE nd_svc_server_start() and clear it AFTER
 * nd_svc_server_stop(): pthread_create() and pthread_join() are the barriers
 * that make a plain global safe to read from the serving thread. Nothing on
 * the phone calls this. An app may -- it links the same library -- and gains
 * nothing by it: the hook it would set is the one in its own address space,
 * on the branch an app process never takes. */
typedef struct {
    void (*spawn)(bool reboot, const char *exe, void *user);
    const char *const *const *poweroff;
    const char *const *const *reboot;
    size_t n;
    void *user;
} nd_svc_halt_sim;

/* NULL clears it. */
void nd_svc_halt_simulate(const nd_svc_halt_sim *sim);

/* Whether THIS process may spawn a real poweroff or reboot.
 *
 * nd_svc_poweroff() resolves "poweroff" along $PATH and spawns it, and a
 * process with no service channel IS the core as far as this header can tell
 * -- so any test that reaches the halt without a simulation installed
 * switches off the machine running the suite. That is not hypothetical:
 * test/apps/SvcApp calls nd_svc_poweroff() for real and is safe only while
 * its parent installed a fake first. On 2026-08-31 it took a developer's
 * workstation down mid-build, and on 2026-09-04 it did so again through a
 * test binary run by hand, with no NEODCT_ROOT in its environment -- which is
 * why one environment variable is no longer the whole interlock.
 *
 * Four rules, in order (nd_svc_halt_allowed_for() is the table):
 *
 *   1. A simulation is installed -> allowed. nd_svc_halt_simulate() means the
 *      spawn cannot reach a real binary, and it is how the halt path is
 *      covered at all.
 *   2. This process was disarmed -> refused. nd_svc_halt_disarm() is sticky,
 *      and test/harness/nd_testguard.c calls it from a constructor linked
 *      into every test binary, so a test cannot forget to.
 *   3. NEODCT_ROOT is set -> refused. `make test` sets it for every binary
 *      and every app they launch, and nothing on the phone ever does.
 *   4. Otherwise only ROOT may. A non-root process asking to SPAWN a real
 *      halt is a test or a bug, and on a desktop it is the person at the
 *      keyboard, whom logind will happily oblige.
 *
 * Refusing surfaces as "this image has no halt binary", which is already the
 * one failure an app is told about; the log line names the rule.
 *
 * ============ AND THE QUESTION THIS TABLE IS NOT ============
 *
 * "May I spawn a halt HERE" is not "may this phone be switched off". Rule 4
 * was written as though it were, and every real phone lost its power button
 * for it:
 *
 *   Power (ndusr) -> SVC_OP_POWEROFF -> the CORE's serving thread, which since
 *   0.5.0b is also ndusr -> halt_resolve() -> rule 4 -> refused, before the
 *   broker was ever asked. nd_broker_halt() sits behind `if (halt.pending)`,
 *   which a refusal never sets, so the one process that still has CAP_SYS_BOOT
 *   never heard about it. The panel said "Power off failed." on every boot of
 *   every phone, and the serial log blamed the image for having no poweroff.
 *
 * The core is not the process that spawns the halt any more, so the gate does
 * not belong where the core decides. It belongs where the spawn happens, which
 * is the broker -- and the broker is root, so rule 4 passes there honestly.
 *
 * The core therefore asks a SECOND question, below: may I hand this to the
 * broker? Its rows are the same rows in the same order, with "there is a
 * broker to ask" in place of "this process is root". The interlock is not
 * weakened by it: rows 2 and 3 still refuse, so a disarmed test binary and
 * anything under NEODCT_ROOT cannot delegate a real halt either -- and the
 * broker, forked from that same test binary, has inherited the same latch and
 * would refuse a second time. What the test suite gives up is only the ability
 * to say "nothing was RESOLVED"; what it keeps is "nothing was SPAWNED", which
 * is the property the two workstation shutdowns were about. */
bool nd_svc_halt_allowed(void);

/* The rule itself, every input a parameter, so a test can cover the whole
 * table without being root or on a phone.
 *
 * READ IT AS: may THIS process fork and exec a real poweroff. That is a
 * narrower question than "may the phone be halted" -- see above. */
bool nd_svc_halt_allowed_for(bool sim_installed, bool disarmed, bool in_test_root, bool is_root);

/* The delegation question: may this process ask the root broker to halt on its
 * behalf? True on a phone, whose core is ndusr and whose broker is root.
 *
 * `have_broker` is nd_broker_default() != NULL, which nd-core sets only after
 * a successful drop -- so on a root core with no broker this is false and the
 * spawn table above answers instead, exactly as it always did. */
bool nd_svc_halt_delegate_allowed(void);
bool nd_svc_halt_delegate_allowed_for(bool sim_installed, bool disarmed, bool in_test_root,
                                      bool have_broker);

/* Rule 2. Sticky: once called, this process never resolves a real halt.
 * Production never calls it; the test harness does, before main(). */
void nd_svc_halt_disarm(void);

/* ------------------------------------------------------------------ *
 * The format simulation -- TESTS ONLY, for the same reason
 * ------------------------------------------------------------------ *
 *
 * A test suite that can repartition the machine it is running on is not a
 * test suite either, and this one is worse than the halt: poweroff at least
 * announces itself. So `run` replaces the spawn-and-wait and NOTHING ELSE --
 * the validation, the nd_storage_card() read, the absent and non-removable
 * refusals and the logging are all real, which is what lets a test prove that
 * the core picks the device rather than assert it.
 *
 * `run` is handed the device the CORE resolved, so a test can assert on it,
 * and returns what the helper would have: 0 for success.
 *
 * The clock verb needs no partner to this. nd_clock_set() has honoured
 * NEODCT_ROOT since it was written -- "leaving the real clock alone" -- so a
 * test already runs the whole path, bound included, without moving the build
 * machine's clock.
 *
 * Same barriers as the halt hook: set it before nd_svc_server_start(), clear
 * it after nd_svc_server_stop(). */
typedef struct {
    int (*run)(const char *device, void *user);
    void *user;
} nd_svc_format_sim;

/* NULL clears it. */
void nd_svc_format_simulate(const nd_svc_format_sim *sim);

/* The layout's partner to the format simulation: `run` replaces the
 * spawn-and-wait of `neodct-sdcard layout` and nothing else. The absent-card
 * refusal in front of it is real. Same barriers. */
typedef struct {
    int (*run)(void *user);
    void *user;
} nd_svc_layout_sim;

/* NULL clears it. */
void nd_svc_layout_simulate(const nd_svc_layout_sim *sim);

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

/* ------------------------------------------------------------------ *
 * WHICH VERBS A PARTICULAR SOCKET CARRIES
 * ------------------------------------------------------------------ *
 *
 * ============ WHY THERE IS A MASK AT ALL ============
 *
 * The socket used to be all-or-nothing, and an untrusted app got nothing:
 * nd_proc_launch_app() called nd_svc_server_open() only when the app was
 * trusted, so an app the owner installed from a card had no channel and
 * every nd_svc_* call in it answered "not present". It could not read the
 * battery. It could not read the clock. It could not tell whether the phone
 * had signal. Those are three things a wallpaper clock or a game pause
 * screen wants and none of them costs the owner anything.
 *
 * All-or-nothing was right while the only untrusted thing was the browser,
 * where the socket is a straight line from a web page to a root thread that
 * will send an SMS. It stopped being right when an installed app became
 * untrusted too, because it answered a question nobody had asked: an
 * installed app does not need SEND_SMS, and refusing SEND_SMS is not a
 * reason to refuse BATTERY.
 *
 * ============ AND WHY IT DOES NOT WEAKEN "VALIDATE THE RECORD" ============
 *
 * spec-app-services.md 5 is blunt that this wire has no peer credential
 * check: no SO_PEERCRED, no SCM_CREDENTIALS, nothing. So nothing a SENDER
 * says about itself can be believed, and a mask asserted in the request would
 * be worthless.
 *
 * This mask is not in the request. It is bound to the SOCKET at creation, by
 * the core, before the fork -- so it is exactly as unforgeable as the
 * socket's existence already is, which is the property the withholding
 * relied on in the first place. A child cannot widen it, and neither can
 * anything the child execs, because there is nothing on the wire to widen.
 *
 * The refusal happens BEFORE dispatch, not inside each verb, so a verb added
 * later is refused to untrusted callers until somebody deliberately adds its
 * bit here.
 *
 * ============ WHAT IS DELIBERATELY NOT IN THE UNTRUSTED SET ============
 *
 * The bit numbers are the wire op numbers, which live in nd_svc.c because
 * they are the wire and not the API; nd_svc.c carries a _Static_assert that
 * these two constants still name the operations this comment says they do.
 *
 *   SEND_SMS            costs the owner money, and is the verb the whole
 *                       withholding was written for.
 *   REBOOT, POWEROFF    turn the phone off. An app the owner installed from
 *                       a card must not be able to end a call that way.
 *   SET_CLOCK           the clock gates every TLS "not valid before" check
 *                       and the update system's signature check.
 *   LAYOUT_CARD,        they rewrite the owner's storage. FORMAT_START
 *   FORMAT_*            erases it.
 *   BATTERY_QUICKSTART  it looks like a read and it is not: it WRITES to the
 *                       fuel gauge, telling it to re-learn the cell from
 *                       scratch. An app that called it in a loop would leave
 *                       the owner with a battery meter that lies.
 *
 * What is left is the pair that only ever reads: the modem snapshot (signal
 * bars, registration, whether a call is up) and the battery snapshot. */
#define ND_SVC_OPS_ALL 0xFFFFFFFFu

/* SVC_OP_MODEM_STATUS (2) and SVC_OP_BATTERY (3). */
#define ND_SVC_OPS_UNTRUSTED ((1u << 2) | (1u << 3))

/* socketpair(AF_UNIX, SOCK_SEQPACKET). Both ends are O_CLOEXEC; the child's
 * has its flag cleared by the descriptor plan, exactly as the key channel
 * and the crash pipe do. Owned by the caller.
 *
 * `allowed_ops` is the mask above. Anything outside it is answered
 * SVC_ST_NO_SERVICE before it is dispatched -- the same answer a core with
 * no modem gives, so an app needs no new branch and draws what it already
 * drew when the service was absent. */
nd_err nd_svc_server_open(nd_svc_server **out, uint32_t allowed_ops);

/* The mask test on its own, exposed for the same reason nd_broker.h exposes
 * its two policy decisions: what a test can reach is what gets tested, and a
 * test that has to stand up a server, fork a child and drive a socket to ask
 * "is POWEROFF in the untrusted set" can only cover the cases the host
 * happens to support. `op` is a wire op number; anything at or above 32 has
 * no bit and is refused. */
bool nd_svc__op_allowed(uint32_t allowed_ops, uint32_t op);

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

/* Resolve and run poweroff/reboot with THIS process's privilege. The core
 * decides; the broker performs, because /sbin/poweroff needs CAP_SYS_BOOT and
 * an unprivileged nd-core has none. See nd_broker.h. */
/* Say that this process is an APP, not the core.
 *
 * Every verb here works on both sides of the boundary, and the test for which
 * side used to be "do I have a socket?". An untrusted app has none BECAUSE it
 * was refused one, so it answered that question as though it were the core and
 * tried to act locally. Called once by nd_ui_init_app(); the core never calls
 * it. See the comment on g_is_app_process. */
void nd_svc_mark_app_process(void);

bool nd_svc_halt_now(bool reboot);

#endif /* ND_SVC_H_INCLUDED */

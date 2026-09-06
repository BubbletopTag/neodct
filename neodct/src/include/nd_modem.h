/* nd_modem.h -- the cellular modem, on its own thread.
 *
 * ============ THE ONE DELIBERATE DEVIATION FROM 1:1 ============
 *
 * Today the modem is polled from read_keypress(). An app that never calls it
 * -- Koki reads the input device directly -- can silently miss an incoming
 * call. OPEN-QUESTIONS.md question 1 is answered: FIX IT. The modem gets its
 * own thread in the core process and calls always interrupt whatever is
 * running. On RING the core signals the app child (SIGTERM), the child runs
 * app_shutdown() and exits, the core reclaims the screen and shows the call
 * UI. No golden frame covers this, so nothing in the oracle changes.
 *
 * ============ THREADING RULES ============
 *
 * The modem thread owns the serial descriptor. Everything in this header is
 * safe to call from the UI thread; the implementation takes the modem's own
 * mutex. Two consequences:
 *
 *   - Never hold a modem call across a fork(). CODING-STANDARDS.md section 1.1
 *     exists because of this exact structure: a child that inherits a locked
 *     malloc arena hangs on its first allocation.
 *   - The readouts below are SNAPSHOTS. Two consecutive calls can disagree.
 *     Take one snapshot per frame and render from it.
 *
 * ============ "NO SIGNAL" IS NOT "ZERO BARS" ============
 *
 * nd_modem_signal_level() returns -1 for "unknown", 0..4 otherwise, and the
 * difference is visible: the home screen falls back to the layout's sim_val
 * when it is unknown but draws an empty meter when it is genuinely 0.
 *
 * ============ AND "NO MODEM" IS NOT "A BROKEN MODEM" ============
 *
 * There are FOUR link states, not two, and the two that are not "yes" and
 * "no" are why this header grew a section. See nd_modem_link.
 */

#ifndef ND_MODEM_H_INCLUDED
#define ND_MODEM_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ND_MODEM_LOCK_FILE    "/tmp/neodct-modem.lock"
#define ND_MODEM_DEFAULT_PORT "AUTO" /* probes ttyUSB2 then ttyUSB3 */

/* One line of "why is there no modem": a candidate list with a reason each. */
#define ND_MODEM_PROBE_WHY_MAX 200

/* Timings, all load-bearing for a 1:1 port of the poll cadence. */
#define ND_POLL_URC_S              0.5
#define ND_SMS_PROMPT_TIMEOUT_S    5.0
#define ND_SMS_SEND_TIMEOUT_S      30.0
#define ND_POLL_SIGNAL_S           5.0
#define ND_POLL_NET_S              20.0
#define ND_POLL_OPERATOR_S         60.0
#define ND_PROBE_RETRY_S           10.0

/* THE BOOT GRACE. How long after the service starts "no modem has answered"
 * is nothing to report, and how often to ask during it.
 *
 * A SIM7600 is not ready when the UI is. It enumerates, drops and enumerates
 * again while its own firmware boots, and S45modem holds the AT port for its
 * data session at exactly the moment the first probe lands. The service used
 * to take any of that as its answer: "Simulation" in the carrier line the
 * instant the home screen came up, or a fault notice for a modem that had
 * been seen and then re-enumerated -- both on a phone whose modem was fine
 * thirty seconds later. Inside the window the link reports
 * ND_MODEM_LINK_PROBING (an empty meter and the layout's own "No Service"),
 * a lost modem is probed for again rather than declared broken, and the
 * probe runs every ND_MODEM_BOOT_PROBE_S instead of every ND_PROBE_RETRY_S.
 *
 * system.modem.boot_grace_s overrides the default; 0 is the old behaviour,
 * and what QEMU and the tests want. */
#define ND_MODEM_BOOT_GRACE_DEFAULT_S 30.0
#define ND_MODEM_BOOT_PROBE_S         2.0

/* THE CEILING ON A GRACE THAT KEEPS BEING EXTENDED.
 *
 * The grace above is measured from the moment ModemService starts, and that
 * turned out to be the wrong clock. On the phone the UI is up about two
 * seconds after power-on, while a SIM7600 needs several seconds just to
 * enumerate on the USB bus and /dev/ttyUSB* does not get its `dialout` group
 * until udev has replayed the coldplug. So the thirty seconds were being
 * spent waiting for a bus that had not settled, and the verdict -- announced
 * out loud, in the carrier line -- was reached before the radio had finished
 * starting.
 *
 * The rule now is that the grace does not run while there is NOTHING TO
 * PROBE: every probe that finds no candidate AT port at all, and the first
 * probe that finds one, push the deadline out by a fresh grace. A phone whose
 * modem appears at t+25s therefore still gets its full window to answer AT
 * from the moment it appears, instead of two seconds of it.
 *
 * That could obviously run for ever, so it is capped here, measured from the
 * start of the service. Sixty seconds is chosen against S45modem, which
 * allows MODEM_PORT_WAIT_S=30 for the node alone; past this the phone stops
 * waiting and says what it has found. The cap is not applied at all when the
 * grace is 0 -- QEMU and the tests ask for no grace and must keep getting
 * exactly none. */
#define ND_MODEM_LATE_GRACE_MAX_S 60.0

/* How long a LIVE modem may go without a single successful AT transaction
 * before the service stops believing in it.
 *
 * Generous on purpose. A SIM7600 that is busy registering routinely misses
 * the 1.5 s AT+CSQ and the 2.0 s AT+CLCC, and the whole design treats one
 * timeout as nothing at all -- so this must sit far above any run of ordinary
 * misses. Ninety seconds is roughly eighteen consecutive failed CSQ polls at
 * ND_POLL_SIGNAL_S spacing. A modem that has said nothing for a minute and a
 * half is not busy, it is gone. */
#define ND_MODEM_FAULT_AFTER_S     90.0

/* How long a write to the AT port may make no progress before the port is
 * declared dead. A modem asserting flow control for a moment is not a modem
 * that has gone, and the write retries; a modem whose USB stack has wedged
 * returns EAGAIN for ever, and the UI thread blocked in dial() or hangup()
 * behind that write is the phone frozen solid. Two seconds is a hundred
 * times what any AT line needs. */
#define ND_MODEM_WRITE_STALL_S     2.0

/* What Simulation Mode puts on the home screen in place of an operator name,
 * and how many bars it draws. See nd_modem_signal_level() for why the two
 * numbers differ on network presence rather than being a fixed constant. */
#define ND_MODEM_SIM_CARRIER       "Simulation"
#define ND_MODEM_SIM_BARS_ONLINE   4
#define ND_MODEM_SIM_BARS_OFFLINE  1

/* And what a modem that IS there but cannot be talked to puts there instead.
 *
 * "Simulation" was the answer to both questions until 0.5.8b, and on a phone
 * with a SIM7600 in it that is a lie told next to a full signal meter -- see
 * ND_MODEM_LINK_UNREACHABLE. This string is deliberately not an operator
 * name and not "No Service": a phone in a tunnel says No Service, and this
 * phone is not in a tunnel, its radio is unreachable. Eleven characters, one
 * more than "Simulation", so it fits the same slot in the home layout. */
#define ND_MODEM_UNREACHABLE_CARRIER "Modem ERROR"
#define ND_MODEM_SIM_ROUTE_TTL_S   2.0
#define ND_CLCC_POLL_S             2.0
#define ND_AUDIO_RESTART_HOLDOFF_S 3.0
#define ND_TRANSACT_SLEEP_S        0.02
#define ND_SMS_WAIT_SLEEP_S        0.05
#define ND_PROMPT_SLEEP_S          0.02
#define ND_MODEM_READ_CHUNK        512
#define ND_MODEM_PROMPT_CHUNK      64
#define ND_MODEM_EVENT_QUEUE_MAX   8 /* deque(maxlen=8) -- oldest is dropped */

/* CSQ rssi 0..31 (99 = unknown) mapped to 0..4 bars at roughly
 * -105/-93/-81/-73 dBm. */
extern const int ND_BAR_THRESHOLDS[4]; /* { 2, 8, 14, 20 } */

typedef enum {
    ND_CALL_IDLE = 0,
    ND_CALL_CALLING,
    ND_CALL_RINGING,
    ND_CALL_CONNECTED
} nd_call_state;

typedef enum { ND_SMS_OK = 0, ND_SMS_BUSY, ND_SMS_ERROR } nd_sms_st;

#define ND_MODEM_NUMBER_MAX 32
#define ND_MODEM_TEXT_MAX   1024
#define ND_MODEM_DETAIL_MAX 128

typedef struct {
    int32_t index;
    char sender[ND_MODEM_NUMBER_MAX];
    char text[ND_MODEM_TEXT_MAX];
    int64_t timestamp; /* seconds; 0 when the modem gave none */
    bool unread;
} nd_sms_rec;

typedef enum {
    ND_MODEM_EV_NONE = 0,
    ND_MODEM_EV_RING,     /* incoming call; number is the caller ID  */
    ND_MODEM_EV_SMS,      /* new message at index                    */
    ND_MODEM_EV_HANGUP,   /* the far end went away                   */
    ND_MODEM_EV_CONNECTED /* the call came up                        */
} nd_modem_ev_kind;

typedef struct {
    nd_modem_ev_kind kind;
    char number[ND_MODEM_NUMBER_MAX];
    int32_t index;
} nd_modem_event;

/* One consistent read of everything the UI paints. */
typedef struct {
    bool hardware;
    char port[64];
    char imei[17];
    char operator_name[32]; /* empty means "No Service" */
    int32_t signal_level;   /* -1 unknown, else 0..4    */
    int32_t csq_rssi;       /* -1 unknown, else 0..31   */
    bool registered;
    nd_call_state state;
    char caller_id[ND_MODEM_NUMBER_MAX];
    int32_t call_secs; /* -1 when not connected */
    /* ---- APPENDED, see OPEN-QUESTIONS.md M-16 ----
     *
     * +CEREG <stat>, -1 for Python's None. status_snapshot() has carried it
     * since it was written (ModemService line 1067) and the frozen struct
     * dropped it; the engineering Modem app draws it as its REG row and
     * `registered` cannot stand in, because that bool cannot tell HOME from
     * ROAMING, nor either of them from "nothing has answered yet". Appended,
     * so no existing field moves. */
    int32_t reg_stat;

    /* ---- APPENDED, see OPEN-QUESTIONS.md M-17 ----
     *
     * Why the last probe found no AT port, "" when there is a modem. The
     * Python had nothing like it and did not need one: it printed to a
     * console the developer was already watching. A phone in a pocket has no
     * console, and "SIMULATION" on a device with a modem visibly plugged into
     * it is not a diagnosis -- so the reason rides along and the engineering
     * Modem app draws it. Appended, so no existing field moves. */
    char probe_why[ND_MODEM_PROBE_WHY_MAX];
} nd_modem_status;

typedef struct nd_modem nd_modem;

/* Starts the reader thread. Never fails in a way the caller can act on --
 * with no hardware AT ALL it runs in simulation mode, driven by the
 * /tmp/neodct_sim_* files, which is how the whole UI is testable on a
 * desktop. A modem that is present and cannot be opened is NOT that; it is
 * ND_MODEM_LINK_UNREACHABLE and it says so. */
nd_err nd_modem_open(nd_modem **out);
void nd_modem_close(nd_modem *m);

/* ============ WHEN A CALL OR A TEXT IS ALLOWED TO BE PRETEND ============
 *
 * dial() fakes a connect after two seconds and send_sms() reports success
 * without transmitting anything whenever there is no modem, and that is the
 * feature that makes the whole UI drivable on a desktop with no radio.
 *
 * It is also, on a phone, the worst possible failure: the owner watches a
 * call timer run for a call that was never placed, or sees a text marked
 * sent that nobody will ever receive. It used to be gated on "am I talking
 * to a modem right now", which is false for every kind of probe failure --
 * see ND_MODEM_LINK_UNREACHABLE.
 *
 * The gate is now the LINK STATE, and it is simulation only when this
 * device genuinely has no radio: ND_MODEM_LINK_SIM, or ND_MODEM_LINK_PROBING
 * with no candidate AT port seen yet. UNREACHABLE and FAULT both refuse, and
 * send_sms() puts the probe's reason in `detail` so the failure names itself
 * on screen. (A LIVE modem with system.modem.allow_calls=OFF still
 * simulates: that is a deliberate development switch, not a failure.) */
bool nd_modem_dial(nd_modem *m, const char *number);
bool nd_modem_answer(nd_modem *m);
bool nd_modem_hangup(nd_modem *m);

/* (ok, detail). detail is rendered verbatim by Messages as
 * "Send failed: <detail>", so its wording is user-visible. */
bool nd_modem_send_sms(nd_modem *m, const char *number, const char *text, char *detail,
                       size_t detail_sz);

nd_sms_st nd_modem_fetch_sms(nd_modem *m, int32_t index, nd_sms_rec *out);
nd_sms_st nd_modem_read_stored_sms(nd_modem *m, nd_sms_rec *out, size_t max, size_t *n_out);

/* The tick. Called from the modem thread; the UI never calls it. */
void nd_modem_poll(nd_modem *m);

/* Pull one queued event, oldest first. false when the queue is empty.
 * requeue puts one back at the FRONT -- the core does that when it cannot
 * handle an event yet. */
bool nd_modem_take_pending_event(nd_modem *m, nd_modem_event *out);
void nd_modem_requeue_event(nd_modem *m, const nd_modem_event *e);

void nd_modem_status_snapshot(nd_modem *m, nd_modem_status *out);

/* ------------------------------------------------------------------ *
 * struct nd_lines -- the reply of one AT transaction
 * ------------------------------------------------------------------ *
 *
 * COMPLETED HERE RATHER THAN FORWARD-DECLARED. This header named the type for
 * nd_modem_send_at() and no public header ever finished it, so the definition
 * lived in lib/nd_modem_priv.h -- which meant the engineering Modem app (id
 * 9005), the one caller send_at() exists for, could not allocate one and
 * could not call the function as declared. That is OPEN-QUESTIONS.md M-3, and
 * this is the fix it asks for: the definition and its two size constants
 * moved up, verbatim. No signature changed and no field moved; nd_modem_priv.h
 * still gets all of it through this header, so every existing call site in
 * lib/ and in test_modem.c is untouched.
 *
 * A flat pool plus an offset table, not [64][512]: a reply is almost always
 * two short lines and the fixed array would be 32 KB of the modem's 18.
 *
 * The nd_modem__ prefix on the accessors is kept even though they are public
 * now, because renaming them would touch every call site in the AT engine to
 * buy nothing -- see M-3's note.
 *
 * ============ AND IT STAYS `struct nd_lines`, NEVER A TYPEDEF ============
 *
 * nd_text.h -- also frozen, also public -- already spells `nd_lines` as a
 * typedef for its wrapped-text line list. Struct tags and typedef names are
 * different namespaces in C, so the definition below is legal beside it, but
 * a `typedef struct nd_lines nd_lines;` here is NOT: any translation unit
 * that included both headers would fail to compile, and nd_ui.h pulls this
 * one in, so that is most of the tree. The typedef therefore stays in
 * lib/nd_modem_priv.h, where only the AT engine sees it, and a caller out
 * here writes `struct nd_lines`.
 *
 * It is 4 KB and change. An app keeps it static or on the heap, never on the
 * stack (CODING-STANDARDS.md 1.5).
 */

/* Collected intermediate lines for one transaction. M-5 records these as two
 * of the four bounds the Python does not have. */
#define ND_MODEM_LINES_MAX  64
#define ND_MODEM_LINES_POOL 4096

struct nd_lines {
    char pool[ND_MODEM_LINES_POOL];
    size_t used;
    uint16_t off[ND_MODEM_LINES_MAX];
    size_t n;
    bool truncated; /* a line or the pool did not fit; the Python has no cap */
};

/* Empty the collector. A caller MUST reset before the first use: the struct is
 * not zeroed for it. */
void nd_modem__lines_reset(struct nd_lines *l);
void nd_modem__lines_add(struct nd_lines *l, const char *line);
/* NULL past the end. The returned pointer is into `l`'s own pool and lives as
 * long as `l` does, or until the next reset. */
const char *nd_modem__lines_get(const struct nd_lines *l, size_t i);

/* Raw AT passthrough for the engineering Modem app. timeout 0 means 5.0 s.
 * final_out receives the final result line ("OK", "+CME ERROR: 10", ...) and
 * lines_out the intermediate ones. */
nd_err nd_modem_send_at(nd_modem *m, const char *cmd, double timeout, char *final_out,
                        size_t final_sz, struct nd_lines *lines_out);

/* ---- readouts polled once per rendered frame ---- */
int32_t nd_modem_signal_level(nd_modem *m);         /* -1 == unknown        */
const char *nd_modem_operator_display(nd_modem *m); /* NULL == "No Service" */
bool nd_modem_registered(nd_modem *m);
/* label is "CALLING" / "RINGING" / "CONNECTED"; *secs is -1 unless connected */
void nd_modem_call_status(nd_modem *m, const char **label, int32_t *secs);

nd_call_state nd_modem_state(nd_modem *m);
const char *nd_modem_caller_id(nd_modem *m); /* NULL when none */
bool nd_modem_has_hardware(nd_modem *m);

/* ------------------------------------------------------------------ *
 * The link: four states, because two was a lie and three was not enough
 * ------------------------------------------------------------------ *
 *
 * nd_modem_has_hardware() answers "am I talking to a modem right now", and
 * for years the service had nothing else -- so both ways of answering "no"
 * came out as Simulation Mode:
 *
 *   ND_MODEM_LINK_SIM     No modem was ever found. On QEMU, or on a phone
 *                         with nothing plugged in, this is CORRECT and the
 *                         phone should say so plainly. Calls and texts are
 *                         still simulated end to end, so the right thing to
 *                         show is "Simulation", not "No Service" -- there IS
 *                         a service, it is just a pretend one.
 *
 *   ND_MODEM_LINK_LIVE    A modem answered AT and is being talked to.
 *
 *   ND_MODEM_LINK_FAULT   A modem WAS found and then failed: a hard errno on
 *                         a port already adopted, or ND_MODEM_FAULT_AFTER_S
 *                         with nothing answering. Reporting this as
 *                         Simulation is the bug this enum exists to fix. It
 *                         tells the owner of a broken phone that everything
 *                         is fine, and it is the one state where the phone
 *                         must look obviously wrong: zero bars, no carrier
 *                         name at all, and a modal notice once.
 *
 *   ND_MODEM_LINK_PROBING No modem has answered YET, and the boot grace has
 *                         not run out -- see ND_MODEM_BOOT_GRACE_DEFAULT_S.
 *                         The carrier line is left to the layout ("No
 *                         Service") and the meter is empty, which is what a
 *                         phone whose radio is still coming up looks like.
 *                         Appended after FAULT so no existing value moves.
 *
 *   ND_MODEM_LINK_UNREACHABLE
 *                         The kernel enumerated at least one candidate AT
 *                         port and NOT ONE of them could be talked to, with
 *                         the boot grace already spent. There is a radio in
 *                         this phone; the service cannot reach it.
 *
 * A NULL modem is ND_MODEM_LINK_SIM: a core with no ModemService is not a
 * core with a broken one.
 *
 * ============ WHY UNREACHABLE HAD TO EXIST ============
 *
 * SIM was being returned for both "there is no radio" and "there is a radio
 * and every way of opening it failed", because the only thing that could
 * produce FAULT was nd_modem__drop_hardware(), and that is reachable only
 * from a port we had ALREADY ADOPTED. Every way a probe can fail on a phone
 * that has a modem -- /dev/ttyUSB2 still root:root 0600 because the udev
 * coldplug has not replayed it, S45modem holding the AT-port flock, a
 * SIM7600 whose firmware has not finished booting and does not answer AT
 * inside a second -- left `faulted` false and landed in SIM.
 *
 * That is the single most misleading thing this service could say. It puts
 * the word "Simulation" in the carrier line, and because Simulation Mode
 * draws its meter from /proc/net/route, it puts FOUR FULL BARS beside it on
 * a phone whose radio the UI cannot open at all. It also, silently, turns
 * dial() into a fake two-second connect and send_sms() into a success that
 * transmits nothing -- see nd_modem_dial() and nd_modem_send_sms(). A phone
 * lying to its owner about a call it never placed is worse than a phone that
 * plainly says it is broken.
 *
 * So: SIM is now reserved for the case where /sys/class/tty enumerated no
 * candidate at all, which is the only case in which it is true. Anything
 * else is UNREACHABLE, which renders as an EMPTY meter and the carrier line
 * ND_MODEM_UNREACHABLE_CARRIER, raises the one-shot notice through
 * nd_modem_take_pending_fault() carrying the probe's own reason, and refuses
 * to fake calls and texts.
 *
 * Appended after PROBING, so no existing value moves and a UI that has never
 * heard of it simply does not match its FAULT check. That is deliberate: the
 * two readouts below already say the right thing for this state on their
 * own, so a status bar needs no change at all to stop lying. */
typedef enum {
    ND_MODEM_LINK_SIM = 0,
    ND_MODEM_LINK_LIVE,
    ND_MODEM_LINK_FAULT,
    ND_MODEM_LINK_PROBING,
    ND_MODEM_LINK_UNREACHABLE
} nd_modem_link;

nd_modem_link nd_modem_link_state(nd_modem *m);

/* The one-shot the UI drains, exactly as nd_battery_take_pending_warning()
 * is drained by nd_ui_show_pending_battery_warning(): the service latches a
 * fault from its own thread, the UI THREAD pops it at a safe point and puts a
 * modal on the screen. Returns NULL when there is nothing pending, and the
 * reason string when there is -- and having returned it once, returns NULL
 * until the modem faults again. Never draws anything itself. */
const char *nd_modem_take_pending_fault(nd_modem *m);

#ifdef __cplusplus
}
#endif

#endif /* ND_MODEM_H_INCLUDED */

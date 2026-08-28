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
 * (3) when it is unknown but draws an empty meter when it is genuinely 0.
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
 * with no hardware it runs in simulation mode, driven by the /tmp/neodct_sim_*
 * files, which is how the whole UI is testable on a desktop. */
nd_err nd_modem_open(nd_modem **out);
void nd_modem_close(nd_modem *m);

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

/* ---- APPENDED, for the call log ----
 *
 * How many seconds the LAST FINISHED call spent connected; 0 for one that
 * never came up, and for a call still in progress. Not a snapshot of a live
 * call -- nd_modem_call_status()'s `secs` is that -- but the reading that
 * survives the hangup, because the core writes the call log after the line is
 * already down and call_status() reports -1 by then.
 *
 * Cleared when the next call starts, so it is only ever the most recent
 * completed one. Appended, so no existing declaration moves. */
int32_t nd_modem_last_call_secs(nd_modem *m);

#ifdef __cplusplus
}
#endif

#endif /* ND_MODEM_H_INCLUDED */

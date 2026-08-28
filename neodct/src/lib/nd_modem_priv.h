/* nd_modem_priv.h -- what the four modem translation units say to each other.
 *
 * NOT a public header. Nothing outside lib/ may depend on any of this, with
 * one deliberate exception: test/unit/test_modem.c includes it by relative
 * path. The AT engine is the piece of this subsystem that a pty can exercise
 * without a modem, and it can only be exercised if the test can build a
 * nd_modem that has NOT started its thread and drive transact()/poll() by
 * hand. Everything the test reaches is named nd_modem__* so that "this is
 * internal" is visible at the call site.
 *
 * ============ WHO OWNS WHAT ============
 *
 *   fd, rx[], every next_* timer, the audio children:  the modem thread ONLY.
 *   state, caller_id, csq, reg_stat, operator, imei, port, hardware, the
 *   event ring:                                        guarded by st_mu.
 *
 * The modem thread never holds st_mu across a syscall, a sleep or a
 * transaction, so a UI-thread snapshot never waits longer than a few
 * instructions even while send_sms() is thirty seconds into its ack wait.
 *
 * ============ MEMORY ============
 *
 * One nd_modem is about 18 KB and there is exactly one of them:
 *     event ring   8 x 1064 B = 8512 B   (a simulated SMS body is 1 KB)
 *     rx buffer              = 8192 B
 *     everything else       ~= 1300 B
 * All of it is allocated once in nd_modem_open() and freed in
 * nd_modem_close(); nothing here allocates per tick.
 */

#ifndef ND_MODEM_PRIV_H_INCLUDED
#define ND_MODEM_PRIV_H_INCLUDED

#include <pthread.h>
#include <sys/types.h>
#include <termios.h>

#include "nd_modem.h"

/* nd_modem.h carries every other constant from the Python but not the baud
 * rate, because B115200 is a termios token and the public header does not
 * include <termios.h>. */
#define ND_MODEM_BAUD B115200

/* musl's default thread stack is 128 KB against glibc's 8 MB (MUSL.md). Set
 * explicitly so the difference can never become a confusing crash: this
 * thread's deepest frame is one nd_path_resolve() buffer inside one
 * transaction, so 256 KB is two orders of magnitude of slack. */
#define ND_MODEM_STACK_BYTES (256u * 1024u)

/* ------------------------------------------------------------------ *
 * Constants the public header does not carry
 * ------------------------------------------------------------------ */

#define ND_MODEM_ASOUND_DIR "/proc/asound"
#define ND_MODEM_TTY_DIR    "/sys/class/tty"
#define ND_MODEM_SIM_CSQ    "/tmp/neodct_sim_csq"
#define ND_MODEM_SIM_RING   "/tmp/neodct_sim_ring"
#define ND_MODEM_SIM_OPS    "/tmp/neodct_sim_operator"
#define ND_MODEM_SIM_SMS    "/tmp/neodct_sim_sms"

#define ND_MODEM_PCM_FORMAT       "S16_LE"
#define ND_MODEM_PCM_RATE_DEFAULT 16000

/* R-7. The Python's _rxbuf is a bytes object that only ever loses COMPLETE
 * lines, so a port that emits binary with no '\n' -- the Qualcomm DIAG port is
 * exactly that -- grows it without bound. 8 KB is far more than any real AT
 * exchange needs; on overflow the buffer is dropped whole and resynchronises
 * at the next newline. See OPEN-QUESTIONS.md M-2. */
#define ND_MODEM_RXBUF_MAX 8192

/* One decoded line. The longest thing a SIM7600 emits in text mode is a
 * +CMGL header plus a 160-character body, well inside this. */
#define ND_MODEM_LINE_MAX 512

/* Candidate AT ports considered in one probe. A SIM7600 enumerates five. */
#define ND_MODEM_CAND_MAX 32

/* ND_MODEM_PROBE_WHY_MAX is public now -- the reason crosses the service wire
 * so the engineering Modem app can draw it. It arrives with nd_modem.h. */

#define ND_MODEM_PORT_MAX 96
#define ND_MODEM_WHY_MAX  128

/* AT+CLCC <stat>. */
#define ND_CLCC_CONNECTED 0
#define ND_CLCC_HELD      1
#define ND_CLCC_CALLING   2
#define ND_CLCC_RINGING   3
#define ND_CLCC_INCOMING  4
#define ND_CLCC_WAITING   5

/* Two SMS "indices" that are not SIM slots. The frozen nd_modem_event cannot
 * spell "a simulated message arrived" or "sweep the SIM", so both ride out on
 * ND_MODEM_EV_SMS with a negative index. See OPEN-QUESTIONS.md M-1. */
#define ND_MODEM_SMS_IDX_SIM    (-1)
#define ND_MODEM_SMS_IDX_STORED (-2)

/* struct nd_lines, its two size constants and the three nd_modem__lines_*
 * accessors moved into nd_modem.h when the engineering Modem app was ported
 * -- the app is the caller nd_modem_send_at() exists for and could not
 * allocate one from out here. OPEN-QUESTIONS.md M-3. They arrive through the
 * #include above; nothing in lib/ or in test_modem.c had to change.
 *
 * The TYPEDEF could not go with them: nd_text.h spells `nd_lines` as its own
 * typedef, and the two would collide in every translation unit that includes
 * both. It stays here, where only the AT engine and test_modem.c see it, and
 * the public spelling is `struct nd_lines`. */
typedef struct nd_lines nd_lines;

/* ------------------------------------------------------------------ *
 * The event ring
 * ------------------------------------------------------------------ */

/* The ten (kind, detail) tuples ModemService.append()s. The frozen public
 * enum has four; the mapping onto it happens in nd_modem_take_pending_event()
 * and only there. */
typedef enum {
    ND_MEV_INCOMING = 0,     /* ("incoming", number|None)         */
    ND_MEV_CONNECTED,        /* ("connected", caller_id|None)     */
    ND_MEV_ENDED,            /* ("ended", line)                   */
    ND_MEV_MISSED,           /* ("missed", text)                  */
    ND_MEV_SMS_RECEIVED,     /* ("sms_received", index)           */
    ND_MEV_SMS_SIM,          /* ("sms_sim", (sender, body))       */
    ND_MEV_SMS_SENT,         /* ("sms_sent", number)              */
    ND_MEV_SMS_STORED_CHECK, /* ("sms_stored_check", None)        */
    ND_MEV_MODEM_LOST,       /* ("modem_lost", why)               */
    ND_MEV_MODEM_FOUND       /* ("modem_found", port)             */
} nd_mev_kind;

typedef struct {
    nd_mev_kind kind;
    bool has_detail; /* Python's None is a value, not an empty string */
    int32_t index;
    char sender[ND_MODEM_NUMBER_MAX];
    char text[ND_MODEM_TEXT_MAX];
} nd_mev;

/* ------------------------------------------------------------------ *
 * The request handshake
 * ------------------------------------------------------------------ */

typedef enum {
    ND_REQ_DIAL = 0,
    ND_REQ_ANSWER,
    ND_REQ_HANGUP,
    ND_REQ_SEND_SMS,
    ND_REQ_FETCH_SMS,
    ND_REQ_READ_STORED,
    ND_REQ_SEND_AT
} nd_req_kind;

typedef struct nd_modem_req {
    nd_req_kind kind;
    const char *s1; /* number, or the raw AT command */
    const char *s2; /* SMS text */
    int32_t i1;     /* SMS index */
    double timeout;

    bool ok;
    nd_sms_st sms_st;
    char detail[ND_MODEM_DETAIL_MAX];
    nd_sms_rec *rec_out;
    size_t rec_max;
    size_t rec_n;
    char *final_out;
    size_t final_sz;
    nd_lines *lines_out;
    nd_err err;

    bool done;
} nd_modem_req;

/* ------------------------------------------------------------------ *
 * The service
 * ------------------------------------------------------------------ */

struct nd_modem {
    /* ---- guarded by st_mu ---- */
    pthread_mutex_t st_mu;
    nd_call_state state;
    bool hardware;
    char port[ND_MODEM_PORT_MAX];
    char imei[17];
    bool imei_known;
    char operator_name[32];
    bool operator_known; /* Python's self.operator is None until COPS answers */
    char caller_id[ND_MODEM_NUMBER_MAX];
    bool caller_id_known;
    int32_t csq;      /* -1 is Python's None, 99 is the modem's "unknown" */
    int32_t reg_stat; /* -1 is Python's None                              */

    nd_mev ev[ND_MODEM_EVENT_QUEUE_MAX];
    size_t ev_head;
    size_t ev_count;

    /* One simulated message waits here between the ("sms_sim", ...) event and
     * the fetch_sms() the core answers it with. */
    bool sim_sms_pending;
    char sim_sms_sender[ND_MODEM_NUMBER_MAX];
    char sim_sms_body[ND_MODEM_TEXT_MAX];

    /* Snapshot buffers for the two readouts that hand back a const char *.
     * Written under st_mu, read by the caller immediately afterwards. */
    char op_display[32];
    char cid_display[ND_MODEM_NUMBER_MAX];

    /* ---- the modem thread's own, no lock ---- */
    int fd;
    int lock_fd;
    bool lock_held;
    uint8_t rx[ND_MODEM_RXBUF_MAX];
    size_t rx_len;
    bool rx_overflow_logged;

    double next_urc;
    double next_csq;
    double next_net;
    double next_cops;
    double next_probe;
    /* The last reason the probe gave, so a failure that repeats every
     * PROBE_RETRY_S is printed ONCE and not forever. Cleared on success, so a
     * modem that is lost and fails again says so again. */
    char last_probe_why[ND_MODEM_PROBE_WHY_MAX];
    double next_clcc;
    double next_audio_restart;

    bool sim_connect_armed;
    double sim_connect_at;
    bool sim_ring_mtime_known;
    double sim_ring_mtime;

    pid_t audio_pid; /* aplay:   PCM port -> speaker */
    bool audio_live;
    pid_t mic_pid; /* arecord: mic -> PCM port     */
    bool mic_live;
    char active_pcm_port[ND_MODEM_PORT_MAX];
    bool pcm_active;
    int32_t mic_fails;
    bool pcm_cleanup;
    bool pcm_retry;

    int32_t call_stat; /* -1 is Python's None */
    bool call_connected;
    double call_connected_at;
    /* How long the last finished call was CONNECTED. Latched by set_state()
     * on the way to IDLE, because call_connected_at is reused by the next
     * dial and the call log is written after the line is already down. */
    int32_t last_call_secs;

    int32_t pcm_rate;
    char configured_port[ND_MODEM_PORT_MAX];
    bool allow_calls;

    /* Scratch for one transaction, so nothing is allocated per command.
     * `rx_lines` holds what read_pending() just split off the wire; `collected`
     * is where a transaction accumulates when the caller wants no lines back.
     * They are never in use for the same purpose at the same time -- nothing
     * that fills `rx_lines` re-enters read_pending(). */
    nd_lines collected;
    nd_lines rx_lines;

    /* ---- the request handshake ---- */
    pthread_mutex_t req_mu;
    pthread_cond_t req_cv;  /* the thread waits here for work or a tick */
    pthread_cond_t done_cv; /* callers wait here for a slot or a result */
    nd_modem_req *pending;
    bool quit;
    pthread_t thread;
    bool thread_started;
};

/* ------------------------------------------------------------------ *
 * Internals shared between the four .c files, and reachable from the test
 * ------------------------------------------------------------------ */

/* CLOCK_MONOTONIC, not nd_time_monotonic(). The virtual clock exists to make
 * FRAMES deterministic and only advances when one is committed; a thread that
 * genuinely sleeps on a serial port cannot be driven by it, and reading it
 * from two threads would be a race for nothing. The modem draws no pixels, so
 * no golden frame can tell the difference. OPEN-QUESTIONS.md M-4. */
double nd_modem__now(void);
void nd_modem__nap(double seconds);

/* Construction without the thread: zeroes the state, reads the three one-shot
 * settings, opens the lock file. Does NOT probe. */
nd_err nd_modem__create(nd_modem **out);
void nd_modem__destroy(nd_modem *m);

/* --- locking (flock(2), because busybox flock in S45modem uses it) --- */
void nd_modem__lock(nd_modem *m);
void nd_modem__unlock(nd_modem *m);

bool nd_modem__acquire(nd_modem *m);
void nd_modem__release(nd_modem *m);

/* --- the AT engine, nd_modem_at.c --- */
int nd_modem__open_port(const char *dev);
size_t nd_modem__read_pending(nd_modem *m, nd_lines *out);
bool nd_modem__transact(nd_modem *m, const char *cmd, double timeout, char *final_out,
                        size_t final_sz, nd_lines *lines_out);
bool nd_modem__command(nd_modem *m, const char *cmd, double timeout, char *final_out,
                       size_t final_sz, nd_lines *lines_out);
void nd_modem__drop_hardware(nd_modem *m, const char *why);
bool nd_modem__is_final(const char *line);
bool nd_modem__is_urc(const char *line);
size_t nd_modem__decode_line(const uint8_t *raw, size_t len, char *out, size_t out_sz);
bool nd_modem__parse_int(const char *s, int32_t *out);
bool nd_modem__parse_hex(const char *s, int32_t *out);
bool nd_modem__wait_sms_prompt(nd_modem *m, double timeout);

/* --- the state machine, nd_modem.c --- */
void nd_modem__handle_urc(nd_modem *m, const char *line);
void nd_modem__parse_reg(nd_modem *m, const char *line);
void nd_modem__parse_csq(nd_modem *m, const nd_lines *lines);
void nd_modem__parse_cops(nd_modem *m, const nd_lines *lines);
void nd_modem__poll_clcc(nd_modem *m);
int32_t nd_modem__bars(int32_t csq);
void nd_modem__queue(nd_modem *m, const nd_mev *e);
void nd_modem__queue_front(nd_modem *m, const nd_mev *e);
bool nd_modem__take(nd_modem *m, nd_mev *out);
size_t nd_modem__candidate_ports(nd_modem *m, char ports[][ND_MODEM_PORT_MAX], size_t max);
bool nd_modem__probe_hardware(nd_modem *m);
void nd_modem__adopt(nd_modem *m, int fd, const char *dev);
void nd_modem__init_modem(nd_modem *m);
size_t nd_modem__filter_number(const char *in, char *out, size_t out_sz);
size_t nd_modem__parse_sms_records(const nd_lines *lines, const char *header, nd_sms_rec *out,
                                   size_t max);

/* --- simulation, nd_modem_sim.c --- */
void nd_modem__poll_sim(nd_modem *m, double now);
bool nd_modem__sim_read_int(const char *path, int32_t *out);
bool nd_modem__sim_read_text(const char *path, char *out, size_t out_sz);

/* --- call audio, nd_modem_audio.c --- */
void nd_modem__pcm_port(char *out, size_t out_sz);
bool nd_modem__find_capture_device(char *out, size_t out_sz);
void nd_modem__start_call_audio(nd_modem *m);
void nd_modem__stop_call_audio(nd_modem *m);

/* The call is over: latch how long it was connected into last_call_secs and
 * drop the connect stamp. Idempotent -- the second caller for one call finds
 * call_connected already false and does nothing.
 *
 * TWO PLACES END A CALL AND THEY DO NOT AGREE ON ORDER. The URC handlers run
 * set_state(IDLE) and then stop_call_audio(); do_hangup() runs
 * stop_call_audio() and then set_state(IDLE). Both of those clear the connect
 * stamp, so whichever ran first used to destroy the reading the other was
 * going to take -- which is why this is one function called from both rather
 * than the same three lines written twice.
 *
 * CALL WITH THE STATE LOCK HELD. It takes no lock of its own; both callers
 * already hold one. */
void nd_modem__note_call_ended(nd_modem *m);
void nd_modem__watch_audio_proc(nd_modem *m, double now);

#endif /* ND_MODEM_PRIV_H_INCLUDED */

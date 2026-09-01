/* nd_svc.c -- the app -> core service channel, both ends of it.
 *
 * nd_svc.h is the contract and docs/c-rewrite/spec-app-services.md is the
 * design. What follows is the part a reader of this file needs.
 *
 * ============ WHO RUNS WHAT ============
 *
 *   the CORE   nd_svc_server_*, and svc_thread() below. One thread, created
 *              AFTER the fork and destroyed after the waitpid, serving one
 *              socket that dies with the launch.
 *   an APP     nd_svc_client_*, and the six nd_svc_* calls. No thread; the
 *              app blocks on its own request because it has nothing else to
 *              do until the answer comes.
 *
 * ============ WHY THE SERVING IS ON A THREAD ============
 *
 * The core's UI thread is in nd_proc_launch_app()'s pump loop for the whole
 * life of the app, and that loop is what SCANS THE I2C KEY MATRIX --
 * nd_input.c polls the matrix synchronously from inside
 * nd_input_read_event(). The matrix has no kernel queue behind it, so a key
 * pressed while nobody is scanning is not delayed, it is LOST. An SMS send
 * takes up to 37 seconds. Serving that inline would cost the user every key
 * they pressed during it, on the hardware only -- under evdev the kernel
 * buffers and the bug is invisible on every desktop.
 *
 * ============ WHY NOTHING HERE NEEDS A LOCK IT DOES NOT TAKE ============
 *
 * nd_modem.h states its own calls are safe from another thread. The battery
 * has no lock and does not need one HERE, because OPEN-QUESTIONS.md X-13
 * records that the core ticks no service at all while an app child runs --
 * pump_keys() calls nd_input_read_event() directly, never
 * nd_ui_read_keypress(). For the exact window in which this thread exists,
 * it is the only thing in the process touching nd_battery. That is a
 * precondition: if the pump loop ever grows a battery tick, the battery
 * needs a mutex the same day.
 *
 * ============ THE RECORDS ============
 *
 * Fixed-size PODs over SOCK_SEQPACKET, so the kernel does the framing and a
 * half-written request from a wedged child cannot desynchronise anything.
 * Both ends are the same libneodct build in the same process family on the
 * same machine -- the argument nd_input.h already makes for putting native
 * struct input_event on its pipe -- so there is no endianness or padding
 * question. Every record still carries magic, version and its own size, and
 * every record is memset to zero before it is filled so that no uninitialised
 * stack byte crosses the boundary in either direction.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_battery.h"
#include "nd_log.h"
#include "nd_modem.h"
#include "nd_svc.h"
#include "nd_types.h"
#include "nd_ui.h"

/* ------------------------------------------------------------------ *
 * The wire
 * ------------------------------------------------------------------ */

#define SVC_MAGIC   0x4E445356u /* "NDSV" */
#define SVC_VERSION 1u

typedef enum {
    SVC_OP_SEND_SMS = 1,
    SVC_OP_MODEM_STATUS = 2,
    SVC_OP_BATTERY = 3,
    SVC_OP_BATTERY_QUICKSTART = 4
} svc_op;

/* Outcome of the exchange, as opposed to the outcome of the operation. */
typedef enum {
    SVC_ST_OK = 0,          /* the request ran; `ok` says what it returned */
    SVC_ST_NO_SERVICE = 1,  /* the core has no such service                */
    SVC_ST_BAD_REQUEST = 2, /* validation refused it                       */
    SVC_ST_UNAVAILABLE = 3  /* never reached a core: no channel, timeout,  */
                            /* a dead peer, or an interrupted wait         */
} svc_status;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t size; /* sizeof(svc_req), so a mismatched build is caught */
    uint32_t op;
    uint32_t text_len; /* SEND_SMS only; strlen(text) */
    uint32_t pad;
    char number[ND_MODEM_NUMBER_MAX];
    char text[ND_MODEM_TEXT_MAX];
} svc_req;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t op;
    uint32_t status;  /* svc_status                                    */
    uint32_t ok;      /* the operation's own boolean result            */
    uint32_t present; /* the core has this service at all              */
    int32_t err;      /* errno the service left behind; 0 when none    */
    char detail[ND_MODEM_DETAIL_MAX];
    nd_modem_status modem;
    nd_battery_snap battery;
    double vcell;
    uint32_t have_vcell;
    uint32_t hardware; /* battery only: nd_battery_has_hardware() */
    int32_t level;
} svc_resp;

/* The receive buffers are one byte longer than the record, so an oversized
 * datagram comes back with n != sizeof(record) and is refused rather than
 * silently truncated into something that looks well-formed. */
#define REQ_BUF_SZ  (sizeof(svc_req) + 1u)
#define RESP_BUF_SZ (sizeof(svc_resp) + 1u)

/* The serving thread's stack. See nd_svc_server_start() for why it is stated
 * rather than inherited. */
#define ND_SVC_STACK_BYTES (128u * 1024u)

/* ------------------------------------------------------------------ *
 * A real monotonic clock
 * ------------------------------------------------------------------ *
 *
 * Deliberately NOT nd_time_monotonic(): under capture that reads the virtual
 * clock, which advances one tick per COMMITTED FRAME and would leave a
 * timeout here waiting for a frame nobody is going to draw. These are real
 * deadlines on real syscalls.
 */
static double svc_now(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int svc_ms_until(double deadline)
{
    double left = deadline - svc_now();

    if (left <= 0.0)
        return 0;
    if (left > 3600.0)
        left = 3600.0;
    return (int)(left * 1000.0);
}

/* ------------------------------------------------------------------ *
 * Framed send and receive
 * ------------------------------------------------------------------ */

/* One whole record, or a failure. SOCK_SEQPACKET never writes a partial
 * datagram, so there is no resume loop to get wrong.
 *
 * ============ WHY A WRITE NEEDS A DEADLINE TOO ============
 *
 * The obvious hazard is a reader that has gone, and that one is easy: the
 * send fails with EPIPE (MSG_NOSIGNAL keeps it an error rather than a death).
 * The hazard that needs the deadline is a peer that is ALIVE AND NOT READING.
 * A hostile app can post request after request and never read an answer; the
 * core's socket buffer fills, and a blocking send would park the service
 * thread there for ever -- which then survives nd_svc_server_stop()'s join,
 * gets detached, and never exits. One untrusted child would leak a core
 * thread per launch. So the write is bounded like everything else, and a peer
 * that will not drain is treated as a peer that has gone. */
static bool svc_send(int fd, const void *rec, size_t len, double deadline)
{
    for (;;) {
        struct pollfd pfd;
        ssize_t n = send(fd, rec, len, MSG_NOSIGNAL | MSG_DONTWAIT);

        if (n == (ssize_t)len)
            return true;
        if (n >= 0)
            return false; /* a short datagram is not something to retry */
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return false;

        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        n = poll(&pfd, 1u, svc_ms_until(deadline));
        if (n == 0)
            return false; /* alive, but not draining */
        if (n < 0 && errno != EINTR)
            return false;
    }
}

typedef enum { SVC_RX_OK = 0, SVC_RX_TIMEOUT, SVC_RX_EOF, SVC_RX_INTR, SVC_RX_ERROR } svc_rx;

/* Wait for one record until `deadline`. `want` is the exact size a valid
 * record has; `buf` is one byte larger so an oversized datagram is visible.
 *
 * ============ EINTR IS NOT ONE QUESTION BUT TWO ============
 *
 * nd_app.h's teardown contract requires a blocked read in an APP to return
 * EINTR rather than resume: that is how a SIGTERM for an incoming call gets
 * an app out of a thirty-second send. But EINTR does not say WHICH signal
 * arrived, and an app that spawns a child -- Tones, MusicPlayer, Browser --
 * may one day have the SIGCHLD reaper spec-apps-core.md R12 asks for. Aborting
 * every send because an aplay exited would be a bug that only appeared on the
 * phone, and only sometimes.
 *
 * So the question asked is not "was I interrupted" but "am I being torn
 * down": honour_exit callers give up only when nd_app_should_exit() is true,
 * which is set by the SIGTERM handler and by nothing else. Every other signal
 * resumes the wait, still bounded by the same deadline. The CORE passes
 * false -- a SIGCHLD landing on the service thread is not a reason to stop
 * serving. */
static svc_rx svc_recv(int fd, void *buf, size_t buf_sz, size_t want, double deadline,
                       bool honour_exit)
{
    for (;;) {
        struct pollfd pfd;
        int rc;
        ssize_t n;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        rc = poll(&pfd, 1u, svc_ms_until(deadline));
        if (rc < 0) {
            if (errno == EINTR) {
                if (honour_exit && nd_app_should_exit())
                    return SVC_RX_INTR;
                continue;
            }
            return SVC_RX_ERROR;
        }
        if (rc == 0)
            return SVC_RX_TIMEOUT;

        n = recv(fd, buf, buf_sz, 0);
        if (n == 0)
            return SVC_RX_EOF;
        if (n < 0) {
            if (errno == EINTR) {
                if (honour_exit && nd_app_should_exit())
                    return SVC_RX_INTR;
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return SVC_RX_ERROR;
        }
        if ((size_t)n != want)
            return SVC_RX_ERROR;
        return SVC_RX_OK;
    }
}

/* ------------------------------------------------------------------ *
 * Validation -- everything the CHILD sends, before it reaches a service
 * ------------------------------------------------------------------ */

/* A NUL inside the array, so every later strlen/strcpy on it is bounded. */
static bool bounded_string(const char *s, size_t cap)
{
    return memchr(s, '\0', cap) != NULL;
}

/* Attacker-influenced text that ends up inside AT+CMGS="<number>"\r.
 * REJECTED, not filtered: do_send_sms() filters it too, and the core must not
 * be one refactor of a downstream filter away from letting an app write
 * "\r\nATH\r\n into the command stream. */
static bool valid_number(const char *s)
{
    size_t i;

    if (s[0] == '\0')
        return false;
    for (i = 0u; s[i] != '\0'; i++) {
        if (i >= ND_MODEM_NUMBER_MAX - 1u)
            return false;
        if (s[i] == '+') {
            if (i != 0u)
                return false; /* a plus anywhere but the front is not a number */
            continue;
        }
        if (strchr("0123456789*#", s[i]) == NULL)
            return false;
    }
    return true;
}

/* Well-formed UTF-8, refusing overlongs, surrogates and anything past
 * U+10FFFF. The composer only ever produces UTF-8, so a legitimate request
 * always passes; the point is that ascii_replace()'s decoder in the modem
 * layer is never handed bytes it can be spared. */
static bool valid_utf8(const char *s, size_t len)
{
    size_t i = 0u;

    while (i < len) {
        uint8_t c = (uint8_t)s[i];
        size_t extra;
        uint32_t cp;
        size_t k;

        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            extra = 1u;
            cp = (uint32_t)(c & 0x1F);
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2u;
            cp = (uint32_t)(c & 0x0F);
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3u;
            cp = (uint32_t)(c & 0x07);
        } else {
            return false; /* a continuation byte, or an overlong/oversized lead */
        }
        if (i + extra >= len)
            return false; /* the continuation bytes run off the end */
        for (k = 1u; k <= extra; k++) {
            uint8_t cc = (uint8_t)s[i + k];

            if ((cc & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (uint32_t)(cc & 0x3F);
        }
        if (extra == 2u && cp < 0x800u)
            return false;
        if (extra == 3u && cp < 0x10000u)
            return false;
        if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
            return false;
        i += extra + 1u;
    }
    return true;
}

static bool valid_request(const svc_req *r)
{
    if (r->magic != SVC_MAGIC || r->version != SVC_VERSION || r->size != (uint32_t)sizeof *r)
        return false;
    if (!bounded_string(r->number, sizeof r->number) || !bounded_string(r->text, sizeof r->text))
        return false;

    switch (r->op) {
    case SVC_OP_SEND_SMS:
        if (!valid_number(r->number))
            return false;
        if (r->text_len == 0u || r->text_len >= (uint32_t)sizeof r->text)
            return false;
        if (r->text[r->text_len] != '\0' || strlen(r->text) != (size_t)r->text_len)
            return false; /* an embedded NUL, or a length that lied */
        return valid_utf8(r->text, (size_t)r->text_len);
    case SVC_OP_MODEM_STATUS:
    case SVC_OP_BATTERY:
    case SVC_OP_BATTERY_QUICKSTART:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ *
 * The core's side: serving one request
 * ------------------------------------------------------------------ */

static void resp_init(svc_resp *out, uint32_t op)
{
    memset(out, 0, sizeof *out);
    out->magic = SVC_MAGIC;
    out->version = SVC_VERSION;
    out->size = (uint32_t)sizeof *out;
    out->op = op;
    out->status = SVC_ST_OK;
    /* The "nothing is known" snapshot, so a caller that ignores the return
     * still renders "--" and not uninitialised memory. */
    out->modem.signal_level = -1;
    out->modem.csq_rssi = -1;
    out->modem.reg_stat = -1;
    out->modem.call_secs = -1;
    out->battery.crate = NAN;
}

/* CALLED ONLY AFTER valid_request(). Every string in `req` is NUL-terminated
 * inside its array and the number is drawn from [0-9*#+] alone, which is what
 * makes the log line below safe to print and the AT command safe to build. */
static void serve(nd_ui *ui, const svc_req *req, svc_resp *out)
{
    resp_init(out, req->op);

    switch (req->op) {
    case SVC_OP_SEND_SMS:
        if (ui->modem == NULL) {
            out->status = SVC_ST_NO_SERVICE;
            return;
        }
        out->present = 1u;
        nd_log(ND_LOG_OS, "App service: send_sms to %s (%u bytes)", req->number,
               (unsigned)req->text_len);
        out->ok =
            nd_modem_send_sms(ui->modem, req->number, req->text, out->detail, sizeof out->detail)
                ? 1u
                : 0u;
        return;

    case SVC_OP_MODEM_STATUS:
        if (ui->modem == NULL) {
            out->status = SVC_ST_NO_SERVICE;
            return;
        }
        out->present = 1u;
        nd_modem_status_snapshot(ui->modem, &out->modem);
        out->ok = 1u;
        return;

    case SVC_OP_BATTERY:
        if (ui->battery == NULL) {
            out->status = SVC_ST_NO_SERVICE;
            return;
        }
        /* `present` means the core HAS a BatteryService. Whether that
         * service found a chip on the bus is a different answer and rides in
         * `hardware`, because FuelGauge draws them differently. */
        out->present = 1u;
        /* FuelGauge renders strerror() of the errno debug_snapshot() left
         * behind, and errno does not cross a socket -- so it is read here and
         * carried explicitly. */
        errno = 0;
        out->ok = nd_battery_debug_snapshot(ui->battery, &out->battery) ? 1u : 0u;
        out->err = (int32_t)errno;
        out->hardware = nd_battery_has_hardware(ui->battery) ? 1u : 0u;
        out->have_vcell = nd_battery_vcell(ui->battery, &out->vcell) ? 1u : 0u;
        out->level = nd_battery_level(ui->battery);
        return;

    case SVC_OP_BATTERY_QUICKSTART:
    default:
        if (ui->battery == NULL) {
            out->status = SVC_ST_NO_SERVICE;
            return;
        }
        out->present = 1u;
        out->ok = nd_battery_quickstart(ui->battery) ? 1u : 0u;
        nd_log(ND_LOG_BATT, "App service: quick-start %s", out->ok ? "sent" : "FAILED");
        return;
    }
}

/* ------------------------------------------------------------------ *
 * The core's side: the server object and its thread
 * ------------------------------------------------------------------ */

struct nd_svc_server {
    int fd;       /* our end; the thread owns it once started */
    int child_fd; /* the child's end, until we close it       */
    nd_ui *ui;
    pthread_t tid;
    bool started;

    /* mu guards all three flags and is what makes the stop path race-free.
     * A plain volatile would be a data race under TSAN and, worse, would
     * make the free-vs-detach handover below unprovable. */
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool quit;      /* the stopper wants the thread to leave      */
    bool finished;  /* the thread has left its loop               */
    bool abandoned; /* the stopper gave up: the thread frees this */
};

static void server_destroy(nd_svc_server *s)
{
    if (s->fd >= 0)
        (void)close(s->fd);
    if (s->child_fd >= 0)
        (void)close(s->child_fd);
    (void)pthread_cond_destroy(&s->cv);
    (void)pthread_mutex_destroy(&s->mu);
    free(s);
}

static void *svc_thread(void *arg)
{
    nd_svc_server *s = arg;
    bool abandoned;

    for (;;) {
        char buf[REQ_BUF_SZ];
        svc_req req;
        svc_resp resp;
        svc_rx rx;
        bool stop;

        (void)pthread_mutex_lock(&s->mu);
        stop = s->quit;
        (void)pthread_mutex_unlock(&s->mu);
        if (stop)
            break;

        /* A short slice, so a stop is noticed promptly even on a platform
         * where shutdown() does not wake a blocked poll. */
        rx = svc_recv(s->fd, buf, sizeof buf, sizeof req, svc_now() + ND_SVC_POLL_S, false);
        if (rx == SVC_RX_TIMEOUT)
            continue;
        if (rx != SVC_RX_OK)
            break; /* the child has gone, or sent something malformed */

        memcpy(&req, buf, sizeof req);
        if (!valid_request(&req)) {
            nd_log_err(ND_LOG_OS, "App service: refused a malformed request (op %u)",
                       (unsigned)req.op);
            resp_init(&resp, req.op);
            resp.status = SVC_ST_BAD_REQUEST;
        } else {
            serve(s->ui, &req, &resp);
        }
        if (!svc_send(s->fd, &resp, sizeof resp, svc_now() + ND_SVC_TIMEOUT_S))
            break; /* the app stopped listening; waitpid will say why */
    }

    (void)pthread_mutex_lock(&s->mu);
    s->finished = true;
    abandoned = s->abandoned;
    (void)pthread_cond_signal(&s->cv);
    (void)pthread_mutex_unlock(&s->mu);

    /* The handover: whoever learns last that the other is done owns the
     * free. If the stopper is still waiting it joins and frees; if it gave
     * up and detached us, this is the only thread left that can. */
    if (abandoned)
        server_destroy(s);
    return NULL;
}

nd_err nd_svc_server_open(nd_svc_server **out)
{
    nd_svc_server *s;
    int sv[2] = {-1, -1};

    if (out == NULL)
        return ND_ERR_INVAL;
    *out = NULL;

    /* SOCK_SEQPACKET: the kernel keeps the message boundaries, so a child
     * that writes half a request cannot desynchronise the core's parser and
     * there is no length prefix for it to lie in. */
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
        nd_log_err(ND_LOG_OS, "App service: socketpair: %s", strerror(errno));
        return ND_ERR_IO;
    }

    /* owned by the caller; released by nd_svc_server_stop() or _free() */
    s = calloc(1u, sizeof *s);
    if (s == NULL) {
        (void)close(sv[0]);
        (void)close(sv[1]);
        return ND_ERR_NOMEM;
    }
    s->fd = sv[0];
    s->child_fd = sv[1];
    if (pthread_mutex_init(&s->mu, NULL) != 0) {
        (void)close(sv[0]);
        (void)close(sv[1]);
        free(s);
        return ND_ERR_IO;
    }
    if (pthread_cond_init(&s->cv, NULL) != 0) {
        (void)pthread_mutex_destroy(&s->mu);
        (void)close(sv[0]);
        (void)close(sv[1]);
        free(s);
        return ND_ERR_IO;
    }
    *out = s;
    return ND_OK;
}

int nd_svc_server_child_fd(const nd_svc_server *s)
{
    return s != NULL ? s->child_fd : -1;
}

nd_err nd_svc_server_start(nd_svc_server *s, nd_ui *ui)
{
    if (s == NULL || ui == NULL)
        return ND_ERR_INVAL;

    /* Ours goes now: while we hold the child's end open, a child that exits
     * would not close the socket and the thread's recv would never see EOF. */
    if (s->child_fd >= 0) {
        (void)close(s->child_fd);
        s->child_fd = -1;
    }
    s->ui = ui;
    {
        pthread_attr_t attr;
        bool have_attr = pthread_attr_init(&attr) == 0;
        int rc;

        /* Set explicitly, exactly as nd_modem_priv.h does and for the same
         * reason: musl's default thread stack is 128 KB against glibc's
         * 8 MB (MUSL.md), and a difference that large must never be able to
         * become a confusing crash on the target only. One iteration of the
         * loop holds a request, a response and a receive buffer -- about
         * 2.6 KB -- and the deepest thing below it is one nd_log() line
         * buffer, so 128 KB is more than an order of magnitude of slack. */
        if (have_attr)
            (void)pthread_attr_setstacksize(&attr, ND_SVC_STACK_BYTES);
        rc = pthread_create(&s->tid, have_attr ? &attr : NULL, svc_thread, s);
        if (have_attr)
            (void)pthread_attr_destroy(&attr);

        if (rc != 0) {
            /* pthread_create RETURNS the error; it does not set errno. */
            nd_log_err(ND_LOG_OS, "App service: cannot start the serving thread: %s", strerror(rc));
            return ND_ERR_IO;
        }
    }
    s->started = true;
    return ND_OK;
}

void nd_svc_server_free(nd_svc_server *s)
{
    if (s == NULL)
        return;
    if (s->started) {
        nd_svc_server_stop(s);
        return;
    }
    server_destroy(s);
}

void nd_svc_server_stop(nd_svc_server *s)
{
    struct timespec deadline;
    bool finished;

    if (s == NULL)
        return;
    if (!s->started) {
        server_destroy(s);
        return;
    }

    (void)pthread_mutex_lock(&s->mu);
    s->quit = true;
    (void)pthread_mutex_unlock(&s->mu);
    /* Wakes a poll or a recv that is already blocked, so the common case
     * costs nothing at all rather than one poll slice. */
    (void)shutdown(s->fd, SHUT_RDWR);

    /* CLOCK_REALTIME because that is the base pthread_cond_timedwait() uses
     * by default; the fraction is carried so that changing ND_SVC_JOIN_S to
     * something that is not a whole number does not silently round it away. */
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)ND_SVC_JOIN_S;
    deadline.tv_nsec += (long)((ND_SVC_JOIN_S - (double)(time_t)ND_SVC_JOIN_S) * 1e9);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec += 1;
    }

    (void)pthread_mutex_lock(&s->mu);
    while (!s->finished) {
        if (pthread_cond_timedwait(&s->cv, &s->mu, &deadline) != 0)
            break;
    }
    finished = s->finished;
    if (!finished)
        s->abandoned = true;
    (void)pthread_mutex_unlock(&s->mu);

    if (finished) {
        (void)pthread_join(s->tid, NULL);
        server_destroy(s);
        return;
    }

    /* Still inside a request -- in practice only a send already on the wire,
     * which cannot be aborted anyway. Blocking the core for up to
     * thirty-seven seconds to watch it finish is the freeze this design
     * exists to avoid, so it is detached and frees itself. */
    nd_log(ND_LOG_OS, "App service: a request is still running; leaving it to finish");
    (void)pthread_detach(s->tid);
}

/* ------------------------------------------------------------------ *
 * The app's side: one process-global channel
 * ------------------------------------------------------------------ *
 *
 * Process-global for the reason nd_ui.c already gives g_sim and
 * g_ring_seen_at: there is exactly one nd_ui per process, so state with
 * nowhere to live in the frozen struct lives beside it rather than widening
 * it. An app is single-threaded through this path -- it is blocked on the
 * answer, by construction.
 */

static int g_client_fd = -1;

static int env_fd(const char *name)
{
    const char *s = getenv(name);
    char *end = NULL;
    long v;

    if (s == NULL || s[0] == '\0')
        return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0 || v > 1000000L)
        return -1;
    return (int)v;
}

void nd_svc_client_open_from_env(void)
{
    int fd = env_fd(ND_ENV_SERVICE_FD);

    nd_svc_client_close();
    if (fd < 0)
        return; /* a hand-run nd-apprun, or nd-shoot: no core to ask */
    if (fcntl(fd, F_GETFD) < 0) {
        nd_log_err(ND_LOG_OS, "%s=%d is not a descriptor", ND_ENV_SERVICE_FD, fd);
        return;
    }
    g_client_fd = fd;
}

void nd_svc_client_close(void)
{
    if (g_client_fd >= 0)
        (void)close(g_client_fd);
    g_client_fd = -1;
}

bool nd_svc_client_active(void)
{
    return g_client_fd >= 0;
}

/* Post one request and wait for its answer. On any transport failure the
 * channel is CLOSED rather than retried: a peer that has gone or a stream
 * that has lost its framing will not recover, and an app that kept trying
 * would spend its whole life in here. */
static svc_status svc_call(uint32_t op, const char *number, const char *text, svc_resp *out,
                           double timeout_s)
{
    svc_req req;
    char buf[RESP_BUF_SZ];
    svc_rx rx;

    resp_init(out, op);
    out->status = SVC_ST_UNAVAILABLE;
    if (g_client_fd < 0)
        return SVC_ST_UNAVAILABLE;

    /* memset first: no uninitialised stack byte crosses the boundary. */
    memset(&req, 0, sizeof req);
    req.magic = SVC_MAGIC;
    req.version = SVC_VERSION;
    req.size = (uint32_t)sizeof req;
    req.op = op;
    /* Both are refused locally when they do not fit rather than truncated:
     * half a phone number is a different phone number, and half a message is
     * the tail-dropping OPEN-QUESTIONS.md C-2 already ruled against. */
    if (number != NULL && nd_strlcpy(req.number, number, sizeof req.number) >= sizeof req.number) {
        out->status = SVC_ST_BAD_REQUEST;
        return SVC_ST_BAD_REQUEST;
    }
    if (text != NULL) {
        size_t n = nd_strlcpy(req.text, text, sizeof req.text);

        if (n >= sizeof req.text) {
            out->status = SVC_ST_BAD_REQUEST;
            return SVC_ST_BAD_REQUEST;
        }
        req.text_len = (uint32_t)n;
    }

    if (!svc_send(g_client_fd, &req, sizeof req, svc_now() + ND_SVC_TIMEOUT_S)) {
        nd_log_err(ND_LOG_OS, "App service: cannot reach the core: %s", strerror(errno));
        nd_svc_client_close();
        return SVC_ST_UNAVAILABLE;
    }

    rx = svc_recv(g_client_fd, buf, sizeof buf, sizeof *out, svc_now() + timeout_s, true);
    if (rx != SVC_RX_OK) {
        if (rx == SVC_RX_EOF)
            nd_log_err(ND_LOG_OS, "App service: the core closed the channel");
        else if (rx == SVC_RX_TIMEOUT)
            nd_log_err(ND_LOG_OS, "App service: no answer in %.0fs", timeout_s);
        else if (rx == SVC_RX_INTR)
            nd_log(ND_LOG_OS, "App service: abandoning a request; this app is shutting down");
        nd_svc_client_close();
        resp_init(out, op);
        out->status = SVC_ST_UNAVAILABLE;
        return SVC_ST_UNAVAILABLE;
    }

    memcpy(out, buf, sizeof *out);
    if (out->magic != SVC_MAGIC || out->version != SVC_VERSION ||
        out->size != (uint32_t)sizeof *out || out->op != op) {
        nd_log_err(ND_LOG_OS, "App service: the core answered something else");
        nd_svc_client_close();
        resp_init(out, op);
        out->status = SVC_ST_UNAVAILABLE;
        return SVC_ST_UNAVAILABLE;
    }
    /* The response is untrusted too: re-terminate every string in it. */
    out->detail[sizeof out->detail - 1u] = '\0';
    out->modem.port[sizeof out->modem.port - 1u] = '\0';
    out->modem.imei[sizeof out->modem.imei - 1u] = '\0';
    out->modem.operator_name[sizeof out->modem.operator_name - 1u] = '\0';
    out->modem.caller_id[sizeof out->modem.caller_id - 1u] = '\0';
    out->modem.probe_why[sizeof out->modem.probe_why - 1u] = '\0';
    out->modem.sim_state[sizeof out->modem.sim_state - 1u] = '\0';
    out->modem.reg_cause[sizeof out->modem.reg_cause - 1u] = '\0';
    out->modem.cell_mode[sizeof out->modem.cell_mode - 1u] = '\0';
    return (svc_status)out->status;
}

/* ------------------------------------------------------------------ *
 * What the three apps call
 * ------------------------------------------------------------------ */

/* nd_strlcpy() dereferences dst whenever dst_sz > 0, and `detail` is
 * documented as optional. */
static void set_detail(char *detail, size_t detail_sz, const char *s)
{
    if (detail != NULL && detail_sz > 0u)
        (void)nd_strlcpy(detail, s, detail_sz);
}

bool nd_svc_modem_present(const nd_ui *ui)
{
    svc_resp resp;

    if (ui != NULL && ui->modem != NULL)
        return true;
    if (svc_call(SVC_OP_MODEM_STATUS, NULL, NULL, &resp, ND_SVC_TIMEOUT_S) != SVC_ST_OK)
        return false;
    return resp.present != 0u;
}

bool nd_svc_send_sms(const nd_ui *ui, const char *number, const char *text, char *detail,
                     size_t detail_sz)
{
    svc_resp resp;
    svc_status st;

    if (detail != NULL && detail_sz > 0u)
        detail[0] = '\0';
    if (number == NULL || text == NULL)
        return false;

    if (ui != NULL && ui->modem != NULL)
        return nd_modem_send_sms(ui->modem, number, text, detail, detail_sz);

    st = svc_call(SVC_OP_SEND_SMS, number, text, &resp, ND_SVC_SMS_TIMEOUT_S);
    switch (st) {
    case SVC_ST_OK:
        /* The modem's own wording, verbatim, including on success. */
        set_detail(detail, detail_sz, resp.detail);
        return resp.ok != 0u;
    case SVC_ST_NO_SERVICE:
        set_detail(detail, detail_sz, "no modem in the core");
        return false;
    case SVC_ST_BAD_REQUEST:
        set_detail(detail, detail_sz, "the core refused the request");
        return false;
    case SVC_ST_UNAVAILABLE:
    default:
        set_detail(detail, detail_sz,
                   nd_svc_client_active() ? "no answer from the core" : "core service is gone");
        return false;
    }
}

bool nd_svc_modem_status(const nd_ui *ui, nd_modem_status *out)
{
    svc_resp resp;

    if (out == NULL)
        return false;
    if (ui != NULL && ui->modem != NULL) {
        nd_modem_status_snapshot(ui->modem, out);
        return true;
    }
    /* nd_modem_status_snapshot(NULL, out) is the documented way to get the
     * "nothing is known" snapshot, and it is what the failure path owes its
     * caller. */
    nd_modem_status_snapshot(NULL, out);
    if (svc_call(SVC_OP_MODEM_STATUS, NULL, NULL, &resp, ND_SVC_TIMEOUT_S) != SVC_ST_OK)
        return false;
    if (resp.present == 0u)
        return false;
    *out = resp.modem;
    return true;
}

bool nd_svc_battery_present(const nd_ui *ui)
{
    svc_resp resp;

    if (ui != NULL && ui->battery != NULL)
        return true;
    if (svc_call(SVC_OP_BATTERY, NULL, NULL, &resp, ND_SVC_TIMEOUT_S) != SVC_ST_OK)
        return false;
    return resp.present != 0u;
}

bool nd_svc_battery_read(const nd_ui *ui, nd_svc_battery *out)
{
    svc_resp resp;

    if (out == NULL)
        return false;
    memset(out, 0, sizeof *out);
    out->snap.crate = NAN;

    if (ui != NULL && ui->battery != NULL) {
        errno = 0;
        out->ok = nd_battery_debug_snapshot(ui->battery, &out->snap);
        out->err = (int32_t)errno;
        out->hardware = nd_battery_has_hardware(ui->battery);
        out->have_vcell = nd_battery_vcell(ui->battery, &out->vcell);
        out->level = nd_battery_level(ui->battery);
        return true;
    }

    if (svc_call(SVC_OP_BATTERY, NULL, NULL, &resp, ND_SVC_TIMEOUT_S) != SVC_ST_OK)
        return false;
    if (resp.present == 0u)
        return false;
    out->ok = resp.ok != 0u;
    out->err = resp.err;
    out->hardware = resp.hardware != 0u;
    out->have_vcell = resp.have_vcell != 0u;
    out->vcell = resp.vcell;
    out->level = resp.level;
    out->snap = resp.battery;
    return true;
}

bool nd_svc_battery_quickstart(const nd_ui *ui)
{
    svc_resp resp;

    if (ui != NULL && ui->battery != NULL)
        return nd_battery_quickstart(ui->battery);
    if (svc_call(SVC_OP_BATTERY_QUICKSTART, NULL, NULL, &resp, ND_SVC_TIMEOUT_S) != SVC_ST_OK)
        return false;
    return resp.ok != 0u;
}

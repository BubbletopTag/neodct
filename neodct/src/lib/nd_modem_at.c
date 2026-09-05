/* nd_modem_at.c -- the AT engine: termios, flock, the line reader, transact.
 *
 * A port of the bottom half of System/core/ModemService/__init__.py: the part
 * that knows about a serial port and a line-oriented protocol, and nothing
 * about calls, SMS or the phone. Keeping the split means the whole of this
 * file can be exercised against a pty with no modem present, which is what
 * test/unit/test_modem.c does.
 *
 * ============ WHY flock(2) AND NOT fcntl LOCKS ============
 *
 * /tmp/neodct-modem.lock is shared with two shell programs -- the S45modem
 * boot script and the `atcmd` helper -- and both take it with busybox `flock`,
 * which is flock(2). POSIX record locks (fcntl F_SETLK) live in a completely
 * separate namespace: two processes using different families both "succeed"
 * and then talk over each other on the same port. It has to be flock(2).
 *
 * ============ THREE THINGS THAT LOOK WRONG AND ARE NOT ============
 *
 *  1. c_cflag is ASSIGNED, not OR-ed. The Python writes
 *     attrs[2] = CS8|CREAD|CLOCAL, which clears the parity, stop-bit and
 *     flow-control bits the port came up with. cfsetspeed() afterwards puts
 *     the baud bits back.
 *  2. A URC that arrives mid-command is handled AND still appended to the
 *     collected lines. That double handling is how AT+CEREG?'s own reply gets
 *     parsed into _reg_stat: the reply looks exactly like the URC.
 *  3. The final result line is NOT appended to the collected lines, but
 *     "NO CARRIER" / "BUSY" / "NO ANSWER" are additionally routed through the
 *     URC handler on the way out, because a call collapsing during a command
 *     is a state change as well as a result code.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "nd_log.h"
#include "nd_modem_priv.h"
#include "nd_paths.h"
#include "nd_types.h"

/* FINAL_CODES, verbatim and in source order. */
static const char *const FINAL_CODES[] = {
    "OK", "ERROR", "NO CARRIER", "NO DIALTONE", "BUSY", "NO ANSWER",
};

/* URC_PREFIXES, verbatim and in source order. */
static const char *const URC_PREFIXES[] = {
    "RING",   "+CLIP:",  "VOICE CALL:", "MISSED_CALL:", "NO CARRIER",
    "+CMTI:", "+CEREG:", "+CREG:",      "+CPIN:",       "+SIMCARD:",
};

/* ------------------------------------------------------------------ *
 * Time
 * ------------------------------------------------------------------ */

double nd_modem__now(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void nd_modem__nap(double seconds)
{
    struct timespec ts;

    if (seconds <= 0.0)
        return;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    if (ts.tv_nsec < 0)
        ts.tv_nsec = 0;
    if (ts.tv_nsec > 999999999L)
        ts.tv_nsec = 999999999L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

/* ------------------------------------------------------------------ *
 * The collected-line pool
 * ------------------------------------------------------------------ */

void nd_modem__lines_reset(nd_lines *l)
{
    if (l == NULL)
        return;
    l->used = 0u;
    l->n = 0u;
    l->truncated = false;
}

void nd_modem__lines_add(nd_lines *l, const char *line)
{
    size_t len;

    if (l == NULL || line == NULL)
        return;

    len = strlen(line);
    if (l->n >= ND_MODEM_LINES_MAX || l->used + len + 1u > sizeof l->pool) {
        /* The Python appends without a bound; see OPEN-QUESTIONS.md M-5. */
        l->truncated = true;
        return;
    }
    l->off[l->n] = (uint16_t)l->used;
    memcpy(&l->pool[l->used], line, len + 1u);
    l->used += len + 1u;
    l->n++;
}

const char *nd_modem__lines_get(const nd_lines *l, size_t i)
{
    if (l == NULL || i >= l->n)
        return NULL;
    return &l->pool[l->off[i]];
}

/* ------------------------------------------------------------------ *
 * Small parsers
 * ------------------------------------------------------------------ */

/* Python's str.strip() with no argument. For a string that came out of
 * decode("ascii","replace") the only characters that can be whitespace are
 * these ten -- everything above 0x7F has already become U+FFFD, which is not
 * whitespace. */
static bool is_py_space(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' ||
           c == 0x1cu || c == 0x1du || c == 0x1eu || c == 0x1fu;
}

size_t nd_modem__decode_line(const uint8_t *raw, size_t len, char *out, size_t out_sz)
{
    size_t start = 0u;
    size_t end = len;
    size_t w = 0u;
    size_t i;

    if (out == NULL || out_sz == 0u)
        return 0u;
    out[0] = '\0';
    if (raw == NULL)
        return 0u;

    /* strip() first: only ASCII can be whitespace, so doing it before the
     * decode gives the same answer and never splits a replacement character. */
    while (start < end && is_py_space(raw[start]))
        start++;
    while (end > start && is_py_space(raw[end - 1u]))
        end--;

    for (i = start; i < end; i++) {
        if (raw[i] < 0x80u) {
            if (w + 1u >= out_sz)
                break;
            out[w++] = (char)raw[i];
        } else {
            /* decode("ascii", "replace"): the ASCII codec's unit is one byte,
             * so EVERY byte over 0x7F becomes its own U+FFFD. */
            if (w + 3u >= out_sz)
                break;
            out[w++] = (char)0xefu;
            out[w++] = (char)0xbfu;
            out[w++] = (char)0xbdu;
        }
    }
    out[w] = '\0';
    return w;
}

bool nd_modem__parse_int(const char *s, int32_t *out)
{
    const char *p = s;
    bool neg = false;
    bool any = false;
    long long v = 0;

    if (s == NULL || out == NULL)
        return false;

    while (*p != '\0' && is_py_space((uint8_t)*p))
        p++;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        any = true;
        v = v * 10 + (*p - '0');
        if (v > 2147483647LL)
            return false; /* Python has bignums; an AT field that big is junk */
        p++;
    }
    while (*p != '\0' && is_py_space((uint8_t)*p))
        p++;
    if (!any || *p != '\0')
        return false;

    *out = (int32_t)(neg ? -v : v);
    return true;
}

bool nd_modem__parse_hex(const char *s, int32_t *out)
{
    const char *p = s;
    bool neg = false;
    bool any = false;
    long long v = 0;

    if (s == NULL || out == NULL)
        return false;

    while (*p != '\0' && is_py_space((uint8_t)*p))
        p++;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }
    /* int(x, 16) accepts the 0x prefix as well as bare digits. */
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        p += 2;
    for (;;) {
        int d;

        if (*p >= '0' && *p <= '9')
            d = *p - '0';
        else if (*p >= 'a' && *p <= 'f')
            d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F')
            d = *p - 'A' + 10;
        else
            break;
        any = true;
        v = v * 16 + d;
        if (v > 2147483647LL)
            return false;
        p++;
    }
    while (*p != '\0' && is_py_space((uint8_t)*p))
        p++;
    if (!any || *p != '\0')
        return false;

    *out = (int32_t)(neg ? -v : v);
    return true;
}

bool nd_modem__is_final(const char *line)
{
    size_t i;

    if (line == NULL)
        return false;
    for (i = 0u; i < ND_ARRAY_LEN(FINAL_CODES); i++) {
        if (strcmp(line, FINAL_CODES[i]) == 0)
            return true;
    }
    return strncmp(line, "+CME ERROR", 10u) == 0 || strncmp(line, "+CMS ERROR", 10u) == 0;
}

bool nd_modem__is_urc(const char *line)
{
    size_t i;

    if (line == NULL)
        return false;
    for (i = 0u; i < ND_ARRAY_LEN(URC_PREFIXES); i++) {
        if (strncmp(line, URC_PREFIXES[i], strlen(URC_PREFIXES[i])) == 0)
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ *
 * The lock
 * ------------------------------------------------------------------ */

bool nd_modem__acquire(nd_modem *m)
{
    if (m == NULL)
        return false;
    if (m->lock_fd < 0)
        return true; /* no lock file: serialise with nobody, but keep working */
    if (flock(m->lock_fd, LOCK_EX | LOCK_NB) != 0)
        return false; /* S45modem or atcmd owns the port right now */
    m->lock_held = true;
    return true;
}

void nd_modem__release(nd_modem *m)
{
    if (m == NULL || m->lock_fd < 0)
        return;
    (void)flock(m->lock_fd, LOCK_UN);
    m->lock_held = false;
}

/* ------------------------------------------------------------------ *
 * The port
 * ------------------------------------------------------------------ */

int nd_modem__open_port(const char *dev)
{
    char resolved[ND_PATH_MAX];
    struct termios t;
    int fd;

    if (nd_path_resolve(resolved, sizeof resolved, dev) != ND_OK) {
        errno = ENAMETOOLONG;
        return -1;
    }

    /* O_CLOEXEC because Python 3 sets it on every os.open() (PEP 446) and the
     * modem must not leak the port into aplay. */
    fd = open(resolved, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;

    /* close() may set errno of its own, and the caller prints ours to say why
     * a port was rejected -- so save it across the teardown on both paths. */
    if (tcgetattr(fd, &t) != 0) {
        int saved = errno;

        (void)close(fd);
        errno = saved;
        return -1;
    }
    t.c_iflag = 0;
    t.c_oflag = 0;
    t.c_cflag = CS8 | CREAD | CLOCAL; /* assignment, not |=; see the header */
    t.c_lflag = 0;
    (void)cfsetispeed(&t, ND_MODEM_BAUD);
    (void)cfsetospeed(&t, ND_MODEM_BAUD);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        int saved = errno;

        (void)close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------ *
 * Reading
 * ------------------------------------------------------------------ */

static void rx_append(nd_modem *m, const uint8_t *chunk, size_t n)
{
    if (m->rx_len + n > sizeof m->rx) {
        /* R-7: drop the whole buffer and resynchronise at the next newline.
         * Anything that has gone 8 KB without one is not an AT session. */
        if (!m->rx_overflow_logged) {
            nd_log_err(ND_LOG_MODEM, "rx buffer over %u bytes with no line end; discarding",
                       (unsigned)sizeof m->rx);
            m->rx_overflow_logged = true;
        }
        m->rx_len = 0u;
    }
    if (n > sizeof m->rx)
        n = sizeof m->rx;
    memcpy(&m->rx[m->rx_len], chunk, n);
    m->rx_len += n;
}

/* Split every COMPLETE line out of the rx buffer. The trailing partial line
 * stays put, which is the whole reason the buffer exists. */
static void rx_split(nd_modem *m, nd_lines *out)
{
    size_t consumed = 0u;

    for (;;) {
        uint8_t *nl = memchr(&m->rx[consumed], '\n', m->rx_len - consumed);
        char line[ND_MODEM_LINE_MAX];
        size_t raw_len;

        if (nl == NULL)
            break;
        raw_len = (size_t)(nl - &m->rx[consumed]);
        if (nd_modem__decode_line(&m->rx[consumed], raw_len, line, sizeof line) > 0u)
            nd_modem__lines_add(out, line);
        consumed += raw_len + 1u;
    }
    if (consumed > 0u) {
        memmove(m->rx, &m->rx[consumed], m->rx_len - consumed);
        m->rx_len -= consumed;
    }
}

size_t nd_modem__read_pending(nd_modem *m, nd_lines *out)
{
    uint8_t chunk[ND_MODEM_READ_CHUNK];

    nd_modem__lines_reset(out);
    if (m == NULL || m->fd < 0)
        return 0u;

    for (;;) {
        ssize_t n = read(m->fd, chunk, sizeof chunk);

        if (n == 0)
            break; /* Python: `if not chunk: break` */
        if (n < 0) {
            if (errno == EINTR)
                continue; /* PEP 475: Python retries this one for you */
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            {
                char why[ND_MODEM_WHY_MAX];

                (void)snprintf(why, sizeof why, "port read failed: %s", strerror(errno));
                nd_modem__drop_hardware(m, why);
            }
            nd_modem__lines_reset(out);
            return 0u;
        }
        rx_append(m, chunk, (size_t)n);
    }

    rx_split(m, out);
    return out->n;
}

/* ------------------------------------------------------------------ *
 * Writing
 * ------------------------------------------------------------------ */

/* On failure `why` says what went wrong, in the words drop_hardware() will
 * print. */
static bool port_write(nd_modem *m, const void *data, size_t len, char *why, size_t why_sz)
{
    const uint8_t *p = data;
    size_t left = len;
    double stalled_since = 0.0;

    while (left > 0u) {
        ssize_t n = write(m->fd, p, left);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* The Python's single os.write() would have raised
                 * BlockingIOError here and been caught as an OSError, taking
                 * the port down. A modem that has asserted flow control for
                 * a moment is not a modem that has gone away, so this waits
                 * the same 20 ms the transact loop uses and tries again --
                 * but not for ever. A modem whose USB stack has wedged
                 * answers EAGAIN indefinitely, and the UI thread is blocked
                 * in dial() or hangup() behind this loop; retrying without a
                 * bound was the phone freezing solid. ND_MODEM_WRITE_STALL_S
                 * is the bound, and running into it is the port failing.
                 * Noted in OPEN-QUESTIONS.md M-6. */
                double now = nd_modem__now();

                if (stalled_since == 0.0) {
                    stalled_since = now;
                } else if (now - stalled_since >= ND_MODEM_WRITE_STALL_S) {
                    (void)snprintf(why, why_sz, "port write stalled for %.0fs",
                                   now - stalled_since);
                    return false;
                }
                nd_modem__nap(ND_TRANSACT_SLEEP_S);
                continue;
            }
            (void)snprintf(why, why_sz, "port write failed: %s", strerror(errno));
            return false;
        }
        stalled_since = 0.0; /* progress: the clock starts over */
        p += (size_t)n;
        left -= (size_t)n;
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * transact
 * ------------------------------------------------------------------ */

bool nd_modem__transact(nd_modem *m, const char *cmd, double timeout, char *final_out,
                        size_t final_sz, nd_lines *lines_out)
{
    char wire[ND_MODEM_LINE_MAX + 2];
    nd_lines *coll;
    double deadline;
    int n;
    size_t i;

    if (final_out != NULL && final_sz > 0u)
        final_out[0] = '\0';
    coll = (lines_out != NULL) ? lines_out : &m->collected;
    nd_modem__lines_reset(coll);

    if (m->fd < 0)
        return false;

    /* Stale URCs first: they are routed to the state machine and DISCARDED,
     * never mixed into this command's reply. */
    (void)nd_modem__read_pending(m, &m->rx_lines);
    for (i = 0u; i < m->rx_lines.n; i++)
        nd_modem__handle_urc(m, nd_modem__lines_get(&m->rx_lines, i));

    n = snprintf(wire, sizeof wire, "%s\r", cmd);
    if (n < 0 || (size_t)n >= sizeof wire) {
        nd_log_err(ND_LOG_MODEM, "AT command longer than %u bytes refused",
                   (unsigned)ND_MODEM_LINE_MAX);
        return false;
    }
    {
        char why[ND_MODEM_WHY_MAX];

        if (!port_write(m, wire, (size_t)n, why, sizeof why)) {
            nd_modem__drop_hardware(m, why);
            nd_modem__lines_reset(coll);
            return false;
        }
    }

    deadline = nd_modem__now() + timeout;
    while (nd_modem__now() < deadline) {
        (void)nd_modem__read_pending(m, &m->rx_lines);
        for (i = 0u; i < m->rx_lines.n; i++) {
            const char *line = nd_modem__lines_get(&m->rx_lines, i);

            if (nd_modem__is_final(line)) {
                if (strcmp(line, "NO CARRIER") == 0 || strcmp(line, "BUSY") == 0 ||
                    strcmp(line, "NO ANSWER") == 0)
                    nd_modem__handle_urc(m, line);
                if (final_out != NULL && final_sz > 0u)
                    (void)nd_strlcpy(final_out, line, final_sz);
                /* THE ONE PLACE THE WATCHDOG IS FED. Every AT exchange in the
                 * system funnels through this return, so "the modem answered
                 * something, anything, recently" is exactly this timestamp.
                 * Note it is a FINAL LINE and not an OK: "+CME ERROR: 10" is
                 * a modem that is alive and disagreeing, which is health, not
                 * fault. nd_modem.h's ND_MODEM_FAULT_AFTER_S. */
                nd_modem__lock(m);
                m->last_ok_at = nd_modem__now();
                nd_modem__unlock(m);
                return true;
            }
            if (nd_modem__is_urc(line))
                nd_modem__handle_urc(m, line);
            nd_modem__lines_add(coll, line);
        }
        /* read_pending() drops the modem on a hard read error, and the
         * Python would then spin here until the deadline collecting nothing.
         * Leaving early returns the same (None, collected) sooner. */
        if (m->fd < 0)
            return false;
        nd_modem__nap(ND_TRANSACT_SLEEP_S);
    }
    return false;
}

bool nd_modem__command(nd_modem *m, const char *cmd, double timeout, char *final_out,
                       size_t final_sz, nd_lines *lines_out)
{
    bool got;

    if (final_out != NULL && final_sz > 0u)
        final_out[0] = '\0';
    nd_modem__lines_reset(lines_out != NULL ? lines_out : &m->collected);

    if (!m->hardware)
        return false;
    if (!nd_modem__acquire(m))
        return false;
    got = nd_modem__transact(m, cmd, timeout, final_out, final_sz, lines_out);
    nd_modem__release(m);
    return got;
}

/* ------------------------------------------------------------------ *
 * The CMGS prompt
 * ------------------------------------------------------------------ */

/* The '>' arrives with no newline, so the line-based reader would wait for
 * ever. This is the one place that reads the raw buffer directly. */
bool nd_modem__wait_sms_prompt(nd_modem *m, double timeout)
{
    double deadline = nd_modem__now() + timeout;

    while (nd_modem__now() < deadline) {
        uint8_t chunk[ND_MODEM_PROMPT_CHUNK];
        ssize_t n = read(m->fd, chunk, sizeof chunk);

        if (n > 0)
            rx_append(m, chunk, (size_t)n);
        else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return false;

        /* Complete lines are errors or URCs; the prompt never ends one. */
        for (;;) {
            uint8_t *nl = memchr(m->rx, '\n', m->rx_len);
            char line[ND_MODEM_LINE_MAX];
            size_t raw_len;

            if (nl == NULL)
                break;
            raw_len = (size_t)(nl - m->rx);
            (void)nd_modem__decode_line(m->rx, raw_len, line, sizeof line);
            memmove(m->rx, &m->rx[raw_len + 1u], m->rx_len - raw_len - 1u);
            m->rx_len -= raw_len + 1u;

            if (strcmp(line, "ERROR") == 0 || strncmp(line, "+CMS ERROR", 10u) == 0 ||
                strncmp(line, "+CME ERROR", 10u) == 0)
                return false;
            if (nd_modem__is_urc(line))
                nd_modem__handle_urc(m, line);
        }

        if (memchr(m->rx, '>', m->rx_len) != NULL) {
            m->rx_len = 0u; /* swallow the prompt and its trailing space */
            return true;
        }
        nd_modem__nap(ND_PROMPT_SLEEP_S);
    }
    return false;
}

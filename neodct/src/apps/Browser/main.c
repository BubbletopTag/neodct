/* apps/Browser/main.c -- the launcher around netsurf-fb.
 *
 * A one-to-one port of System/apps/Browser/main.py (261 lines). App id 11,
 * stock menu. SESSION-SCOPE.md keeps this one out of the stub set: netsurf is
 * already a standalone program, so the app is a launcher -- build the command
 * line, hand over the screen, run it, take the screen back.
 *
 * ============ IT IS netsurf-fb, NOT CAGE AND NOT WEBKIT ============
 *
 * ARCHITECTURE.md says the browser is "WebKitGTK running under the cage
 * compositor". It is not, and spec-apps-core.md section 11 records the
 * correction: both defconfigs set BR2_PACKAGE_NETSURF=y and
 * BR2_PACKAGE_NETSURF_FRAMEBUFFER=y, and there is no cage or webkit package
 * in either. /usr/bin/netsurf-fb draws straight to /dev/fb0 and reads evdev
 * itself.
 *
 * ============ THE PIPE MUST BE DRAINED WHILE NETSURF IS ALIVE ============
 *
 * main.py's own comment: "Popen, not run(): the pipe has to be drained while
 * netsurf is alive or it fills and blocks it." netsurf logs every fetch, every
 * SSL complaint and a periodic "neodct-mem:" RSS line; 64 KB of pipe is
 * minutes of browsing, after which the browser stops dead with no diagnostic
 * anywhere. So nd_browser_pump() runs BEFORE the wait and returns only at EOF.
 * There is no thread: a thread reading a pipe would outlive the process it is
 * reading, and this process also has to fork(), which is the one thing you do
 * not want threads around for (CODING-STANDARDS.md 1.1).
 *
 * ============ WHY THE STDERR IS TAGGED AT ALL ============
 *
 * Quoting main.py, because the reasoning is not recoverable from the code:
 * netsurf's stderr "used to go straight to /dev/console untagged, so on a 64MB
 * phone the most useful stream in the system was also the only one you could
 * not tell apart from kernel noise". Everything now goes through the pump:
 * tagged [Browser], purple, with the CPU figure the memory lines were missing.
 *
 * ============ THE ONE STRUCTURAL DIFFERENCE ============
 *
 * In Python the browser ran inside the core, so its key bridge could own a
 * thread and scan the i2c expander directly. Apps are separate processes now
 * and none of them touches the bus -- the core reads the keypad and forwards
 * presses and releases down an inherited pipe. So the bridge here is driven
 * from this launcher's own poll loop, on the same descriptor set as the stderr
 * pump, with no thread anywhere. The bridge OBJECT and every keycode decision
 * inside it are still lib/nd_t9_bridge.c's. See BR-2 in OPEN-QUESTIONS.md.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_input.h"
#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_t9.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"

#include "browser.h"

extern char **environ;

/* 50 ms between wakeups while the browser owns the screen. Short enough that
 * a keypad press reaches netsurf without a perceptible lag, long enough that
 * a browsing session costs no measurable CPU in this process -- which matters,
 * because netsurf wants all of it. */
#define POLL_MS 50

/* One read of netsurf's stderr. Small on purpose: the pump reassembles lines
 * itself and a large buffer only delays the first line reaching the console. */
#define CHUNK 512

/* Belt and braces on the two drain loops. The Python has no bound on either
 * and cannot hang because os.read raises at EOF; these cannot hang either, but
 * a launcher that can spin forever on a misbehaving descriptor is not worth
 * the two lines it takes to prevent. */
#define DRAIN_MAX_READS 1024
#define DRAIN_MAX_KEYS  64

/* The pump's own cap is not the Python's 64: this one runs for a whole
 * browsing session and a burst of held-key repeats can be longer than that
 * between two polls. It exists only to bound the loop, not to shape it. */
#define PUMP_MAX_KEYS 256

/* The pid of the netsurf child, so app_shutdown() can kill it when an
 * incoming call arrives. sig_atomic_t because the SIGTERM path reads it; 0
 * means there is no child. See the teardown contract in nd_app.h. */
static volatile sig_atomic_t g_netsurf_pid;

/* ------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------ */

static bool write_all(int fd, const char *buf, size_t len)
{
    size_t done = 0u;

    while (done < len) {
        ssize_t n = write(fd, buf + done, len - done);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        done += (size_t)n;
    }
    return true;
}

/* ASCII lowercase into a caller buffer. Python's str.lower() is Unicode-aware,
 * but every string in _ERROR_HINTS is ASCII and a non-ASCII byte can never
 * match one, so the two agree on the only question being asked. */
static void ascii_lower(char *out, size_t out_sz, const char *in)
{
    size_t i;

    if (out_sz == 0u)
        return;
    for (i = 0u; in[i] != '\0' && i + 1u < out_sz; i++)
        out[i] = (in[i] >= 'A' && in[i] <= 'Z') ? (char)(in[i] + 32) : in[i];
    out[i] = '\0';
}

/* ------------------------------------------------------------------ *
 * UTF-8 with replacement -- Python's .decode("utf-8", "replace")
 * ------------------------------------------------------------------ */
/*
 * netsurf's stderr is ASCII in practice, but "in practice" is not a reason to
 * hand an arbitrary byte stream from another program to the serial console
 * unchecked. This is the WHATWG / Unicode "maximal subpart" algorithm, which
 * is the one CPython's decoder implements: an ill-formed sequence is replaced
 * by ONE U+FFFD covering its longest well-formed prefix, not one per byte.
 *
 * The single deviation is NUL, which Python decodes to U+0000 and passes
 * through. A C string cannot carry it, so it becomes U+FFFD here. Recorded as
 * BR-5.
 */

static void put_replacement(char *out, size_t out_sz, size_t *o)
{
    static const char fffd[3] = {'\xEF', '\xBF', '\xBD'};
    size_t i;

    for (i = 0u; i < sizeof fffd; i++) {
        if (*o + 1u < out_sz)
            out[(*o)++] = fffd[i];
    }
}

static void sanitise_utf8(char *out, size_t out_sz, const char *in, size_t len)
{
    size_t i = 0u;
    size_t o = 0u;

    if (out_sz == 0u)
        return;

    while (i < len) {
        uint8_t b = (uint8_t)in[i];
        size_t need;
        uint8_t lo = 0x80u;
        uint8_t hi = 0xBFu;
        size_t k;
        bool ok = true;

        if (b != 0u && b < 0x80u) {
            if (o + 1u < out_sz)
                out[o++] = (char)b;
            i++;
            continue;
        }

        /* The lead-byte table, with the overlong, surrogate and >U+10FFFF
         * exclusions folded into the first continuation byte's range. */
        if (b >= 0xC2u && b <= 0xDFu) {
            need = 1u;
        } else if (b == 0xE0u) {
            need = 2u;
            lo = 0xA0u;
        } else if (b >= 0xE1u && b <= 0xECu) {
            need = 2u;
        } else if (b == 0xEDu) {
            need = 2u;
            hi = 0x9Fu;
        } else if (b >= 0xEEu && b <= 0xEFu) {
            need = 2u;
        } else if (b == 0xF0u) {
            need = 3u;
            lo = 0x90u;
        } else if (b >= 0xF1u && b <= 0xF3u) {
            need = 3u;
        } else if (b == 0xF4u) {
            need = 3u;
            hi = 0x8Fu;
        } else {
            /* NUL, a stray continuation byte, 0xC0/0xC1, or 0xF5..0xFF. */
            put_replacement(out, out_sz, &o);
            i++;
            continue;
        }

        for (k = 0u; k < need; k++) {
            uint8_t c;
            uint8_t clo = (k == 0u) ? lo : 0x80u;
            uint8_t chi = (k == 0u) ? hi : 0xBFu;

            if (i + 1u + k >= len) {
                ok = false;
                break;
            }
            c = (uint8_t)in[i + 1u + k];
            if (c < clo || c > chi) {
                ok = false;
                break;
            }
        }

        if (!ok) {
            put_replacement(out, out_sz, &o);
            i += 1u + k; /* the maximal well-formed subpart, consumed once */
            continue;
        }

        for (k = 0u; k < need + 1u; k++) {
            if (o + 1u < out_sz)
                out[o++] = in[i + k];
        }
        i += need + 1u;
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------------ *
 * _describe_exit
 * ------------------------------------------------------------------ */

/* main.py's _SIGNAL_NOTES. Three entries and no more: every other signal gets
 * the bare form. */
static const struct {
    int sig;
    const char *note;
} SIGNAL_NOTES[] = {
    {6, "SIGABRT"},
    {9, "SIGKILL, possible OOM"},
    {11, "SIGSEGV"},
};

int nd_browser_returncode(const nd_proc_status *st)
{
    if (st == NULL)
        return 0;
    if (st->signalled)
        return -st->signo;
    return st->exit_status;
}

size_t nd_browser_describe_exit(char *out, size_t out_sz, int returncode)
{
    int sig;
    size_t i;
    int n;

    if (out == NULL || out_sz == 0u)
        return 0u;

    if (returncode == 0)
        n = snprintf(out, out_sz, "neodct-browser: exited normally");
    else if (returncode > 0)
        n = snprintf(out, out_sz, "neodct-browser: exited with code %d", returncode);
    else {
        sig = -returncode;
        n = snprintf(out, out_sz, "neodct-browser: KILLED by signal %d", sig);
        for (i = 0u; i < ND_ARRAY_LEN(SIGNAL_NOTES); i++) {
            if (SIGNAL_NOTES[i].sig == sig) {
                n = snprintf(out, out_sz, "neodct-browser: KILLED by signal %d (%s)", sig,
                             SIGNAL_NOTES[i].note);
                break;
            }
        }
    }
    return (n < 0) ? 0u : (size_t)n;
}

/* ------------------------------------------------------------------ *
 * _classify and _tagged
 * ------------------------------------------------------------------ */

/* main.py: "Substrings that mark a line as a failure rather than chatter.
 * Kept broad on purpose -- a missed error is worse than a line painted too
 * brightly." Note "timed out" carries its space. */
static const char *const ERROR_HINTS[] = {
    "ssl",    "tls",     "certificate", "handshake", "verify", "error", "failed",
    "cannot", "refused", "timed out",   "unable",    "denied", "abort",
};

#define MEM_PREFIX "neodct-mem:"

/* A sanitised line can be three times its raw length, because every ill-formed
 * byte becomes a three-byte U+FFFD. The lowercase scratch has to cover that or
 * a long line would be classified on its first kilobyte only. */
#define LOWER_MAX (ND_BROWSER_LINE_MAX * 3 + 1)

bool nd_browser_is_mem_line(const char *line)
{
    char low[LOWER_MAX];

    if (line == NULL)
        return false;
    ascii_lower(low, sizeof low, line);
    return strncmp(low, MEM_PREFIX, sizeof MEM_PREFIX - 1u) == 0;
}

int nd_browser_classify(const char *line)
{
    char low[LOWER_MAX];
    size_t i;

    if (line == NULL)
        return ND_BROWSER_COLOUR_PLAIN;

    ascii_lower(low, sizeof low, line);

    /* The order is the Python's and it is load-bearing: a memory line is
     * plain even when it says "error", and an error hint beats a URL, so a
     * failing navigation comes out red rather than blue. */
    if (strncmp(low, MEM_PREFIX, sizeof MEM_PREFIX - 1u) == 0)
        return ND_BROWSER_COLOUR_PLAIN;
    for (i = 0u; i < ND_ARRAY_LEN(ERROR_HINTS); i++) {
        if (strstr(low, ERROR_HINTS[i]) != NULL)
            return ND_BROWSER_COLOUR_ERROR;
    }
    if (strstr(low, "http://") != NULL || strstr(low, "https://") != NULL)
        return ND_BROWSER_COLOUR_URL;
    return ND_BROWSER_COLOUR_PLAIN;
}

size_t nd_browser_tagged(char *out, size_t out_sz, const char *body, int code)
{
    char painted[ND_LOG_TAG_MAX + 32];
    int n;

    if (out == NULL || out_sz == 0u)
        return 0u;
    if (body == NULL)
        body = "";

    (void)nd_log_paint(painted, sizeof painted, "[" ND_BROWSER_TAG "]", code, true);
    /* The space is the Python's " " between paint() and body: it belongs to
     * the remainder, not to the painted tag, so it is never coloured. */
    n = snprintf(out, out_sz, "%s %s", painted, body);
    return (n < 0) ? 0u : (size_t)n;
}

void nd_browser_log_console(const char *text)
{
    char path[ND_PATH_MAX];
    int fd;

    if (text == NULL)
        return;
    if (nd_path_resolve(path, sizeof path, ND_BROWSER_CONSOLE) != ND_OK)
        return;

    /* Opened and closed per call, unbuffered, as the Python's
     * `with open(CONSOLE, "wb", buffering=0)` does. O_APPEND rather than that
     * call's O_CREAT|O_TRUNC: on a character device the two are the same
     * syscall, and O_APPEND is the only spelling that also behaves when a host
     * test points ND_ROOT at a directory where "the console" is a file. BR-9.
     * Every failure is swallowed -- a phone with no console must still
     * browse. */
    fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0)
        return;
    /* Every failure is swallowed, as the Python's bare except does. */
    if (write_all(fd, text, strlen(text)))
        (void)write_all(fd, "\r\n", 2u);
    (void)close(fd);
}

/* ------------------------------------------------------------------ *
 * _CpuSampler
 * ------------------------------------------------------------------ */

void nd_browser_cpu_init(nd_browser_cpu *c, pid_t pid)
{
    if (c == NULL)
        return;
    memset(c, 0, sizeof *c);
    c->pid = pid;
}

/* utime and stime, fields 14 and 15 of /proc/<pid>/stat.
 *
 * BUG PRESERVED ON PURPOSE. main.py's comment says to index from the closing
 * ")" because comm may itself contain spaces, and then indexes the plain
 * whitespace split at [13]/[14] anyway. "netsurf-fb" has no space in it, so
 * the code works and the comment describes a fix that was never applied.
 * spec-apps-core.md says port it as-is; this does. BR-1.
 */
static bool read_busy_ticks(pid_t pid, unsigned long long *out)
{
    char virt[ND_PATH_MAX];
    char path[ND_PATH_MAX];
    char buf[512];
    FILE *f;
    size_t n;
    size_t i;
    int field = 0;
    unsigned long long utime = 0u;
    unsigned long long stime = 0u;
    bool have_u = false;
    bool have_s = false;

    if (nd_snprintf(virt, sizeof virt, "/proc/%ld/stat", (long)pid) != ND_OK)
        return false;
    if (nd_path_resolve(path, sizeof path, virt) != ND_OK)
        return false;

    f = fopen(path, "rb");
    if (f == NULL)
        return false;
    n = fread(buf, 1u, sizeof buf - 1u, f);
    (void)fclose(f);
    buf[n] = '\0';

    /* Python's bytes.split() with no argument: runs of whitespace, leading
     * whitespace ignored, so field 0 is the pid. */
    i = 0u;
    while (i < n) {
        size_t start;

        while (i < n && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r'))
            i++;
        if (i >= n)
            break;
        start = i;
        while (i < n && buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\n' && buf[i] != '\r')
            i++;
        if (field == 13 || field == 14) {
            char tok[32];
            char *end = NULL;
            unsigned long long v;
            size_t len = i - start;

            if (len == 0u || len >= sizeof tok)
                return false;
            memcpy(tok, buf + start, len);
            tok[len] = '\0';
            errno = 0;
            v = strtoull(tok, &end, 10);
            if (errno != 0 || end == tok || *end != '\0')
                return false; /* Python's ValueError */
            if (field == 13) {
                utime = v;
                have_u = true;
            } else {
                stime = v;
                have_s = true;
            }
        }
        field++;
        if (field > 14)
            break;
    }

    if (!have_u || !have_s)
        return false; /* Python's IndexError */
    *out = utime + stime;
    return true;
}

bool nd_browser_cpu_percent_at(nd_browser_cpu *c, double now, double *out)
{
    unsigned long long busy = 0u;
    double elapsed;
    double ticks;

    if (c == NULL || out == NULL)
        return false;
    if (!read_busy_ticks(c->pid, &busy))
        return false;

    if (!c->have_last) {
        c->have_last = true;
        c->last_busy = busy;
        c->last_now = now;
        return false; /* Python returns None on the first call */
    }

    elapsed = now - c->last_now;
    {
        unsigned long long prev = c->last_busy;

        c->last_busy = busy;
        c->last_now = now;
        if (elapsed <= 0.0)
            return false;
        /* os.sysconf("SC_CLK_TCK"), with the Python's 100 fallback for a
         * platform that does not have it. */
        {
            long tck = sysconf(_SC_CLK_TCK);

            ticks = (tck > 0) ? (double)tck : 100.0;
        }
        *out = 100.0 * ((double)busy - (double)prev) / (ticks * elapsed);
    }
    return true;
}

bool nd_browser_cpu_percent(nd_browser_cpu *c, double *out)
{
    return nd_browser_cpu_percent_at(c, nd_time_monotonic(), out);
}

/* ------------------------------------------------------------------ *
 * _pump_browser_log
 * ------------------------------------------------------------------ */

static void emit_line(int console_fd, nd_browser_cpu *cpu, const char *raw, size_t len)
{
    char clean[ND_BROWSER_LINE_MAX * 3 + 1];
    char body[sizeof clean + 32];
    char out[sizeof body + 96];
    size_t i;
    bool blank = true;
    int code;
    double pct = 0.0;

    /* rstrip("\r\n") */
    while (len > 0u && (raw[len - 1u] == '\r' || raw[len - 1u] == '\n'))
        len--;

    /* if not line.strip(): continue -- Python's str.strip() with no argument
     * strips whitespace, so a line of spaces and tabs is skipped too. */
    for (i = 0u; i < len; i++) {
        char ch = raw[i];

        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\v' && ch != '\f') {
            blank = false;
            break;
        }
    }
    if (blank)
        return;

    sanitise_utf8(clean, sizeof clean, raw, len);

    code = nd_browser_classify(clean);
    (void)nd_strlcpy(body, clean, sizeof body);

    if (nd_browser_is_mem_line(body) && cpu != NULL && nd_browser_cpu_percent(cpu, &pct)) {
        /* Fold CPU in beside the memory the browser already reports, so one
         * line answers "is it thrashing or is it spinning?". */
        char suffix[32];

        (void)snprintf(suffix, sizeof suffix, " cpu=%.0f%%", pct);
        (void)nd_strlcat(body, suffix, sizeof body);
    }

    if (console_fd < 0)
        return;

    (void)nd_browser_tagged(out, sizeof out, body, code);
    if (!write_all(console_fd, out, strlen(out)))
        return;
    (void)write_all(console_fd, "\r\n", 2u);
}

/* Hand whatever the core has queued to the bridge, so netsurf sees it as
 * keyboard input. With no bridge the presses are still consumed: the channel
 * is a pipe with a finite buffer at the far end of which the core is writing,
 * and a browsing session is long enough to fill it.
 *
 * read_EVENT, not read_key. nd_input_read_key(in, 0.0) returns ND_KEY_NONE as
 * soon as it consumes a RELEASE, because its zero deadline has already passed
 * by then -- so a loop that stops at the first NONE stops after the first
 * press of every press/release pair. The Python could use read_key here
 * because the i2c scanner it read reported no releases at all; the channel
 * this reads reports both. */
static int pump_keys(nd_input *input, nd_t9_bridge *bridge)
{
    int i;

    if (input == NULL)
        return 0;
    for (i = 0; i < PUMP_MAX_KEYS; i++) {
        nd_key_event ev;

        if (!nd_input_read_event(input, 0.0, &ev))
            break;
        if (ev.pressed && bridge != NULL)
            nd_t9_bridge_handle_code(bridge, ev.code);
    }
    return i;
}

void nd_browser_pump(int stderr_fd, int console_fd, nd_browser_cpu *cpu, nd_input *input,
                     nd_t9_bridge *bridge)
{
    char line[ND_BROWSER_LINE_MAX];
    size_t len = 0u;
    int input_fd = (input != NULL) ? nd_input_fd(input) : -1;

    if (stderr_fd < 0)
        return;

    for (;;) {
        struct pollfd pfd[2];
        nfds_t nfds = 1u;
        int r;

        pfd[0].fd = stderr_fd;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        if (input_fd >= 0) {
            pfd[1].fd = input_fd;
            pfd[1].events = POLLIN;
            pfd[1].revents = 0;
            nfds = 2u;
        }

        r = poll(pfd, nfds, POLL_MS);
        if (r < 0) {
            if (errno == EINTR) {
                /* nd-apprun's SIGTERM handler is installed WITHOUT SA_RESTART
                 * precisely so this returns rather than resuming -- nd_app.h's
                 * teardown contract names this read as the one that has to be
                 * interruptible. */
                if (nd_app_should_exit())
                    return;
                continue;
            }
            break;
        }

        if (nfds == 2u && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            /* A hangup with nothing left to read means the core has gone.
             * Stop polling the descriptor rather than spinning on it for the
             * rest of the browsing session -- POLLHUP is level-triggered and
             * would otherwise make this loop the busiest thing on the phone. */
            if (pump_keys(input, bridge) == 0 && (pfd[1].revents & (POLLHUP | POLLERR)) != 0)
                input_fd = -1;
        }

        if ((pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            char chunk[CHUNK];
            ssize_t n = read(stderr_fd, chunk, sizeof chunk);
            ssize_t k;

            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (nd_app_should_exit())
                        return;
                    continue;
                }
                break;
            }
            if (n == 0)
                break; /* EOF: netsurf closed its stderr, i.e. it is gone */

            for (k = 0; k < n; k++) {
                if (chunk[k] == '\n') {
                    emit_line(console_fd, cpu, line, len);
                    len = 0u;
                } else if (len < sizeof line) {
                    line[len++] = chunk[k];
                } else {
                    /* A line longer than the buffer: emit what we have and
                     * treat the rest as the next line. Python's readline has
                     * no bound; a fixed buffer is what replaces it. BR-7. */
                    emit_line(console_fd, cpu, line, len);
                    len = 0u;
                    line[len++] = chunk[k];
                }
            }
        }

        if (nd_app_should_exit())
            return;
    }

    if (len > 0u)
        emit_line(console_fd, cpu, line, len); /* a last line with no newline */
}

/* ------------------------------------------------------------------ *
 * _drain_input
 * ------------------------------------------------------------------ */

void nd_browser_drain_input(nd_ui *ui)
{
    int fd;
    int i;

    if (ui == NULL)
        return;

    /* The keypad channel keeps receiving events even while netsurf reads its
     * own devices, so this descriptor has a session's worth of presses behind
     * it that the launcher must not replay as menu actions. */
    fd = ui->keypad_fd;
    if (fd >= 0) {
        for (i = 0; i < DRAIN_MAX_READS; i++) {
            struct pollfd p;
            char buf[4096]; /* the Python's 4096, not the 24 used elsewhere */
            ssize_t n;

            p.fd = fd;
            p.events = POLLIN;
            p.revents = 0;
            if (poll(&p, 1u, 0) <= 0)
                break;
            n = read(fd, buf, sizeof buf);
            if (n <= 0)
                break; /* `if not os.read(...): break`, and OSError -> pass */
        }
    }

    /* Then the decoder's own queue. In the Python this was the i2c scanner's
     * _pending list; here it is whatever nd_input buffered out of the channel.
     * The Python's matrix.read_key(0) becomes read_EVENT here for the reason
     * pump_keys gives: read_key(0) reports NONE the moment it eats a release,
     * so it would leave half a queue behind. */
    if (ui->input != NULL) {
        for (i = 0; i < DRAIN_MAX_KEYS; i++) {
            nd_key_event ev;

            if (!nd_input_read_event(ui->input, 0.0, &ev))
                break;
        }
    }
}

/* ------------------------------------------------------------------ *
 * _dump_dmesg_tail
 * ------------------------------------------------------------------ */

static double now_s(void)
{
    return nd_time_monotonic();
}

void nd_browser_dump_dmesg_tail(int lines)
{
    char path[ND_PATH_MAX];
    const char *argv[2];
    nd_proc_spec spec;
    nd_proc_status st;
    pid_t pid = -1;
    int pipefd[2] = {-1, -1};
    int devnull = -1;
    char *ring = NULL; /* owned here; freed before every return below */
    size_t ring_len = 0u;
    size_t ring_head = 0u;
    size_t cap;
    char cur[ND_BROWSER_LINE_MAX];
    size_t cur_len = 0u;
    bool cur_over = false;
    double deadline;
    bool timed_out = false;
    size_t i;

    if (lines <= 0)
        return;
    cap = (size_t)lines;

    if (!nd_path_exists(ND_BROWSER_DMESG))
        return;
    if (nd_path_resolve(path, sizeof path, ND_BROWSER_DMESG) != ND_OK)
        return;

    ring = malloc(cap * ND_BROWSER_LINE_MAX);
    if (ring == NULL)
        return;

    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        free(ring);
        return;
    }

    {
        char devnull_path[ND_PATH_MAX];

        if (nd_path_resolve(devnull_path, sizeof devnull_path, "/dev/null") == ND_OK)
            devnull = open(devnull_path, O_WRONLY | O_CLOEXEC);
    }

    /* argv[0] is "dmesg", which is what subprocess.run(["dmesg"]) passes. */
    argv[0] = "dmesg";
    argv[1] = NULL;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.envp = NULL; /* inherit, as subprocess.run does */
    spec.owner = ND_OWNER_SYSTEM;
    spec.fds[spec.n_fds].child_fd = 1;
    spec.fds[spec.n_fds].our_fd = pipefd[1];
    spec.n_fds++;
    if (devnull >= 0) {
        /* capture_output also captures stderr; the Python then ignores it. */
        spec.fds[spec.n_fds].child_fd = 2;
        spec.fds[spec.n_fds].our_fd = devnull;
        spec.n_fds++;
    }

    if (nd_proc_spawn(path, &spec, &pid) != ND_OK) {
        (void)close(pipefd[0]);
        (void)close(pipefd[1]);
        if (devnull >= 0)
            (void)close(devnull);
        free(ring);
        return;
    }

    (void)close(pipefd[1]);
    pipefd[1] = -1;
    if (devnull >= 0) {
        (void)close(devnull);
        devnull = -1;
    }

    deadline = now_s() + ND_BROWSER_DMESG_TIMEOUT;

    for (;;) {
        struct pollfd p;
        char chunk[CHUNK];
        ssize_t n;
        ssize_t k;
        double remaining = deadline - now_s();

        if (remaining <= 0.0) {
            timed_out = true;
            break;
        }

        p.fd = pipefd[0];
        p.events = POLLIN;
        p.revents = 0;
        n = poll(&p, 1u, (int)(remaining * 1000.0) + 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0) {
            timed_out = true;
            break;
        }

        n = read(pipefd[0], chunk, sizeof chunk);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break; /* EOF */

        for (k = 0; k < n; k++) {
            if (chunk[k] == '\n') {
                char *slot = ring + ((ring_head + ring_len) % cap) * ND_BROWSER_LINE_MAX;

                cur[cur_len] = '\0';
                (void)nd_strlcpy(slot, cur, ND_BROWSER_LINE_MAX);
                if (ring_len < cap)
                    ring_len++;
                else
                    ring_head = (ring_head + 1u) % cap;
                cur_len = 0u;
                cur_over = false;
            } else if (cur_len + 1u < sizeof cur) {
                if (!cur_over)
                    cur[cur_len++] = chunk[k];
            } else {
                cur_over = true; /* truncated; see BR-7 */
            }
        }
    }

    (void)close(pipefd[0]);
    pipefd[0] = -1;

    if (timed_out) {
        /* subprocess.run(timeout=5) kills the child and raises, and the bare
         * except in _dump_dmesg_tail then discards everything read so far. */
        (void)kill(pid, SIGKILL);
        (void)nd_proc_wait(pid, 2.0, &st);
        free(ring);
        return;
    }

    /* A final line with no trailing newline; splitlines() would keep it. */
    if (cur_len > 0u) {
        char *slot = ring + ((ring_head + ring_len) % cap) * ND_BROWSER_LINE_MAX;

        cur[cur_len] = '\0';
        (void)nd_strlcpy(slot, cur, ND_BROWSER_LINE_MAX);
        if (ring_len < cap)
            ring_len++;
        else
            ring_head = (ring_head + 1u) % cap;
    }

    (void)nd_proc_wait(pid, ND_BROWSER_DMESG_TIMEOUT, &st);

    /* Untagged, exactly as the Python: _dump_dmesg_tail calls _log_console
     * directly rather than _tagged, so the kernel's own text reaches the
     * console verbatim. */
    for (i = 0u; i < ring_len; i++)
        nd_browser_log_console(ring + ((ring_head + i) % cap) * ND_BROWSER_LINE_MAX);

    free(ring);
}

/* ------------------------------------------------------------------ *
 * _start_key_bridge
 * ------------------------------------------------------------------ */

bool nd_browser_needs_key_bridge(const nd_ui *ui)
{
    nd_keymap km;

    /* Whenever the framework can answer, believe it. */
    if (ui != NULL && ui->has_matrix_keypad)
        return true;

    /* It cannot yet inside an app process: nd_ui_init_app() derives
     * has_matrix_keypad from the inherited PIPE, which has no matrix by
     * construction. So ask the same file the core asks -- keymap.json exists
     * and names a driver that is present exactly on the keypad-only hardware
     * where netsurf can see no keyboard at all. BR-3. */
    return nd_keymap_load(ND_PATH_KEYMAP, &km) == ND_OK;
}

/* ------------------------------------------------------------------ *
 * run(ui)
 * ------------------------------------------------------------------ */

/* Python's env.copy() plus env.setdefault("HOME", "/NeoDCT/User").
 * owned by the caller; free with free() -- the strings themselves are
 * environ's and a literal, and neither is ours to release. */
static const char **build_envp(void)
{
    static const char home_entry[] = "HOME=" ND_BROWSER_HOME_DIR;
    const char **envp;
    size_t n = 0u;
    size_t i;
    bool have_home = false;

    for (i = 0u; environ[i] != NULL; i++) {
        n++;
        if (strncmp(environ[i], "HOME=", 5u) == 0)
            have_home = true;
    }

    envp = calloc(n + 2u, sizeof *envp);
    if (envp == NULL)
        return NULL;
    for (i = 0u; i < n; i++)
        envp[i] = environ[i];
    if (!have_home)
        envp[n++] = home_entry;
    envp[n] = NULL;
    return envp;
}

static void log_tagged(const char *body, int code)
{
    char out[ND_BROWSER_LINE_MAX + 96];

    (void)nd_browser_tagged(out, sizeof out, body, code);
    nd_browser_log_console(out);
}

int app_run(nd_ui *ui)
{
    char bin[ND_PATH_MAX];
    char console_path[ND_PATH_MAX];
    const char *argv[3];
    const char **envp = NULL;
    nd_proc_spec spec;
    nd_proc_status st;
    nd_browser_cpu cpu;
    nd_uinput_kbd kbd;
    nd_t9_bridge *bridge = NULL;
    bool have_kbd = false;
    pid_t pid = -1;
    int pipefd[2] = {-1, -1};
    int devnull = -1;
    int console_fd = -1;
    int returncode = 0;
    char body[128];

    /* `if not os.path.exists(browser): return` -- silently, with no screen.
     * An image built without BR2_PACKAGE_NETSURF still has the menu entry. */
    if (!nd_path_exists(ND_BROWSER_BIN))
        return 0;
    if (nd_path_resolve(bin, sizeof bin, ND_BROWSER_BIN) != ND_OK)
        return 0;

    /* The bridge goes up BEFORE netsurf starts, so the uinput device exists by
     * the time netsurf enumerates /dev/input. */
    if (nd_browser_needs_key_bridge(ui)) {
        if (nd_uinput_open(&kbd, NULL, NULL) == ND_OK) {
            have_kbd = true;
            /* The browser bridge, not the shell one: netsurf needs arrows to
             * scroll and follow links, the keypad has no Left or Right key at
             * all, so 2/4/6/8 stand in for the d-pad and # reaches text entry
             * for a URL. Built without a thread and without an input source of
             * its own -- this launcher's poll loop is the source. */
            bridge = nd_t9_bridge_new_for_test(ND_BRIDGE_BROWSER, &kbd);
            if (bridge == NULL) {
                nd_uinput_close(&kbd);
                have_kbd = false;
            }
        }
    }

    envp = build_envp();
    if (envp == NULL)
        goto done;

    if (nd_path_resolve(console_path, sizeof console_path, ND_BROWSER_CONSOLE) == ND_OK)
        console_fd = open(console_path, O_WRONLY | O_APPEND | O_CLOEXEC);

    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        nd_log_err(ND_LOG_BROWSER, "pipe2: %s", strerror(errno));
        goto done;
    }

    {
        char devnull_path[ND_PATH_MAX];

        if (nd_path_resolve(devnull_path, sizeof devnull_path, "/dev/null") == ND_OK)
            devnull = open(devnull_path, O_WRONLY | O_CLOEXEC);
    }

    argv[0] = ND_BROWSER_BIN;
    argv[1] = ND_BROWSER_HOME;
    argv[2] = NULL;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.envp = envp;
    spec.owner = ND_OWNER_SYSTEM;
    if (devnull >= 0) {
        spec.fds[spec.n_fds].child_fd = 1; /* stdout=DEVNULL */
        spec.fds[spec.n_fds].our_fd = devnull;
        spec.n_fds++;
    }
    /* stderr through a pipe rather than straight at the console, so every line
     * can be tagged and the memory lines can pick up a CPU figure on the way
     * past. */
    spec.fds[spec.n_fds].child_fd = 2;
    spec.fds[spec.n_fds].our_fd = pipefd[1];
    spec.n_fds++;

    if (nd_proc_spawn(bin, &spec, &pid) != ND_OK)
        goto done;

    g_netsurf_pid = (sig_atomic_t)pid;

    (void)close(pipefd[1]);
    pipefd[1] = -1;
    if (devnull >= 0) {
        (void)close(devnull);
        devnull = -1;
    }

    (void)snprintf(body, sizeof body, "neodct-browser: started pid %ld", (long)pid);
    log_tagged(body, ND_BROWSER_COLOUR_PLAIN);

    nd_browser_cpu_init(&cpu, pid);
    nd_browser_pump(pipefd[0], console_fd, &cpu, ui != NULL ? ui->input : NULL, bridge);
    (void)close(pipefd[0]);
    pipefd[0] = -1;

    memset(&st, 0, sizeof st);
    if (nd_app_should_exit()) {
        /* The phone is ringing. Take the screen back now rather than waiting
         * out a browser that has been told to go and has not. */
        (void)nd_proc_terminate(pid, 1.0, &st);
    } else {
        (void)nd_proc_wait(pid, -1.0, &st);
    }
    g_netsurf_pid = 0;

    returncode = nd_browser_returncode(&st);
    (void)nd_browser_describe_exit(body, sizeof body, returncode);
    log_tagged(body, returncode != 0 ? ND_BROWSER_COLOUR_ERROR : ND_BROWSER_COLOUR_PLAIN);

    if (returncode < 0)
        nd_browser_dump_dmesg_tail(ND_BROWSER_DMESG_LINES);

done:
    if (pipefd[0] >= 0)
        (void)close(pipefd[0]);
    if (pipefd[1] >= 0)
        (void)close(pipefd[1]);
    if (devnull >= 0)
        (void)close(devnull);
    if (console_fd >= 0)
        (void)close(console_fd);
    free((void *)(uintptr_t)(const void *)envp);

    /* Tear the virtual keyboard down before the UI resumes reading the keypad,
     * so nothing double-consumes presses. */
    if (bridge != NULL)
        nd_t9_bridge_free_for_test(bridge);
    if (have_kbd)
        nd_uinput_close(&kbd);

    nd_browser_drain_input(ui);

    /* Repaint the UI over whatever the browser left on the fb. */
    if (ui != NULL && ui->fb != NULL && ui->canvas != NULL)
        (void)nd_ui_present(ui);

    return 0;
}

/* nd_app.h's teardown contract, step 4: release what the process holds and
 * kill any child it spawned. netsurf owns /dev/fb0 -- if it survives, the
 * incoming-call screen is drawn underneath a web page. */
void app_shutdown(void)
{
    pid_t pid = (pid_t)g_netsurf_pid;
    nd_proc_status st;

    if (pid <= 0)
        return;
    g_netsurf_pid = 0;

    (void)kill(pid, SIGTERM);
    /* A moment, and no more: the core is waiting on this process so it can
     * start the ringer. 200 ms is enough for netsurf to unmap the framebuffer
     * and not enough for anyone to hear. */
    if (nd_proc_wait(pid, 0.2, &st) != ND_OK)
        (void)kill(pid, SIGKILL);
}

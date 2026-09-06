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
#include "nd_media.h"
#include "nd_paths.h"
#include "nd_priv.h"
#include "nd_proc.h"
#include "nd_t9.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

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
    if (text == NULL)
        return;

    /* fd 2, not the open("/dev/console") the Python did and the port copied.
     * browser.h has the whole argument -- the node this app can never open,
     * the year of discarded stderr it cost, and why the answer is not a udev
     * rule.
     *
     * Every failure is still swallowed, as the Python's bare except did: a
     * phone whose log has gone must still browse. */
    if (write_all(STDERR_FILENO, text, strlen(text)))
        (void)write_all(STDERR_FILENO, "\r\n", 2u);
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

/* Drain whatever the core has queued on the inherited key channel.
 *
 * ============ IT USED TO TYPE THEM INTO A uinput KEYBOARD ============
 *
 * This launcher owned the bridge: it opened /dev/uinput, built an
 * nd_t9_bridge over it, and typed every press into it so netsurf saw a
 * keyboard. That stopped working the day the core started running this app as
 * ndusr_ut, because /dev/uinput is granted to group ndusr and ndusr_ut is
 * deliberately not in it -- and it must stay that way, since a process that
 * can inject keys on a phone with no compositor can drive the real UI. The
 * open returned EACCES, there was no else branch, and the browser has read
 * and discarded every keypress on hardware ever since.
 *
 * The device now belongs to the core, which is allowed to make one, and this
 * app is handed only the /dev/input/eventN path to pass on. See nd_proc.h's
 * THE KEY DEVICE.
 *
 * THE DRAIN IS STILL NECESSARY and is now the whole job: the channel is a
 * pipe with a finite buffer that the core is writing into, and a browsing
 * session is long enough to fill it.
 *
 * read_EVENT, not read_key. nd_input_read_key(in, 0.0) returns ND_KEY_NONE as
 * soon as it consumes a RELEASE, because its zero deadline has already passed
 * by then -- so a loop that stops at the first NONE stops after the first
 * press of every press/release pair. The Python could use read_key here
 * because the i2c scanner it read reported no releases at all; the channel
 * this reads reports both. */
static int pump_keys(nd_input *input)
{
    int i;

    if (input == NULL)
        return 0;
    for (i = 0; i < PUMP_MAX_KEYS; i++) {
        nd_key_event ev;

        if (!nd_input_read_event(input, 0.0, &ev))
            break;
    }
    return i;
}

void nd_browser_pump(int stderr_fd, int console_fd, nd_browser_cpu *cpu, nd_input *input)
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
            if (pump_keys(input) == 0 && (pfd[1].revents & (POLLHUP | POLLERR)) != 0)
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
    ND_UNUSED(ui);

    /* The core hands the fact down in NEODCT_KEYPAD_MATRIX (nd_app.h). This
     * used to re-read keymap.json instead, because ui->has_matrix_keypad was
     * false in every app process on every device -- BR-3, now closed.
     *
     * Asking the core is not merely tidier, it is more correct: a keymap.json
     * on disk is a CLAIM about the hardware, while this is the backend the
     * core actually opened. If the matrix failed and the core fell back to
     * evdev, the old code would have started a bridge against a keyboard that
     * is already reaching netsurf, and doubled every press.
     *
     * nd_app_keypad_is_matrix() and NOT ui->has_matrix_keypad, even though
     * the second is derived from the first: only the second has the NEODCT_T9
     * override folded into it, and this question is about the hardware. A
     * developer forcing T9 on over a real keyboard must not thereby get a
     * second keyboard bridged on top of it. */
    return nd_app_keypad_is_matrix();
}

/* ------------------------------------------------------------------ *
 * run(ui)
 * ------------------------------------------------------------------ */

/* Python's env.copy() plus env.setdefault("HOME", "/NeoDCT/User"), plus the
 * two names the two programs downstream of us look the key device up under.
 *
 * ============ WHY THE SAME PATH THREE TIMES ============
 *
 * The core publishes one fact -- NEODCT_KEY_EVDEV, "the evdev node I made for
 * you" -- and two independent programs have to be told it in their own
 * vocabulary, neither of which we get to change:
 *
 *   NSFB_INPUT_DEV       libnsfb honours it (src/surface/linux.c) and opens
 *                        exactly that node instead of scanning
 *                        /dev/input/event0..31. Worth setting even when the
 *                        scan would have worked: netsurf takes every EV_KEY
 *                        device it finds, up to eight, so on a machine with a
 *                        real keyboard the scan is also how a browser ends up
 *                        reading the developer's keystrokes. Naming the node
 *                        removes both problems at once.
 *   NEODCT_KEYPAD_DEVICE nd_media.h's override, the highest-priority input to
 *                        nd_media_discover_keypad(). neodct-play is exec'd BY
 *                        netsurf when a <video> is clicked and inherits this
 *                        environment, so mpv gets keys without netsurf
 *                        knowing anything about it. Without it neodct-play
 *                        falls back to scanning /dev/input for a device
 *                        called "neodct-t9-keypad" -- which is how mpv came
 *                        to have no keys at all on hardware, because the
 *                        device with that name was the browser's own bridge
 *                        and it had stopped being creatable.
 *
 * Neither is set when the core made no device, which is the normal case on a
 * machine with a real keyboard: an empty NSFB_INPUT_DEV would stop netsurf
 * finding the keyboard it can see perfectly well.
 *
 * owned by the caller; free with free() -- the strings are environ's, this
 * function's statics and two caller-owned buffers, and none is ours to
 * release. */
static const char **build_envp(char *nsfb_entry, size_t nsfb_sz, char *media_entry, size_t media_sz)
{
    static const char home_entry[] = "HOME=" ND_BROWSER_HOME_DIR;
    const char *node = nd_app_key_evdev();
    const char **envp;
    size_t n = 0u;
    size_t i;
    bool have_home = false;

    nsfb_entry[0] = '\0';
    media_entry[0] = '\0';
    if (node != NULL) {
        if (nd_snprintf(nsfb_entry, nsfb_sz, "%s=%s", ND_BROWSER_NSFB_DEV_ENV, node) != ND_OK)
            nsfb_entry[0] = '\0';
        if (nd_snprintf(media_entry, media_sz, "%s=%s", ND_MEDIA_KEYPAD_DEVICE_ENV, node) != ND_OK)
            media_entry[0] = '\0';
    }

    for (i = 0u; environ[i] != NULL; i++) {
        n++;
        if (strncmp(environ[i], "HOME=", 5u) == 0)
            have_home = true;
    }

    envp = calloc(n + 4u, sizeof *envp);
    if (envp == NULL)
        return NULL;
    n = 0u;
    for (i = 0u; environ[i] != NULL; i++) {
        /* Drop any inherited copy of the two we are about to set, so a stale
         * node path from an earlier launch cannot reach netsurf. */
        if (nsfb_entry[0] != '\0' &&
            strncmp(environ[i], ND_BROWSER_NSFB_DEV_ENV "=", sizeof ND_BROWSER_NSFB_DEV_ENV) == 0)
            continue;
        if (media_entry[0] != '\0' && strncmp(environ[i], ND_MEDIA_KEYPAD_DEVICE_ENV "=",
                                              sizeof ND_MEDIA_KEYPAD_DEVICE_ENV) == 0)
            continue;
        envp[n++] = environ[i];
    }
    if (!have_home)
        envp[n++] = home_entry;
    if (nsfb_entry[0] != '\0')
        envp[n++] = nsfb_entry;
    if (media_entry[0] != '\0')
        envp[n++] = media_entry;
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
    char nsfb_env[ND_PATH_MAX + 32];
    char media_env[ND_PATH_MAX + 32];
    const char *argv[3];
    const char **envp = NULL;
    nd_proc_spec spec;
    nd_proc_status st;
    nd_browser_cpu cpu;
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

    /* ============ REFUSE, DO NOT DEGRADE ============
     *
     * On a phone whose only input is the i2c matrix there is nothing in
     * /dev/input for netsurf to read, so without the core's key device it
     * comes up with no keyboard at all: no scrolling, no link, no URL, and --
     * because every one of netsurf's own quit paths is reached through the
     * keys it does not have -- no way out either. That is not a browser with
     * a limitation, it is a full-screen program the owner cannot dismiss, and
     * for the whole of 0.5.x it looked exactly like a hung phone.
     *
     * So say so and go back to the menu. This is the same judgement the core
     * makes when it cannot confine an untrusted app (nd_proc.c): a thing that
     * cannot be started correctly is not started.
     *
     * The core has already logged WHY at error level -- the manifest, the
     * uinput permission or the udev grant -- so this only has to be the
     * sentence a person reads. */
    if (nd_browser_needs_key_bridge(ui) && nd_app_key_evdev() == NULL) {
        nd_msgdialog d;

        nd_log_err(ND_LOG_BROWSER,
                   "refusing to start netsurf: this phone's keypad is the i2c matrix and no key "
                   "device was made for this app, so the browser would ignore every key and "
                   "could not be closed. See " ND_ENV_KEY_EVDEV " and nd_proc.h.");
        nd_msgdialog_init(&d, ui,
                          "The keypad could not be\nconnected to the browser,\nso it would not "
                          "answer\nany key.\n\nThe log says why.");
        nd_msgdialog_set_title(&d, "Browser");
        (void)nd_msgdialog_show(&d);
        return 0;
    }

    envp = build_envp(nsfb_env, sizeof nsfb_env, media_env, sizeof media_env);
    if (envp == NULL)
        goto done;

    /* fd 2, not an open() of /dev/console -- see nd_browser_log_console(). */
    console_fd = STDERR_FILENO;

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
    {
        /* See ND_BROWSER_HOME_ENV. An empty value is treated as unset, so
         * exporting the variable blank cannot leave netsurf with no URL. */
        const char *want = getenv(ND_BROWSER_HOME_ENV);
        bool overridden = want != NULL && want[0] != '\0';

        argv[1] = overridden ? want : ND_BROWSER_HOME;
        if (overridden)
            nd_log(ND_LOG_BROWSER, "start URL overridden by " ND_BROWSER_HOME_ENV ": %s", argv[1]);
    }
    argv[2] = NULL;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.envp = envp;
    spec.owner = ND_OWNER_SYSTEM;

    /* The browser runs as ndusr_ut. SECURITY-PLAN.md section 1, and the
     * single most valuable line in Phase 1: a NetSurf RCE stops yielding
     * root and starts yielding a process that can browse the web and write
     * to one directory.
     *
     * Concretely, after this it cannot read /NeoDCT/User/db (contacts,
     * messages, the call log), /NeoDCT/User/.remote (the ssh keys) or
     * /NeoDCT/User/.ndsys (the update records), and it cannot open
     * /dev/ttyUSB2 -- so `echo ATD1900555xxxx; > /dev/ttyUSB2`, which
     * SECURITY-AUDIT.md section 4 Q1 answers "yes, trivially", becomes
     * EACCES. It keeps the screen and the keys, because a browser that
     * cannot draw is not a browser; section 2's mount namespace is what
     * narrows THAT, by giving it a /dev with almost nothing in it.
     *
     * The lookup is here, in the parent, because getpwnam allocates and
     * reads a file and neither is allowed after a fork. An image with no
     * ndusr_ut leaves run_as.valid false, which nd_priv_become() treats as a
     * no-op -- the browser then runs exactly as it did before, rather than
     * refusing to open.
     *
     * Everything the browser starts inherits this, which is the point:
     * neodct-play is exec'd BY netsurf when a <video> is clicked, so the
     * media player -- the other half of the plan's untrusted set, and the
     * one an MMS attachment will reach -- is confined by the same line. */
    /* THIS APP NO LONGER DOES THE CONFINING, AND CANNOT.
     *
     * It used to call nd_priv_lookup(ND_PRIV_USER_UT) here and ask for a
     * mount namespace, back when apps were root. Apps are ndusr now, and both
     * of those need capabilities ndusr does not have -- so the drop failed
     * with EPERM inside nd_priv_become() and the child _exit(122)d BEFORE
     * execve. netsurf did not start at all, and the phone said "no mount
     * namespaces in this kernel" on a kernel that has them.
     *
     * The core does it instead, when it launches THIS app: nd_proc.c's
     * UNTRUSTED_APPS runs /NeoDCT/System/apps/Browser as ndusr_ut, inside a
     * namespace with ND_PROC_UNTRUSTED_HIDE_PATHS emptied, and with no
     * service socket. So by the time this code runs the whole process is
     * already the untrusted one, and netsurf inherits all of it across the
     * plain fork below -- uid, namespace, no_new_privs -- with nothing left
     * here to ask for or to get wrong.
     *
     * spec.run_as is deliberately left invalid: nd_priv_become() treats that
     * as a documented no-op, which is exactly right when there is nobody left
     * to become. */

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
    nd_browser_pump(pipefd[0], console_fd, &cpu, ui != NULL ? ui->input : NULL);
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
    /* console_fd is fd 2 and is NOT closed: it is this process's stderr, and
     * the rest of libneodct logs through it after we return. */
    free((void *)(uintptr_t)(const void *)envp);

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

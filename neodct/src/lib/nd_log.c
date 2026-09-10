/* nd_log.c -- the colourful serial log.
 *
 * A one-for-one port of System/core/logstyle.py. The Python wrapped
 * sys.stdout and painted lines as they went past; C has no such seam, so the
 * painting happens here instead and nd_log() produces the same bytes the
 * wrapper would have produced for print("[TAG] ...").
 *
 * The palette, the two derived-colour formulas and the tag-splitting rules
 * are checked against neodct/tests/golden/log/logref.json by
 * test/unit/test_nd_log.c. That file was generated from the running Python,
 * so it is the oracle rather than a restatement of this code.
 *
 * Ordering guarantee callers depend on: a whole line, newline included, is
 * emitted with one fwrite(), so the modem thread and the UI thread cannot
 * interleave halves of a line on a 115200-baud console where that would be
 * very hard to read.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "nd_log.h"
#include "nd_paths.h"

#define ND_ESC_RESET "\033[0m"
#define ND_ESC_BOLD  "\033[1m"

/* Defined with the rest of the system-log sink, far below, because that is
 * where the argument for it belongs. Forward-declared here because the three
 * emitters that call it come first. */
static void syslog_emit(int pri, const char *plain);
#define ND_SYSLOG_PRI_INFO 14 /* user.info */
#define ND_SYSLOG_PRI_ERR  11 /* user.err  */

/* ------------------------------------------------------------------ *
 * Colour on or off
 * ------------------------------------------------------------------ */

/* Two plain bools rather than a lock: both threads that could race compute
 * the same answer from the same environment, so the worst case is that the
 * work is done twice. */
static bool g_colour_resolved;
static bool g_colour_on;

static bool resolve_colour(void)
{
    const char *value;

    /* NO_COLOR set to ANYTHING, including the empty string, disables colour.
     * That is the no-color.org convention and the Python honours it with
     * `is not None`, not with a truthiness test. */
    if (getenv(ND_ENV_NO_COLOR) != NULL)
        return false;

    value = getenv(ND_ENV_COLOR);
    if (value == NULL)
        return true;

    /* Exactly these three, case-sensitively, matching the Python's
     * `not in ("0", "no", "off")`. "OFF" leaves colour on. */
    if (strcmp(value, "0") == 0 || strcmp(value, "no") == 0 || strcmp(value, "off") == 0)
        return false;

    return true;
}

bool nd_log_colour_enabled(void)
{
    if (!g_colour_resolved) {
        g_colour_on = resolve_colour();
        g_colour_resolved = true;
    }
    return g_colour_on;
}

void nd_log_set_colour(bool on)
{
    g_colour_on = on;
    g_colour_resolved = true;
}

/* ------------------------------------------------------------------ *
 * The palette
 * ------------------------------------------------------------------ */

struct tag_colour {
    const char *tag;
    int code;
};

/* logstyle.TAG_COLOURS, in its source order. Linear search over 22 entries is
 * faster than any hash at this size and needs no initialisation. */
static const struct tag_colour NAMED[] = {
    {"MODEM", 39},    {"ndsys", 33},  {"UPDATE", 33},  {"CORE", 46},     {"OS", 46},
    {"Launcher", 82}, {"BATT", 226},  {"FUEL", 226},   {"NOTIFY", 201},  {"INPUT", 51},
    {"KEYMAP", 87},   {"SETUP", 214}, {"UI", 120},     {"FB", 123},      {"KERNEL", 244},
    {"sdcard", 180},  {"CLOCK", 129}, {"RSHELL", 162}, {"Browser", 141}, {"CRASH", 196},
    {"ERROR", 196},   {"FATAL", 196},
};

/* logstyle.APP_TAGS. Membership only -- the colour is derived. */
static const char *const APP_TAGS[] = {"Koki",  "Music",   "CallLog",  "Settings",   "PB",
                                       "Tones", "Games",   "Messages", "Clock",      "Calculator",
                                       "Power", "MicTest", "BtAudio",  "Sleepy",     "Calendar"};

static unsigned tag_char_sum(const char *tag)
{
    unsigned sum = 0u;
    size_t i;

    /* Python sums ord(c) over the tag's characters. Every tag in the project
     * is ASCII, so summing bytes agrees; a non-ASCII tag would differ, and
     * _split_tag would not have recognised it as a tag anyway. */
    for (i = 0u; tag[i] != '\0'; i++)
        sum += (unsigned char)tag[i];

    return sum;
}

int nd_log_colour_for(const char *tag)
{
    size_t i;

    if (tag == NULL)
        return 250;

    for (i = 0u; i < ND_ARRAY_LEN(NAMED); i++) {
        if (strcmp(tag, NAMED[i].tag) == 0)
            return NAMED[i].code;
    }

    for (i = 0u; i < ND_ARRAY_LEN(APP_TAGS); i++) {
        if (strcmp(tag, APP_TAGS[i]) == 0) {
            /* 141..176 walks a purple/pink band so the apps read as a group. */
            return 141 + (int)(tag_char_sum(tag) % 36u);
        }
    }

    /* Anything unregistered gets a stable colour from its own name, avoiding
     * the darkest greys (unreadable) and the reds (reserved for failures).
     * This is why a subsystem added later is consistent from its first boot
     * without anyone having to register it. */
    return 22 + (int)(tag_char_sum(tag) % 180u);
}

/* ------------------------------------------------------------------ *
 * Painting
 * ------------------------------------------------------------------ */

/* snprintf's int return, clamped, so -Wconversion has nothing to complain
 * about and a formatting error cannot become a huge size_t. */
static size_t clamped(int n)
{
    return n < 0 ? (size_t)0 : (size_t)n;
}

size_t nd_log_paint(char *out, size_t out_sz, const char *text, int code, bool bold)
{
    if (out == NULL || out_sz == 0u)
        return 0u;

    if (text == NULL)
        text = "";

    if (!nd_log_colour_enabled())
        return clamped(snprintf(out, out_sz, "%s", text));

    return clamped(snprintf(out, out_sz, "%s\033[38;5;%dm%s" ND_ESC_RESET, bold ? ND_ESC_BOLD : "",
                            code, text));
}

bool nd_log_split_tag(const char *line, char *tag_out, size_t tag_sz, const char **rest_out)
{
    const char *close;
    size_t end;
    size_t len;
    size_t i;

    if (line == NULL || tag_out == NULL || rest_out == NULL || tag_sz == 0u)
        return false;

    if (line[0] != '[')
        return false;

    close = strchr(line, ']');
    if (close == NULL)
        return false;

    /* The Python's `if end < 2: return None`. end is the index of ']', so
     * this rejects "[]" and accepts "[A]" -- a one-character tag IS a tag,
     * despite what the edge-case list in logref.py calls it. */
    end = (size_t)(close - line);
    if (end < 2u)
        return false;

    len = end - 1u;
    if (len >= tag_sz)
        return false;

    for (i = 0u; i < len; i++) {
        int c = (int)(unsigned char)line[1u + i];
        if (isalnum(c) == 0 && c != '_' && c != '-')
            return false;
    }

    memcpy(tag_out, line + 1, len);
    tag_out[len] = '\0';
    *rest_out = close + 1;
    return true;
}

static bool line_is_blank(const char *line)
{
    size_t i;

    for (i = 0u; line[i] != '\0'; i++) {
        if (isspace((int)(unsigned char)line[i]) == 0)
            return false;
    }
    return true;
}

size_t nd_log_render(char *out, size_t out_sz, const char *line)
{
    /* A tag plus the escape sequences around it: 64 + 4 + 11 + 4 is the worst
     * case, so 128 has room to spare and this never allocates. */
    char painted[128];
    char tag[ND_LOG_TAG_MAX];
    const char *rest = NULL;

    if (out == NULL || out_sz == 0u)
        return 0u;

    if (line == NULL)
        line = "";

    /* `if not _ENABLED or not line.strip(): return line`. A whitespace-only
     * line is passed through with its whitespace intact -- that matters
     * because the boot banner contains indented blank lines. */
    if (!nd_log_colour_enabled() || line_is_blank(line))
        return clamped(snprintf(out, out_sz, "%s", line));

    if (!nd_log_split_tag(line, tag, sizeof tag, &rest))
        return clamped(snprintf(out, out_sz, "%s", line));

    /* The brackets are painted WITH the tag; the character after ']' -- almost
     * always a space -- belongs to the unpainted remainder. Getting that
     * boundary wrong shifts every reset sequence by one byte. */
    {
        char bracketed[ND_LOG_TAG_MAX + 4];
        (void)snprintf(bracketed, sizeof bracketed, "[%s]", tag);
        (void)nd_log_paint(painted, sizeof painted, bracketed, nd_log_colour_for(tag), true);
    }

    return clamped(snprintf(out, out_sz, "%s%s", painted, rest));
}

/* ------------------------------------------------------------------ *
 * Emitting
 * ------------------------------------------------------------------ */

static void emit(FILE *stream, const char *rendered)
{
    /* One fwrite for the text and its newline together. Two calls would let
     * another thread's line land between them. */
    char buf[ND_LOG_LINE_MAX + 128];
    size_t n = clamped(snprintf(buf, sizeof buf, "%s\n", rendered));

    if (n >= sizeof buf)
        n = sizeof buf - 1u;

    (void)fwrite(buf, 1u, n, stream);
    (void)fflush(stream);
}

void nd_logv(const char *tag, const char *fmt, va_list ap)
{
    char raw[ND_LOG_LINE_MAX];
    char rendered[ND_LOG_LINE_MAX + 64];
    int head;

    if (tag == NULL)
        tag = "";
    if (fmt == NULL)
        fmt = "";

    head = snprintf(raw, sizeof raw, "[%s] ", tag);
    if (head < 0)
        return;

    if ((size_t)head < sizeof raw)
        (void)vsnprintf(raw + head, sizeof raw - (size_t)head, fmt, ap);

    (void)nd_log_render(rendered, sizeof rendered, raw);
    emit(stdout, rendered);
    /* `raw` and not `rendered`: the log file wants the sentence, not the
     * escape sequences that colour it on a terminal. */
    syslog_emit(ND_SYSLOG_PRI_INFO, raw);
}

void nd_log(const char *tag, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    nd_logv(tag, fmt, ap);
    va_end(ap);
}

void nd_log_errv(const char *tag, const char *fmt, va_list ap)
{
    char raw[ND_LOG_LINE_MAX];
    char painted[ND_LOG_LINE_MAX + 64];
    int head;

    if (tag == NULL)
        tag = "";
    if (fmt == NULL)
        fmt = "";

    head = snprintf(raw, sizeof raw, "[%s] ", tag);
    if (head < 0)
        return;

    if ((size_t)head < sizeof raw)
        (void)vsnprintf(raw + head, sizeof raw - (size_t)head, fmt, ap);

    /* Everything on stderr is a failure worth seeing, and in the Python
     * tracebacks arrive there as untagged lines -- so the WHOLE line is
     * painted red, tag included, and not in bold. */
    (void)nd_log_paint(painted, sizeof painted, raw, ND_LOG_ERROR_COLOUR, false);
    emit(stderr, painted);
    syslog_emit(ND_SYSLOG_PRI_ERR, raw);
}

void nd_log_err(const char *tag, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    nd_log_errv(tag, fmt, ap);
    va_end(ap);
}

void nd_log_line(const char *line)
{
    char rendered[ND_LOG_LINE_MAX + 64];

    (void)nd_log_render(rendered, sizeof rendered, line);
    emit(stdout, rendered);
    syslog_emit(ND_SYSLOG_PRI_INFO, line);
}

/* ------------------------------------------------------------------ *
 * The divider and the banner
 * ------------------------------------------------------------------ */

size_t nd_log_rule(char *out, size_t out_sz, char ch, size_t width, int code)
{
    char bar[ND_LOG_BANNER_COLS + 1];
    size_t i;

    if (out == NULL || out_sz == 0u)
        return 0u;

    if (width > ND_LOG_BANNER_COLS)
        width = ND_LOG_BANNER_COLS;

    for (i = 0u; i < width; i++)
        bar[i] = ch;
    bar[width] = '\0';

    return nd_log_paint(out, out_sz, bar, code, true);
}

size_t nd_log_banner_lines(const char *path, char out[][ND_LOG_BANNER_COLS], size_t max)
{
    char resolved[ND_PATH_MAX];
    FILE *fh;
    size_t n = 0u;

    if (out == NULL || max == 0u)
        return 0u;

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return 0u;

    /* A missing banner is normal -- images without one simply do not have the
     * file -- so this is silent, exactly like the Python's `except OSError`. */
    fh = fopen(resolved, "r");
    if (fh == NULL)
        return 0u;

    while (n < max && fgets(out[n], (int)ND_LOG_BANNER_COLS, fh) != NULL) {
        size_t len = strlen(out[n]);
        while (len > 0u && (out[n][len - 1u] == '\n' || out[n][len - 1u] == '\r'))
            out[n][--len] = '\0';
        n++;
    }

    (void)fclose(fh);

    /* The Python does read().rstrip("\n").split("\n"), so trailing blank lines
     * at the end of the file are dropped but interior ones are kept. */
    while (n > 0u && out[n - 1u][0] == '\0')
        n--;

    return n;
}

/* ------------------------------------------------------------------ *
 * The system log
 * ------------------------------------------------------------------ *
 *
 * ============ WHY THIS EXISTS ============
 *
 * Everything above goes to stdout and stderr, and nd_log_redirect_serial()
 * points both at /dev/ttyFIQ0. On a phone with a serial cable that is the
 * right answer and it is the only answer this had for a long time. On a phone
 * WITHOUT one -- which is every phone that has been closed up, including the
 * one this was written on, whose console pads are unsoldered -- it means the
 * operating system's log goes to a device nobody is reading, and there is no
 * copy of it anywhere. /NeoDCT/User/logs/core.log is 0 bytes on a running
 * phone for exactly this reason: the boot script points nd-core's stderr at
 * it and the dup2 above throws that away a moment later.
 *
 * What that costs is not hypothetical. Bluetooth would not start because
 * /run/dbus did not exist; the daemon said so on a stderr going to a dead
 * port, and finding it took a telnet session and a diagnostic binary
 * cross-compiled for the phone. One line of log would have done it.
 *
 * So every line goes to syslogd as well. busybox syslogd is already running
 * (S01syslogd), it writes /var/log/messages, /var/log is a symlink onto the
 * /tmp tmpfs, and it rotates at 200 KB -- so this is bounded, costs no flash
 * and needs no rotation of its own. And /var/log/messages is the file
 * `ndlink logs` already prints, so the log arrives over the debug link with
 * nothing further to build.
 *
 * ============ WHY NOT syslog(3) ============
 *
 * musl's syslog() sends on a BLOCKING socket. A datagram socket to a syslogd
 * that has stopped reading blocks the sender, and the callers here include
 * the UI thread and the modem thread -- so a wedged log daemon would freeze
 * the phone. That is the same failure the format-card verb was retired for
 * (nd_svc.c), and it is not worth risking for a log line.
 *
 * This sends with MSG_DONTWAIT on a non-blocking socket and DROPS the line if
 * the kernel will not take it. A log that loses a line under pressure is
 * strictly better than a phone that stops.
 *
 * ============ THE WIRE ============
 *
 * "<PRI>ident: text", which is what busybox logger sends and what busybox
 * syslogd parses. No timestamp: syslogd stamps what it receives, and a
 * timestamp from here would be the one this phone has BEFORE the clock
 * service has corrected it. Colour is deliberately not sent -- the escapes
 * are for a terminal and this is a file somebody greps.
 */

/* Written once, before any thread exists, and read without a lock for the
 * same reason nd_svc.c's allowed_ops is: pthread_create() is the barrier.
 * -1 means "not open", which is the state every process is in unless it
 * asked, so nothing changes for a test or a tool that never calls open. */
static int g_syslog_fd = -1;
static char g_syslog_ident[32];

/* The priorities are LOG_USER (1) << 3 | severity, spelled out at the top of
 * this file rather than included from <syslog.h>: the only two this uses are
 * those, and the header would also bring in the blocking syslog(3) that the
 * paragraph above exists to avoid. */
static void syslog_emit(int pri, const char *plain)
{
    char buf[ND_LOG_LINE_MAX + 64];
    int n;

    if (g_syslog_fd < 0 || plain == NULL)
        return;

    n = snprintf(buf, sizeof buf, "<%d>%s: %s", pri, g_syslog_ident, plain);
    if (n <= 0)
        return;
    if ((size_t)n >= sizeof buf)
        n = (int)sizeof buf - 1;

    /* Every error ignored, deliberately and exhaustively: EAGAIN is the drop
     * this is designed around, ENOTCONN is a syslogd that has gone away, and
     * there is nowhere to report either that is not this. Reporting it here
     * would recurse. */
    (void)send(g_syslog_fd, buf, (size_t)n, MSG_DONTWAIT | MSG_NOSIGNAL);
}

nd_err nd_log_syslog_open(const char *ident)
{
    struct sockaddr_un addr;
    char resolved[ND_PATH_MAX];
    int fd;

    if (g_syslog_fd >= 0)
        return ND_OK; /* already open; opening twice is not an error */
    if (ident == NULL || ident[0] == '\0')
        ident = "NeoDCT";

    if (nd_path_resolve(resolved, sizeof resolved, ND_PATH_DEV_LOG) != ND_OK)
        return ND_ERR_TOOLONG;

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    /* nd_strlcpy rather than snprintf, and the length checked from its return
     * value rather than by a strlen() before it.
     *
     * sun_path is 108 bytes and `resolved` is up to ND_PATH_MAX, so the guard
     * is needed either way -- but GCC cannot see a preceding strlen() as
     * proving anything about a later snprintf, and at -O1 and above it fails
     * -Wformat-truncation on the copy. That is what broke `make ASAN=1`,
     * which is built at a higher optimisation level than the default one, so
     * the ordinary build stayed green and only the sanitiser build did not.
     * nd_strlcpy is not a format function and reports the length it wanted,
     * which is the same check in one step. */
    if (nd_strlcpy(addr.sun_path, resolved, sizeof addr.sun_path) >= sizeof addr.sun_path)
        return ND_ERR_TOOLONG;

    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return ND_ERR_IO;

    /* CONNECTED, so every later line is one send() with no address, and so a
     * syslogd that is not there is discovered here -- once, at startup --
     * rather than on every line for the life of the process. */
    if (connect(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof addr) < 0) {
        (void)close(fd);
        return ND_ERR_NOTFOUND;
    }

    (void)snprintf(g_syslog_ident, sizeof g_syslog_ident, "%s", ident);
    g_syslog_fd = fd;
    return ND_OK;
}

void nd_log_syslog_close(void)
{
    if (g_syslog_fd >= 0)
        (void)close(g_syslog_fd);
    g_syslog_fd = -1;
    g_syslog_ident[0] = '\0';
}

bool nd_log_syslog_active(void)
{
    return g_syslog_fd >= 0;
}

/* ------------------------------------------------------------------ *
 * The serial console
 * ------------------------------------------------------------------ */

nd_err nd_log_redirect_serial(char *chosen_out, size_t chosen_sz)
{
    const char *device;
    const char *env;
    int fd;
    nd_err rc = ND_OK;

    /* /dev/ttyFIQ0 is the real Rockchip/Luckfox console; /dev/ttyAMA0 is
     * QEMU's PL011. The environment override is the last resort rather than
     * the first, because on hardware it is usually left over from a QEMU
     * session and pointing at a device that is not there.
     *
     * DELIBERATELY NOT nd_platform.h, and it is worth saying so because the
     * ordering looks exactly like the platform guesses that were converted.
     * It is not one. Everywhere else, access("/dev/ttyFIQ0") was a PROXY for
     * "which machine is this"; here the node being tested IS the node about
     * to be opened, which is the "did this device enumerate" question
     * nd_platform.h says to keep answering with evidence. Asking the flag
     * would be a regression rather than a tidy-up: a hardware kernel built
     * without the FIQ debugger would be told to open a console that is not
     * there, and the phone would lose its serial log -- the same class of
     * fault the flag was introduced to end in nd_crash.c. */
    if (nd_path_exists(ND_PATH_SERIAL_FIQ)) {
        device = ND_PATH_SERIAL_FIQ;
    } else if (nd_path_exists(ND_PATH_SERIAL_AMA)) {
        device = ND_PATH_SERIAL_AMA;
    } else {
        env = getenv(ND_ENV_SERIAL_DEVICE);
        device = (env != NULL && env[0] != '\0') ? env : ND_PATH_SERIAL_AMA;
    }

    if (chosen_out != NULL && chosen_sz > 0u) {
        (void)snprintf(chosen_out, chosen_sz, "%s", device);
    }

    fd = open(device, O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        nd_log_err(ND_LOG_LAUNCHER, "Serial redirect failed for %s: %s", device, strerror(errno));
        rc = ND_ERR_IO;
        goto done;
    }

    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
        nd_log_err(ND_LOG_LAUNCHER, "Serial redirect failed for %s: %s", device, strerror(errno));
        rc = ND_ERR_IO;
        goto done;
    }

    nd_log(ND_LOG_LAUNCHER, "Serial console active: %s", device);

done:
    if (fd >= 0 && fd != STDOUT_FILENO && fd != STDERR_FILENO)
        (void)close(fd);
    return rc;
}

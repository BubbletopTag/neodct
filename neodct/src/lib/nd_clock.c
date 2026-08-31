/* nd_clock.c -- keeping the phone's clock honest.
 *
 * Port of System/core/ClockService/__init__.py. Its docstring is the reason
 * this module exists and is worth restating: a phone with no battery-backed
 * RTC boots at the Unix epoch, and every TLS certificate on the internet has
 * a "not valid before" date, so a clock reading 1970 fails validation on every
 * HTTPS site at once. That is what the browser's privacy warnings actually
 * were.
 *
 * Two mechanisms, because one of them has to work before the network does.
 *
 *   1. A floor, applied SYNCHRONOUSLY at boot with no network at all. The
 *      clock is never allowed to read earlier than the moment this image was
 *      built (version.prop's system.os.buildepoch) or the last time we synced.
 *      The date will be wrong -- it will be the build date -- but wrong in the
 *      one direction that keeps certificates valid, and it costs nothing.
 *
 *   2. SNTP, once there is a route, on a detached thread. Written here rather
 *      than pulled in as a package: busybox has no ntpd (only rdate, whose
 *      protocol is long dead) and a whole NTP daemon is a lot of image for a
 *      phone that needs one query per boot. The wire format is 48 bytes and
 *      the useful part is one 64-bit fixed-point number.
 *
 * ============ WHY NOT date -s AND hwclock -w ============
 *
 * The Python shells out, and says so: "this runs as a plain script on a
 * busybox system and shelling out is what everything else here does". The C
 * cannot. nd_clock_start() runs a thread, and CODING-STANDARDS.md 1.1 bans
 * fork() from a threaded process unless execve() is the child's first
 * statement -- and even done correctly, forking to set a clock we can set with
 * one syscall is two processes and a busybox dependency for nothing. So
 * clock_settime(CLOCK_REALTIME) and ioctl(RTC_SET_TIME) directly. The
 * observable behaviour is the same, including the UTC-not-localtime RTC
 * convention: there is no /etc/adjtime in the overlay, so busybox hwclock -w
 * writes UTC, and so do we.
 *
 * ============ THE SANDBOX GUARD ============
 *
 * Setting the machine clock is the only thing in this module that reaches
 * outside the process, and it is not undoable. When NEODCT_ROOT is set the
 * whole filesystem this module reads -- version.prop, /NeoDCT/User/.clock,
 * /proc/net/route -- is a test fixture, so acting on a fixture's contents by
 * moving the developer's real clock would be wrong whether or not a test
 * asked for it. In production NEODCT_ROOT is unset and this costs one
 * comparison against '\0'. The log line is emitted either way, because the
 * log line is the load-bearing part: a clock that moves silently explains
 * nothing later.
 */

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <linux/rtc.h>

#include "nd_clock.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_settings.h"
#include "nd_types.h"
#include "nd_vclock.h"

/* The NTP pool, which is volunteer-operated and deliberately not run by any
 * single company. The numbered names are the pool's own recommended form:
 * each resolves to a different rotating set of servers, so trying 0/1/2 in
 * turn reaches different operators rather than retrying one.
 *
 * No corporate time server here on purpose. An NTP query tells whoever answers
 * that this device exists, roughly where it is, and when it was switched on --
 * which is not much, but it is not nothing, and it is not worth handing to an
 * advertising company for a clock reading. */
const char *const ND_NTP_SERVERS[ND_NTP_SERVER_COUNT] = {
    "0.pool.ntp.org",
    "1.pool.ntp.org",
    "2.pool.ntp.org",
};

/* /proc is read through nd_path_resolve() like everything else, so a host test
 * can hand this module a route table without one. */
#define CLOCK_PROC_ROUTE      "/proc/net/route"
#define CLOCK_PROC_IPV6_ROUTE "/proc/net/ipv6_route"

/* hwclock's own search order, minus /dev/misc/rtc which no kernel this decade
 * creates. */
static const char *const CLOCK_RTC_DEVICES[2] = {"/dev/rtc0", "/dev/rtc"};

/* One prop line. version.prop's longest value is a version string; 640 leaves
 * room for a build path nobody has invented yet. A longer line is split by
 * fgets and its tail treated as a fresh line, which cannot produce a false
 * match for a key that is only ever at the start of a real line. */
#define CLOCK_LINE_MAX 640

/* The NTP packet, both ways. 48 bytes exactly -- the reply's transmit
 * timestamp sits at offset 40 as seconds-since-1900, and the fraction at 44 is
 * deliberately ignored: it is 15 picoseconds per LSB and this is a phone. */
#define CLOCK_NTP_PACKET  48u
#define CLOCK_NTP_TX_SECS 40u

/* Thread stacks are explicit per MUSL.md: musl's default is 128 KB against
 * glibc's 8 MB, and a difference that large is exactly the kind of thing that
 * works on the desktop and faults on the phone. This worker's frames are a
 * getaddrinfo call and a 48-byte buffer, so 128 KB is generous. */
#define CLOCK_WORKER_STACK (128u * 1024u)

/* ------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------ */

static bool clock_is_sandboxed(void)
{
    return nd_path_root()[0] != '\0';
}

/* Python's str.strip(): both ends, the six ASCII whitespace characters. In
 * place, because every caller owns its buffer. */
static void strip_inplace(char *s)
{
    size_t start = 0u;
    size_t end = strlen(s);

    while (end > start && (s[end - 1u] == ' ' || s[end - 1u] == '\t' || s[end - 1u] == '\n' ||
                           s[end - 1u] == '\r' || s[end - 1u] == '\f' || s[end - 1u] == '\v'))
        end--;
    while (start < end && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' ||
                           s[start] == '\r' || s[start] == '\f' || s[start] == '\v'))
        start++;

    if (start > 0u)
        memmove(s, s + start, end - start);
    s[end - start] = '\0';
}

/* Python's int(str): optional sign, decimal digits, nothing else. An empty
 * string, a float, or trailing junk is a ValueError, which the callers turn
 * into None. */
static bool parse_epoch(const char *text, time_t *out)
{
    char *end = NULL;
    long long value;

    if (text[0] == '\0')
        return false;

    errno = 0;
    value = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return false;

    *out = (time_t)value;
    return true;
}

/* _read_prop(path, key): the FIRST line that startswith(key + "=") yields
 * everything after that '=', stripped. No comment handling and no tolerance
 * for whitespace before the key -- the Python has neither, and version.prop is
 * machine-written. */
static bool read_prop(const char *path, const char *key, char *out, size_t out_sz)
{
    char resolved[ND_PATH_MAX];
    char line[CLOCK_LINE_MAX];
    size_t key_len = strlen(key);
    FILE *f;
    bool found = false;

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return false;

    f = fopen(resolved, "r");
    if (f == NULL)
        return false;

    while (fgets(line, (int)sizeof line, f) != NULL) {
        if (strncmp(line, key, key_len) != 0 || line[key_len] != '=')
            continue;
        (void)nd_strlcpy(out, line + key_len + 1u, out_sz);
        strip_inplace(out);
        found = true;
        break;
    }

    (void)fclose(f);
    return found;
}

static void gmt_string(time_t when, const char *fmt, char *out, size_t out_sz)
{
    struct tm tm_buf;

    nd_time_gmtime((double)when, &tm_buf);
    if (strftime(out, out_sz, fmt, &tm_buf) == 0u)
        (void)nd_strlcpy(out, "?", out_sz);
}

/* ------------------------------------------------------------------ *
 * The two files we remember things in
 * ------------------------------------------------------------------ */

bool nd_clock_build_epoch(time_t *out)
{
    char raw[CLOCK_LINE_MAX];
    time_t value;

    if (!read_prop(ND_PATH_VERSION_PROP, "system.os.buildepoch", raw, sizeof raw))
        return false;
    if (!parse_epoch(raw, &value))
        return false;

    if (out != NULL)
        *out = value;
    return true;
}

bool nd_clock_last_known(time_t *out)
{
    char resolved[ND_PATH_MAX];
    char raw[CLOCK_LINE_MAX];
    time_t value;
    FILE *f;
    size_t n;

    if (nd_path_resolve(resolved, sizeof resolved, ND_PATH_CLOCK_STATE) != ND_OK)
        return false;

    f = fopen(resolved, "r");
    if (f == NULL)
        return false;

    n = fread(raw, 1u, sizeof raw - 1u, f);
    raw[n] = '\0';
    (void)fclose(f);

    strip_inplace(raw);
    if (!parse_epoch(raw, &value))
        return false;

    if (out != NULL)
        *out = value;
    return true;
}

/* The atomic rename and the fsync are both load-bearing: this file lives on
 * the only writable partition, which is UBIFS on raw NAND, and a phone loses
 * power mid-write far more often than a desktop does. */
bool nd_clock_remember(time_t when)
{
    char resolved[ND_PATH_MAX];
    char tmp[ND_PATH_MAX];
    FILE *f;
    bool ok = false;

    /* dirname(ND_PATH_CLOCK_STATE). Spelt out rather than derived because the
     * two constants live next to each other in nd_paths.h and a derived
     * dirname would be one more thing to get wrong at boot. */
    if (nd_mkdir_p(ND_PATH_USER, 0755u) != ND_OK)
        return false;

    if (nd_path_resolve(resolved, sizeof resolved, ND_PATH_CLOCK_STATE) != ND_OK)
        return false;
    if (nd_snprintf(tmp, sizeof tmp, "%s.tmp", resolved) != ND_OK)
        return false;

    f = fopen(tmp, "w");
    if (f == NULL)
        return false;

    if (fprintf(f, "%lld\n", (long long)when) < 0)
        goto done;
    if (fflush(f) != 0)
        goto done;
    if (fsync(fileno(f)) != 0)
        goto done;
    ok = true;

done:
    if (fclose(f) != 0)
        ok = false;
    if (ok && rename(tmp, resolved) != 0)
        ok = false;
    if (!ok)
        (void)unlink(tmp);
    return ok;
}

/* ------------------------------------------------------------------ *
 * Moving the clock
 * ------------------------------------------------------------------ */

/* Push the new time into the RTC so a warm reboot keeps it. Harmless when
 * there is no RTC or no battery behind it, which is the case on the QEMU
 * target and on a bare Luckfox board. */
static void write_rtc(time_t when)
{
    struct tm tm_buf;
    struct rtc_time rtc;
    size_t i;

    nd_time_gmtime((double)when, &tm_buf);

    memset(&rtc, 0, sizeof rtc);
    rtc.tm_sec = tm_buf.tm_sec;
    rtc.tm_min = tm_buf.tm_min;
    rtc.tm_hour = tm_buf.tm_hour;
    rtc.tm_mday = tm_buf.tm_mday;
    rtc.tm_mon = tm_buf.tm_mon;
    rtc.tm_year = tm_buf.tm_year;
    rtc.tm_wday = tm_buf.tm_wday;
    rtc.tm_yday = tm_buf.tm_yday;
    rtc.tm_isdst = 0;

    for (i = 0u; i < ND_ARRAY_LEN(CLOCK_RTC_DEVICES); i++) {
        char resolved[ND_PATH_MAX];
        int fd;

        if (nd_path_resolve(resolved, sizeof resolved, CLOCK_RTC_DEVICES[i]) != ND_OK)
            continue;

        fd = open(resolved, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;

        if (ioctl(fd, RTC_SET_TIME, &rtc) != 0)
            nd_log(ND_LOG_CLOCK, "RTC %s refused the write: %s", CLOCK_RTC_DEVICES[i],
                   strerror(errno));
        (void)close(fd);
        return;
    }
}

bool nd_clock_set(time_t when, const char *reason)
{
    char before[32];
    char after[32];
    struct timespec ts;

    /* Logged BEFORE anything moves, and unconditionally. A clock that jumps
     * silently is the sort of thing that explains a later mystery -- an SSL
     * error, a file with a future timestamp, an update that looks older than
     * it is -- and none of that is diagnosable after the fact.
     *
     * The first timestamp deliberately carries no "UTC" suffix and the second
     * does, exactly as the Python prints it. */
    gmt_string((time_t)nd_time_now(), "%Y-%m-%d %H:%M:%S", before, sizeof before);
    gmt_string(when, "%Y-%m-%d %H:%M:%S UTC", after, sizeof after);
    nd_log(ND_LOG_CLOCK, "setting time (%s): %s -> %s", reason != NULL ? reason : "unspecified",
           before, after);

    if (clock_is_sandboxed()) {
        nd_log(ND_LOG_CLOCK, "NEODCT_ROOT is set; leaving the real clock alone.");
        return true;
    }

    ts.tv_sec = when;
    ts.tv_nsec = 0;
    if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
        nd_log_err(ND_LOG_CLOCK, "clock_settime failed: %s", strerror(errno));
        return false;
    }

    write_rtc(when);
    return true;
}

bool nd_clock_apply_floor(time_t *settled)
{
    time_t candidate;
    time_t floor_epoch = 0;

    /* max(build_epoch(), last_known(), 0), skipping the ones that are None.
     * The 0 in that tuple is what makes a missing version.prop AND a missing
     * state file mean "do nothing" rather than "set the clock to 1970". */
    if (nd_clock_build_epoch(&candidate) && candidate > floor_epoch)
        floor_epoch = candidate;
    if (nd_clock_last_known(&candidate) && candidate > floor_epoch)
        floor_epoch = candidate;

    if (floor_epoch <= 0)
        return false;
    if (nd_time_now() >= (double)floor_epoch)
        return false;

    (void)nd_clock_set(floor_epoch, "floor: build date or last sync");
    if (settled != NULL)
        *settled = floor_epoch;
    return true;
}

/* ------------------------------------------------------------------ *
 * SNTP
 * ------------------------------------------------------------------ */

nd_err nd_clock_query(const char *server, int timeout_s, time_t *out)
{
    /* First byte 0x1b: LI=0, VN=3, Mode=3 (client). The other 47 are zero on
     * the way out. */
    uint8_t packet[CLOCK_NTP_PACKET] = {0x1bu};
    uint8_t reply[CLOCK_NTP_PACKET];
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct timeval tv;
    char port[8];
    ssize_t got;
    uint32_t seconds;
    int64_t epoch;
    int fd = -1;
    nd_err rc;

    if (server == NULL || out == NULL)
        return ND_ERR_INVAL;
    if (timeout_s <= 0)
        timeout_s = ND_NTP_QUERY_TIMEOUT_S;

    (void)nd_snprintf(port, sizeof port, "%d", ND_NTP_PORT);

    /* AF_INET only, exactly as the Python's socket(AF_INET, SOCK_DGRAM). The
     * phone's data bearer being IPv6-only with NAT64 is a real risk and is
     * recorded in spec-core-services.md; it is NOT quietly fixed here, because
     * fixing it changes which servers answer and that is a behaviour change
     * the owner has to see. */
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(server, port, &hints, &res) != 0 || res == NULL)
        return ND_ERR_IO;

    fd = socket(res->ai_family, res->ai_socktype | SOCK_CLOEXEC, res->ai_protocol);
    if (fd < 0) {
        rc = ND_ERR_IO;
        goto done;
    }

    tv.tv_sec = timeout_s;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, (socklen_t)sizeof tv) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, (socklen_t)sizeof tv) != 0) {
        rc = ND_ERR_IO;
        goto done;
    }

    if (sendto(fd, packet, sizeof packet, 0, res->ai_addr, res->ai_addrlen) !=
        (ssize_t)sizeof packet) {
        rc = ND_ERR_IO;
        goto done;
    }

    got = recvfrom(fd, reply, sizeof reply, 0, NULL, NULL);
    if (got < 0) {
        rc = (errno == EAGAIN || errno == EWOULDBLOCK) ? ND_ERR_TIMEOUT : ND_ERR_IO;
        goto done;
    }
    if ((size_t)got < sizeof reply) {
        nd_log(ND_LOG_CLOCK, "short NTP reply from %s (%d bytes)", server, (int)got);
        rc = ND_ERR_PARSE;
        goto done;
    }

    seconds = ((uint32_t)reply[CLOCK_NTP_TX_SECS] << 24) |
              ((uint32_t)reply[CLOCK_NTP_TX_SECS + 1u] << 16) |
              ((uint32_t)reply[CLOCK_NTP_TX_SECS + 2u] << 8) |
              (uint32_t)reply[CLOCK_NTP_TX_SECS + 3u];
    if (seconds == 0u) {
        nd_log(ND_LOG_CLOCK, "%s sent a zero timestamp", server);
        rc = ND_ERR_PARSE;
        goto done;
    }

    /* Computed in int64 rather than time_t: ND_CLOCK_SANE_MAX is 4102444800,
     * which does not fit a 32-bit time_t, and the comparison has to happen
     * before the value is narrowed. */
    epoch = (int64_t)seconds - (int64_t)ND_NTP_EPOCH_OFFSET;
    if (epoch < (int64_t)ND_CLOCK_SANE_MIN || epoch > (int64_t)ND_CLOCK_SANE_MAX) {
        /* A reply outside this range is not a time, it is a fault or an
         * attack. An unset server answers with something near its own epoch,
         * and trusting it would push the phone back to exactly the broken
         * state this module exists to fix. */
        nd_log(ND_LOG_CLOCK, "%s sent an implausible time (%lld)", server, (long long)epoch);
        rc = ND_ERR_PARSE;
        goto done;
    }

    *out = (time_t)epoch;
    rc = ND_OK;

done:
    if (fd >= 0)
        (void)close(fd);
    freeaddrinfo(res);
    return rc;
}

bool nd_clock_sync(const char *const *servers, size_t n, int timeout_s, time_t *out)
{
    size_t i;

    if (servers == NULL)
        return false;

    /* Some carriers hijack or drop the NTP pool, so one dead server must not
     * be the end of it. */
    for (i = 0u; i < n; i++) {
        /* "NTP from " plus a hostname, which DNS caps at 253 characters. */
        char reason[288];
        time_t when;

        if (nd_clock_query(servers[i], timeout_s, &when) != ND_OK)
            continue;

        (void)nd_snprintf(reason, sizeof reason, "NTP from %s", servers[i]);
        (void)nd_clock_set(when, reason);
        (void)nd_clock_remember(when);
        if (out != NULL)
            *out = when;
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------ *
 * Is there a network yet
 * ------------------------------------------------------------------ */

/* fields[index] of a whitespace-split line, Python's str.split() semantics:
 * runs of whitespace, no empty fields. Returns false when the line has fewer
 * than index+1 fields. */
static bool field_at(const char *line, size_t index, char *out, size_t out_sz)
{
    const char *p = line;
    size_t seen = 0u;

    for (;;) {
        const char *start;

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v')
            p++;
        if (*p == '\0')
            return false;

        start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '\f' &&
               *p != '\v')
            p++;

        if (seen == index) {
            size_t len = (size_t)(p - start);

            if (len >= out_sz)
                return false;
            memcpy(out, start, len);
            out[len] = '\0';
            return true;
        }
        seen++;
    }
}

/* Is this /proc/net/ipv6_route line the LOOPBACK's?
 *
 * THE TWO FILES PUT THE INTERFACE IN DIFFERENT PLACES, which is why there are
 * two checks rather than one shared helper:
 *
 *   /proc/net/route        Iface is the FIRST field  -- read with field_at(0)
 *   /proc/net/ipv6_route   Iface is the LAST field   -- read backwards, here
 *
 * This exists because of a real phone. On a NeoDCT with no network at all,
 * /proc/net/ipv6_route contains
 *
 *   00000000000000000000000000000000 00 000...000 00 ... ffffffff ... lo
 *
 * -- the kernel's unreachable ::/0 entry, which every Linux box has -- and
 * that matched "destination is all zeros, prefix length is 00" exactly. So
 * nd_clock_has_route() answered TRUE on a box with nothing but loopback.
 *
 * For ClockService, which is what this was written for, the cost was only a
 * pointless SNTP attempt. It became visible when the home screen's signal
 * meter started asking the same question, because "there is a network" is a
 * claim a user can see and disagree with. A default route out of lo is not a
 * route to anywhere. */
static bool v6_line_is_loopback(const char *line)
{
    size_t end = strlen(line);
    size_t start;

    while (end > 0u && (line[end - 1u] == '\n' || line[end - 1u] == '\r' ||
                        line[end - 1u] == ' ' || line[end - 1u] == '\t'))
        end--;
    start = end;
    while (start > 0u && line[start - 1u] != ' ' && line[start - 1u] != '\t')
        start--;
    return (end - start) == 2u && line[start] == 'l' && line[start + 1u] == 'o';
}

static bool has_route_v4(void)
{
    char resolved[ND_PATH_MAX];
    char line[CLOCK_LINE_MAX];
    FILE *f;
    bool first = true;
    bool found = false;

    if (nd_path_resolve(resolved, sizeof resolved, CLOCK_PROC_ROUTE) != ND_OK)
        return false;

    f = fopen(resolved, "r");
    if (f == NULL)
        return false;

    while (fgets(line, (int)sizeof line, f) != NULL) {
        char dest[64];
        char dummy[64];

        /* splitlines()[1:] -- the first line is the column header. */
        if (first) {
            first = false;
            continue;
        }
        /* len(fields) > 2 and fields[1] == "00000000" */
        if (!field_at(line, 2u, dummy, sizeof dummy))
            continue;
        /* Iface is field 0 here. See v6_line_is_loopback(). */
        if (field_at(line, 0u, dummy, sizeof dummy) && strcmp(dummy, "lo") == 0)
            continue;
        if (field_at(line, 1u, dest, sizeof dest) && strcmp(dest, "00000000") == 0) {
            found = true;
            break;
        }
    }

    (void)fclose(f);
    return found;
}

static bool has_route_v6(void)
{
    static const char zeros[33] = "00000000000000000000000000000000";
    char resolved[ND_PATH_MAX];
    char line[CLOCK_LINE_MAX];
    FILE *f;
    bool found = false;

    if (nd_path_resolve(resolved, sizeof resolved, CLOCK_PROC_IPV6_ROUTE) != ND_OK)
        return false;

    f = fopen(resolved, "r");
    if (f == NULL)
        return false;

    /* No header on this one, so every line counts. */
    while (fgets(line, (int)sizeof line, f) != NULL) {
        char dest[64];
        char plen[64];

        if (!field_at(line, 1u, plen, sizeof plen))
            continue;
        if (v6_line_is_loopback(line))
            continue;
        if (field_at(line, 0u, dest, sizeof dest) && strcmp(dest, zeros) == 0 &&
            strcmp(plen, "00") == 0) {
            found = true;
            break;
        }
    }

    (void)fclose(f);
    return found;
}

bool nd_clock_has_route(void)
{
    return has_route_v4() || has_route_v6();
}

/* ------------------------------------------------------------------ *
 * Boot
 * ------------------------------------------------------------------ */

typedef struct {
    size_t n;
    char **names; /* n strdup'd strings */
} clock_worker_args;

static void sleep_seconds(unsigned int seconds)
{
    struct timespec req;

    req.tv_sec = (time_t)seconds;
    req.tv_nsec = 0;
    while (nanosleep(&req, &req) != 0 && errno == EINTR)
        ;
}

/* The body of the background sync, callable directly for background=false. */
static void clock_sync_when_routed(const char *const *servers, size_t n)
{
    char today[16];
    time_t when = 0;
    size_t attempt;
    bool routed = false;

    /* Wait for a route rather than hammering a nameserver that cannot be
     * reached yet. The modem can take a while, and may never arrive. The
     * check comes FIRST and the sleep after, so a route already present at
     * boot means no delay at all. */
    for (attempt = 0u; attempt < (size_t)ND_CLOCK_ROUTE_TRIES; attempt++) {
        if (nd_clock_has_route()) {
            routed = true;
            break;
        }
        sleep_seconds(ND_CLOCK_ROUTE_SLEEP_S);
    }

    if (!routed) {
        gmt_string((time_t)nd_time_now(), "%Y-%m-%d", today, sizeof today);
        nd_log(ND_LOG_CLOCK, "no route after 5 minutes; keeping %s", today);
        return;
    }

    if (nd_clock_sync(servers, n, ND_NTP_QUERY_TIMEOUT_S, &when)) {
        char stamp[32];

        gmt_string(when, "%Y-%m-%d %H:%M:%S UTC", stamp, sizeof stamp);
        nd_log(ND_LOG_CLOCK, "synced: %s", stamp);
    } else {
        gmt_string((time_t)nd_time_now(), "%Y-%m-%d", today, sizeof today);
        nd_log(ND_LOG_CLOCK, "no NTP server answered; keeping %s", today);
    }
}

static void worker_args_free(clock_worker_args *args)
{
    size_t i;

    if (args == NULL)
        return;
    for (i = 0u; i < args->n; i++)
        free(args->names[i]);
    free(args->names);
    free(args);
}

/* owned by the thread; freed here and nowhere else */
static void *clock_worker(void *arg)
{
    clock_worker_args *args = arg;

    clock_sync_when_routed((const char *const *)args->names, args->n);
    worker_args_free(args);
    return NULL;
}

/* The server list has to outlive nd_clock_start(), because the thread reads it
 * up to five minutes later and the caller's array may be on a stack frame that
 * is long gone. So it is copied. */
static clock_worker_args *worker_args_new(const char *const *servers, size_t n)
{
    clock_worker_args *args = calloc(1u, sizeof *args);
    size_t i;

    if (args == NULL)
        return NULL;
    if (n == 0u)
        return args;

    args->names = calloc(n, sizeof *args->names);
    if (args->names == NULL) {
        free(args);
        return NULL;
    }

    for (i = 0u; i < n; i++) {
        args->names[i] = strdup(servers[i]);
        if (args->names[i] == NULL) {
            args->n = i;
            worker_args_free(args);
            return NULL;
        }
    }
    args->n = n;
    return args;
}

bool nd_clock_ntp_enabled(void)
{
    char stored[32];

    (void)nd_settings_get_copy(ND_SET_CLOCK_NTP, ND_SET_CLOCK_NTP_DFLT, stored, sizeof stored);
    /* Default true on an unreadable or absent value: a phone that lost its
     * settings should come back with a clock that fixes itself. */
    return nd_setting_is_enabled(stored, true);
}

void nd_clock_start(bool background, const char *const *servers, size_t n)
{
    clock_worker_args *args;
    pthread_attr_t attr;
    pthread_t thread;
    sigset_t all;
    sigset_t saved;
    int rc;

    if (servers == NULL) {
        servers = ND_NTP_SERVERS;
        n = ND_NTP_SERVER_COUNT;
    }

    /* Synchronous, because it needs no network and everything that follows --
     * the TLS handshake the browser is about to attempt -- depends on it.
     *
     * ALWAYS, whatever the NTP setting says. The floor is not a sync: it moves
     * a clock that reads 1970 up to the build epoch, and a phone that skipped
     * it could not verify an update's signature or complete a TLS handshake.
     * "I set my own time" is a request to be left alone by the network, not a
     * request to be allowed to boot into 1970. */
    (void)nd_clock_apply_floor(NULL);

    /* The setting gates the NETWORK half only, and is read here rather than in
     * the worker so that a phone with it off never starts a thread at all. */
    if (!nd_clock_ntp_enabled()) {
        nd_log(ND_LOG_CLOCK, "NTP sync is off; keeping the clock as it is");
        return;
    }

    if (!background) {
        clock_sync_when_routed(servers, n);
        return;
    }

    args = worker_args_new(servers, n);
    if (args == NULL) {
        nd_log_err(ND_LOG_CLOCK, "out of memory starting the sync thread");
        return;
    }

    if (pthread_attr_init(&attr) != 0) {
        worker_args_free(args);
        return;
    }
    /* Detached: nothing joins this, and it must not delay boot on a phone
     * whose carrier may take a minute to attach, or never attach. */
    (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    (void)pthread_attr_setstacksize(&attr, CLOCK_WORKER_STACK);

    /* A new thread inherits the creator's signal mask, so blocking everything
     * across the pthread_create() leaves this worker unable to receive a
     * signal at all -- SIGTERM and SIGINT keep going to the main thread, which
     * is the one that installed handlers for them. That is also what the
     * Python did without anyone writing it down: CPython delivers signals only
     * to the main thread, so ClockService's daemon thread was never a
     * candidate either. Without this, nd-core's shutdown handler could run on
     * this thread while main sat in its idle sleep. */
    (void)sigfillset(&all);
    (void)pthread_sigmask(SIG_SETMASK, &all, &saved);

    rc = pthread_create(&thread, &attr, clock_worker, args);

    (void)pthread_sigmask(SIG_SETMASK, &saved, NULL);
    (void)pthread_attr_destroy(&attr);

    if (rc != 0) {
        nd_log_err(ND_LOG_CLOCK, "cannot start the sync thread: %s", strerror(rc));
        worker_args_free(args);
    }
}

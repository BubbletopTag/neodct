/* test_clock.c -- the offline floor and an SNTP reply that must be refused.
 *
 * A port of neodct/tests/test_clockservice.py, case for case, plus the things
 * that test could not reach from Python: _has_route over both proc files, and
 * start() actually running to completion.
 *
 * Its docstring is worth keeping, because it says what the module is for:
 * a phone with no battery-backed RTC boots at the Unix epoch, and every TLS
 * certificate on the internet has a "not valid before" date -- so a clock
 * reading 1970 fails validation on every HTTPS site at once. These tests pin
 * the two behaviours that stop that: the offline floor, and a sync that
 * refuses an implausible answer.
 *
 * ============ NOTHING HERE MOVES THE MACHINE'S CLOCK ============
 *
 * The Python monkeypatched set_clock, "because a test that sets the machine's
 * clock is a test that ruins someone's day". C cannot monkeypatch, so
 * nd_clock.c refuses to call clock_settime() while NEODCT_ROOT is set --
 * which it always is under this harness, and never is on the phone. The log
 * line still comes out, so the "setting the clock says so" case below is
 * testing the real thing.
 *
 * Time itself comes from nd_vclock, pinned to 2024-01-01 12:34:56 UTC, which
 * is what makes "the clock is behind the build date" and "the clock is ahead
 * of it" two fixtures rather than two dates that stop being true next year.
 *
 * The socket cases DO open a socket -- to 127.0.0.1, answered by a fake NTP
 * server in this process. Binding UDP 123 needs privilege, so those cases
 * announce themselves as skipped rather than failing on a machine without it.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "nd_clock.h"
#include "nd_types.h"
#include "nd_vclock.h"
#include "platform_test.h"

/* The same build epoch the Python fixture uses. */
#define BUILD_EPOCH 1787359945 /* 2026-08-22 00:52:25 UTC */
#define GOOD_EPOCH  1800000000 /* 2027-01-15 08:00:00 UTC */

/* nd_vclock's pinned reading, i.e. "now" for every case in this file. */
#define VCLOCK_NOW 1704112496 /* 2024-01-01 12:34:56 UTC */

static int g_skipped;

/* ------------------------------------------------------------------ *
 * Fixtures
 * ------------------------------------------------------------------ */

static void write_version(long long epoch)
{
    char text[128];

    (void)snprintf(text, sizeof text, "system.os.buildepoch=%lld\n", epoch);
    pt_write_text(ND_PATH_VERSION_PROP, text);
}

/* A /proc/net/route with a default route in it: the header line the Python
 * skips, then a line whose second field is the all-zeroes destination. */
static void write_route_v4(bool with_default)
{
    if (with_default)
        pt_write_text("/proc/net/route",
                      "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\n"
                      "eth0\t00000000\t0102000A\t0003\t0\t0\t0\t00000000\n"
                      "eth0\t0002000A\t00000000\t0001\t0\t0\t0\t00FFFFFF\n");
    else
        pt_write_text("/proc/net/route",
                      "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\n"
                      "eth0\t0002000A\t00000000\t0001\t0\t0\t0\t00FFFFFF\n");
}

static void write_route_v6(bool with_default)
{
    if (with_default)
        pt_write_text("/proc/net/ipv6_route", "00000000000000000000000000000000 00 "
                                              "00000000000000000000000000000000 00 "
                                              "fe80000000000000021122fffe334455 00000400 00000001 "
                                              "00000000 00000003 wwan0\n");
    else
        pt_write_text("/proc/net/ipv6_route", "fe800000000000000000000000000000 40 "
                                              "00000000000000000000000000000000 00 "
                                              "00000000000000000000000000000000 00000100 "
                                              "00000000 00000001 wwan0\n");
}

/* ------------------------------------------------------------------ *
 * Capturing the log
 * ------------------------------------------------------------------ */

static int g_saved_stdout = -1;

static void capture_begin(void)
{
    char resolved[ND_PATH_MAX];
    int fd;

    pt_write_text("/capture.log", "");
    (void)fflush(stdout);
    g_saved_stdout = dup(STDOUT_FILENO);
    if (nd_path_resolve(resolved, sizeof resolved, "/capture.log") != ND_OK)
        return;
    fd = open(resolved, O_WRONLY | O_TRUNC);
    if (fd >= 0) {
        (void)dup2(fd, STDOUT_FILENO);
        (void)close(fd);
    }
}

static void capture_end(char *out, size_t out_sz)
{
    (void)fflush(stdout);
    if (g_saved_stdout >= 0) {
        (void)dup2(g_saved_stdout, STDOUT_FILENO);
        (void)close(g_saved_stdout);
        g_saved_stdout = -1;
    }
    if (pt_read_text("/capture.log", out, out_sz) == (size_t)-1)
        out[0] = '\0';
}

/* ------------------------------------------------------------------ *
 * A fake NTP server on the loopback
 * ------------------------------------------------------------------ */

/* ONE server for the whole binary, bound once in main(). Per-case binding was
 * the obvious shape and it was wrong: `make test` runs the suite twice (plain
 * and ASan) and a developer may have a sibling run going, so a second binder
 * of UDP 123 lost the race and eleven cases quietly reported themselves
 * skipped. Binding once, with a retry, means the cases either all run or all
 * announce that they did not. */
typedef struct {
    int fd;
    pthread_mutex_t lock; /* guards drop_first, reply, reply_len, served */
    int drop_first;       /* stay silent for this many requests */
    uint8_t reply[64];
    size_t reply_len;
    int served;
    volatile int stop;
    pthread_t thread;
    bool running;
} fake_ntp;

static fake_ntp g_server;

/* 48 bytes with the transmit timestamp at offset 40, big-endian
 * seconds-since-1900. */
static size_t ntp_reply(uint8_t *out, size_t out_sz, long long epoch)
{
    uint32_t seconds = (uint32_t)(epoch + (long long)ND_NTP_EPOCH_OFFSET);

    if (out_sz < 48u)
        return 0u;
    memset(out, 0, 48u);
    out[0] = 0x1bu;
    out[40] = (uint8_t)((seconds >> 24) & 0xFFu);
    out[41] = (uint8_t)((seconds >> 16) & 0xFFu);
    out[42] = (uint8_t)((seconds >> 8) & 0xFFu);
    out[43] = (uint8_t)(seconds & 0xFFu);
    return 48u;
}

static void *fake_ntp_thread(void *arg)
{
    fake_ntp *s = arg;

    while (s->stop == 0) {
        uint8_t buf[128];
        uint8_t out[64];
        struct sockaddr_in from;
        socklen_t from_len = (socklen_t)sizeof from;
        size_t out_len;
        ssize_t n;

        n = recvfrom(s->fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &from_len);
        if (n < 0)
            continue; /* the 200 ms receive timeout, so stop is seen promptly */

        (void)pthread_mutex_lock(&s->lock);
        s->served++;
        out_len = s->served <= s->drop_first ? 0u : s->reply_len;
        if (out_len > 0u)
            memcpy(out, s->reply, out_len);
        (void)pthread_mutex_unlock(&s->lock);

        if (out_len > 0u)
            (void)sendto(s->fd, out, out_len, 0, (struct sockaddr *)&from, from_len);
    }
    return NULL;
}

/* Binds UDP 123 on the loopback. Privileged, and contended when two copies of
 * the suite overlap, so it retries for a couple of seconds before giving up. */
static bool fake_ntp_open(fake_ntp *s)
{
    struct sockaddr_in addr;
    struct timeval tv;
    int attempt;

    memset(s, 0, sizeof *s);
    s->fd = -1;
    if (pthread_mutex_init(&s->lock, NULL) != 0)
        return false;

    s->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->fd < 0)
        return false;

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)ND_NTP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    for (attempt = 0; attempt < 20; attempt++) {
        struct timespec wait = {0, 100 * 1000 * 1000};

        if (bind(s->fd, (struct sockaddr *)&addr, (socklen_t)sizeof addr) == 0)
            break;
        if (attempt == 19) {
            (void)close(s->fd);
            s->fd = -1;
            return false;
        }
        (void)nanosleep(&wait, NULL);
    }

    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    (void)setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, (socklen_t)sizeof tv);

    if (pthread_create(&s->thread, NULL, fake_ntp_thread, s) != 0) {
        (void)close(s->fd);
        s->fd = -1;
        return false;
    }
    s->running = true;
    return true;
}

static void fake_ntp_close(fake_ntp *s)
{
    if (s->running) {
        s->stop = 1;
        (void)pthread_join(s->thread, NULL);
        (void)close(s->fd);
        s->fd = -1;
        s->running = false;
    }
    (void)pthread_mutex_destroy(&s->lock);
}

static void skip(const char *name)
{
    g_skipped++;
    fprintf(stderr, "SKIP %s (cannot bind UDP %d on the loopback)\n", name, ND_NTP_PORT);
}

/* Point the server at this case's answer and zero its request counter.
 * Returns false, having said so, when there is no server to point. */
static bool serve(const char *name, const uint8_t *reply, size_t reply_len, int drop_first)
{
    if (!g_server.running) {
        skip(name);
        return false;
    }

    if (reply_len > sizeof g_server.reply)
        reply_len = sizeof g_server.reply;

    (void)pthread_mutex_lock(&g_server.lock);
    g_server.served = 0;
    g_server.drop_first = drop_first;
    g_server.reply_len = reply_len;
    if (reply != NULL && reply_len > 0u)
        memcpy(g_server.reply, reply, reply_len);
    (void)pthread_mutex_unlock(&g_server.lock);
    return true;
}

static int served(void)
{
    int n;

    (void)pthread_mutex_lock(&g_server.lock);
    n = g_server.served;
    (void)pthread_mutex_unlock(&g_server.lock);
    return n;
}

/* ================================================================== *
 * The offline floor
 * ================================================================== */

/* The date will be wrong, but wrong in the direction that keeps certificates
 * valid, which is the whole point. */
static void test_a_clock_at_the_epoch_is_pushed_up_to_the_build_date(void)
{
    time_t settled = 0;

    write_version(BUILD_EPOCH);

    CHECK(nd_clock_apply_floor(&settled));
    CHECK_INT(settled, BUILD_EPOCH);
}

/* Never move a good clock backwards to the build date. */
static void test_a_clock_already_ahead_is_left_alone(void)
{
    time_t settled = 0;

    write_version(VCLOCK_NOW - 86400);

    CHECK(!nd_clock_apply_floor(&settled));
    CHECK_INT(settled, 0);
}

/* After one good sync, later boots start from that rather than from whenever
 * the image happened to be built. */
static void test_the_last_synced_time_beats_the_build_date(void)
{
    time_t settled = 0;

    write_version(BUILD_EPOCH);
    CHECK(nd_clock_remember(BUILD_EPOCH + 999999));

    CHECK(nd_clock_apply_floor(&settled));
    CHECK_INT(settled, BUILD_EPOCH + 999999);
}

static void test_a_missing_or_corrupt_state_file_is_not_fatal(void)
{
    time_t settled = 0;
    time_t junk = 0;

    write_version(BUILD_EPOCH);
    pt_write_text(ND_PATH_CLOCK_STATE, "not a number");

    CHECK(!nd_clock_last_known(&junk));
    CHECK(nd_clock_apply_floor(&settled));
    CHECK_INT(settled, BUILD_EPOCH); /* falls back to the build date */
}

static void test_with_nothing_to_go_on_the_clock_is_left_alone(void)
{
    /* No version.prop, no state file: max(None, None, 0) is 0, which is
     * falsy, so the Python returns without touching anything. A phone in this
     * state has a broken image, and moving its clock to 1970 would not help. */
    CHECK(!nd_clock_apply_floor(NULL));
}

static void test_a_build_epoch_that_is_not_a_number_is_ignored(void)
{
    time_t out = 0;

    pt_write_text(ND_PATH_VERSION_PROP, "system.os.buildepoch=tuesday\n");
    CHECK(!nd_clock_build_epoch(&out));

    pt_write_text(ND_PATH_VERSION_PROP, "system.os.buildepoch=17873.59945\n");
    CHECK(!nd_clock_build_epoch(&out));

    /* The key must start the line -- _read_prop has no whitespace tolerance
     * and no comment handling, because version.prop is machine-written. */
    pt_write_text(ND_PATH_VERSION_PROP, "  system.os.buildepoch=1787359945\n");
    CHECK(!nd_clock_build_epoch(&out));

    pt_write_text(ND_PATH_VERSION_PROP, "system.os.versionname=NeoDCT\n"
                                        "system.os.buildepoch=1787359945\n");
    CHECK(nd_clock_build_epoch(&out));
    CHECK_INT(out, BUILD_EPOCH);
}

static void test_the_remembered_time_round_trips(void)
{
    char resolved[ND_PATH_MAX];
    char raw[64];
    time_t out = 0;

    CHECK(nd_clock_remember(GOOD_EPOCH));
    CHECK(nd_clock_last_known(&out));
    CHECK_INT(out, GOOD_EPOCH);

    /* "%d\n", and nothing left behind: the temp file is renamed onto the
     * real one, which is what survives power loss mid-write. */
    CHECK(pt_read_text(ND_PATH_CLOCK_STATE, raw, sizeof raw) != (size_t)-1);
    CHECK_STR(raw, "1800000000\n");
    CHECK(nd_path_resolve(resolved, sizeof resolved, ND_PATH_CLOCK_STATE) == ND_OK);
    (void)nd_strlcat(resolved, ".tmp", sizeof resolved);
    CHECK(access(resolved, F_OK) != 0);
}

/* ================================================================== *
 * The SNTP reply
 * ================================================================== */

static void test_a_good_reply_is_read_as_the_time(void)
{
    uint8_t reply[64];
    time_t out = 0;

    if (!serve("a good reply is read as the time", reply,
               ntp_reply(reply, sizeof reply, GOOD_EPOCH), 0))
        return;

    CHECK(nd_clock_query("127.0.0.1", 1, &out) == ND_OK);
    CHECK_INT(out, GOOD_EPOCH);
}

/* An unset server answers with something near its own epoch. Trusting it would
 * push the phone back to exactly the broken state we are fixing. */
static void test_a_reply_from_before_2020_is_refused(void)
{
    uint8_t reply[64];
    time_t out = 12345;

    if (!serve("a reply from before 2020 is refused", reply,
               ntp_reply(reply, sizeof reply, 946684800), 0))
        return;

    CHECK(nd_clock_query("127.0.0.1", 1, &out) == ND_ERR_PARSE);
    CHECK_INT(out, 12345); /* untouched */
}

static void test_a_zero_timestamp_is_refused(void)
{
    uint8_t reply[48];
    time_t out = 0;

    memset(reply, 0, sizeof reply);
    reply[0] = 0x1bu;
    if (!serve("a zero timestamp is refused", reply, sizeof reply, 0))
        return;

    CHECK(nd_clock_query("127.0.0.1", 1, &out) == ND_ERR_PARSE);
}

static void test_a_truncated_reply_is_refused(void)
{
    uint8_t reply[11];
    time_t out = 0;

    memset(reply, 0, sizeof reply);
    reply[0] = 0x1bu;
    if (!serve("a truncated reply is refused", reply, sizeof reply, 0))
        return;

    CHECK(nd_clock_query("127.0.0.1", 1, &out) == ND_ERR_PARSE);
}

static void test_a_silent_server_times_out(void)
{
    time_t out = 0;

    if (!serve("a silent server times out", NULL, 0u, 99))
        return;

    CHECK(nd_clock_query("127.0.0.1", 1, &out) == ND_ERR_TIMEOUT);
}

/* Some carriers hijack or drop the NTP pool, so one dead server must not be
 * the end of it. */
static void test_sync_moves_to_the_next_server_when_one_is_silent(void)
{
    static const char *const servers[3] = {"127.0.0.1", "127.0.0.1", "127.0.0.1"};
    uint8_t reply[64];
    char log[4096];
    time_t out = 0;

    if (!serve("sync moves to the next server when one is silent", reply,
               ntp_reply(reply, sizeof reply, GOOD_EPOCH), 2))
        return;

    capture_begin();
    CHECK(nd_clock_sync(servers, 3u, 1, &out));
    capture_end(log, sizeof log);

    CHECK_INT(out, GOOD_EPOCH);
    CHECK_INT(served(), 3);
}

static void test_sync_returns_nothing_when_every_server_is_silent(void)
{
    static const char *const servers[2] = {"127.0.0.1", "127.0.0.1"};
    time_t out = 555;

    if (!serve("sync returns nothing when every server is silent", NULL, 0u, 99))
        return;

    CHECK(!nd_clock_sync(servers, 2u, 1, &out));
    CHECK_INT(out, 555);
    CHECK_INT(served(), 2);

    /* Nothing was learned, so nothing is remembered for next boot. */
    CHECK(!nd_clock_last_known(NULL));
}

static void test_a_successful_sync_is_remembered_for_next_boot(void)
{
    static const char *const servers[1] = {"127.0.0.1"};
    uint8_t reply[64];
    char log[4096];
    time_t out = 0;

    if (!serve("a successful sync is remembered for next boot", reply,
               ntp_reply(reply, sizeof reply, GOOD_EPOCH), 0))
        return;

    capture_begin();
    CHECK(nd_clock_sync(servers, 1u, 1, &out));
    capture_end(log, sizeof log);

    CHECK(nd_clock_last_known(&out));
    CHECK_INT(out, GOOD_EPOCH);
    /* The reason names the server, so the serial log says where the time came
     * from as well as that it moved. */
    CHECK(strstr(log, "NTP from 127.0.0.1") != NULL);
}

/* ================================================================== *
 * Who gets told the phone exists
 * ================================================================== */

/* An NTP query tells whoever answers that this device exists, roughly where it
 * is, and when it was switched on. That is not much, but it is not worth
 * handing to an advertising company for a clock reading -- so the pool, which
 * is volunteer-run and not any one company's. */
static void test_the_time_servers_are_the_volunteer_pool_only(void)
{
    static const char *const unwanted[5] = {"google", "amazon", "facebook", "apple", "microsoft"};
    size_t i;

    for (i = 0u; i < (size_t)ND_NTP_SERVER_COUNT; i++) {
        const char *server = ND_NTP_SERVERS[i];
        size_t len = strlen(server);
        size_t j;

        CHECK(len > strlen("pool.ntp.org"));
        CHECK_STR(server + len - strlen("pool.ntp.org"), "pool.ntp.org");
        for (j = 0u; j < ND_ARRAY_LEN(unwanted); j++)
            CHECK(strstr(server, unwanted[j]) == NULL);
    }
}

/* 0/1/2.pool.ntp.org each resolve to a different rotating set, so a retry
 * reaches a different operator instead of the same one twice. */
static void test_the_pool_is_asked_by_its_numbered_names(void)
{
    size_t i;
    size_t j;

    for (i = 0u; i < (size_t)ND_NTP_SERVER_COUNT; i++) {
        for (j = i + 1u; j < (size_t)ND_NTP_SERVER_COUNT; j++)
            CHECK(strcmp(ND_NTP_SERVERS[i], ND_NTP_SERVERS[j]) != 0);
    }
    CHECK(strncmp(ND_NTP_SERVERS[0], "0.", 2u) == 0);
}

/* A clock that jumps silently explains nothing later. Every change is logged
 * with where it came from. */
static void test_setting_the_clock_says_so(void)
{
    char log[4096];

    capture_begin();
    CHECK(nd_clock_set(GOOD_EPOCH, "NTP from 0.pool.ntp.org"));
    capture_end(log, sizeof log);

    CHECK(strstr(log, "[CLOCK]") != NULL);
    CHECK(strstr(log, "setting time") != NULL);
    CHECK(strstr(log, "0.pool.ntp.org") != NULL);
    /* Old time first with no suffix, new time second with UTC -- exactly the
     * shape the Python prints, and both are gmtime. */
    CHECK(strstr(log, "2024-01-01 12:34:56 -> 2027-01-15 08:00:00 UTC") != NULL);
    /* And it did NOT touch the machine, because NEODCT_ROOT is set. */
    CHECK(strstr(log, "leaving the real clock alone") != NULL);
}

/* ================================================================== *
 * Is there a network yet
 * ================================================================== */

static void test_no_proc_files_at_all_means_no_route(void)
{
    CHECK(!nd_clock_has_route());
}

static void test_a_v4_default_route_counts(void)
{
    write_route_v4(true);
    CHECK(nd_clock_has_route());
}

static void test_a_v4_table_without_a_default_does_not(void)
{
    write_route_v4(false);
    CHECK(!nd_clock_has_route());
}

static void test_the_header_line_is_skipped(void)
{
    /* "Destination" is not "00000000", but a reader that forgot splitlines()[1:]
     * would still be wrong on a table whose header happened to line up. This
     * pins the skip. */
    pt_write_text("/proc/net/route", "Iface\t00000000\tGateway\tFlags\n");
    CHECK(!nd_clock_has_route());
}

static void test_a_v6_default_route_counts(void)
{
    /* The phone's data bearer is IPv6-only with NAT64 on some carriers, so
     * "no v4 route" is not "no network". */
    write_route_v4(false);
    write_route_v6(true);
    CHECK(nd_clock_has_route());
}

static void test_a_v6_table_without_a_default_does_not(void)
{
    write_route_v4(false);
    write_route_v6(false);
    CHECK(!nd_clock_has_route());
}

/* ================================================================== *
 * start()
 * ================================================================== */

static void test_start_floors_the_clock_then_syncs(void)
{
    static const char *const servers[1] = {"127.0.0.1"};
    uint8_t reply[64];
    char log[8192];
    time_t out = 0;

    if (!serve("start floors the clock then syncs", reply,
               ntp_reply(reply, sizeof reply, GOOD_EPOCH), 0))
        return;

    write_version(BUILD_EPOCH);
    write_route_v4(true);

    capture_begin();
    nd_clock_start(false, servers, 1u);
    capture_end(log, sizeof log);

    /* The floor runs first and synchronously, because the browser's TLS
     * handshake depends on it and cannot wait for a carrier to attach. */
    CHECK(strstr(log, "floor: build date or last sync") != NULL);
    CHECK(strstr(log, "NTP from 127.0.0.1") != NULL);
    CHECK(strstr(log, "synced: 2027-01-15 08:00:00 UTC") != NULL);
    CHECK(nd_clock_last_known(&out));
    CHECK_INT(out, GOOD_EPOCH);
}

static void test_start_reports_when_nobody_answers(void)
{
    static const char *const servers[1] = {"127.0.0.1"};
    char log[8192];

    if (!serve("start reports when nobody answers", NULL, 0u, 99))
        return;

    write_route_v4(true);

    capture_begin();
    /* background=false so the whole walk happens before this returns; the
     * production call is background=true and the code path is the same
     * function. start() does not take a timeout -- it uses the real
     * ND_NTP_QUERY_TIMEOUT_S -- so ONE silent server here, not three, or this
     * case alone would cost fifteen seconds. */
    nd_clock_start(false, servers, 1u);
    capture_end(log, sizeof log);

    CHECK_INT(served(), 1);
    CHECK(strstr(log, "no NTP server answered; keeping 2024-01-01") != NULL);
    CHECK(!nd_clock_last_known(NULL));
}

static void test_the_background_sync_runs_on_its_own_thread(void)
{
    static const char *const servers[1] = {"127.0.0.1"};
    uint8_t reply[64];
    time_t out = 0;
    int waited;

    if (!serve("the background sync runs on its own thread", reply,
               ntp_reply(reply, sizeof reply, GOOD_EPOCH), 0))
        return;

    write_version(BUILD_EPOCH);
    write_route_v4(true);

    nd_clock_start(true, servers, 1u);

    /* nd_clock_start returns immediately -- that is the point of it, boot must
     * not wait for a carrier. The state file appearing is how we know the
     * detached worker got there. */
    for (waited = 0; waited < 200 && !nd_clock_last_known(&out); waited++) {
        struct timespec ts = {0, 10 * 1000 * 1000};

        (void)nanosleep(&ts, NULL);
    }

    CHECK(nd_clock_last_known(&out));
    CHECK_INT(out, GOOD_EPOCH);
}

int main(void)
{
    int rc;

    /* Pinned time, so "before the build date" and "after it" are properties of
     * the fixture rather than of the day the test is run. */
    nd_vclock_enable();

    /* Bound once, before any case runs. See the note above fake_ntp. */
    (void)fake_ntp_open(&g_server);

    RUN(test_a_clock_at_the_epoch_is_pushed_up_to_the_build_date);
    RUN(test_a_clock_already_ahead_is_left_alone);
    RUN(test_the_last_synced_time_beats_the_build_date);
    RUN(test_a_missing_or_corrupt_state_file_is_not_fatal);
    RUN(test_with_nothing_to_go_on_the_clock_is_left_alone);
    RUN(test_a_build_epoch_that_is_not_a_number_is_ignored);
    RUN(test_the_remembered_time_round_trips);

    RUN(test_a_good_reply_is_read_as_the_time);
    RUN(test_a_reply_from_before_2020_is_refused);
    RUN(test_a_zero_timestamp_is_refused);
    RUN(test_a_truncated_reply_is_refused);
    RUN(test_a_silent_server_times_out);
    RUN(test_sync_moves_to_the_next_server_when_one_is_silent);
    RUN(test_sync_returns_nothing_when_every_server_is_silent);
    RUN(test_a_successful_sync_is_remembered_for_next_boot);

    RUN(test_the_time_servers_are_the_volunteer_pool_only);
    RUN(test_the_pool_is_asked_by_its_numbered_names);
    RUN(test_setting_the_clock_says_so);

    RUN(test_no_proc_files_at_all_means_no_route);
    RUN(test_a_v4_default_route_counts);
    RUN(test_a_v4_table_without_a_default_does_not);
    RUN(test_the_header_line_is_skipped);
    RUN(test_a_v6_default_route_counts);
    RUN(test_a_v6_table_without_a_default_does_not);

    RUN(test_start_floors_the_clock_then_syncs);
    RUN(test_start_reports_when_nobody_answers);
    RUN(test_the_background_sync_runs_on_its_own_thread);

    fake_ntp_close(&g_server);

    rc = pt_report("test_clock");
    if (g_skipped != 0)
        fprintf(stderr, "test_clock: %d socket cases skipped\n", g_skipped);
    nd_vclock_disable();
    return rc;
}

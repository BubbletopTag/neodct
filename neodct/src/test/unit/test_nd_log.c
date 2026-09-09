/* test_nd_log.c -- nd_log against the recorded Python output.
 *
 * Every expected byte string below was taken from
 * neodct/tests/golden/log/logref.json, which neodct/tools/logref.py generated
 * by running the real System/core/logstyle.py. They are not a restatement of
 * what nd_log.c does; they are what the phone printed before the rewrite.
 *
 * The four things this has to pin, because they are the four things
 * logstyle.py actually does:
 *
 *   1. the named palette
 *   2. the derived colour for the eleven app tags
 *   3. the derived colour for a tag nobody has registered
 *   4. the tag-splitting rules, including the cases that must NOT be painted
 *
 * Regenerate the oracle with:
 *     python3 neodct/tools/logref.py --out neodct/tests/golden/log/
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "nd_log.h"
#include "nd_paths.h"

static int g_failures;
static int g_checks;

static void check_int(const char *what, int got, int want)
{
    g_checks++;
    if (got != want) {
        g_failures++;
        (void)fprintf(stderr, "FAIL %-28s got %d, want %d\n", what, got, want);
    }
}

/* Escapes are invisible on a terminal and a diff of two invisible strings is
 * useless, so print ESC as "\x1b" the way the oracle's JSON does. */
static void print_escaped(const char *s)
{
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '\033')
            (void)fputs("\\x1b", stderr);
        else
            (void)fputc(*p, stderr);
    }
}

static void check_str(const char *what, const char *got, const char *want)
{
    g_checks++;
    if (strcmp(got, want) != 0) {
        g_failures++;
        (void)fprintf(stderr, "FAIL %-28s\n  got  \"", what);
        print_escaped(got);
        (void)fputs("\"\n  want \"", stderr);
        print_escaped(want);
        (void)fputs("\"\n", stderr);
    }
}

static void check_bool(const char *what, bool got, bool want)
{
    check_int(what, got ? 1 : 0, want ? 1 : 0);
}

/* ---- 1. the named palette, all 22 entries ---- */
static void test_named_palette(void)
{
    static const struct {
        const char *tag;
        int code;
    } expect[] = {
        {"MODEM", 39},    {"ndsys", 33},  {"UPDATE", 33},  {"CORE", 46},     {"OS", 46},
        {"Launcher", 82}, {"BATT", 226},  {"FUEL", 226},   {"NOTIFY", 201},  {"INPUT", 51},
        {"KEYMAP", 87},   {"SETUP", 214}, {"UI", 120},     {"FB", 123},      {"KERNEL", 244},
        {"sdcard", 180},  {"CLOCK", 129}, {"RSHELL", 162}, {"Browser", 141}, {"CRASH", 196},
        {"ERROR", 196},   {"FATAL", 196},
    };

    for (size_t i = 0u; i < ND_ARRAY_LEN(expect); i++)
        check_int(expect[i].tag, nd_log_colour_for(expect[i].tag), expect[i].code);
}

/* ---- 2. the app band, 141 + (sum of bytes % 36) ---- */
static void test_app_band(void)
{
    static const struct {
        const char *tag;
        int code;
    } expect[] = {
        {"Koki", 143},  {"Music", 150},      {"CallLog", 163}, {"Settings", 162},
        {"PB", 143},    {"Tones", 158},      {"Games", 166},   {"Messages", 173},
        {"Clock", 165}, {"Calculator", 167}, {"Power", 162},
    };

    for (size_t i = 0u; i < ND_ARRAY_LEN(expect); i++)
        check_int(expect[i].tag, nd_log_colour_for(expect[i].tag), expect[i].code);
}

/* ---- 3. the unregistered band, 22 + (sum of bytes % 180) ----
 *
 * This is the half a port is most likely to get wrong, because it looks
 * correct until someone adds a subsystem. */
static void test_unregistered_band(void)
{
    static const struct {
        const char *tag;
        int code;
    } expect[] = {
        {"MEDIA", 194},    {"GPS", 76},    {"WIFI", 145},     {"NFC", 57},
        {"Bluetooth", 72}, {"Camera", 67}, {"nd-apprun", 39}, {"CUBE", 129},
        {"Dialer", 75},    {"T9", 163},    {"x", 142},        {"ZZ_LONG_TAG_NAME", 40},
    };

    for (size_t i = 0u; i < ND_ARRAY_LEN(expect); i++)
        check_int(expect[i].tag, nd_log_colour_for(expect[i].tag), expect[i].code);
}

/* ---- 4. the fourteen splitting edge cases, byte for byte ---- */
static void test_render_edge_cases(void)
{
    static const struct {
        const char *in;
        const char *out;
    } expect[] = {
        {"[MODEM] ordinary line", "\033[1m\033[38;5;39m[MODEM]\033[0m ordinary line"},
        {"no tag at all", "no tag at all"},
        /* "[]" -- the first ']' is at index 1, and the rule is index >= 2. */
        {"[] empty tag", "[] empty tag"},
        /* "[A]" -- index 2, so a ONE-character tag IS a tag. */
        {"[A] one character, too short for the end>=2 rule",
         "\033[1m\033[38;5;87m[A]\033[0m one character, too short for the end>=2 rule"},
        {"[AB] two characters", "\033[1m\033[38;5;153m[AB]\033[0m two characters"},
        {"[has space] not alphanumeric, must not be treated as a tag",
         "[has space] not alphanumeric, must not be treated as a tag"},
        {"[under_score] allowed", "\033[1m\033[38;5;119m[under_score]\033[0m allowed"},
        {"[with-dash] allowed", "\033[1m\033[38;5;27m[with-dash]\033[0m allowed"},
        /* The space after ']' belongs to the remainder, so a line with no
         * space still paints only the brackets. */
        {"[MODEM]no space after the bracket",
         "\033[1m\033[38;5;39m[MODEM]\033[0mno space after the bracket"},
        {"   [MODEM] leading whitespace means no tag",
         "   [MODEM] leading whitespace means no tag"},
        {"", ""},
        {"   ", "   "},
        {"[MODEM] trailing spaces   ", "\033[1m\033[38;5;39m[MODEM]\033[0m trailing spaces   "},
        {"[UPDATE] a line with an embedded ] bracket",
         "\033[1m\033[38;5;33m[UPDATE]\033[0m a line with an embedded ] bracket"},
    };

    char buf[512];

    for (size_t i = 0u; i < ND_ARRAY_LEN(expect); i++) {
        (void)nd_log_render(buf, sizeof buf, expect[i].in);
        check_str(expect[i].in[0] != '\0' ? expect[i].in : "(empty line)", buf, expect[i].out);
    }
}

/* ---- colour off means no escapes anywhere ---- */
static void test_colour_off(void)
{
    char buf[256];

    nd_log_set_colour(false);

    (void)nd_log_render(buf, sizeof buf, "[MODEM] plain");
    check_str("render, colour off", buf, "[MODEM] plain");

    (void)nd_log_paint(buf, sizeof buf, "text", 39, true);
    check_str("paint, colour off", buf, "text");

    (void)nd_log_rule(buf, sizeof buf, '=', 8u, 46);
    check_str("rule, colour off", buf, "========");

    nd_log_set_colour(true);
}

static void test_rule(void)
{
    char buf[256];

    (void)nd_log_rule(buf, sizeof buf, '=', 8u, 46);
    check_str("rule, colour on", buf, "\033[1m\033[38;5;46m========\033[0m");
}

static void test_split_tag(void)
{
    char tag[ND_LOG_TAG_MAX];
    const char *rest = NULL;

    check_bool("split [MODEM]", nd_log_split_tag("[MODEM] x", tag, sizeof tag, &rest), true);
    check_str("split tag text", tag, "MODEM");
    check_str("split remainder", rest, " x");

    check_bool("split []", nd_log_split_tag("[] x", tag, sizeof tag, &rest), false);
    check_bool("split no bracket", nd_log_split_tag("plain", tag, sizeof tag, &rest), false);
    check_bool("split unterminated", nd_log_split_tag("[MODEM x", tag, sizeof tag, &rest), false);

    /* A tag longer than the caller's buffer is a REFUSAL, not a truncation --
     * a truncated tag would silently get a different derived colour. */
    check_bool("split overlong", nd_log_split_tag("[MODEM] x", tag, 3u, &rest), false);
}

/* ---- the ND_ROOT hook, which everything else's tests will lean on ---- */
static void test_path_root(void)
{
    char buf[ND_PATH_MAX];

    check_int("set root", (int)nd_path_set_root("/tmp/ndroot"), (int)ND_OK);
    check_int("resolve", (int)nd_path_resolve(buf, sizeof buf, "/NeoDCT/User/x"), (int)ND_OK);
    check_str("resolve prefixed", buf, "/tmp/ndroot/NeoDCT/User/x");

    /* A relative path came from a command line or a fixture and means what it
     * says, so the prefix must not be applied to it. */
    check_int("resolve rel", (int)nd_path_resolve(buf, sizeof buf, "rel/path"), (int)ND_OK);
    check_str("resolve rel plain", buf, "rel/path");

    check_int("join",
              (int)nd_path_join(buf, sizeof buf, "/NeoDCT/System/apps/Koki", "manifest.json"),
              (int)ND_OK);
    check_str("join prefixed", buf, "/tmp/ndroot/NeoDCT/System/apps/Koki/manifest.json");

    check_int("clear root", (int)nd_path_set_root(NULL), (int)ND_OK);
    check_int("resolve bare", (int)nd_path_resolve(buf, sizeof buf, "/NeoDCT/x"), (int)ND_OK);
    check_str("resolve unprefixed", buf, "/NeoDCT/x");
}

/* ============ THE SYSTEM-LOG SINK ============
 *
 * Nothing above this point could have caught the fault that put it there: the
 * log went to a serial port with nothing attached, and every test of the
 * RENDERING passed the whole time. So this tests the delivery instead. It
 * stands up a datagram socket where /dev/log would be, under a staged root,
 * and reads what actually arrives.
 *
 * The two claims that matter:
 *   - the line arrives at all, in the shape busybox syslogd parses
 *   - it arrives WITHOUT the colour escapes, because this one is a file
 *     somebody greps and not a terminal
 */
static void test_the_system_log_sink(void)
{
    char root[] = "/tmp/ndlogXXXXXX";
    /* Sized to sun_path, not to ND_PATH_MAX: this string has to fit an
     * AF_UNIX address and the compiler is right to insist the copy cannot
     * truncate. /tmp/ndlogXXXXXX/dev/log is 22 bytes. */
    char sockpath[108];
    char devdir[128];
    struct sockaddr_un addr;
    char got[512];
    ssize_t n;
    int srv;

    if (mkdtemp(root) == NULL) {
        (void)fprintf(stderr, "test_nd_log: no temp dir; skipping the syslog sink\n");
        return;
    }
    (void)snprintf(devdir, sizeof devdir, "%s/dev", root);
    (void)mkdir(devdir, 0755);
    (void)snprintf(sockpath, sizeof sockpath, "%s%s", root, ND_PATH_DEV_LOG);

    srv = socket(AF_UNIX, SOCK_DGRAM, 0);
    check_int("syslog: server socket", srv >= 0, 1);
    if (srv < 0)
        return;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    (void)snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sockpath);
    check_int("syslog: bind", bind(srv, (struct sockaddr *)&addr, sizeof addr) == 0, 1);

    check_int("syslog: root", (int)nd_path_set_root(root), (int)ND_OK);
    check_int("syslog: open", (int)nd_log_syslog_open("ndtest"), (int)ND_OK);
    check_int("syslog: active", nd_log_syslog_active() ? 1 : 0, 1);

    /* Colour is ON for this whole file, so a line that arrives clean proves
     * the sink takes the unpainted text and not what goes to the terminal. */
    nd_log("MODEM", "hello %d", 42);
    memset(got, 0, sizeof got);
    n = recv(srv, got, sizeof got - 1u, 0);
    check_int("syslog: a line arrived", n > 0, 1);
    check_str("syslog: info line", got, "<14>ndtest: [MODEM] hello 42");

    nd_log_err("BT", "no dbus");
    memset(got, 0, sizeof got);
    n = recv(srv, got, sizeof got - 1u, 0);
    check_int("syslog: an error arrived", n > 0, 1);
    check_str("syslog: error line", got, "<11>ndtest: [BT] no dbus");

    /* Closed, and then silent: a process that never opens the sink must
     * behave exactly as it did before this existed. */
    nd_log_syslog_close();
    check_int("syslog: inactive after close", nd_log_syslog_active() ? 1 : 0, 0);
    nd_log("MODEM", "into the void");
    check_int("syslog: nothing after close",
              recv(srv, got, sizeof got - 1u, MSG_DONTWAIT) < 0 ? 1 : 0, 1);

    /* No syslogd to talk to is not a failure the caller has to act on, and it
     * must leave the sink shut rather than half-open. */
    (void)unlink(sockpath);
    check_int("syslog: no daemon", (int)nd_log_syslog_open("ndtest"), (int)ND_ERR_NOTFOUND);
    check_int("syslog: still inactive", nd_log_syslog_active() ? 1 : 0, 0);

    (void)close(srv);
    (void)rmdir(devdir);
    (void)rmdir(root);
    (void)nd_path_set_root(NULL);
}

int main(void)
{
    /* The oracle records the COLOURED form, so force colour on regardless of
     * how the test runner's terminal is configured. */
    nd_log_set_colour(true);

    test_named_palette();
    test_app_band();
    test_unregistered_band();
    test_render_edge_cases();
    test_rule();
    test_split_tag();
    test_colour_off();
    test_path_root();
    test_the_system_log_sink();

    (void)printf("test_nd_log: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

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
#include <string.h>

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

    (void)printf("test_nd_log: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

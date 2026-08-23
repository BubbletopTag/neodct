/* test_props.c -- the three prop dialects and the two writers.
 *
 * Each case names the Python function it mirrors and, where one exists, the
 * pytest that pins it. Risk R-5 in spec-storage-settings.md is that somebody
 * "helpfully" merges the three dialects into one; these tests are what makes
 * that a build failure rather than a subtle regression in RemoteShell.
 */

#include <stdio.h>
#include <string.h>

#include "nd_props.h"

#include "platform_test.h"

/* SettingsStorage._parse_settings -- the ordinary happy path. */
static void test_settings_dialect_basics(void)
{
    nd_props *p;

    pt_write_text("/p.prop", "  a = 1 \n"
                             "\n"
                             "# a comment\n"
                             "   # an indented comment, which IS a comment here\n"
                             "no equals sign\n"
                             "b=has=two=equals\n"
                             "a=2\n");

    p = nd_props_parse_settings("/p.prop");
    CHECK(p != NULL);
    CHECK_INT(nd_props_count(p), 2);
    /* last duplicate wins */
    CHECK_STR(nd_props_get(p, "a", "?"), "2");
    /* split on the FIRST '=' only */
    CHECK_STR(nd_props_get(p, "b", "?"), "has=two=equals");
    CHECK_STR(nd_props_get(p, "missing", "dflt"), "dflt");
    nd_props_free(p);
}

/* Python's str.splitlines() treats \n, \r\n and a lone \r as line ends. */
static void test_settings_dialect_line_endings(void)
{
    nd_props *p;

    pt_write_text("/p.prop", "a=1\r\nb=2\rc=3\n");

    p = nd_props_parse_settings("/p.prop");
    CHECK_INT(nd_props_count(p), 3);
    CHECK_STR(nd_props_get(p, "a", "?"), "1");
    CHECK_STR(nd_props_get(p, "b", "?"), "2");
    CHECK_STR(nd_props_get(p, "c", "?"), "3");
    nd_props_free(p);
}

/* test_settings_version_layering.py:87 writes exactly these bytes into
 * version.prop and requires the WHOLE file to read as empty. A parser that
 * split bytes and kept the lines that happened to be fine would pass every
 * other test here and fail that one. */
static void test_settings_dialect_rejects_bad_utf8(void)
{
    static const unsigned char corrupt[] = {0x00u, 0xffu, ' ', 'n', 'o', 't', ' ', 'a', ' ',
                                            'p',   'r',   'o', 'p', ' ', 'f', 'i', 'l', 'e'};
    nd_props *p;

    /* A perfectly good line placed AFTER the corruption, to prove that the
     * rejection is whole-file and not per-line. */
    pt_write("/v.prop", corrupt, sizeof corrupt);
    p = nd_props_parse_settings("/v.prop");
    CHECK(p != NULL);
    CHECK_INT(nd_props_count(p), 0);
    nd_props_free(p);

    {
        unsigned char mixed[64];
        size_t n = 0u;

        memcpy(mixed, "good=yes\n", 9u);
        n = 9u;
        mixed[n++] = 0xffu;
        mixed[n++] = '\n';
        pt_write("/v2.prop", mixed, n);

        p = nd_props_parse_settings("/v2.prop");
        CHECK_INT(nd_props_count(p), 0);
        nd_props_free(p);
    }
}

static void test_settings_dialect_missing_file(void)
{
    nd_props *p = nd_props_parse_settings("/nowhere/at/all.prop");

    CHECK(p != NULL);
    CHECK_INT(nd_props_count(p), 0);
    nd_props_free(p);
}

/* Storage._read_state -- errors="replace", so the file is never rejected. */
static void test_lenient_dialect_replaces_bad_bytes(void)
{
    static const unsigned char corrupt[] = {0x00u, 0xffu, 'g', 'a', 'r', 'b', 'a', 'g', 'e'};
    nd_props *p;

    /* test_storage.py:164: this reads as "absent" because no line has an '=',
     * NOT because the file was rejected. */
    pt_write("/s.prop", corrupt, sizeof corrupt);
    p = nd_props_parse_lenient("/s.prop");
    CHECK_INT(nd_props_count(p), 0);
    nd_props_free(p);

    {
        unsigned char mixed[64];
        size_t n = 0u;

        mixed[n++] = 0xffu;
        mixed[n++] = '\n';
        memcpy(mixed + n, "state=mounted\n", 14u);
        n += 14u;
        pt_write("/s2.prop", mixed, n);

        /* The difference from B-1, in one assertion: the good line survives. */
        p = nd_props_parse_lenient("/s2.prop");
        CHECK_INT(nd_props_count(p), 1);
        CHECK_STR(nd_props_get(p, "state", "?"), "mounted");
        nd_props_free(p);
    }
}

/* RemoteShell._read_props -- lines are NOT stripped before the '#' test, so a
 * leading space defeats the comment check. */
static void test_raw_dialect_leading_space_defeats_comment(void)
{
    nd_props *p = NULL;

    pt_write_text("/state.prop", "#enabled=1\n"
                                 " #host=relay.example\n"
                                 "user=neo\n"
                                 "no-equals\n");

    CHECK_INT(nd_props_parse_raw("/state.prop", &p), ND_OK);
    CHECK(p != NULL);
    CHECK_INT(nd_props_count(p), 2);
    CHECK_STR(nd_props_get(p, "#host", "?"), "relay.example");
    CHECK_STR(nd_props_get(p, "user", "?"), "neo");
    CHECK(!nd_props_has(p, "enabled"));
    nd_props_free(p);
}

static void test_raw_dialect_missing_file_is_ok(void)
{
    nd_props *p = NULL;

    /* Only OSError is caught in the Python, so a missing relay.conf is an
     * empty map and not an error. */
    CHECK_INT(nd_props_parse_raw("/no/such/file", &p), ND_OK);
    CHECK(p != NULL);
    CHECK_INT(nd_props_count(p), 0);
    nd_props_free(p);
}

static void test_raw_dialect_decode_error_propagates(void)
{
    static const unsigned char corrupt[] = {'a', '=', 0xffu, '\n'};
    nd_props *p = (nd_props *)0x1;

    /* A UnicodeDecodeError escapes settings() in the Python and is caught only
     * by the launcher's blanket handler. Here it is a return code. */
    pt_write("/bad.prop", corrupt, sizeof corrupt);
    CHECK_INT(nd_props_parse_raw("/bad.prop", &p), ND_ERR_PARSE);
    CHECK(p == NULL);
}

/* SettingsStorage._format_settings vs RemoteShell._write_props. */
static void test_writer_empty_map_edge_case(void)
{
    nd_props *p = nd_props_new();
    char buf[64];

    CHECK_INT(nd_props_write_atomic("/a/settings.prop", p, true), ND_OK);
    CHECK_INT(pt_read_text("/a/settings.prop", buf, sizeof buf), 1);
    CHECK_STR(buf, "\n");

    CHECK_INT(nd_props_write_atomic("/a/state.prop", p, false), ND_OK);
    CHECK_INT(pt_read_text("/a/state.prop", buf, sizeof buf), 0);
    CHECK_STR(buf, "");

    nd_props_free(p);
}

static void test_writer_sorts_and_round_trips(void)
{
    nd_props *p = nd_props_new();
    nd_props *back;
    char buf[256];

    CHECK_INT(nd_props_set(p, "zeta", "last"), ND_OK);
    CHECK_INT(nd_props_set(p, "alpha", "first"), ND_OK);
    CHECK_INT(nd_props_set(p, "Beta", "capital sorts before lowercase"), ND_OK);

    CHECK_INT(nd_props_write_atomic("/d/out.prop", p, true), ND_OK);
    CHECK(pt_read_text("/d/out.prop", buf, sizeof buf) != (size_t)-1);
    CHECK_STR(buf, "Beta=capital sorts before lowercase\nalpha=first\nzeta=last\n");

    /* The temp file must not survive a successful write. */
    CHECK(!nd_path_exists("/d/out.prop.tmp"));

    back = nd_props_parse_settings("/d/out.prop");
    CHECK_INT(nd_props_count(back), 3);
    CHECK_STR(nd_props_get(back, "alpha", "?"), "first");
    nd_props_free(back);
    nd_props_free(p);
}

static void test_map_operations(void)
{
    nd_props *a = nd_props_new();
    nd_props *b = nd_props_new();
    char big[ND_PROP_KEY_MAX + 8];

    CHECK_INT(nd_props_set(a, "k", "1"), ND_OK);
    CHECK_INT(nd_props_set(a, "j", "2"), ND_OK);

    /* Iteration is sorted, which is also the write order. */
    CHECK_STR(nd_props_key_at(a, 0u), "j");
    CHECK_STR(nd_props_key_at(a, 1u), "k");
    CHECK_STR(nd_props_value_at(a, 0u), "2");
    CHECK(nd_props_key_at(a, 2u) == NULL);

    /* Overwriting with a longer value must not corrupt neighbouring keys --
     * the value pool is shared and the slot is reused when it fits. */
    CHECK_INT(nd_props_set(a, "j", "a much longer replacement value"), ND_OK);
    CHECK_STR(nd_props_get(a, "j", "?"), "a much longer replacement value");
    CHECK_STR(nd_props_get(a, "k", "?"), "1");
    CHECK_INT(nd_props_set(a, "j", "s"), ND_OK);
    CHECK_STR(nd_props_get(a, "j", "?"), "s");

    CHECK_INT(nd_props_set(b, "k", "override"), ND_OK);
    CHECK_INT(nd_props_set(b, "new", "3"), ND_OK);
    CHECK_INT(nd_props_update(a, b), ND_OK);
    CHECK_INT(nd_props_count(a), 3);
    CHECK_STR(nd_props_get(a, "k", "?"), "override");

    CHECK_INT(nd_props_remove(a, "k"), ND_OK);
    CHECK_INT(nd_props_remove(a, "k"), ND_ERR_NOTFOUND);
    CHECK(!nd_props_has(a, "k"));
    CHECK_INT(nd_props_count(a), 2);

    memset(big, 'x', sizeof big - 1u);
    big[sizeof big - 1u] = '\0';
    CHECK_INT(nd_props_set(a, big, "v"), ND_ERR_TOOLONG);

    nd_props_free(a);
    nd_props_free(b);
}

int main(void)
{
    RUN(test_settings_dialect_basics);
    RUN(test_settings_dialect_line_endings);
    RUN(test_settings_dialect_rejects_bad_utf8);
    RUN(test_settings_dialect_missing_file);
    RUN(test_lenient_dialect_replaces_bad_bytes);
    RUN(test_raw_dialect_leading_space_defeats_comment);
    RUN(test_raw_dialect_missing_file_is_ok);
    RUN(test_raw_dialect_decode_error_propagates);
    RUN(test_writer_empty_map_edge_case);
    RUN(test_writer_sorts_and_round_trips);
    RUN(test_map_operations);
    return pt_report("test_props");
}

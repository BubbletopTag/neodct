/* test_ndkey.c -- nd-key's token table and argument parsing.
 *
 * Everything here is pure, which is the point of having split it out of
 * main(): the socket half is proved by test_devkey, and what is left is the
 * part where a mistake is invisible. "nd-key 5" pressing the wrong key would
 * not crash, would not warn, and would quietly make every scripted flow that
 * types a digit wrong.
 */

#include <string.h>

#include "nd_keycodes.h"
#include "nd_types.h"

#include "nd_key.h"

#include "platform_test.h"

/* ------------------------------------------------------------------ *
 * Tokens
 * ------------------------------------------------------------------ */

static void test_names_resolve_whatever_the_case(void)
{
    CHECK_INT(nd_key_token_to_code("menu"), ND_KEY_MENU);
    CHECK_INT(nd_key_token_to_code("MENU"), ND_KEY_MENU);
    CHECK_INT(nd_key_token_to_code("MeNu"), ND_KEY_MENU);
    CHECK_INT(nd_key_token_to_code("navikey"), ND_KEY_NAVIKEY);
    CHECK_INT(nd_key_token_to_code("NAVIKEY"), ND_KEY_NAVIKEY);
    CHECK_INT(nd_key_token_to_code("down"), ND_KEY_DOWN);
    CHECK_INT(nd_key_token_to_code("UP"), ND_KEY_UP);
    CHECK_INT(nd_key_token_to_code("clear"), ND_KEY_CLEAR);
    CHECK_INT(nd_key_token_to_code("star"), ND_KEY_STAR);
    CHECK_INT(nd_key_token_to_code("hash"), ND_KEY_HASH);
}

static void test_the_aliases_a_person_would_actually_type(void)
{
    /* Two names already alias two codes inside nd_keycode_for_name(); these
     * are the extra few this tool adds. Each one is a second spelling that
     * has to keep working, so the list is short on purpose. */
    CHECK_INT(nd_key_token_to_code("ok"), ND_KEY_NAVIKEY);
    CHECK_INT(nd_key_token_to_code("select"), ND_KEY_NAVIKEY);
    CHECK_INT(nd_key_token_to_code("enter"), ND_KEY_NAVIKEY); /* from the OS table */
    CHECK_INT(nd_key_token_to_code("back"), ND_KEY_CLEAR);    /* likewise */
    CHECK_INT(nd_key_token_to_code("c"), ND_KEY_CLEAR);
    CHECK_INT(nd_key_token_to_code("cancel"), ND_KEY_CLEAR);
}

static void test_a_single_digit_is_that_digit_key(void)
{
    /* THE case this file exists for. evdev's number row is 2..10 for 1..9 and
     * 11 for 0, which means the digit five is code SIX -- so a tool that took
     * "5" as a raw code would press four and nobody would notice until a
     * scripted PIN entry produced the wrong number. */
    CHECK_INT(nd_key_token_to_code("1"), ND_KEY_1);
    CHECK_INT(nd_key_token_to_code("5"), ND_KEY_5);
    CHECK_INT(nd_key_token_to_code("9"), ND_KEY_9);
    CHECK_INT(nd_key_token_to_code("0"), ND_KEY_0);

    /* Spelled out, so the mapping is asserted and not merely referenced. */
    CHECK_INT(nd_key_token_to_code("5"), 6);
    CHECK_INT(nd_key_token_to_code("0"), 11);
}

static void test_two_or_more_digits_is_a_raw_code(void)
{
    CHECK_INT(nd_key_token_to_code("50"), 50); /* which is MENU */
    CHECK_INT(nd_key_token_to_code("50"), ND_KEY_MENU);
    CHECK_INT(nd_key_token_to_code("108"), ND_KEY_DOWN);
    CHECK_INT(nd_key_token_to_code("11"), ND_KEY_0);
}

static void test_raw_prefix_reaches_the_single_digit_codes(void)
{
    /* The escape hatch: without it evdev codes 1..9 are unreachable, because
     * a bare single digit is spoken for. */
    CHECK_INT(nd_key_token_to_code("raw:5"), 5);
    CHECK_INT(nd_key_token_to_code("raw:6"), 6);
    CHECK_INT(nd_key_token_to_code("raw:50"), 50);
    /* And it is still bounded. */
    CHECK_INT(nd_key_token_to_code("raw:0"), -1);
    CHECK_INT(nd_key_token_to_code("raw:99999"), -1);
    CHECK_INT(nd_key_token_to_code("raw:x"), -1);
    CHECK_INT(nd_key_token_to_code("raw:"), -1);
}

static void test_the_symbols_on_the_keys(void)
{
    CHECK_INT(nd_key_token_to_code("*"), ND_KEY_STAR);
    CHECK_INT(nd_key_token_to_code("#"), ND_KEY_HASH);
}

static void test_rubbish_resolves_to_nothing(void)
{
    CHECK_INT(nd_key_token_to_code(""), -1);
    CHECK_INT(nd_key_token_to_code(NULL), -1);
    CHECK_INT(nd_key_token_to_code("nosuchkey"), -1);
    CHECK_INT(nd_key_token_to_code("5x"), -1);
    CHECK_INT(nd_key_token_to_code("-1"), -1);
    CHECK_INT(nd_key_token_to_code("99999"), -1);
    /* A name longer than the lowercase buffer must be refused, not truncated
     * into something that happens to match. */
    CHECK_INT(nd_key_token_to_code("mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm"
                                   "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm"),
              -1);
}

/* ------------------------------------------------------------------ *
 * Which keys the phone actually has
 * ------------------------------------------------------------------ */

static void test_the_matrix_column_is_honest(void)
{
    /* The sixteen from nd_kpsetup_targets[]. */
    CHECK(nd_key_on_matrix(ND_KEY_NAVIKEY));
    CHECK(nd_key_on_matrix(ND_KEY_CLEAR));
    CHECK(nd_key_on_matrix(ND_KEY_UP));
    CHECK(nd_key_on_matrix(ND_KEY_DOWN));
    CHECK(nd_key_on_matrix(ND_KEY_5));
    CHECK(nd_key_on_matrix(ND_KEY_0));
    CHECK(nd_key_on_matrix(ND_KEY_STAR));
    CHECK(nd_key_on_matrix(ND_KEY_HASH));

    /* And the ones a flow must not depend on. LEFT and RIGHT are the pair the
     * brief calls out; MENU is the one that surprises people, because the
     * home screen has a "Menu" label and the key behind it is NaviKey. */
    CHECK(!nd_key_on_matrix(ND_KEY_LEFT));
    CHECK(!nd_key_on_matrix(ND_KEY_RIGHT));
    CHECK(!nd_key_on_matrix(ND_KEY_MENU));
    CHECK(!nd_key_on_matrix(ND_KEY_SPACE));
}

/* ------------------------------------------------------------------ *
 * Argument parsing
 * ------------------------------------------------------------------ */

static bool parse(char **argv, int argc, nd_key_opts *o, char *err, size_t err_sz)
{
    return nd_key_parse_args(argc, argv, o, err, err_sz);
}

static void test_defaults(void)
{
    char *argv[] = {"nd-key", "MENU"};
    nd_key_opts o;
    char err[160];

    CHECK(parse(argv, 2, &o, err, sizeof err));
    CHECK_INT(o.action, ND_KEY_ACT_TAP);
    CHECK_INT(o.delay_ms, ND_KEY_DEFAULT_DELAY_MS);
    CHECK(o.socket_path == NULL);
    CHECK(!o.list);
    CHECK(!o.help);
    CHECK_INT(o.first_key, 1);
}

static void test_hold_and_release(void)
{
    char *hold[] = {"nd-key", "--hold", "DOWN"};
    char *rel[] = {"nd-key", "--release", "DOWN"};
    nd_key_opts o;
    char err[160];

    CHECK(parse(hold, 3, &o, err, sizeof err));
    CHECK_INT(o.action, ND_KEY_ACT_HOLD);
    CHECK_INT(o.first_key, 2);

    CHECK(parse(rel, 3, &o, err, sizeof err));
    CHECK_INT(o.action, ND_KEY_ACT_RELEASE);
}

static void test_delay_is_validated(void)
{
    char *good[] = {"nd-key", "--delay", "250", "1"};
    char *bad[] = {"nd-key", "--delay", "abc", "1"};
    char *huge[] = {"nd-key", "--delay", "999999", "1"};
    char *missing[] = {"nd-key", "--delay"};
    nd_key_opts o;
    char err[160];

    CHECK(parse(good, 4, &o, err, sizeof err));
    CHECK_INT(o.delay_ms, 250);

    CHECK(!parse(bad, 4, &o, err, sizeof err));
    CHECK(err[0] != '\0');
    CHECK(!parse(huge, 4, &o, err, sizeof err));
    CHECK(!parse(missing, 2, &o, err, sizeof err));
}

static void test_socket_option(void)
{
    char *argv[] = {"nd-key", "--socket", "/tmp/x", "MENU"};
    char *missing[] = {"nd-key", "--socket"};
    nd_key_opts o;
    char err[160];

    CHECK(parse(argv, 4, &o, err, sizeof err));
    CHECK_STR(o.socket_path, "/tmp/x");
    CHECK_INT(o.first_key, 3);

    CHECK(!parse(missing, 2, &o, err, sizeof err));
}

static void test_list_and_help_need_no_keys(void)
{
    char *l[] = {"nd-key", "--list"};
    char *h[] = {"nd-key", "--help"};
    nd_key_opts o;
    char err[160];

    CHECK(parse(l, 2, &o, err, sizeof err));
    CHECK(o.list);
    CHECK(parse(h, 2, &o, err, sizeof err));
    CHECK(o.help);
}

static void test_usage_errors(void)
{
    char *none[] = {"nd-key"};
    char *unknown_opt[] = {"nd-key", "--wat", "MENU"};
    char *bad_key[] = {"nd-key", "MENU", "nosuchkey"};
    nd_key_opts o;
    char err[160];

    CHECK(!parse(none, 1, &o, err, sizeof err));
    CHECK(!parse(unknown_opt, 3, &o, err, sizeof err));

    /* Every key is resolved before any is sent, so a typo in the last one
     * does not press the first three and then fail half way through a flow. */
    CHECK(!parse(bad_key, 3, &o, err, sizeof err));
    CHECK(strstr(err, "nosuchkey") != NULL);
}

static void test_everything_after_the_first_key_is_a_key(void)
{
    /* "--hold" here is the name of nothing, so it must be reported as a bad
     * KEY rather than silently switching the action half way along. */
    char *argv[] = {"nd-key", "MENU", "--hold"};
    nd_key_opts o;
    char err[160];

    CHECK(!parse(argv, 3, &o, err, sizeof err));
    CHECK(strstr(err, "--hold") != NULL);
}

int main(void)
{
    RUN(test_names_resolve_whatever_the_case);
    RUN(test_the_aliases_a_person_would_actually_type);
    RUN(test_a_single_digit_is_that_digit_key);
    RUN(test_two_or_more_digits_is_a_raw_code);
    RUN(test_raw_prefix_reaches_the_single_digit_codes);
    RUN(test_the_symbols_on_the_keys);
    RUN(test_rubbish_resolves_to_nothing);
    RUN(test_the_matrix_column_is_honest);
    RUN(test_defaults);
    RUN(test_hold_and_release);
    RUN(test_delay_is_validated);
    RUN(test_socket_option);
    RUN(test_list_and_help_need_no_keys);
    RUN(test_usage_errors);
    RUN(test_everything_after_the_first_key_is_a_key);
    return pt_report("test_ndkey");
}

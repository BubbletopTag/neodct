/* test_t9_engine.c -- a direct port of neodct/tests/test_t9_engine.py.
 *
 * Every case below has the same name as the pytest it came from, so a
 * behaviour argument can be settled by reading the two side by side. The
 * three added at the end come from test_framework_predictive.py, which drives
 * the engine through the widgets; only the engine half is ported here.
 *
 * The clock is injected, so nothing sleeps: advancing time past the multi-tap
 * window is a variable assignment.
 */

#include <stdio.h>
#include <string.h>

#include "nd_keycodes.h"
#include "nd_t9.h"

#include "platform_test.h"

/* The NeoDCT evdev codes, spelled as the pytest spells them. */
#define K1   2
#define K2   3
#define K3   4
#define K4   5
#define K5   6
#define K6   7
#define K7   8
#define K8   9
#define K9   10
#define K0   11
#define STAR 42
#define HASH 43
#define UP   103 /* a nav key the engine must not consume */

typedef struct {
    double t;
} fake_clock;

static double fake_now(void *ctx)
{
    return ((const fake_clock *)ctx)->t;
}

static fake_clock g_clock;

static void make(nd_t9_engine *e, nd_t9_filter f, double timeout)
{
    g_clock.t = 100.0;
    CHECK_INT(nd_t9_engine_init(e, f, timeout, fake_now, &g_clock), ND_OK);
}

/* ("append", ch) and friends, as one assertion. */
static void check_op(nd_t9_op op, nd_t9_opkind kind, char ch, const char *where)
{
    g_checks++;
    if (op.kind != kind || (kind == ND_T9_OP_APPEND && op.ch != ch) ||
        (kind == ND_T9_OP_REPLACE && op.ch != ch)) {
        g_failures++;
        fprintf(stderr, "FAIL %s  got (kind %d, '%c') want (kind %d, '%c')\n", where, (int)op.kind,
                op.ch ? op.ch : '?', (int)kind, ch ? ch : '?');
    }
}

#define APPEND(op, c)  check_op((op), ND_T9_OP_APPEND, (c), __FILE__ ":" ND_STR(__LINE__))
#define REPLACE(op, c) check_op((op), ND_T9_OP_REPLACE, (c), __FILE__ ":" ND_STR(__LINE__))
#define NOTHING(op)    check_op((op), ND_T9_OP_NONE, '\0', __FILE__ ":" ND_STR(__LINE__))
#define ND_STR_(x)     #x
#define ND_STR(x)      ND_STR_(x)

/* --- basic multi-tap cycling (abc mode) --- */

static void test_default_mode_is_abc(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    CHECK_STR(nd_t9_mode_label(nd_t9_engine_mode(&e)), "abc");
}

static void test_first_press_appends_first_letter(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    APPEND(nd_t9_engine_press(&e, K2), 'a');
}

static void test_second_press_same_key_cycles(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    (void)nd_t9_engine_press(&e, K2);
    REPLACE(nd_t9_engine_press(&e, K2), 'b');
    REPLACE(nd_t9_engine_press(&e, K2), 'c');
}

static void test_cycle_includes_digit_and_wraps(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    (void)nd_t9_engine_press(&e, K2); /* a */
    (void)nd_t9_engine_press(&e, K2); /* b */
    (void)nd_t9_engine_press(&e, K2); /* c */
    REPLACE(nd_t9_engine_press(&e, K2), '2');
    REPLACE(nd_t9_engine_press(&e, K2), 'a');
}

static void test_different_key_commits_and_appends(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    APPEND(nd_t9_engine_press(&e, K2), 'a');
    APPEND(nd_t9_engine_press(&e, K3), 'd');
}

static void test_press_after_timeout_starts_new_letter(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    (void)nd_t9_engine_press(&e, K2);
    g_clock.t += 1.5;
    APPEND(nd_t9_engine_press(&e, K2), 'a');
}

static void test_press_within_timeout_cycles(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    (void)nd_t9_engine_press(&e, K2);
    g_clock.t += 0.5;
    REPLACE(nd_t9_engine_press(&e, K2), 'b');
}

static void test_zero_key_is_space_then_zero(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    APPEND(nd_t9_engine_press(&e, K0), ' ');
    REPLACE(nd_t9_engine_press(&e, K0), '0');
}

static void test_one_key_cycles_punctuation(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    APPEND(nd_t9_engine_press(&e, K1), '.');
    REPLACE(nd_t9_engine_press(&e, K1), ',');
}

static void test_star_not_consumed_in_abc_mode(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    NOTHING(nd_t9_engine_press(&e, STAR));
}

static void test_nav_key_returns_none_and_commits_pending(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    (void)nd_t9_engine_press(&e, K2);
    NOTHING(nd_t9_engine_press(&e, UP));
    /* The pending cycle was committed, so the next press starts a letter. */
    APPEND(nd_t9_engine_press(&e, K2), 'a');
}

static void test_reset_clears_pending_cycle(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    (void)nd_t9_engine_press(&e, K2);
    nd_t9_engine_reset(&e);
    APPEND(nd_t9_engine_press(&e, K2), 'a');
}

/* --- mode cycling (# key) --- */

static void check_mode(nd_t9_op op, const char *want)
{
    g_checks++;
    if (op.kind != ND_T9_OP_MODE || strcmp(nd_t9_mode_label(op.mode), want) != 0) {
        g_failures++;
        fprintf(stderr, "FAIL mode: got kind %d \"%s\" want \"%s\"\n", (int)op.kind,
                nd_t9_mode_label(op.mode), want);
    }
}

static void test_hash_cycles_modes_any_filter(void)
{
    nd_t9_engine e;

    /* Typing starts in abc -- what every field has always done -- and # walks
     * all the way round, predictive included, back to it. */
    make(&e, ND_T9_FILTER_ANY, 1.0);
    CHECK_STR(nd_t9_mode_label(nd_t9_engine_mode(&e)), "abc");
    check_mode(nd_t9_engine_press(&e, HASH), "ABC");
    check_mode(nd_t9_engine_press(&e, HASH), "123");
    check_mode(nd_t9_engine_press(&e, HASH), "word");
    check_mode(nd_t9_engine_press(&e, HASH), "abc");
}

static void test_upper_mode_appends_uppercase(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    (void)nd_t9_engine_press(&e, HASH); /* -> ABC */
    APPEND(nd_t9_engine_press(&e, K2), 'A');
    REPLACE(nd_t9_engine_press(&e, K2), 'B');
}

static void test_mode_change_commits_pending_cycle(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    (void)nd_t9_engine_press(&e, K2);   /* pending "a" */
    (void)nd_t9_engine_press(&e, HASH); /* -> ABC      */
    APPEND(nd_t9_engine_press(&e, K2), 'A');
}

static void test_123_mode_appends_digits_without_cycling(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    (void)nd_t9_engine_press(&e, HASH); /* ABC */
    (void)nd_t9_engine_press(&e, HASH); /* 123 */
    APPEND(nd_t9_engine_press(&e, K2), '2');
    APPEND(nd_t9_engine_press(&e, K2), '2');
}

static void test_123_mode_star_is_literal(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    (void)nd_t9_engine_press(&e, HASH);
    (void)nd_t9_engine_press(&e, HASH); /* 123 */
    APPEND(nd_t9_engine_press(&e, STAR), '*');
}

/* --- letters-only filter --- */

static void test_letters_filter_has_no_123_mode(void)
{
    nd_t9_engine e;
    const nd_t9_mode *modes;
    size_t n = 0u;
    size_t i;
    bool has_123 = false;

    /* A letters-only field keeps predictive -- it is the same alphabet -- but
     * must never offer the digit mode. */
    make(&e, ND_T9_FILTER_LETTERS, 1.0);
    check_mode(nd_t9_engine_press(&e, HASH), "ABC");
    check_mode(nd_t9_engine_press(&e, HASH), "word");
    check_mode(nd_t9_engine_press(&e, HASH), "abc");

    modes = nd_t9_engine_modes(&e, &n);
    for (i = 0u; i < n; i++) {
        if (modes[i] == ND_T9_MODE_123)
            has_123 = true;
    }
    CHECK(!has_123);
}

static void test_letters_filter_cycle_has_no_digit(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_LETTERS, 1.0);
    (void)nd_t9_engine_press(&e, K2); /* a */
    (void)nd_t9_engine_press(&e, K2); /* b */
    (void)nd_t9_engine_press(&e, K2); /* c */
    REPLACE(nd_t9_engine_press(&e, K2), 'a');
}

static void test_letters_filter_zero_is_space(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_LETTERS, 1.0);
    APPEND(nd_t9_engine_press(&e, K0), ' ');
}

/* --- numbers-only filter --- */

static void test_numbers_filter_digits_are_literal(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_NUMBERS, 1.0);
    CHECK_STR(nd_t9_mode_label(nd_t9_engine_mode(&e)), "123");
    APPEND(nd_t9_engine_press(&e, K2), '2');
    APPEND(nd_t9_engine_press(&e, K2), '2');
}

static void test_numbers_filter_star_and_hash_are_literal(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_NUMBERS, 1.0);
    APPEND(nd_t9_engine_press(&e, STAR), '*');
    APPEND(nd_t9_engine_press(&e, HASH), '#');
    /* hash must NOT switch modes when there is only one */
    CHECK_STR(nd_t9_mode_label(nd_t9_engine_mode(&e)), "123");
}

/* --- char_allowed --- */

static void test_char_allowed_any(void)
{
    CHECK(nd_t9_char_allowed('a', ND_T9_FILTER_ANY));
    CHECK(nd_t9_char_allowed('5', ND_T9_FILTER_ANY));
}

static void test_char_allowed_letters_rejects_digits(void)
{
    CHECK(nd_t9_char_allowed('a', ND_T9_FILTER_LETTERS));
    CHECK(nd_t9_char_allowed(' ', ND_T9_FILTER_LETTERS));
    CHECK(!nd_t9_char_allowed('5', ND_T9_FILTER_LETTERS));
}

static void test_char_allowed_numbers(void)
{
    CHECK(nd_t9_char_allowed('5', ND_T9_FILTER_NUMBERS));
    CHECK(nd_t9_char_allowed('*', ND_T9_FILTER_NUMBERS));
    CHECK(nd_t9_char_allowed('#', ND_T9_FILTER_NUMBERS));
    CHECK(nd_t9_char_allowed('+', ND_T9_FILTER_NUMBERS));
    CHECK(!nd_t9_char_allowed('a', ND_T9_FILTER_NUMBERS));
    CHECK(!nd_t9_char_allowed(' ', ND_T9_FILTER_NUMBERS));
}

/* --- predictive, from test_framework_predictive.py --- */

/* Switch to the predictive mode the way the widgets do: by index into
 * modes(), not by counting # presses. */
static void enter_word_mode(nd_t9_engine *e)
{
    const nd_t9_mode *modes;
    size_t n = 0u;
    size_t i;

    modes = nd_t9_engine_modes(e, &n);
    for (i = 0u; i < n; i++) {
        if (modes[i] == ND_T9_MODE_WORD) {
            (void)nd_t9_engine_set_mode_index(e, i);
            return;
        }
    }
    CHECK(false);
}

static void test_a_sequence_of_digits_becomes_a_word(void)
{
    nd_t9_engine e;
    nd_t9_op op;

    /* test_a_sequence_of_digits_becomes_a_word: the widget asks the
     * dictionary once per press, with the sequence so far. */
    make(&e, ND_T9_FILTER_ANY, 1.0);
    enter_word_mode(&e);

    op = nd_t9_engine_press(&e, K4);
    CHECK_INT(op.kind, ND_T9_OP_WORD);
    CHECK_STR(op.digits, "4");
    op = nd_t9_engine_press(&e, K6);
    CHECK_STR(op.digits, "46");
    op = nd_t9_engine_press(&e, K6);
    CHECK_STR(op.digits, "466");
    op = nd_t9_engine_press(&e, K3);
    CHECK_STR(op.digits, "4663");
    CHECK_STR(nd_t9_engine_word_digits(&e), "4663");
}

static void test_star_offers_the_next_word(void)
{
    nd_t9_engine e;
    nd_t9_op op;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    enter_word_mode(&e);
    (void)nd_t9_engine_press(&e, K4);
    (void)nd_t9_engine_press(&e, K3);

    op = nd_t9_engine_press(&e, STAR);
    CHECK_INT(op.kind, ND_T9_OP_NEXT);
    CHECK_STR(op.digits, "43");
    /* Deliberately does NOT reset: the digits typed ARE the word. */
    CHECK_STR(nd_t9_engine_word_digits(&e), "43");
}

static void test_star_with_nothing_typed_does_nothing(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    enter_word_mode(&e);
    NOTHING(nd_t9_engine_press(&e, STAR));
}

static void test_a_space_ends_the_word_and_is_typed(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    enter_word_mode(&e);
    (void)nd_t9_engine_press(&e, K4);
    (void)nd_t9_engine_press(&e, K6);

    /* Key 0 carries no letters, so the word ends and the press falls through
     * to the ordinary space cycle -- which is the caller's cue to commit. */
    APPEND(nd_t9_engine_press(&e, K0), ' ');
    CHECK_STR(nd_t9_engine_word_digits(&e), "");
}

static void test_punctuation_ends_the_word_too(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    enter_word_mode(&e);
    (void)nd_t9_engine_press(&e, K4);
    APPEND(nd_t9_engine_press(&e, K1), '.');
    CHECK_STR(nd_t9_engine_word_digits(&e), "");
}

static void test_changing_mode_keeps_no_digits(void)
{
    nd_t9_engine e;

    /* # commits: the widget keeps the guess in the field, the engine drops
     * the digits that produced it. */
    make(&e, ND_T9_FILTER_ANY, 1.0);
    enter_word_mode(&e);
    (void)nd_t9_engine_press(&e, K4);
    (void)nd_t9_engine_press(&e, K6);
    check_mode(nd_t9_engine_press(&e, HASH), "abc");
    CHECK_STR(nd_t9_engine_word_digits(&e), "");
}

static void test_clear_takes_a_digit_off(void)
{
    nd_t9_engine e;
    const char *left;

    /* Not a letter off the guessed word: the user typed digits, so that is
     * what an undo has to undo. */
    make(&e, ND_T9_FILTER_ANY, 1.0);
    enter_word_mode(&e);
    (void)nd_t9_engine_press(&e, K4);
    (void)nd_t9_engine_press(&e, K6);
    (void)nd_t9_engine_press(&e, K6);
    (void)nd_t9_engine_press(&e, K3);

    left = nd_t9_engine_pop_word_digit(&e);
    CHECK_STR(left, "466");
    left = nd_t9_engine_pop_word_digit(&e);
    CHECK_STR(left, "46");
    left = nd_t9_engine_pop_word_digit(&e);
    CHECK_STR(left, "4");
    left = nd_t9_engine_pop_word_digit(&e);
    CHECK_STR(left, "");
    /* Nothing left to drop: the caller now treats Clear as a backspace. */
    CHECK(nd_t9_engine_pop_word_digit(&e) == NULL);
}

static void test_clear_word_and_set_mode_index_wrap(void)
{
    nd_t9_engine e;

    make(&e, ND_T9_FILTER_ANY, 1.0);
    enter_word_mode(&e);
    (void)nd_t9_engine_press(&e, K4);
    nd_t9_engine_clear_word(&e);
    CHECK_STR(nd_t9_engine_word_digits(&e), "");

    /* set_mode_index takes the index modulo the cycle length. */
    CHECK_STR(nd_t9_mode_label(nd_t9_engine_set_mode_index(&e, 5u)), "abc");
}

static void test_the_digit_accumulator_stops_at_the_cap(void)
{
    nd_t9_engine e;
    int i;

    /* Deviation C-3, approved: the Python grows this without limit. Beyond
     * ND_T9_DIGITS_MAX the engine stops appending rather than keeping a
     * sequence no dictionary key could ever match. */
    make(&e, ND_T9_FILTER_ANY, 1.0);
    enter_word_mode(&e);
    for (i = 0; i < ND_T9_DIGITS_MAX + 10; i++)
        (void)nd_t9_engine_press(&e, K2);
    CHECK_INT(strlen(nd_t9_engine_word_digits(&e)), ND_T9_DIGITS_MAX);
}

static void test_an_unknown_filter_is_refused(void)
{
    nd_t9_engine e;

    /* T9Engine.__init__ raises ValueError; the C returns ND_ERR_INVAL. */
    CHECK_INT(nd_t9_engine_init(&e, (nd_t9_filter)99, 1.0, NULL, NULL), ND_ERR_INVAL);
}

int main(void)
{
    RUN(test_default_mode_is_abc);
    RUN(test_first_press_appends_first_letter);
    RUN(test_second_press_same_key_cycles);
    RUN(test_cycle_includes_digit_and_wraps);
    RUN(test_different_key_commits_and_appends);
    RUN(test_press_after_timeout_starts_new_letter);
    RUN(test_press_within_timeout_cycles);
    RUN(test_zero_key_is_space_then_zero);
    RUN(test_one_key_cycles_punctuation);
    RUN(test_star_not_consumed_in_abc_mode);
    RUN(test_nav_key_returns_none_and_commits_pending);
    RUN(test_reset_clears_pending_cycle);
    RUN(test_hash_cycles_modes_any_filter);
    RUN(test_upper_mode_appends_uppercase);
    RUN(test_mode_change_commits_pending_cycle);
    RUN(test_123_mode_appends_digits_without_cycling);
    RUN(test_123_mode_star_is_literal);
    RUN(test_letters_filter_has_no_123_mode);
    RUN(test_letters_filter_cycle_has_no_digit);
    RUN(test_letters_filter_zero_is_space);
    RUN(test_numbers_filter_digits_are_literal);
    RUN(test_numbers_filter_star_and_hash_are_literal);
    RUN(test_char_allowed_any);
    RUN(test_char_allowed_letters_rejects_digits);
    RUN(test_char_allowed_numbers);
    RUN(test_a_sequence_of_digits_becomes_a_word);
    RUN(test_star_offers_the_next_word);
    RUN(test_star_with_nothing_typed_does_nothing);
    RUN(test_a_space_ends_the_word_and_is_typed);
    RUN(test_punctuation_ends_the_word_too);
    RUN(test_changing_mode_keeps_no_digits);
    RUN(test_clear_takes_a_digit_off);
    RUN(test_clear_word_and_set_mode_index_wrap);
    RUN(test_the_digit_accumulator_stops_at_the_cap);
    RUN(test_an_unknown_filter_is_refused);
    return pt_report("test_t9_engine");
}

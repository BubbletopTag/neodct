/* test_calculator.c -- the Calculator app: the arithmetic, and both frames.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. format_number() agrees with the Python's for thirty-three values,
 *     including the four that decide the branches: an exact integer under
 *     10**12, an exact integer AT 10**12 (which takes the "%.8f" path and
 *     comes back looking like an integer anyway), a result longer than
 *     MAX_DIGITS + 2 (which becomes "%.6e"), and NaN/inf ("Error"). Every
 *     expectation in the table was produced by RUNNING
 *     System/apps/Calculator/main.py's own format_number, not by reasoning
 *     about it.
 *
 *  2. The tables are the Python's dicts: OP_FOR_OPTION has exactly four
 *     entries and Equals is not one of them; DIGIT_KEYS runs 1..9 then 0,
 *     which is a phone's layout and not ASCII's.
 *
 *  3. The state machine reproduces ten recorded sequences, again taken from
 *     the Python by running it -- including `2 + 3 * 4 =` being 20 rather
 *     than 14 (the fold is left to right, there is no precedence), `007`
 *     collapsing to `7`, and division by zero becoming "Error" where C
 *     arithmetic would have produced an infinity.
 *
 *  4. The twelve-character cap counts what the Python counts: characters
 *     after lstrip("-") with the points removed. A full entry still accepts
 *     a decimal point, because type_point() has no cap of its own.
 *
 *  5. BOTH GOLDEN FRAMES. app-calculator is 1, 2, 3 typed; the reference is
 *     the fourth committed frame. app-calculator-options is 7 then Enter;
 *     the reference is the third. Judged by the SHA-256 over raw RGB that
 *     goldenframe.py compares, so a pass here is a pass there, and the
 *     differing-pixel count is printed when it is not.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set (the Makefile
 * passes it) and the font is found relative to it.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "smallapp_test.h"

#include "../../apps/Calculator/calculator.h"

/* ------------------------------------------------------------------ *
 * The app's exported surface
 * ------------------------------------------------------------------ */

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    const char *(*format_number)(double, char *, size_t);
    char (*op_for_option)(int32_t);
    char (*digit_for_key)(int32_t);
    void (*init)(nd_calc *);
    void (*fold)(nd_calc *);
    void (*apply_option)(nd_calc *, int32_t);
    void (*type_digit)(nd_calc *, char);
    void (*type_point)(nd_calc *);
    bool (*clear)(nd_calc *);
    const char *(*display_text)(const nd_calc *, char *, size_t);
    const char *const *options;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&api.format_number = sa_sym(h, "nd_calc_format_number");
    *(void **)&api.op_for_option = sa_sym(h, "nd_calc_op_for_option");
    *(void **)&api.digit_for_key = sa_sym(h, "nd_calc_digit_for_key");
    *(void **)&api.init = sa_sym(h, "nd_calc_init");
    *(void **)&api.fold = sa_sym(h, "nd_calc_fold");
    *(void **)&api.apply_option = sa_sym(h, "nd_calc_apply_option");
    *(void **)&api.type_digit = sa_sym(h, "nd_calc_type_digit");
    *(void **)&api.type_point = sa_sym(h, "nd_calc_type_point");
    *(void **)&api.clear = sa_sym(h, "nd_calc_clear");
    *(void **)&api.display_text = sa_sym(h, "nd_calc_display_text");
    api.options = dlsym(h, "nd_calc_options");

    return api.run != NULL && api.shutdown != NULL && api.format_number != NULL &&
           api.op_for_option != NULL && api.digit_for_key != NULL && api.init != NULL &&
           api.fold != NULL && api.apply_option != NULL && api.type_digit != NULL &&
           api.type_point != NULL && api.clear != NULL && api.display_text != NULL &&
           api.options != NULL;
}

/* ------------------------------------------------------------------ *
 * 1. format_number()
 * ------------------------------------------------------------------ */

static void expect_fmt(double v, const char *want)
{
    char out[ND_CALC_FMT_CAP];
    char what[96];

    (void)snprintf(what, sizeof what, "format_number(%.17g)", v);
    CHECK_STR(api.format_number(v, out, sizeof out), want, what);
}

static void test_format_number(void)
{
    /* Produced by running System/apps/Calculator/main.py's format_number. */
    expect_fmt(0.0, "0");
    expect_fmt(-0.0, "0");
    expect_fmt(7.0, "7");
    expect_fmt(-7.0, "-7");
    expect_fmt(123.0, "123");
    expect_fmt(0.5, "0.5");
    expect_fmt(-0.5, "-0.5");
    expect_fmt(1.0 / 3.0, "0.33333333");
    expect_fmt(2.0 / 3.0, "0.66666667");
    expect_fmt(2.0 / 7.0, "0.28571429");
    expect_fmt(1e11, "100000000000");
    expect_fmt(999999999999.0, "999999999999");

    /* 10**12 EXACTLY: `abs(value) < 10 ** MAX_DIGITS` is false, so it takes
     * the "%.8f" branch -- and comes back looking like an integer anyway,
     * because the strip removes the point. The two branches meet here and
     * the seam is invisible on screen. */
    expect_fmt(1e12, "1000000000000");
    expect_fmt(1000000000001.0, "1000000000001");
    expect_fmt(123456789012.0, "123456789012");
    expect_fmt(1234567890123.0, "1234567890123");

    /* Thirteen digits is len 13, which is <= MAX_DIGITS + 2; sixteen is not,
     * and drops to "%.6e". */
    expect_fmt(1e15, "1.000000e+15");
    expect_fmt(1e16, "1.000000e+16");
    expect_fmt(1e20, "1.000000e+20");
    expect_fmt(-1e20, "-1.000000e+20");
    expect_fmt(1e100, "1.000000e+100");

    expect_fmt(1.5, "1.5");
    expect_fmt(0.1 + 0.2, "0.3");
    expect_fmt(12345678.9, "12345678.9");
    expect_fmt(3.14159265358979, "3.14159265");

    /* Eight decimals is all "%.8f" has, so anything smaller vanishes -- and
     * a negative one keeps its sign over a zero. Both are the Python's. */
    expect_fmt(1e-8, "0.00000001");
    expect_fmt(1e-9, "0");
    expect_fmt(1e-12, "0");
    expect_fmt(-1e-12, "-0");

    expect_fmt(NAN, "Error");
    expect_fmt(INFINITY, "Error");
    expect_fmt(-INFINITY, "Error");
}

/* ------------------------------------------------------------------ *
 * 2. The tables
 * ------------------------------------------------------------------ */

static void test_tables(void)
{
    int32_t i;
    int n_ops = 0;
    int n_digits = 0;

    CHECK_STR(api.options[0], "Equals", "OPTIONS[0]");
    CHECK_STR(api.options[1], "Add", "OPTIONS[1]");
    CHECK_STR(api.options[2], "Subtract", "OPTIONS[2]");
    CHECK_STR(api.options[3], "Multiply", "OPTIONS[3]");
    CHECK_STR(api.options[4], "Divide", "OPTIONS[4]");

    CHECK_INT(api.op_for_option(0), '\0', "Equals is NOT in OP_FOR_OPTION");
    CHECK_INT(api.op_for_option(1), '+', "1 -> +");
    CHECK_INT(api.op_for_option(2), '-', "2 -> -");
    CHECK_INT(api.op_for_option(3), '*', "3 -> *");
    CHECK_INT(api.op_for_option(4), '/', "4 -> /");
    for (i = -5; i < 64; i++) {
        if (api.op_for_option(i) != '\0')
            n_ops++;
    }
    CHECK_INT(n_ops, 4, "exactly four operators");

    /* DIGIT_KEYS = {2:'1', 3:'2', ... 10:'9', 11:'0'} */
    CHECK_INT(api.digit_for_key(2), '1', "key 2 is '1'");
    CHECK_INT(api.digit_for_key(10), '9', "key 10 is '9'");
    CHECK_INT(api.digit_for_key(11), '0', "key 11 is '0' and NOT ':'");
    CHECK_INT(api.digit_for_key(12), '\0', "key 12 is not a digit");
    CHECK_INT(api.digit_for_key(1), '\0', "key 1 is not a digit");
    for (i = -5; i < 256; i++) {
        if (api.digit_for_key(i) != '\0')
            n_digits++;
    }
    CHECK_INT(n_digits, 10, "exactly ten digit keys");
}

/* ------------------------------------------------------------------ *
 * 3. The state machine
 * ------------------------------------------------------------------ */

static void type(nd_calc *c, const char *digits)
{
    const char *p;

    for (p = digits; *p != '\0'; p++)
        api.type_digit(c, *p);
}

static void expect_state(const nd_calc *c, const char *entry, bool has_acc, double acc, char op,
                         const char *what)
{
    char note[128];

    (void)snprintf(note, sizeof note, "%s: entry", what);
    CHECK_STR(c->entry, entry, note);
    (void)snprintf(note, sizeof note, "%s: has_acc", what);
    CHECK_INT(c->has_acc, has_acc, note);
    if (has_acc) {
        (void)snprintf(note, sizeof note, "%s: acc", what);
        CHECK_DBL(c->acc, acc, note);
    }
    (void)snprintf(note, sizeof note, "%s: pending_op", what);
    CHECK_INT(c->pending_op, op, note);
}

static void test_state_machine(void)
{
    nd_calc c;

    /* 12 + 5 = 17. The Add folds 12 into the accumulator and empties the
     * entry, which is why the display falls back to showing the accumulator
     * until the next digit. */
    api.init(&c);
    type(&c, "12");
    api.apply_option(&c, 1);
    expect_state(&c, "", true, 12.0, '+', "12 then Add");
    type(&c, "5");
    api.apply_option(&c, 0);
    expect_state(&c, "17", false, 0.0, '\0', "12 + 5 =");

    /* Divide by zero. Python raises ZeroDivisionError; C would have produced
     * an infinity and printed "Error" only by accident. */
    api.init(&c);
    type(&c, "8");
    api.apply_option(&c, 4);
    type(&c, "0");
    api.apply_option(&c, 0);
    expect_state(&c, "Error", false, 0.0, '\0', "8 / 0 =");
    /* And a digit clears it. */
    type(&c, "3");
    expect_state(&c, "3", false, 0.0, '\0', "Error then 3");

    /* A leading zero is REPLACED, not typed after. */
    api.init(&c);
    type(&c, "0");
    expect_state(&c, "0", false, 0.0, '\0', "0");
    type(&c, "5");
    expect_state(&c, "5", false, 0.0, '\0', "0 then 5");

    /* "007" collapses to "7" one digit at a time. */
    api.init(&c);
    type(&c, "007");
    expect_state(&c, "7", false, 0.0, '\0', "007");
    api.apply_option(&c, 0);
    expect_state(&c, "7", false, 0.0, '\0', "007 then Equals");

    /* A point on an empty entry is "0.", and a second one does nothing. */
    api.init(&c);
    api.type_point(&c);
    expect_state(&c, "0.", false, 0.0, '\0', "point on empty");
    api.type_point(&c);
    expect_state(&c, "0.", false, 0.0, '\0', "second point");

    api.init(&c);
    type(&c, "1");
    api.type_point(&c);
    type(&c, "5");
    expect_state(&c, "1.5", false, 0.0, '\0', "1.5");

    /* Left to right, no precedence: 2 + 3 * 4 is (2+3)*4 = 20. */
    api.init(&c);
    type(&c, "2");
    api.apply_option(&c, 1);
    type(&c, "3");
    api.apply_option(&c, 3);
    expect_state(&c, "", true, 5.0, '*', "2 + 3 then Multiply");
    type(&c, "4");
    api.apply_option(&c, 0);
    expect_state(&c, "20", false, 0.0, '\0', "2 + 3 * 4 =");

    api.init(&c);
    type(&c, "9");
    api.apply_option(&c, 2);
    type(&c, "4");
    api.apply_option(&c, 0);
    expect_state(&c, "5", false, 0.0, '\0', "9 - 4 =");

    api.init(&c);
    type(&c, "6");
    api.apply_option(&c, 4);
    type(&c, "4");
    api.apply_option(&c, 0);
    expect_state(&c, "1.5", false, 0.0, '\0', "6 / 4 =");

    /* _fold() with nothing pending just moves the entry into the
     * accumulator. */
    api.init(&c);
    type(&c, "5");
    api.fold(&c);
    expect_state(&c, "", true, 5.0, '\0', "fold with no operator");

    /* An "Error" entry stops a fold dead: nothing is parsed and nothing is
     * assigned. */
    api.init(&c);
    (void)nd_strlcpy(c.entry, "Error", sizeof c.entry);
    api.fold(&c);
    expect_state(&c, "Error", false, 0.0, '\0', "fold on Error");
}

static void test_digit_cap(void)
{
    nd_calc c;

    /* Twelve characters, and the thirteenth is refused. */
    api.init(&c);
    type(&c, "1234567890123");
    expect_state(&c, "123456789012", false, 0.0, '\0', "twelve digits");

    /* type_point() has NO cap of its own, so a full entry still takes one. */
    api.type_point(&c);
    expect_state(&c, "123456789012.", false, 0.0, '\0', "full entry plus a point");

    /* The count is of characters after lstrip("-") with the points removed,
     * so a point does not use one of the twelve. */
    api.init(&c);
    type(&c, "1");
    api.type_point(&c);
    type(&c, "12345678901");
    CHECK_STR(c.entry, "1.12345678901", "a point costs no digit");
    CHECK_INT((int)strlen(c.entry), 13, "thirteen characters, twelve of them digits");
}

static void test_clear(void)
{
    nd_calc c;
    char out[ND_CALC_FMT_CAP];

    /* Three cases, in the Python's order: delete a character, clear the
     * pending state, then leave. */
    api.init(&c);
    type(&c, "12");
    CHECK(!api.clear(&c), "Clear on a non-empty entry does not leave");
    CHECK_STR(c.entry, "1", "one character gone");
    CHECK(!api.clear(&c), "still not leaving");
    CHECK_STR(c.entry, "", "entry now empty");

    /* apply_option(1) on an EMPTY entry folds nothing -- but it still sets
     * the operator, because the Python's `if self.entry != "Error"` is the
     * only guard on that line. So Clear now has state to eat first. */
    api.apply_option(&c, 1);
    CHECK(!c.has_acc && c.pending_op == '+', "Add on an empty entry sets only the operator");
    CHECK(!api.clear(&c), "Clear eats the lone operator rather than leaving");
    CHECK(api.clear(&c), "Clear with no entry and no state leaves");

    /* And with an accumulator as well as an operator, both go at once. */
    api.init(&c);
    type(&c, "4");
    api.apply_option(&c, 1);
    CHECK(c.has_acc && c.pending_op == '+', "4 then Add leaves an accumulator");
    CHECK(!api.clear(&c), "Clear eats the pending state rather than leaving");
    CHECK(!c.has_acc && c.pending_op == '\0', "state gone");
    CHECK(api.clear(&c), "and the next Clear leaves");

    /* "Error" goes in ONE press, not five. */
    api.init(&c);
    (void)nd_strlcpy(c.entry, "Error", sizeof c.entry);
    CHECK(!api.clear(&c), "Clear on Error does not leave");
    CHECK_STR(c.entry, "", "Error clears whole");

    /* display_text(): entry, else the accumulator, else "0". */
    api.init(&c);
    CHECK_STR(api.display_text(&c, out, sizeof out), "0", "empty shows 0");
    type(&c, "9");
    CHECK_STR(api.display_text(&c, out, sizeof out), "9", "the entry wins");
    api.apply_option(&c, 1);
    CHECK_STR(api.display_text(&c, out, sizeof out), "9", "then the accumulator shows");
}

/* ------------------------------------------------------------------ *
 * 5. The two golden frames
 * ------------------------------------------------------------------ */

/* nd-shoot's CALC_FRAMES / CALC_OPT_FRAMES, and for the same reason: the
 * Clear keys that let the app out each redraw, so the budget is what stops
 * the recording where uistub's ScriptExhausted stopped the Python's. */
#define CALC_FRAMES     4
#define CALC_OPT_FRAMES 3

static void run_frame_case(const int32_t *keys, size_t n_keys, int64_t budget, const char *slug)
{
    sa_fixture fx;
    int rc;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    if (!sa_send_all(&fx, keys, n_keys)) {
        CHECK(false, "key script");
        sa_fx_free(&fx);
        return;
    }

    nd_vclock_enable();
    nd_capture_set_budget(fx.cap, budget);
    rc = api.run(&fx.ui);

    CHECK_INT(rc, 0, "app_run returns 0");
    CHECK_INT(nd_capture_frames_drawn(fx.cap), budget, "the expected number of frames committed");
    /* Checked BEFORE the budget is cleared: nd_capture_clear_budget() resets
     * the flag along with the counter. */
    CHECK(nd_capture_exhausted(fx.cap), "the Clears after the reference frame were refused");
    nd_capture_clear_budget(fx.cap);
    /* The clock advances once per COMMITTED frame; a refused one must not
     * have ticked it. */
    CHECK_INT(nd_vclock_frame(), (uint64_t)budget, "clock ticked once per committed frame");

    sa_expect_golden(&fx, nd_capture_recent(fx.cap, 0u), slug);

    nd_vclock_disable();
    sa_fx_free(&fx);
}

static void test_golden_calculator(void)
{
    /* shoot_docs.py: [DIGIT[1], DIGIT[2], DIGIT[3]] plus the Clears that let
     * a C app out of a loop the Python left by raising. */
    static const int32_t KEYS[] = {ND_KEY_1,     ND_KEY_2,     ND_KEY_3,    ND_KEY_CLEAR,
                                   ND_KEY_CLEAR, ND_KEY_CLEAR, ND_KEY_CLEAR};

    run_frame_case(KEYS, ND_ARRAY_LEN(KEYS), CALC_FRAMES, "app-calculator");
}

static void test_golden_calculator_options(void)
{
    /* shoot_docs.py: [DIGIT[7], ENTER]. VerticalList does not flush the
     * channel before drawing, so the three Clears can be queued up front. */
    static const int32_t KEYS[] = {ND_KEY_7, ND_KEY_ENTER, ND_KEY_CLEAR, ND_KEY_CLEAR,
                                   ND_KEY_CLEAR};

    run_frame_case(KEYS, ND_ARRAY_LEN(KEYS), CALC_OPT_FRAMES, "app-calculator-options");
}

/* ------------------------------------------------------------------ *
 * 6. Robustness
 * ------------------------------------------------------------------ */

static void test_null_safety(void)
{
    char out[8];

    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown(); /* must be safe with nothing held */

    api.init(NULL);
    api.fold(NULL);
    api.apply_option(NULL, 0);
    api.type_digit(NULL, '1');
    api.type_point(NULL);
    CHECK(api.clear(NULL), "clear(NULL) says leave rather than faulting");
    CHECK_STR(api.display_text(NULL, out, sizeof out), "0", "display_text(NULL) is \"0\"");

    /* A buffer too small to hold the answer truncates rather than writing
     * off the end. "1000000000000" in four bytes is "100". */
    {
        char tiny[4];

        CHECK_STR(api.format_number(1e12, tiny, sizeof tiny), "100", "format_number truncates");
    }
    sa_checks++; /* reaching here without a fault is the claim */
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    void *h = sa_begin("Calculator", "ndcalc");

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }

    RUN(test_format_number);
    RUN(test_tables);
    RUN(test_state_machine);
    RUN(test_digit_cap);
    RUN(test_clear);
    RUN(test_golden_calculator);
    RUN(test_golden_calculator_options);
    RUN(test_null_safety);

    return sa_end(h, "test_calculator");
}

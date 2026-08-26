/* calculator.h -- the parts of the Calculator app a unit test can reach.
 *
 * System/apps/Calculator/main.py is a module-level format_number() and a
 * Calculator class holding five methods that are pure state machines --
 * _fold, apply_option, type_digit, type_point -- plus draw() and loop(),
 * which are not.
 *
 * The state machine is the whole of this app that is not "call a widget", so
 * it is declared here rather than left static inside main.c, and
 * test/unit/test_calculator.c dlopen()s the BUILT app.so and dlsym()s it --
 * the same arrangement test_cubebench.c and test_phonebook.c use, so the test
 * exercises the artefact that ships and not a second copy compiled with
 * different flags.
 *
 * ============ WHY THE ENTRY IS A FIXED BUFFER ============
 *
 * The Python's `self.entry` is an unbounded str. It cannot actually grow
 * without limit -- type_digit() refuses once twelve non-sign, non-point
 * characters are in it, and every other assignment comes from
 * format_number(), which is capped at MAX_DIGITS + 2 = 14 characters. The
 * longest reachable value is therefore about sixteen bytes. ND_CALC_ENTRY_CAP
 * is 64, which is four times that, and every append checks it anyway:
 * CODING-STANDARDS.md section 1.4 does not have an exception for "the caller
 * already proved it fits".
 */

#ifndef ND_CALCULATOR_H_INCLUDED
#define ND_CALCULATOR_H_INCLUDED

#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* APP_ID = 7 -- manifest.json, and the Options VerticalList's app_id, which
 * is what puts "7-1" in the corner of golden/app-calculator-options.png. */
#define ND_CALC_APP_ID 7

/* MAX_DIGITS = 12. Two separate rules read it: the type_digit() cap, and
 * format_number()'s 10**MAX_DIGITS integer ceiling and MAX_DIGITS + 2 length
 * ceiling. It is one number in the Python and it is one number here. */
#define ND_CALC_MAX_DIGITS 12

/* See the header comment: sixteen bytes is the reachable maximum. */
#define ND_CALC_ENTRY_CAP 64

/* "%.8f" of DBL_MAX is 309 integer digits, a point and eight decimals.
 * format_number() formats into this before deciding whether the result is
 * short enough to keep, so the intermediate has to fit even when the answer
 * will not. */
#define ND_CALC_FMT_CAP 384

/* OPTIONS, in order. The list golden/app-calculator-options.png shows the
 * first three rows of. */
#define ND_CALC_OPTION_COUNT 5
extern const char *const nd_calc_options[ND_CALC_OPTION_COUNT];

/* OP_FOR_OPTION = {1: "+", 2: "-", 3: "*", 4: "/"}. Option 0 is Equals and
 * has no operator; '\0' is this table's spelling of "not in the dict". */
char nd_calc_op_for_option(int32_t choice);

/* DIGIT_KEYS = {2: "1", ... 11: "0"}. '\0' when the key is not a digit key.
 * Note 11 is '0' and not '9': the keypad runs 1..9 then 0, as a phone does. */
char nd_calc_digit_for_key(int32_t key);

/* ------------------------------------------------------------------ *
 * format_number()
 * ------------------------------------------------------------------ */

/* Writes at most out_sz bytes including the terminator, and returns out.
 *
 * "Error" for NaN and both infinities. Otherwise the Python's two branches:
 * an exact integer under 10**12 prints as an integer, anything else prints
 * "%.8f" with trailing zeros and then a trailing point stripped, and a result
 * longer than MAX_DIGITS + 2 is replaced by "%.6e".
 *
 * out_sz below ND_CALC_FMT_CAP truncates rather than failing, because there
 * is no failure path in the Python and the only caller passes a big enough
 * buffer. */
const char *nd_calc_format_number(double value, char *out, size_t out_sz);

/* ------------------------------------------------------------------ *
 * The state machine
 * ------------------------------------------------------------------ */

/* Calculator.__init__ minus the widgets. `has_acc` is the C spelling of
 * `self.acc is not None`, and `pending_op` is '\0' where the Python has
 * None -- both are states the display and the Clear key can distinguish, so
 * neither can be folded into a sentinel value of the number itself. */
typedef struct {
    char entry[ND_CALC_ENTRY_CAP]; /* what the user is typing */
    double acc;                    /* folded left-hand value  */
    bool has_acc;
    char pending_op;
} nd_calc;

void nd_calc_init(nd_calc *c);

/* _fold(): fold the current entry into the accumulator via the pending op.
 *
 * Division by zero sets entry to "Error" and clears both accumulator and
 * operator, which is what `except ZeroDivisionError` does -- C's floating
 * point would quietly hand back an infinity instead, so the test for a zero
 * divisor is explicit and is the ONLY place this port adds a branch the
 * Python does not have written down. */
void nd_calc_fold(nd_calc *c);

/* apply_option(choice): 0 Equals, 1..4 the operators, anything else ignored. */
void nd_calc_apply_option(nd_calc *c, int32_t choice);

/* type_digit(ch) and type_point(). Both are no-ops once the twelve-character
 * limit is reached; type_point() is additionally a no-op when the entry
 * already holds a point. */
void nd_calc_type_digit(nd_calc *c, char ch);
void nd_calc_type_point(nd_calc *c);

/* The Clear key's whole behaviour, so the loop reads as the Python's if/elif
 * chain does and the three cases can be tested without a key channel.
 *
 * Returns true when the app should return -- i.e. Clear on an empty entry
 * with no pending state. The caller redraws in the other two cases and only
 * in those two. */
bool nd_calc_clear(nd_calc *c);

/* draw()'s first line: the entry if there is one, else the accumulator, else
 * "0". Returns `out`. */
const char *nd_calc_display_text(const nd_calc *c, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* ND_CALCULATOR_H_INCLUDED */

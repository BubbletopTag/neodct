/* apps/Calculator/main.c -- the Calculator, app id 7.
 *
 * A one-to-one port of System/apps/Calculator/main.py (159 lines), whose own
 * header describes it exactly:
 *
 *     Nokia 5190-style calculator: type a number, open Options with the
 *     softkey, pick Equals/Add/Subtract/Multiply/Divide, keep typing. * or #
 *     inserts the decimal point, Clear deletes (and exits once the entry is
 *     empty).
 *
 * Two golden frames come out of this file: golden/app-calculator.png (the
 * digits 1, 2, 3 typed, "Options" on the softkey) and
 * golden/app-calculator-options.png (7, then Enter, which is the Options list
 * with "7-1" in the corner).
 *
 * ============ FOUR THINGS THAT LOOK WRONG AND ARE PORTED ANYWAY ============
 *
 * 1. THERE IS NO MINUS SIGN AND THE CODE IS FULL OF THEM. type_digit() has a
 *    special case for an entry of "-0", type_point() has one for "-", and
 *    the twelve-character limit is measured after lstrip("-"). Nothing in
 *    this app can ever put a '-' at the front of the entry: there is no sign
 *    key and no unary minus in the Options menu. The three branches are
 *    unreachable and they are reproduced, because a later port that adds a
 *    sign key will want them and because deleting reachable-looking code is
 *    how a port loses a rule nobody wrote down.
 *
 * 2. type_digit()'s `ch != "."` can never be false. Only type_point() writes
 *    a point, and it does not go through type_digit(). Same reasoning.
 *
 * 3. THE PENDING-OPERATION HINT IS DRAWN AT y=16 IN THE 20 px FONT WHILE THE
 *    NUMBER IS AT y=12 IN THE 24 px ONE. They do not share a baseline and
 *    they were not meant to -- text y is the ASCENDER LINE, so the 4 px
 *    offset is what lines the two ascenders up to within a pixel. Neither
 *    number is derived from the other; both are literals in the Python.
 *
 * 4. `Equals` WITH NOTHING PENDING IS NOT A NO-OP. It folds the entry into
 *    the accumulator, formats it back out and clears the accumulator, so
 *    typing "007" and choosing Equals rewrites the display as "7". That is
 *    the Python's behaviour and it is visible on screen.
 *
 * ============ THE ONE BRANCH THIS PORT HAD TO ADD ============
 *
 * `except ZeroDivisionError`. Python raises on 1/0; C returns an infinity and
 * would print "inf" where the phone prints "Error". nd_calc_fold() tests the
 * divisor explicitly. It is the same behaviour reached a different way, and
 * it is the only place in this file where the C says something the Python
 * does not.
 *
 * ============ THE FLOATING POINT IS BIT-FOR-BIT THE PYTHON'S ============
 *
 * CPython's float is a C double and its arithmetic is the platform's, so +,
 * -, * and / agree by construction. The formatting agrees too: both CPython
 * and glibc/musl printf are correctly rounded, so "%.8f" and "%.6e" of the
 * same double produce the same digits. The one conversion that is NOT shared
 * is Python's arbitrary-precision int(): `value == int(value)` is exact for
 * any finite double, and (long long) is not, so nd_calc_format_number() uses
 * trunc() for the test and only casts once fabs(value) < 1e12 has proved the
 * cast safe.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_keycodes.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#include "calculator.h"

/* ------------------------------------------------------------------ *
 * The tables
 * ------------------------------------------------------------------ */

const char *const nd_calc_options[ND_CALC_OPTION_COUNT] = {"Equals", "Add", "Subtract", "Multiply",
                                                           "Divide"};

char nd_calc_op_for_option(int32_t choice)
{
    switch (choice) {
    case 1:
        return '+';
    case 2:
        return '-';
    case 3:
        return '*';
    case 4:
        return '/';
    default:
        return '\0';
    }
}

/* DIGIT_KEYS. Keys 2..10 are '1'..'9' and key 11 is '0' -- the phone's
 * layout, not the ASCII one, so this is a subtraction plus one special
 * case rather than a table lookup dressed up as arithmetic. */
char nd_calc_digit_for_key(int32_t key)
{
    if (key >= ND_KEY_1 && key <= ND_KEY_9)
        return (char)('1' + (key - ND_KEY_1));
    if (key == ND_KEY_0)
        return '0';
    return '\0';
}

/* ------------------------------------------------------------------ *
 * format_number()
 * ------------------------------------------------------------------ */

/* str.rstrip(chars): remove every trailing byte that is in `chars`. Written
 * out because Python strips a SET and repeatedly, which is not what a single
 * "drop the last character if it is a dot" would do -- "1.000" reaches
 * .rstrip(".") as "1." and comes back "1", and a hypothetical "1.." would
 * come back "1" too. */
static void rstrip_set(char *s, const char *chars)
{
    size_t len = strlen(s);

    while (len > 0u && strchr(chars, s[len - 1u]) != NULL)
        len--;
    s[len] = '\0';
}

const char *nd_calc_format_number(double value, char *out, size_t out_sz)
{
    char buf[ND_CALC_FMT_CAP];
    int n;

    if (out == NULL || out_sz == 0u)
        return out;

    /* `value != value` is Python's NaN test spelled the way the Python spells
     * it; `value in (inf, -inf)` is isinf(). */
    if (value != value || isinf(value)) {
        (void)snprintf(out, out_sz, "%s", "Error");
        return out;
    }

    if (value == trunc(value) && fabs(value) < 1e12) {
        /* str(int(value)). The guard above is 10 ** MAX_DIGITS, which is
         * exactly representable, so the cast cannot overflow. int()
         * truncates toward zero and so does the cast; for a value that is
         * already integral the two agree exactly, -0.0 included ("0"). */
        n = snprintf(buf, sizeof buf, "%lld", (long long)value);
    } else {
        n = snprintf(buf, sizeof buf, "%.8f", value);
        if (n > 0 && (size_t)n < sizeof buf) {
            rstrip_set(buf, "0");
            rstrip_set(buf, ".");
        }
    }
    if (n < 0 || (size_t)n >= sizeof buf) {
        /* Unreachable: ND_CALC_FMT_CAP is sized for "%.8f" of DBL_MAX. If it
         * ever is reached, "Error" is the app's own word for "this number
         * cannot be shown" and is better than a truncated one. */
        (void)snprintf(out, out_sz, "%s", "Error");
        return out;
    }

    if (strlen(buf) > (size_t)(ND_CALC_MAX_DIGITS + 2)) {
        n = snprintf(buf, sizeof buf, "%.6e", value);
        if (n < 0 || (size_t)n >= sizeof buf) {
            (void)snprintf(out, out_sz, "%s", "Error");
            return out;
        }
    }

    (void)snprintf(out, out_sz, "%s", buf);
    return out;
}

/* ------------------------------------------------------------------ *
 * The state machine
 * ------------------------------------------------------------------ */

void nd_calc_init(nd_calc *c)
{
    if (c == NULL)
        return;
    memset(c, 0, sizeof *c);
    c->entry[0] = '\0';
    c->acc = 0.0;
    c->has_acc = false;
    c->pending_op = '\0';
}

static bool entry_is(const nd_calc *c, const char *s)
{
    return strcmp(c->entry, s) == 0;
}

/* self.entry += ch. The Python cannot fail here; this can, and the failure is
 * silent for the same reason type_digit()'s length cap is silent -- the key
 * simply does nothing. See calculator.h for why it is unreachable. */
static void entry_append(nd_calc *c, const char *s)
{
    size_t len = strlen(c->entry);
    size_t add = strlen(s);

    if (len + add + 1u > sizeof c->entry)
        return;
    memcpy(c->entry + len, s, add + 1u);
}

/* float(self.entry), with `except ValueError: value = 0.0`.
 *
 * strtod is float()'s equivalent for everything this entry can hold -- digits,
 * one point, and the "1.000000e+20" that format_number() can put back into it.
 * A partial parse is a ValueError to Python and is treated as one here. */
static double entry_value(const nd_calc *c)
{
    char *end = NULL;
    double v;

    if (c->entry[0] == '\0')
        return 0.0;
    v = strtod(c->entry, &end);
    if (end == c->entry || end == NULL || *end != '\0')
        return 0.0;
    return v;
}

void nd_calc_fold(nd_calc *c)
{
    double value;

    if (c == NULL)
        return;
    if (c->entry[0] == '\0' || entry_is(c, "Error"))
        return;

    value = entry_value(c);

    if (!c->has_acc || c->pending_op == '\0') {
        c->acc = value;
        c->has_acc = true;
    } else {
        switch (c->pending_op) {
        case '+':
            c->acc = c->acc + value;
            break;
        case '-':
            c->acc = c->acc - value;
            break;
        case '*':
            c->acc = c->acc * value;
            break;
        case '/':
            /* ZeroDivisionError. See the header comment: this is the one
             * branch the port adds, because C division does not raise. */
            if (value == 0.0) {
                c->acc = 0.0;
                c->has_acc = false;
                c->pending_op = '\0';
                (void)snprintf(c->entry, sizeof c->entry, "%s", "Error");
                return;
            }
            c->acc = c->acc / value;
            break;
        default:
            break;
        }
    }
    c->entry[0] = '\0';
}

void nd_calc_apply_option(nd_calc *c, int32_t choice)
{
    char op;

    if (c == NULL)
        return;

    if (choice == 0) { /* Equals */
        nd_calc_fold(c);
        if (!entry_is(c, "Error") && c->has_acc)
            (void)nd_calc_format_number(c->acc, c->entry, sizeof c->entry);
        c->acc = 0.0;
        c->has_acc = false;
        c->pending_op = '\0';
        return;
    }

    op = nd_calc_op_for_option(choice);
    if (op == '\0') /* `elif choice in OP_FOR_OPTION` -- nothing else matches */
        return;
    nd_calc_fold(c);
    if (!entry_is(c, "Error"))
        c->pending_op = op;
}

/* len(self.entry.lstrip("-").replace(".", "")): every byte that is neither a
 * LEADING '-' nor a point, anywhere. lstrip strips a run, so a hypothetical
 * "--1" counts as one character. */
static size_t significant_len(const char *entry)
{
    size_t n = 0u;

    while (*entry == '-')
        entry++;
    for (; *entry != '\0'; entry++) {
        if (*entry != '.')
            n++;
    }
    return n;
}

void nd_calc_type_digit(nd_calc *c, char ch)
{
    char text[2];

    if (c == NULL)
        return;
    if (entry_is(c, "Error"))
        c->entry[0] = '\0';

    /* self.entry[:-1] on "0" or "-0": a leading zero is replaced rather than
     * typed after, so 0 then 5 is "5" and not "05". Every character this app
     * can hold is one byte, so dropping the last byte drops the last
     * character. */
    if ((entry_is(c, "0") || entry_is(c, "-0")) && ch != '.')
        c->entry[strlen(c->entry) - 1u] = '\0';

    if (significant_len(c->entry) >= (size_t)ND_CALC_MAX_DIGITS)
        return;

    text[0] = ch;
    text[1] = '\0';
    entry_append(c, text);
}

void nd_calc_type_point(nd_calc *c)
{
    if (c == NULL)
        return;
    if (entry_is(c, "Error"))
        c->entry[0] = '\0';
    if (strchr(c->entry, '.') != NULL)
        return;
    /* NOTE: no MAX_DIGITS test here. The Python does not have one either, so
     * a full twelve digits followed by * gains a thirteenth character. */
    entry_append(c, (c->entry[0] == '\0' || entry_is(c, "-")) ? "0." : ".");
}

bool nd_calc_clear(nd_calc *c)
{
    if (c == NULL)
        return true;

    if (c->entry[0] != '\0') {
        if (entry_is(c, "Error"))
            c->entry[0] = '\0';
        else
            c->entry[strlen(c->entry) - 1u] = '\0';
        return false;
    }
    if (c->has_acc || c->pending_op != '\0') {
        c->acc = 0.0;
        c->has_acc = false;
        c->pending_op = '\0';
        return false;
    }
    return true;
}

const char *nd_calc_display_text(const nd_calc *c, char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0u)
        return out;
    if (c == NULL) {
        (void)snprintf(out, out_sz, "%s", "0");
        return out;
    }
    if (c->entry[0] != '\0') {
        (void)snprintf(out, out_sz, "%s", c->entry);
        return out;
    }
    if (c->has_acc)
        return nd_calc_format_number(c->acc, out, out_sz);
    (void)snprintf(out, out_sz, "%s", "0");
    return out;
}

/* ------------------------------------------------------------------ *
 * The screen
 * ------------------------------------------------------------------ */

/* class Calculator's instance state, minus the arithmetic, which is nd_calc.
 * screen_w and content_bottom are READ ONCE IN __init__ and never re-read,
 * so they are cached here too rather than being asked of the context per
 * draw -- the values cannot change under an app, but the correspondence
 * should be visible. */
typedef struct {
    nd_ui *ui;
    nd_softkey softkey;
    int32_t screen_w;
    int32_t content_bottom;
    nd_calc st;
} calc_app;

static void calc_draw(calc_app *a)
{
    nd_ui *ui = a->ui;
    char text[ND_CALC_FMT_CAP];
    int32_t w = 0;
    int32_t x;

    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, a->screen_w, a->content_bottom), ND_BLACK);

    (void)nd_calc_display_text(&a->st, text, sizeof text);
    nd_ui_text_size(ui, text, ui->font_xl, &w, NULL);

    /* max(5, screen_w - 10 - w): right-aligned with a 10 px margin, and
     * pinned to a 5 px left margin once the number is wider than the screen.
     * The two margins are different numbers in the Python and stay different
     * here. */
    x = a->screen_w - 10 - w;
    if (x < 5)
        x = 5;
    (void)nd_draw_text(ui->draw, x, 12, text, ui->font_xl, ND_WHITE);

    /* Small pending-operation hint at the left edge. */
    if (a->st.pending_op != '\0') {
        char op[2];

        op[0] = a->st.pending_op;
        op[1] = '\0';
        (void)nd_draw_text(ui->draw, 8, 16, op, ui->font_n, ND_WHITE);
    }

    nd_softkey_update(&a->softkey, "Options", true);
}

static void calc_open_options(calc_app *a)
{
    nd_vlist menu;
    nd_softkey bar;
    int32_t choice;

    nd_vlist_init(&menu, a->ui, "Options", nd_calc_options, ND_CALC_OPTION_COUNT, ND_CALC_APP_ID);

    /* A SECOND, THROWAWAY BAR. The Python writes SoftKeyBar(self.ui) inline
     * rather than reusing self.softkey, and it matters: this one paints "OK"
     * WITHOUT presenting, and VerticalList.draw() clears only rows 0..145, so
     * the "OK" survives underneath and is presented with the list. Reusing
     * a->softkey would also work, but it would leave a->softkey believing its
     * text is "OK" when the next calc_draw() is about to write "Options"
     * over it -- a difference with no pixels attached today and one waiting
     * for the first person who reads current_text. */
    nd_softkey_init(&bar, a->ui, false);
    nd_softkey_update(&bar, "OK", false);

    choice = nd_vlist_show(&menu);
    if (choice >= 0)
        nd_calc_apply_option(&a->st, choice);
}

int app_run(nd_ui *ui)
{
    calc_app a;

    if (ui == NULL)
        return 1;

    memset(&a, 0, sizeof a);
    a.ui = ui;
    a.screen_w = nd_ui_width(ui);
    a.content_bottom = nd_ui_content_bottom(ui);
    nd_softkey_init(&a.softkey, ui, false);
    nd_calc_init(&a.st);

    calc_draw(&a);

    for (;;) {
        int32_t key = nd_ui_wait_for_key(ui);
        char digit;

        if (key == ND_KEY_ENTER) {
            calc_open_options(&a);
            calc_draw(&a);
        } else if (key == ND_KEY_BACK) {
            if (nd_calc_clear(&a.st))
                return 0;
            calc_draw(&a);
        } else if ((digit = nd_calc_digit_for_key(key)) != '\0') {
            nd_calc_type_digit(&a.st, digit);
            calc_draw(&a);
        } else if (key == ND_KEY_STAR || key == ND_KEY_HASH || key == ND_KEY_DOT) {
            /* * / # / '.' on dev keyboard */
            nd_calc_type_point(&a.st);
            calc_draw(&a);
        }

        /* Not in the Python, which had exceptions to unwind it. nd_app.h:
         * a loop that outlives a frame polls this. */
        if (nd_app_should_exit())
            return 0;
    }
}

/* Nothing is held: no file, no child process, no sound card. The symbol
 * exists because nd_app.h requires every app to export one. */
void app_shutdown(void) {}

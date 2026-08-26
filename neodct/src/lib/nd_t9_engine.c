/* nd_t9_engine.c -- multi-tap and predictive text entry, ported 1:1 from
 * System/hw/t9_engine.py.
 *
 * Pure logic. No I/O, no allocation, and no clock of its own: the multi-tap
 * window needs a monotonic clock, and a clock a test cannot move is a test
 * that has to sleep. Every widget embeds one of these by value.
 *
 * The decision order in nd_t9_engine_press() is the load-bearing part and is
 * kept in the Python's exact sequence, including the fall-through in step 4
 * that nothing about the code makes obvious -- see the comment there.
 */

#include "nd_t9.h"

#include <ctype.h>
#include <string.h>
#include <time.h>

#include "nd_keycodes.h"

/* Key 1's punctuation cycle. '1' is in it so digits stay reachable while in
 * abc mode; everything past ':' exists to make the LinuxShell usable. */
static const char PUNCT_CYCLE[] = ".,?!'\"1-()@/:_;+#*=<>";
/* The same string with digits removed -- what a letters-only field offers. */
static const char PUNCT_CYCLE_LETTERS[] = ".,?!'\"-()@/:_;+#*=<>";

/* Nokia's letter groups, indexed by digit 2..9. */
static const char *const LETTER_CYCLES[8] = {"abc", "def",  "ghi", "jkl",
                                             "mno", "pqrs", "tuv", "wxyz"};

/* What the dev-keyboard path may type in a numbers field; mirrors what the
 * multi-tap cycles can produce there. */
static const char NUMBERS_CHARS[] = "0123456789*#+";

/* The longest cycle is PUNCT_CYCLE at 21, plus room for the appended digit
 * that FILTER_ANY adds to a letter cycle. */
#define CYCLE_MAX 24

static double clock_monotonic(void *ctx)
{
    struct timespec ts;

    ND_UNUSED(ctx);
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* The letter group for a digit character, or NULL for '0' and '1', which
 * carry no letters and therefore cannot be part of a predictive word. */
static const char *letters_for(char digit)
{
    if (digit < '2' || digit > '9')
        return NULL;
    return LETTER_CYCLES[(size_t)(digit - '2')];
}

/* _cycle_for(). Writes the cycle into out and returns its length. */
static size_t cycle_for(const nd_t9_engine *e, char digit, char *out, size_t out_sz)
{
    const char *letters;
    size_t n = 0u;
    size_t i;

    if (digit == '0') {
        out[n++] = ' ';
        if (e->filter == ND_T9_FILTER_ANY)
            out[n++] = '0';
        return n;
    }
    if (digit == '1') {
        const char *src = (e->filter == ND_T9_FILTER_ANY) ? PUNCT_CYCLE : PUNCT_CYCLE_LETTERS;
        size_t len = strlen(src);

        if (len > out_sz)
            len = out_sz;
        memcpy(out, src, len);
        return len;
    }

    letters = letters_for(digit);
    if (letters == NULL)
        return 0u;
    for (i = 0u; letters[i] != '\0' && n < out_sz; i++) {
        char c = letters[i];

        if (e->mode == ND_T9_MODE_UPPER)
            c = (char)toupper((unsigned char)c);
        out[n++] = c;
    }
    /* Only an "any" field lets you reach the digit itself by cycling past
     * the letters. A letters-only field wraps straight back to 'a'. */
    if (e->filter == ND_T9_FILTER_ANY && n < out_sz)
        out[n++] = digit;
    return n;
}

static nd_t9_op op_none(void)
{
    nd_t9_op op;

    op.kind = ND_T9_OP_NONE;
    op.ch = '\0';
    op.mode = ND_T9_MODE_ABC;
    op.digits = NULL;
    return op;
}

nd_err nd_t9_engine_init(nd_t9_engine *e, nd_t9_filter f, double timeout_s, nd_clock_fn clk,
                         void *clk_ctx)
{
    size_t i;

    if (e == NULL)
        return ND_ERR_INVAL;

    memset(e, 0, sizeof *e);

    switch (f) {
    case ND_T9_FILTER_ANY:
        e->modes[0] = ND_T9_MODE_WORD;
        e->modes[1] = ND_T9_MODE_ABC;
        e->modes[2] = ND_T9_MODE_UPPER;
        e->modes[3] = ND_T9_MODE_123;
        e->n_modes = 4u;
        break;
    case ND_T9_FILTER_LETTERS:
        e->modes[0] = ND_T9_MODE_WORD;
        e->modes[1] = ND_T9_MODE_ABC;
        e->modes[2] = ND_T9_MODE_UPPER;
        e->n_modes = 3u;
        break;
    case ND_T9_FILTER_NUMBERS:
        e->modes[0] = ND_T9_MODE_123;
        e->n_modes = 1u;
        break;
    default:
        /* T9Engine.__init__ raises ValueError for an unknown filter. */
        return ND_ERR_INVAL;
    }

    e->filter = f;
    /* Predictive sits BEFORE abc in the # cycle but is not where typing
     * starts: multi-tap is what every existing field expects and what someone
     * who has never opened this phone before will understand. Predictive is a
     * mode you choose, one # press away. */
    e->mode_index = 0u;
    for (i = 0u; i < e->n_modes; i++) {
        if (e->modes[i] == ND_T9_MODE_ABC) {
            e->mode_index = i;
            break;
        }
    }
    e->mode = e->modes[e->mode_index];
    e->timeout_s = (timeout_s > 0.0) ? timeout_s : 1.0;
    e->clock = (clk != NULL) ? clk : clock_monotonic;
    e->clock_ctx = (clk != NULL) ? clk_ctx : NULL;
    e->last_code = ND_KEY_NONE;
    e->cycle_index = 0u;
    e->last_press_at = 0.0;
    e->word_digits[0] = '\0';
    return ND_OK;
}

void nd_t9_engine_reset(nd_t9_engine *e)
{
    if (e == NULL)
        return;
    /* Python's reset() clears _pending_digit and _word_digits and nothing
     * else. _pending_idx and _last_press are deliberately left alone: with no
     * pending digit they can never be consulted, and matching the Python here
     * costs nothing while diverging would need explaining forever. */
    e->last_code = ND_KEY_NONE;
    e->word_digits[0] = '\0';
}

nd_t9_op nd_t9_engine_press(nd_t9_engine *e, int32_t code)
{
    nd_t9_op op = op_none();
    char cycle[CYCLE_MAX];
    size_t n_cycle;
    char digit;
    double now;

    if (e == NULL)
        return op;

    if (code == ND_KEY_HASH) {
        nd_t9_engine_reset(e);
        if (e->n_modes > 1u) {
            e->mode_index = (e->mode_index + 1u) % e->n_modes;
            e->mode = e->modes[e->mode_index];
            op.kind = ND_T9_OP_MODE;
            op.mode = e->mode;
            return op;
        }
        /* A numbers-only field has one mode, so # has nothing to cycle and
         * becomes the literal character it is printed as. */
        op.kind = ND_T9_OP_APPEND;
        op.ch = '#';
        return op;
    }

    if (code == ND_KEY_STAR) {
        if (e->mode == ND_T9_MODE_WORD) {
            /* Next candidate for what has been typed. Deliberately does NOT
             * reset: the digits typed so far ARE the word, and dropping them
             * is what the clear key is for. */
            if (e->word_digits[0] != '\0') {
                op.kind = ND_T9_OP_NEXT;
                op.digits = e->word_digits;
            }
            return op;
        }
        nd_t9_engine_reset(e);
        if (e->mode == ND_T9_MODE_123) {
            op.kind = ND_T9_OP_APPEND;
            op.ch = '*';
        }
        return op;
    }

    digit = nd_key_digit_char(code);
    if (digit == '\0') {
        /* Nav keys land here, and committing the pending cycle is exactly
         * what they should do: moving the cursor ends the letter. */
        nd_t9_engine_reset(e);
        return op;
    }

    if (e->mode == ND_T9_MODE_WORD) {
        if (letters_for(digit) == NULL) {
            /* 0 and 1 carry no letters, so neither can be part of a word key:
             * this is the end of the word. Clear the sequence and FALL
             * THROUGH into the ordinary multi-tap path, which produces the
             * space or the punctuation and, by returning a non-WORD op, tells
             * the caller to commit whatever it was showing. */
            e->word_digits[0] = '\0';
        } else {
            size_t len = strlen(e->word_digits);

            /* ND_T9_DIGITS_MAX: the Python grows this without limit, but the
             * longest dictionary key is 12 and suggest() answers nothing past
             * it, so beyond the cap we stop appending rather than keep a
             * sequence that can never match. Deviation C-3, approved. */
            if (len < ND_T9_DIGITS_MAX) {
                e->word_digits[len] = digit;
                e->word_digits[len + 1u] = '\0';
            }
            op.kind = ND_T9_OP_WORD;
            op.digits = e->word_digits;
            return op;
        }
    }

    if (e->mode == ND_T9_MODE_123) {
        nd_t9_engine_reset(e);
        op.kind = ND_T9_OP_APPEND;
        op.ch = digit;
        return op;
    }

    n_cycle = cycle_for(e, digit, cycle, sizeof cycle);
    if (n_cycle == 0u)
        return op;

    now = e->clock(e->clock_ctx);
    /* Cycling needs no timer: a press of the same key inside the window
     * advances the cycle, and anything else implicitly commits the pending
     * letter and starts fresh. Comparing codes rather than digit characters
     * is the same test -- the map between them is one-to-one -- and lets
     * ND_KEY_NONE stand in for Python's None. */
    if (e->last_code == code && (now - e->last_press_at) <= e->timeout_s) {
        e->cycle_index = (e->cycle_index + 1u) % n_cycle;
        e->last_press_at = now;
        op.kind = ND_T9_OP_REPLACE;
        op.ch = cycle[e->cycle_index];
        return op;
    }

    e->last_code = code;
    e->cycle_index = 0u;
    e->last_press_at = now;
    op.kind = ND_T9_OP_APPEND;
    op.ch = cycle[0];
    return op;
}

nd_t9_mode nd_t9_engine_mode(const nd_t9_engine *e)
{
    return (e != NULL) ? e->mode : ND_T9_MODE_ABC;
}

const nd_t9_mode *nd_t9_engine_modes(const nd_t9_engine *e, size_t *count)
{
    if (e == NULL) {
        if (count != NULL)
            *count = 0u;
        return NULL;
    }
    if (count != NULL)
        *count = e->n_modes;
    return e->modes;
}

nd_t9_mode nd_t9_engine_set_mode_index(nd_t9_engine *e, size_t index)
{
    if (e == NULL || e->n_modes == 0u)
        return ND_T9_MODE_ABC;
    e->mode_index = index % e->n_modes;
    e->mode = e->modes[e->mode_index];
    nd_t9_engine_reset(e);
    return e->mode;
}

const char *nd_t9_engine_word_digits(const nd_t9_engine *e)
{
    return (e != NULL) ? e->word_digits : "";
}

const char *nd_t9_engine_pop_word_digit(nd_t9_engine *e)
{
    size_t len;

    if (e == NULL)
        return NULL;
    len = strlen(e->word_digits);
    if (len == 0u)
        return NULL; /* nothing to drop: the caller backspaces normally */
    e->word_digits[len - 1u] = '\0';
    return e->word_digits;
}

void nd_t9_engine_clear_word(nd_t9_engine *e)
{
    if (e != NULL)
        e->word_digits[0] = '\0';
}

bool nd_t9_char_allowed(char c, nd_t9_filter f)
{
    if (c == '\0')
        return false; /* strchr would find the terminator */
    if (f == ND_T9_FILTER_NUMBERS)
        return strchr(NUMBERS_CHARS, c) != NULL;
    if (f == ND_T9_FILTER_LETTERS)
        return isdigit((unsigned char)c) == 0;
    return true;
}

const char *nd_t9_mode_label(nd_t9_mode m)
{
    switch (m) {
    case ND_T9_MODE_WORD:
        return "word";
    case ND_T9_MODE_ABC:
        return "abc";
    case ND_T9_MODE_UPPER:
        return "ABC";
    case ND_T9_MODE_123:
        return "123";
    default:
        return "abc";
    }
}

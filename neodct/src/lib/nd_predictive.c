/* nd_predictive.c -- PredictiveText, ported from System/ui/framework.py:93.
 *
 * The provisional word lives in the field's own text like any other typed
 * characters, and pending_len records how much of the tail is still
 * provisional. Keeping it in the text rather than beside it is what lets
 * confirm, backspace, wrapping and every caller that reads the text keep
 * working without knowing predictive exists -- the only differences are that
 * those bytes are underlined, and that another key can replace them wholesale.
 *
 * ============ pending_len IS A BYTE COUNT ============
 *
 * The Python counts characters. Everything that can become a pending word is
 * ASCII -- the dictionary is ASCII and the fallback is the digit string -- so
 * the two agree everywhere this can be reached, and bytes are what
 * nd_underline_tail() and nd_textlong's cursor already speak.
 *
 * ============ WHY reset() CLEARS BOTH HALVES ============
 *
 * The Python's own comment: "Both halves or neither: leaving the digits
 * behind in the engine makes the next key continue a word the field has
 * already thrown away, and the sequence then matches nothing."
 */

#include <string.h>

#include "nd_predictive_priv.h"
#include "nd_t9.h"
#include "nd_types.h"
#include "nd_widgets.h"

void nd_predictive_reset(nd_predictive *p, nd_t9_engine *t9)
{
    if (p != NULL) {
        p->pending_len = 0u;
        p->candidate_idx = 0u;
        p->n_candidates = 0u;
    }
    if (t9 != NULL)
        nd_t9_engine_clear_word(t9);
}

size_t nd_pred_insert_at(const nd_pred_field *fld)
{
    if (fld == NULL || fld->text == NULL)
        return 0u;
    /* getattr(self, "cursor", len(self.text)): TextInputLong types at its
     * cursor, TextInput has none and always types at the end. */
    if (fld->cursor != NULL)
        return *fld->cursor;
    return strlen(fld->text);
}

bool nd_pred_show_word(nd_pred_field *fld, const char *word)
{
    size_t len;
    size_t end;
    size_t start;
    size_t wlen;
    size_t tail;

    if (fld == NULL || fld->p == NULL || fld->text == NULL || fld->cap == 0u)
        return false;
    if (word == NULL)
        word = "";

    len = strlen(fld->text);
    end = nd_pred_insert_at(fld);
    if (end > len)
        end = len;
    start = (end >= fld->p->pending_len) ? end - fld->p->pending_len : 0u;
    wlen = strlen(word);
    tail = len - end;

    /* C-2: at the cap the field ignores the key rather than truncating, so
     * this reports failure and changes nothing. */
    if (start + wlen + tail + 1u > fld->cap)
        return false;

    memmove(fld->text + start + wlen, fld->text + end, tail + 1u);
    memcpy(fld->text + start, word, wlen);
    fld->p->pending_len = wlen;
    if (fld->cursor != NULL)
        *fld->cursor = start + wlen;
    return true;
}

bool nd_pred_predict(nd_pred_field *fld, const char *digits)
{
    size_t n;

    if (fld == NULL || fld->p == NULL)
        return false;

    if (digits == NULL || digits[0] == '\0') {
        /* Shrinking the tail to nothing always fits. */
        (void)nd_pred_show_word(fld, "");
        fld->p->n_candidates = 0u;
        return true;
    }

    n = nd_t9_dict_suggest(nd_t9_dict_shared(), digits, fld->p->candidates, ND_T9_MAX_SUGGESTIONS);
    fld->p->n_candidates = n;
    fld->p->candidate_idx = 0u;

    /* No match, or no dictionary installed: show the digits, so the
     * keypresses are visible and * / Clear still behave sensibly, rather than
     * the field silently ignoring the key. */
    return nd_pred_show_word(fld, (n > 0u) ? fld->p->candidates[0] : digits);
}

int32_t nd_pred_next_candidate(nd_pred_field *fld)
{
    size_t was;

    if (fld == NULL || fld->p == NULL || fld->p->n_candidates < 2u)
        return 0;

    was = fld->p->candidate_idx;
    fld->p->candidate_idx = (fld->p->candidate_idx + 1u) % fld->p->n_candidates;
    if (!nd_pred_show_word(fld, fld->p->candidates[fld->p->candidate_idx])) {
        fld->p->candidate_idx = was;
        return -1;
    }
    return 1;
}

void nd_pred_commit(nd_pred_field *fld)
{
    if (fld != NULL)
        nd_predictive_reset(fld->p, fld->t9);
}

nd_pred_status nd_pred_key(nd_pred_field *fld, const nd_t9_op *op, nd_widget_result *out)
{
    if (fld == NULL || op == NULL)
        return ND_PRED_UNHANDLED;

    if (op->kind == ND_T9_OP_WORD) {
        if (!nd_pred_predict(fld, op->digits))
            return ND_PRED_FULL;
        if (out != NULL)
            *out = ND_WIDGET_RESULT_TYPED;
        return ND_PRED_HANDLED;
    }

    if (op->kind == ND_T9_OP_NEXT) {
        int32_t r = nd_pred_next_candidate(fld);

        if (r < 0)
            return ND_PRED_FULL;
        if (r == 0)
            return ND_PRED_UNHANDLED; /* and DELIBERATELY without committing */
        if (out != NULL)
            *out = ND_WIDGET_RESULT_TYPED;
        return ND_PRED_HANDLED;
    }

    /* Any other key ends the word. */
    nd_pred_commit(fld);
    return ND_PRED_UNHANDLED;
}

bool nd_pred_backspace(nd_pred_field *fld)
{
    const char *left;

    if (fld == NULL || fld->p == NULL || fld->p->pending_len == 0u)
        return false;

    /* Clear takes a TYPED DIGIT off, not a guessed letter: "good" becomes
     * "inn" (the guess for "466"), not "goo". */
    left = nd_t9_engine_pop_word_digit(fld->t9);
    if (left == NULL)
        return false;

    if (!nd_pred_predict(fld, left)) {
        /* Only reachable with the field exactly full and a shorter digit
         * string suggesting a longer word. Abandon the pending word rather
         * than leave the field and the engine disagreeing. */
        nd_predictive_reset(fld->p, fld->t9);
        return true;
    }
    if (left[0] == '\0')
        nd_predictive_reset(fld->p, fld->t9);
    return true;
}

/* nd_predictive_priv.h -- PredictiveText, the pending-word behaviour the two
 * text widgets share.
 *
 * NOT a public header. framework.py expresses this as a mixin that reaches
 * into `self.text`, `self.cursor` and `self.t9` on whichever widget inherited
 * it. C has no mixins, so the three things it touches are gathered into one
 * borrowed view -- nd_pred_field -- and every operation takes it.
 *
 * The insertion point is the widget's cursor when it has one and the end of
 * the text when it does not. That single difference is the whole reason
 * TextInput and TextInputLong can share this code: TextInput types at the end
 * and has no cursor field at all.
 *
 * ============ THE ONE THING THE PYTHON NEVER HAD TO DECIDE ============
 *
 * A Python string grows for ever. A C field has ND_TEXTINPUT_CAP or
 * ND_TEXTLONG_CAP bytes, and decision C-2 says that AT THE CAP THE WIDGET
 * IGNORES FURTHER INPUT rather than truncating -- silently dropping the tail
 * of a message somebody typed is worse than refusing the keypress.
 *
 * So every operation that can grow the field reports whether it fitted, and
 * the widget turns "it did not" into ND_WIDGET_RESULT_NONE. For a predictive
 * digit the widget additionally pops that digit back off the engine, so the
 * engine and the visible field never disagree about what has been typed.
 */

#ifndef ND_PREDICTIVE_PRIV_H_INCLUDED
#define ND_PREDICTIVE_PRIV_H_INCLUDED

#include "nd_t9.h"
#include "nd_types.h"
#include "nd_widgets.h"

/* A borrowed view of the mutable half of a text field. Nothing here is
 * owned; the widget outlives every call. */
typedef struct {
    nd_predictive *p;
    nd_t9_engine *t9;
    char *text; /* NUL-terminated, cap bytes including the NUL */
    size_t cap;
    size_t *cursor; /* NULL means "the insertion point is the end" */
} nd_pred_field;

/* _insert_at(): the byte offset the provisional word ends at. */
size_t nd_pred_insert_at(const nd_pred_field *fld);

/* _show_word(): put `word` where the provisional tail is. false when it would
 * not fit, in which case NOTHING is changed. */
bool nd_pred_show_word(nd_pred_field *fld, const char *word);

/* _predict(digits): look the digits up and show the best candidate, or the
 * digits themselves when nothing matches -- so the keypresses stay visible
 * and Clear still behaves. false when the result would not fit. */
bool nd_pred_predict(nd_pred_field *fld, const char *digits);

/* _next_candidate(): 1 when a different candidate is now showing, 0 when
 * there is no second candidate, -1 when it would not fit. */
int32_t nd_pred_next_candidate(nd_pred_field *fld);

/* _commit_word(): accept what is on screen. The characters are already in the
 * text, so this only stops the next keypress replacing them. */
void nd_pred_commit(nd_pred_field *fld);

typedef enum {
    ND_PRED_UNHANDLED = 0, /* not a predictive op -- handle it normally    */
    ND_PRED_HANDLED,       /* *out holds the widget's answer               */
    ND_PRED_FULL           /* would overflow the field; ignore the key     */
} nd_pred_status;

/* _predict_key(): apply one engine op to the pending word. Commits the word
 * for any op that is neither WORD nor NEXT, which is what makes "any other
 * key ends the word" true. */
nd_pred_status nd_pred_key(nd_pred_field *fld, const nd_t9_op *op, nd_widget_result *out);

/* _predict_backspace(): true when Clear was spent on the pending word. */
bool nd_pred_backspace(nd_pred_field *fld);

#endif /* ND_PREDICTIVE_PRIV_H_INCLUDED */

/* nd_textinput.c -- TextInput, the one-line field.
 *
 * Ported from System/ui/framework.py:663. Widget 8 of the fourteen in
 * nd_widgets.h. Used by Phonebook, RemoteShell, the Browser's address bar and
 * anywhere else that wants a name, a number or a host name.
 *
 * ============ THE FIELD HAS NO CURSOR ============
 *
 * Typing always happens at the END of the text. The "_" on screen is a blink
 * marker, not an insertion point -- there is no key that moves it and no
 * state that remembers where it was. TextInputLong is the widget with a real
 * cursor; keeping these two apart is why PredictiveText asks for the
 * insertion point through nd_pred_insert_at() instead of assuming one.
 *
 * ============ THE TEXT JUMPS AS YOU TYPE, AND THAT IS CORRECT ============
 *
 * text_y is centred on the INK height of the displayed string:
 *
 *     text_h = get_text_size(display_text or "A", font_n)[1]
 *     text_y = box_y + max(0, (box_h - text_h) // 2)
 *
 * An empty field showing only the cursor measures 3 px tall at 20 px; "Ag"
 * measures 21. So the line visibly moves down and back up as letters with
 * ascenders and descenders come and go. Reproduce it -- it is what the phone
 * does and widget-textinput.png is captured with it.
 *
 * ============ THE CURSOR DOES NOT BLINK ============
 *
 * show() checks its 0.5 s timer only AFTER ui.wait_for_key() returns, and
 * that blocks. An idle field therefore never blinks; the marker toggles on
 * the first keypress that arrives more than half a second after the last
 * toggle. A C loop with a poll() timeout would blink for real and would then
 * disagree with every captured frame, so the blocking wait is kept.
 *
 * ============ AT THE CAP THE KEY IS IGNORED ============
 *
 * Decision C-2. The Python field is unbounded and never had to choose;
 * dropping the tail of what somebody typed is worse than refusing the key.
 * Every growth path here checks first and returns ND_WIDGET_RESULT_NONE,
 * which the caller already treats as "do not redraw".
 */

#include <string.h>

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_keycodes.h"
#include "nd_predictive_priv.h"
#include "nd_t9.h"
#include "nd_text.h"
#include "nd_timeset.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

/* Python's // on a possibly-negative numerator floors; C's / truncates. Both
 * callers below wrap the result in max(0, ...) so the two only ever differ on
 * a value that is about to be clamped away -- but spell it correctly anyway,
 * because the next person to drop the clamp should not inherit a bug. */
static int32_t floordiv2(int32_t v)
{
    return (v >= 0) ? (v / 2) : -(((-v) + 1) / 2);
}

static const char *nz(const char *s)
{
    return (s != NULL) ? s : "";
}

/* Offset of the start of the codepoint that ends at `pos`. Python's
 * text[:-1] removes one CHARACTER; every character this widget can type is
 * ASCII, but an initial_text handed in by an app need not be. */
static size_t utf8_back(const char *s, size_t pos)
{
    if (pos == 0u)
        return 0u;
    pos--;
    while (pos > 0u && ((unsigned char)s[pos] & 0xC0u) == 0x80u)
        pos--;
    return pos;
}

static nd_pred_field field_of(nd_textinput *t)
{
    nd_pred_field f;

    f.p = &t->predict;
    f.t9 = &t->t9;
    f.text = t->text;
    f.cap = t->cap;
    f.cursor = NULL; /* no cursor: the insertion point is the end */
    return f;
}

nd_err nd_textinput_init(nd_textinput *t, nd_ui *ui, const char *title, const char *prompt,
                         char *text_buf, size_t cap, const char *initial, nd_t9_filter filter)
{
    nd_err rc;

    if (t == NULL || text_buf == NULL || cap == 0u)
        return ND_ERR_INVAL;
    /* draw() copies the line plus the cursor onto the stack, so the cap is a
     * hard ceiling and not a suggestion. */
    if (cap > ND_TEXTINPUT_CAP)
        return ND_ERR_INVAL;

    memset(t, 0, sizeof *t);
    t->ui = ui;
    t->title = title;
    t->prompt = prompt;
    t->text = text_buf;
    t->cap = cap;

    if (nd_strlcpy(text_buf, nz(initial), cap) >= cap)
        return ND_ERR_TOOLONG;

    rc = nd_t9_engine_init(&t->t9, filter, 0.0, NULL, NULL);
    if (rc != ND_OK)
        return rc;
    nd_predictive_reset(&t->predict, &t->t9);
    return ND_OK;
}

void nd_textinput_set_mask(nd_textinput *t, const char *mask)
{
    if (t == NULL)
        return;
    t->mask = (mask != NULL && mask[0] != '\0') ? mask : NULL;
}

void nd_textinput_set_no_autocap(nd_textinput *t, bool on)
{
    if (t != NULL)
        t->no_autocap = on;
}

void nd_textinput_draw(nd_textinput *t, bool blink_state)
{
    char display[ND_TEXTINPUT_CAP + 2];
    nd_ui *ui;
    nd_draw *d;
    int32_t screen_w;
    int32_t content_bottom;
    int32_t header_y;
    int32_t prompt_y;
    int32_t box_y;
    int32_t box_h;
    int32_t box_right;
    int32_t text_h = 0;
    int32_t text_y;
    size_t len;

    if (t == NULL || t->ui == NULL || t->ui->draw == NULL)
        return;
    ui = t->ui;
    d = ui->draw;

    screen_w = nd_ui_width(ui);
    content_bottom = nd_ui_content_bottom(ui);
    header_y = nd_ui_header_divider_y(ui);

    /* Rows 0..content_bottom only. A caller's earlier softkey update survives
     * into this frame -- widget-textinput.png still carries the "Select" the
     * PagedList before it painted. */
    nd_ui_paint_chrome_content(ui);

    /* No fit_text: a long title runs off the right edge. Port the bug. */
    (void)nd_draw_text(d, 5, 5, nz(t->title), ui->font_xl, ND_WHITE);
    (void)nd_draw_line(d, 0, header_y, screen_w, header_y, ND_WHITE, 1);

    prompt_y = header_y + 20;
    (void)nd_draw_text(d, 10, prompt_y, nz(t->prompt), ui->font_n, ND_WHITE);
    /* No mode indicator on a masked field: a field where every slot takes one
     * digit has no modes, and a pencil in the corner claiming otherwise is a
     * control the user will look for and not find. */
    if (t->mask == NULL)
        (void)nd_t9ind_draw(ui, screen_w - 12, prompt_y, &t->t9);

    box_y = prompt_y + 30;
    box_h = nd_max32(24, nd_min32(40, content_bottom - box_y - 10));
    box_right = nd_max32(20, screen_w - 10);
    (void)nd_draw_rect_outline(d, ND_RECT(10, box_y, box_right, box_y + box_h), ND_WHITE, 1);

    /* display_text = self.text + ("_" if blink_state else "") */
    len = nd_strlcpy(display, t->text, sizeof display);
    if (len >= sizeof display)
        len = sizeof display - 1u;
    if (blink_state && len + 1u < sizeof display) {
        display[len] = '_';
        display[len + 1u] = '\0';
    }

    /* `display_text or "A"`: an empty line is measured as an "A" so the box
     * does not collapse, but the empty string is what gets drawn. */
    nd_ui_text_size(ui, (display[0] != '\0') ? display : "A", ui->font_n, NULL, &text_h);
    text_y = box_y + nd_max32(0, floordiv2(box_h - text_h));
    (void)nd_draw_text(d, 15, text_y, display, ui->font_n, ND_WHITE);

    /* The underline is measured against the text WITHOUT the cursor. */
    nd_underline_tail(d, 15, text_y, t->text, t->predict.pending_len, ui->font_n);

    (void)nd_ui_present(ui);
}

nd_widget_result nd_textinput_handle_key(nd_textinput *t, int32_t key)
{
    nd_pred_field fld;
    nd_widget_result action = ND_WIDGET_RESULT_NONE;
    nd_t9_op op;
    nd_pred_status st;
    size_t before;
    size_t len;
    char ch;

    if (t == NULL || t->text == NULL)
        return ND_WIDGET_RESULT_NONE;

    /* A masked field is its own input method and shares nothing with the T9
     * path below -- see nd_textinput_set_mask(). Taken first so that not one
     * keypress reaches the engine and leaves it holding digits the field
     * never showed. */
    if (t->mask != NULL) {
        char digit;

        if (key == ND_KEY_ENTER || key == ND_KEY_KPENTER)
            return ND_WIDGET_RESULT_CONFIRM;
        if (key == ND_KEY_CLEAR) {
            /* Empty means leave, exactly as an ordinary field does -- Clear is
             * the only way back out of either. */
            if (nd_mask_backspace(t->mask, t->text))
                return ND_WIDGET_RESULT_BACKSPACE;
            return ND_WIDGET_RESULT_CANCEL;
        }
        digit = nd_key_digit_char(key);
        if (digit == '\0')
            return ND_WIDGET_RESULT_NONE;
        if (!nd_mask_type(t->mask, t->text, t->cap, digit))
            return ND_WIDGET_RESULT_NONE;
        return ND_WIDGET_RESULT_TYPED;
    }

    fld = field_of(t);

    /* ENTER / KPENTER. The Python's comment notes that legacy 50 was removed;
     * do not put it back, 50 is 'm' on the dev keyboard. */
    if (key == ND_KEY_ENTER || key == ND_KEY_KPENTER)
        return ND_WIDGET_RESULT_CONFIRM;

    if (key == ND_KEY_CLEAR) {
        /* A digit comes off the predictive word first, so a mistyped word can
         * be corrected without losing it. */
        if (nd_pred_backspace(&fld))
            return ND_WIDGET_RESULT_BACKSPACE;
        nd_pred_commit(&fld);
        nd_t9_engine_reset(&t->t9);
        len = strlen(t->text);
        if (len > 0u) {
            t->text[utf8_back(t->text, len)] = '\0';
            return ND_WIDGET_RESULT_BACKSPACE;
        }
        /* Clear on an empty one-line field is how you back out of it. */
        return ND_WIDGET_RESULT_CANCEL;
    }

    if (t->ui != NULL && t->ui->has_matrix_keypad) {
        before = strlen(nd_t9_engine_word_digits(&t->t9));
        op = nd_t9_engine_press(&t->t9, key);
        if (op.kind == ND_T9_OP_NONE)
            return ND_WIDGET_RESULT_NONE;

        st = nd_pred_key(&fld, &op, &action);
        if (st == ND_PRED_HANDLED)
            return action;
        if (st == ND_PRED_FULL) {
            /* Undo the digit this press appended, so the engine and the
             * visible field never disagree about what has been typed. */
            if (strlen(nd_t9_engine_word_digits(&t->t9)) > before)
                (void)nd_t9_engine_pop_word_digit(&t->t9);
            return ND_WIDGET_RESULT_NONE;
        }

        len = strlen(t->text);
        switch (op.kind) {
        case ND_T9_OP_APPEND:
            if (len + 2u > t->cap)
                return ND_WIDGET_RESULT_NONE; /* C-2: ignore, do not truncate */
            t->text[len] = op.ch;
            t->text[len + 1u] = '\0';
            return ND_WIDGET_RESULT_TYPED;
        case ND_T9_OP_REPLACE:
            /* text[:-1] + value. On an empty field text[:-1] is "", so a
             * REPLACE there behaves as an APPEND -- which is what the Python
             * does and is only reachable after a reset mid-cycle. */
            len = utf8_back(t->text, len);
            if (len + 2u > t->cap)
                return ND_WIDGET_RESULT_NONE;
            t->text[len] = op.ch;
            t->text[len + 1u] = '\0';
            return ND_WIDGET_RESULT_TYPED;
        case ND_T9_OP_MODE:
            return ND_WIDGET_RESULT_MODE;
        default:
            return ND_WIDGET_RESULT_NONE;
        }
    }

    /* Dev keyboard (QWERTY). */
    ch = nd_key_dev_char(key);
    if (ch == '\0' || !nd_t9_char_allowed(ch, t->t9.filter))
        return ND_WIDGET_RESULT_NONE;
    len = strlen(t->text);
    if (!t->no_autocap && len == 0u && ch >= 'a' && ch <= 'z')
        ch = (char)(ch - ('a' - 'A')); /* str.upper() on the first character */
    if (len + 2u > t->cap)
        return ND_WIDGET_RESULT_NONE;
    t->text[len] = ch;
    t->text[len + 1u] = '\0';
    return ND_WIDGET_RESULT_TYPED;
}

/* See nd_ui_set_repaint(). The cursor is carried by POINTER so a repaint
 * redraws the field in whatever blink phase it is actually in -- it must not
 * toggle it. "An idle field never blinks" is a documented quirk of this
 * widget (see the header), and an animated wallpaper is not a reason for it
 * to start. */
typedef struct {
    nd_textinput *t;
    const bool *cursor_on;
} textinput_repaint_ctx;

static void textinput_repaint(void *ctx)
{
    const textinput_repaint_ctx *c = ctx;

    nd_textinput_draw(c->t, *c->cursor_on);
}

const char *nd_textinput_show(nd_textinput *t)
{
    nd_softkey softkey;
    bool cursor_on = true;
    double last_blink;
    textinput_repaint_ctx rctx;
    nd_ui_repaint saved;
    const char *out;

    if (t == NULL)
        return NULL;

    nd_softkey_init(&softkey, t->ui, false);
    nd_softkey_update(&softkey, "OK", true);

    last_blink = nd_time_now();
    nd_textinput_draw(t, cursor_on);

    rctx.t = t;
    rctx.cursor_on = &cursor_on;
    saved = nd_ui_set_repaint(t->ui, textinput_repaint, &rctx);

    for (;;) {
        int32_t key;
        nd_widget_result action;
        double now = nd_time_now();

        /* Checked here, before the blocking wait -- which is why an idle
         * field never blinks. See the header comment. */
        if (now - last_blink > 0.5) {
            cursor_on = !cursor_on;
            last_blink = now;
            nd_textinput_draw(t, cursor_on);
        }

        key = nd_ui_wait_for_key(t->ui);
        if (key < 0)
            continue; /* `if key is None: continue` */

        action = nd_textinput_handle_key(t, key);
        if (action == ND_WIDGET_RESULT_CONFIRM) {
            out = t->text;
            break;
        }
        if (action == ND_WIDGET_RESULT_CANCEL) {
            out = NULL;
            break;
        }
        if (action == ND_WIDGET_RESULT_TYPED || action == ND_WIDGET_RESULT_BACKSPACE ||
            action == ND_WIDGET_RESULT_MODE)
            nd_textinput_draw(t, cursor_on);
    }

    nd_ui_restore_repaint(t->ui, saved);
    return out;
}

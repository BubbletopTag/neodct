/* nd_textlong.c -- TextInputLong, the message composer.
 *
 * Ported from System/ui/framework.py:800. Widget 9 of the fourteen in
 * nd_widgets.h. Messages uses it to write an SMS; nothing else does.
 *
 * There is deliberately NO nd_textlong_show(): the composing loop lives in
 * the app, which owns the blink timer, the softkey and what Clear on an empty
 * field means.
 *
 * ============ IT HAS A REAL CURSOR, AND THE CURSOR NEVER MOVES ============
 *
 * Unlike TextInput this widget carries an insertion point, and every edit
 * happens there. No key moves it: it starts at the end of the initial text
 * and only insertion and deletion shift it. That is worth knowing, because
 * the incremental rewrap below is built on it.
 *
 * The cursor is a BYTE offset, as nd_widgets.h says. The Python's is a
 * character index; the two agree for everything the keypad can type, which is
 * ASCII, and the byte offset is what a C wrap and a C underline need.
 *
 * ============ FIXING THE O(n^2) REWRAP (decision C-2) ============
 *
 * `_current_lines()` rewraps the ENTIRE message on every keypress, and each
 * wrap re-measures every candidate line, so composing an n-character message
 * costs O(n^2). Decision C-2 asks for it to be fixed by rewrapping from the
 * edited line rather than from the start.
 *
 * The fix is a watermark, and it rests on one property of the wrapper: the
 * greedy algorithm's whole state between output lines is "where we are in the
 * text". So if a byte offset k is known to begin a wrapped line, then
 * wrapping text[k:] produces exactly the tail of wrapping text -- the earlier
 * lines cannot change what comes after them. wrap_clean_off is such a k and
 * wrap_clean_lines is how many lines precede it.
 *
 * Two things make that safe rather than merely plausible:
 *
 *   1. A line that begins mid-word -- one of the pieces a hard-broken long
 *      word was cut into -- is NOT a valid restart point. Restarted there the
 *      piece is a normal-width word and the wrapper would happily join the
 *      next word onto it. Those lines are detected (no whitespace was
 *      consumed before them) and refused.
 *
 *   2. Every advance is VERIFIED: the wrap restarted at the candidate offset
 *      is compared against the tail it is supposed to reproduce, and the
 *      watermark only moves if they agree. A bug in the offset walk therefore
 *      costs performance, never pixels.
 *
 * The watermark is also kept at least `keep` lines behind the end, so the
 * lines the screen actually shows are always inside the re-wrapped tail.
 *
 * Anything that edits at or before the watermark resets it to zero, which is
 * simply the Python's behaviour with a full rewrap.
 *
 * ============ AT THE CAP THE KEY IS IGNORED ============
 *
 * Decision C-2 again: ND_TEXTLONG_CAP bytes, and at the cap the widget
 * refuses the keypress rather than silently dropping the tail of what
 * somebody typed.
 */

#include <stdio.h>
#include <string.h>

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_keycodes.h"
#include "nd_predictive_priv.h"
#include "nd_t9.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

/* Stack arithmetic, per CODING-STANDARDS.md section 4:
 *   tail   32 * 256 =  8,192 bytes
 *   verify 16 * 256 =  4,096 bytes
 *   scratch          =  1,026 bytes
 * about 13 KB in draw(), which is the deepest frame this widget has. */
#define ND_TL_TAIL_LINES   32u
#define ND_TL_VERIFY_LINES 16u

/* The floor on how many wrapped lines must remain after the watermark. This
 * panel shows five, so ten is five lines of slack before deleting forces a
 * full rewrap. current_lines() raises it if a taller panel ever shows more. */
#define ND_TL_KEEP 10u

static const char *nz(const char *s)
{
    return (s != NULL) ? s : "";
}

static size_t utf8_back(const char *s, size_t pos)
{
    if (pos == 0u)
        return 0u;
    pos--;
    while (pos > 0u && ((unsigned char)s[pos] & 0xC0u) == 0x80u)
        pos--;
    return pos;
}

/* len(self.text): Python counts CHARACTERS, and the header count on screen is
 * the one place that difference is visible. */
static size_t utf8_len(const char *s)
{
    size_t n = 0u;

    while (*s != '\0') {
        if (((unsigned char)*s & 0xC0u) != 0x80u)
            n++;
        s++;
    }
    return n;
}

static nd_pred_field field_of(nd_textlong *t)
{
    nd_pred_field f;

    f.p = &t->predict;
    f.t9 = &t->t9;
    f.text = t->text;
    f.cap = t->cap;
    f.cursor = &t->cursor;
    return f;
}

/* ------------------------------------------------------------------ *
 * The wrap watermark
 * ------------------------------------------------------------------ */

static void wrap_invalidate_at(nd_textlong *t, size_t pos)
{
    if (pos <= t->wrap_clean_off) {
        t->wrap_clean_off = 0u;
        t->wrap_clean_lines = 0u;
    }
}

/* Recover, for each wrapped line, the byte offset in `text` at which its
 * content begins, and whether restarting the wrap there reproduces the rest.
 *
 * The walk works because the wrapper only ever DROPS bytes (runs of spaces
 * between words, and the newline between raw lines) and inserts a single
 * space where it joins two words. So text and the wrapped lines can be walked
 * in lockstep, and the amount of whitespace consumed before a line is exactly
 * the signal for whether that line began at a word boundary.
 *
 * false means the walk did not line up -- the caller then leaves its
 * watermark alone, which is always safe. */
static bool line_offsets(const char *text, const nd_lines *lines, size_t *off, bool *safe,
                         size_t max)
{
    const char *p = text;
    size_t i;

    if (lines->n > max)
        return false;

    for (i = 0u; i < lines->n; i++) {
        const char *line = nd_lines_at(lines, i);
        size_t skipped = 0u;
        const char *q;

        if (line[0] == '\0') {
            /* The unconditional push at the end of a raw line, with nothing
             * accumulated: a blank source line, or a raw line whose last word
             * was hard-broken. Either way the newline (if any) belongs to it. */
            off[i] = (size_t)(p - text);
            safe[i] = true;
            if (p[0] == '\r' && p[1] == '\n')
                p += 2;
            else if (p[0] == '\n')
                p += 1;
            continue;
        }

        while (*p == ' ' || *p == '\n' || *p == '\r') {
            p++;
            skipped++;
        }
        off[i] = (size_t)(p - text);
        /* No whitespace between this line and the last means this line is a
         * continuation piece of a hard-broken word. Restarting there would
         * let the wrapper join the following word onto the piece. */
        safe[i] = (skipped > 0u) || (i == 0u);

        for (q = line; *q != '\0'; q++) {
            if (*q == ' ') {
                if (*p != ' ')
                    return false;
                while (*p == ' ')
                    p++;
            } else {
                if (*p != *q)
                    return false;
                p++;
            }
        }
    }
    return true;
}

/* Move the watermark forward so that only `keep` lines of `tail` remain after
 * it. Returns true when it moved, which is the caller's signal to rewrap.
 *
 * `from` is the buffer `tail` was wrapped out of -- the text at the current
 * watermark WITH the blink marker on the end, not the raw text. Walking the
 * raw text instead makes the last line fail to match and the watermark never
 * moves, which is a silent loss of the whole optimisation. */
static bool wrap_advance(nd_textlong *t, const char *from, const nd_lines *tail, int32_t max_w,
                         size_t keep)
{
    size_t off[ND_TL_TAIL_LINES];
    bool safe[ND_TL_TAIL_LINES];
    char vstore[ND_TL_VERIFY_LINES][ND_TEXT_LINE_MAX];
    nd_lines verify;
    size_t i;
    size_t m;
    size_t j;

    if (tail->n <= keep)
        return false;
    if (!line_offsets(from, tail, off, safe, ND_TL_TAIL_LINES))
        return false;

    i = tail->n - keep;
    while (i > 0u && !safe[i])
        i--;
    if (i == 0u || off[i] == 0u)
        return false;
    /* The marker is one byte past the real text, so a candidate at or beyond
     * it is not an offset into the text at all. */
    if (off[i] > strlen(t->text) - t->wrap_clean_off)
        return false;

    /* Verify before trusting it: wrapping from the candidate must reproduce
     * the tail it claims to. A mismatch costs one wasted wrap, never a
     * miscounted screen. */
    nd_lines_init(&verify, vstore, ND_TL_VERIFY_LINES);
    nd_text_wrap_break(&verify, from + off[i], t->font, max_w);
    if (verify.n == 0u)
        return false;
    if (!verify.truncated && !tail->truncated && verify.n != tail->n - i)
        return false;
    m = verify.n;
    if (m > tail->n - i)
        m = tail->n - i;
    for (j = 0u; j < m; j++) {
        if (strcmp(nd_lines_at(&verify, j), nd_lines_at(tail, i + j)) != 0)
            return false;
    }

    t->wrap_clean_off += off[i];
    t->wrap_clean_lines += i;
    return true;
}

/* _current_lines(): the wrap of `text + cursor marker`, but only from the
 * watermark onward. *total is the count the whole string would have produced,
 * which is what the Python's `start = max(0, len(lines) - max_lines)` needs.
 *
 * `need` is how many lines the screen is about to show, so the watermark can
 * never be so far forward that the window falls off the front of the tail. */
static void current_lines(nd_textlong *t, bool blink_state, nd_lines *out, size_t *total,
                          int32_t max_w, size_t need)
{
    char scratch[ND_TEXTLONG_CAP + 2];
    size_t keep = (need + 3u > ND_TL_KEEP) ? need + 3u : ND_TL_KEEP;
    size_t attempt;

    if (keep > ND_TL_TAIL_LINES - 1u)
        keep = ND_TL_TAIL_LINES - 1u;

    for (attempt = 0u; attempt < 64u; attempt++) {
        size_t n = nd_strlcpy(scratch, t->text + t->wrap_clean_off, sizeof scratch);

        if (n >= sizeof scratch)
            n = sizeof scratch - 1u;
        if (blink_state && n + 1u < sizeof scratch) {
            scratch[n] = '_';
            scratch[n + 1u] = '\0';
        }
        nd_text_wrap_break(out, scratch, t->font, max_w);

        if (out->n < need && t->wrap_clean_off != 0u) {
            /* Deleting has eaten into the window. Go back to the whole
             * string, which is what the Python does on every keypress
             * anyway, and let the next draw re-establish the watermark. */
            t->wrap_clean_off = 0u;
            t->wrap_clean_lines = 0u;
            continue;
        }
        if (out->n <= keep && !out->truncated)
            break;
        if (!wrap_advance(t, scratch, out, max_w, keep))
            break;
    }
    *total = t->wrap_clean_lines + out->n;
}

/* ------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------ */

nd_err nd_textlong_init(nd_textlong *t, nd_ui *ui, const char *title, char *text_buf, size_t cap,
                        const char *initial, nd_t9_filter filter)
{
    nd_err rc;

    if (t == NULL || text_buf == NULL || cap == 0u)
        return ND_ERR_INVAL;
    /* The draw path copies the tail onto the stack, so the cap is a hard
     * ceiling rather than a suggestion. */
    if (cap > ND_TEXTLONG_CAP)
        return ND_ERR_INVAL;

    memset(t, 0, sizeof *t);
    t->ui = ui;
    t->title = title;
    t->text = text_buf;
    t->cap = cap;

    if (nd_strlcpy(text_buf, nz(initial), cap) >= cap)
        return ND_ERR_TOOLONG;
    t->cursor = strlen(text_buf);

    rc = nd_t9_engine_init(&t->t9, filter, 0.0, NULL, NULL);
    if (rc != ND_OK)
        return rc;
    nd_predictive_reset(&t->predict, &t->t9);

    /* getattr(ui, "font_s", None) or ui.font_n */
    t->font = (ui != NULL && ui->font_s != NULL) ? ui->font_s : ((ui != NULL) ? ui->font_n : NULL);
    t->text_area_top = nd_ui_header_divider_y(ui) + 10;
    t->text_area_bottom = nd_ui_content_bottom(ui) - 4;
    return ND_OK;
}

void nd_textlong_set_on_empty_backspace(nd_textlong *t, nd_empty_backspace_fn fn, void *ctx)
{
    if (t == NULL)
        return;
    t->on_empty_backspace = fn;
    t->on_empty_ctx = ctx;
}

const char *nd_textlong_get_text(const nd_textlong *t)
{
    return (t != NULL && t->text != NULL) ? t->text : "";
}

nd_err nd_textlong_set_text(nd_textlong *t, const char *s)
{
    if (t == NULL || t->text == NULL)
        return ND_ERR_INVAL;
    if (nd_strlcpy(t->text, nz(s), t->cap) >= t->cap) {
        t->text[0] = '\0';
        t->cursor = 0u;
        nd_predictive_reset(&t->predict, &t->t9);
        wrap_invalidate_at(t, 0u);
        return ND_ERR_TOOLONG;
    }
    t->cursor = strlen(t->text);
    nd_predictive_reset(&t->predict, &t->t9);
    wrap_invalidate_at(t, 0u);
    return ND_OK;
}

void nd_textlong_clear_text(nd_textlong *t)
{
    if (t == NULL || t->text == NULL)
        return;
    t->text[0] = '\0';
    t->cursor = 0u;
    nd_predictive_reset(&t->predict, &t->t9);
    wrap_invalidate_at(t, 0u);
}

/* ------------------------------------------------------------------ *
 * Drawing
 * ------------------------------------------------------------------ */

void nd_textlong_draw(nd_textlong *t, bool blink_state)
{
    char tstore[ND_TL_TAIL_LINES][ND_TEXT_LINE_MAX];
    nd_lines lines;
    nd_ui *ui;
    nd_draw *d;
    char count[16];
    int32_t screen_w;
    int32_t content_bottom;
    int32_t header_y;
    int32_t cw = 0;
    int32_t line_h = 0;
    int32_t max_w;
    int32_t y;
    size_t total = 0u;
    size_t max_lines;
    size_t start;
    size_t shown;
    size_t i;

    if (t == NULL || t->ui == NULL || t->ui->draw == NULL || t->font == NULL)
        return;
    ui = t->ui;
    d = ui->draw;

    screen_w = nd_ui_width(ui);
    content_bottom = nd_ui_content_bottom(ui);
    header_y = nd_ui_header_divider_y(ui);

    (void)nd_draw_rect_fill(d, ND_RECT(0, 0, screen_w, content_bottom), ND_BLACK);

    /* The title is drawn with no fitting and the character count is drawn
     * flush right, so a long title runs UNDER the count. widget-textinputlong
     * is captured with exactly that overlap; port the bug. */
    (void)nd_draw_text(d, 5, 5, nz(t->title), ui->font_xl, ND_WHITE);
    (void)snprintf(count, sizeof count, "%zu", utf8_len(t->text));
    nd_ui_text_size(ui, count, ui->font_n, &cw, NULL);
    (void)nd_draw_text(d, screen_w - 5 - cw, 5, count, ui->font_n, ND_WHITE);
    (void)nd_t9ind_draw(ui, screen_w - 5 - cw - 10, 5, &t->t9);
    (void)nd_draw_line(d, 0, header_y, screen_w, header_y, ND_WHITE, 1);

    /* line_h is the INK height of "Ag" plus 3 -- a fixed leading over a
     * measurement that happens to include both an ascender and a descender. */
    nd_ui_text_size(ui, "Ag", t->font, NULL, &line_h);
    line_h += 3;
    if (line_h <= 0)
        line_h = 1;
    max_lines = (size_t)nd_max32(
        1, nd_trunc32((double)(t->text_area_bottom - t->text_area_top) / (double)line_h));

    max_w = nd_max32(20, screen_w - 20);
    nd_lines_init(&lines, tstore, ND_TL_TAIL_LINES);
    current_lines(t, blink_state, &lines, &total, max_w, max_lines);

    /* start = max(0, len(lines) - max_lines), in the WHOLE string's numbering,
     * then translated into the re-wrapped tail. current_lines() keeps the
     * watermark far enough back that the window lands inside the tail; the
     * clamps are for a pathological message that defeated every advance. */
    start = (total > max_lines) ? total - max_lines : 0u;
    start = (start >= t->wrap_clean_lines) ? start - t->wrap_clean_lines : 0u;
    if (start > lines.n)
        start = lines.n;
    shown = lines.n - start;
    if (shown > max_lines)
        shown = max_lines;

    y = t->text_area_top;
    for (i = 0u; i < shown; i++) {
        const char *line = nd_lines_at(&lines, start + i);

        (void)nd_draw_text(d, 10, y, line, t->font, ND_WHITE);
        if (t->predict.pending_len != 0u && i == shown - 1u) {
            /* The provisional word is always at the end of the text, so it is
             * on the last line -- minus the blinking cursor, which is not
             * part of it. */
            char body[ND_TEXT_LINE_MAX];
            size_t blen = nd_strlcpy(body, line, sizeof body);
            size_t tail;

            if (blen >= sizeof body)
                blen = sizeof body - 1u;
            if (blink_state && blen > 0u && body[blen - 1u] == '_')
                body[--blen] = '\0';
            tail = (t->predict.pending_len < blen) ? t->predict.pending_len : blen;
            nd_underline_tail(d, 10, y, body, tail, t->font);
        }
        y += line_h;
    }

    (void)nd_ui_present(ui);
}

/* ------------------------------------------------------------------ *
 * Keys
 * ------------------------------------------------------------------ */

nd_widget_result nd_textlong_handle_key(nd_textlong *t, int32_t key)
{
    nd_pred_field fld;
    nd_widget_result action = ND_WIDGET_RESULT_NONE;
    nd_t9_op op;
    nd_pred_status st;
    size_t before;
    size_t len;
    size_t pend_start;
    size_t at;
    char ch;

    if (t == NULL || t->text == NULL)
        return ND_WIDGET_RESULT_NONE;
    fld = field_of(t);
    len = strlen(t->text);
    if (t->cursor > len)
        t->cursor = len;
    /* Where a predictive edit will start writing. Captured BEFORE the edit,
     * because pending_len and the cursor both move underneath it and the
     * watermark has to be invalidated against the earliest byte touched. */
    pend_start = (t->cursor > t->predict.pending_len) ? t->cursor - t->predict.pending_len : 0u;

    if (key == ND_KEY_CLEAR) {
        if (nd_pred_backspace(&fld)) {
            wrap_invalidate_at(t, pend_start);
            return ND_WIDGET_RESULT_BACKSPACE;
        }
        nd_pred_commit(&fld);
        nd_t9_engine_reset(&t->t9);

        if (len == 0u) {
            /* Clear on an already-empty composer is how Messages leaves it. */
            if (t->on_empty_backspace != NULL)
                t->on_empty_backspace(t->on_empty_ctx);
            return ND_WIDGET_RESULT_EMPTY_BACKSPACE;
        }
        if (t->cursor > 0u) {
            at = utf8_back(t->text, t->cursor);
            memmove(t->text + at, t->text + t->cursor, len - t->cursor + 1u);
            t->cursor = at;
            wrap_invalidate_at(t, at);
        }
        return ND_WIDGET_RESULT_BACKSPACE;
    }

    if (t->ui != NULL && t->ui->has_matrix_keypad) {
        before = strlen(nd_t9_engine_word_digits(&t->t9));
        op = nd_t9_engine_press(&t->t9, key);
        if (op.kind == ND_T9_OP_NONE)
            return ND_WIDGET_RESULT_NONE;

        st = nd_pred_key(&fld, &op, &action);
        if (st == ND_PRED_HANDLED) {
            wrap_invalidate_at(t, pend_start);
            return action;
        }
        if (st == ND_PRED_FULL) {
            if (strlen(nd_t9_engine_word_digits(&t->t9)) > before)
                (void)nd_t9_engine_pop_word_digit(&t->t9);
            return ND_WIDGET_RESULT_NONE;
        }

        switch (op.kind) {
        case ND_T9_OP_APPEND:
            if (len + 2u > t->cap)
                return ND_WIDGET_RESULT_NONE; /* C-2: ignore, do not truncate */
            memmove(t->text + t->cursor + 1u, t->text + t->cursor, len - t->cursor + 1u);
            t->text[t->cursor] = op.ch;
            wrap_invalidate_at(t, t->cursor);
            t->cursor++;
            return ND_WIDGET_RESULT_TYPED;
        case ND_T9_OP_REPLACE:
            /* The Python does not move the cursor here, because the character
             * it overwrites and the one it writes are both one CHARACTER. In
             * bytes they need not be, so the offset is corrected -- for
             * anything the keypad can type the correction is zero. */
            if (t->cursor > 0u) {
                size_t start = utf8_back(t->text, t->cursor);
                size_t old = t->cursor - start;

                if (len - old + 2u > t->cap)
                    return ND_WIDGET_RESULT_NONE;
                memmove(t->text + start + 1u, t->text + t->cursor, len - t->cursor + 1u);
                t->text[start] = op.ch;
                t->cursor = start + 1u;
                wrap_invalidate_at(t, start);
            }
            /* Returns "typed" even with the cursor at 0, where nothing
             * happened. Port it. */
            return ND_WIDGET_RESULT_TYPED;
        case ND_T9_OP_MODE:
            return ND_WIDGET_RESULT_MODE;
        default:
            return ND_WIDGET_RESULT_NONE;
        }
    }

    ch = nd_key_dev_char(key);
    if (ch == '\0')
        return ND_WIDGET_RESULT_NONE;
    if (!nd_t9_char_allowed(ch, t->t9.filter))
        return ND_WIDGET_RESULT_NONE;
    if (len == 0u && ch >= 'a' && ch <= 'z')
        ch = (char)(ch - ('a' - 'A'));
    if (len + 2u > t->cap)
        return ND_WIDGET_RESULT_NONE;
    memmove(t->text + t->cursor + 1u, t->text + t->cursor, len - t->cursor + 1u);
    t->text[t->cursor] = ch;
    wrap_invalidate_at(t, t->cursor);
    t->cursor++;
    return ND_WIDGET_RESULT_TYPED;
}

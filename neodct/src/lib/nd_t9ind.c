/* nd_t9ind.c -- the input-mode indicator both text widgets paint, and the
 * pencil that marks predictive mode.
 *
 * Ported from System/ui/framework.py:30 (_draw_pencil), :63
 * (t9_indicator_size) and :76 (draw_t9_indicator).
 *
 * ============ WHY THERE IS A PENCIL AND NOT A GLYPH ============
 *
 * The indicator is text -- "abc" / "ABC" / "123" -- in every mode except
 * predictive, where the alphabet is unchanged and what differs is that the
 * phone guesses the word for you. Nokia marked that with a pencil. This font
 * has no U+270F, so the pencil is drawn.
 *
 * ============ WHY IT IS PLOTTED PER PIXEL ============
 *
 * The Python's own comment, and it is the reason this file exists rather than
 * three calls to nd_draw_polygon():
 *
 *   "Plotted per pixel rather than with line()/polygon(), because at the
 *    ~15px this actually renders at, a polygon's edges land wherever the
 *    rounding puts them and the barrel comes out either a hairline or twice
 *    the weight of the font next to it. Walking the diagonal gives the barrel
 *    an exact number of pixels across, which is what pixel art on a 240x240
 *    panel needs."
 *
 * The two round() calls are Python's, which is round-half-to-EVEN. C's
 * round() is half-away-from-zero and gives a different `half` at size 10
 * (2.5 -> 2 in Python, 3 in C) and a different `point` at size 30
 * (16.5 -> 16 vs 17). Use nd_round_half_even(); nd_widgets.h says so too.
 *
 * ============ NULL-SAFE BY CONTRACT ============
 *
 * _t9_active(ui) is `getattr(ui, "matrix_input", None) is not None`: T9
 * multi-tap only runs on the real i2c keypad, because a development QEMU
 * keyboard has a full QWERTY and takes the DEV_KEYMAP path instead. So on
 * QEMU -- and in every golden frame, which was captured with no keypad
 * device -- the indicator is not drawn at all and size() returns 0.
 */

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_t9.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

/* framework._t9_active(). */
static bool t9_active(const nd_ui *ui)
{
    return ui != NULL && ui->has_matrix_keypad;
}

void nd_draw_pencil(struct nd_draw *d, int32_t x, int32_t y, int32_t size, nd_color c)
{
    int32_t end;
    int32_t half;
    int32_t point;
    int32_t row;
    const int32_t gap = 2; /* background between the point and the barrel */

    if (d == NULL || size <= 0)
        return;

    end = size - 1;
    /* Python's round() is banker's rounding. At size 10 that is the
     * difference between a 2 px and a 3 px barrel. */
    half = nd_max32(1, nd_trunc32(nd_round_half_even((double)size / 4.0)));
    point = nd_max32(4, nd_trunc32(nd_round_half_even((double)size * 0.55)));

    for (row = 0; row < size; row++) {
        int32_t col;

        for (col = 0; col < size; col++) {
            /* Distance along the pencil's axis, and off its centreline, both
             * measured from the point in the bottom-left corner. */
            int32_t along = col + (end - row);
            int32_t across = col - (end - row);
            int32_t width;

            if (across < 0)
                across = -across;
            if (along > 2 * end)
                continue;
            if (along <= point) {
                width = along * half / point; /* taper; both operands >= 0 */
            } else if (along <= point + gap) {
                continue; /* the collar */
            } else {
                width = half;
            }
            if (across <= width)
                (void)nd_draw_point(d, x + col, y + row, c);
        }
    }
}

int32_t nd_t9ind_size(const nd_ui *ui, const nd_t9_engine *t9, const char **label_out,
                      int32_t *pencil_out)
{
    const char *label;
    int32_t tw = 0;
    int32_t th = 0;
    int32_t size;

    if (!t9_active(ui) || t9 == NULL || ui->font_n == NULL)
        return 0;

    if (nd_t9_engine_mode(t9) != ND_T9_MODE_WORD) {
        /* The label IS the mode name in the other three modes. */
        label = nd_t9_mode_label(nd_t9_engine_mode(t9));
        nd_ui_text_size(ui, label, ui->font_n, &tw, &th);
        if (label_out != NULL)
            *label_out = label;
        if (pencil_out != NULL)
            *pencil_out = 0;
        return tw;
    }

    label = nd_t9_mode_label(ND_T9_MODE_ABC);
    nd_ui_text_size(ui, label, ui->font_n, &tw, &th);
    /* int(th * 0.85) -- truncation, not rounding, and floored at 8. */
    size = nd_max32(8, nd_trunc32((double)th * 0.85));
    if (label_out != NULL)
        *label_out = label;
    if (pencil_out != NULL)
        *pencil_out = size;
    return size + ND_T9_PENCIL_GAP + tw;
}

int32_t nd_t9ind_draw(nd_ui *ui, int32_t right, int32_t y, const nd_t9_engine *t9)
{
    const char *label = NULL;
    int32_t pencil = 0;
    int32_t width;
    int32_t x;

    width = nd_t9ind_size(ui, t9, &label, &pencil);
    if (width == 0 || label == NULL)
        return 0;

    x = right - width;
    if (pencil > 0) {
        int32_t text_h = 0;

        nd_ui_text_size(ui, label, ui->font_n, NULL, &text_h);
        /* Sit the pencil on the text's baseline rather than its box top, or
         * it floats above short lowercase letters. */
        nd_draw_pencil(ui->draw, x, y + nd_max32(0, text_h - pencil), pencil, ND_WHITE);
        x += pencil + ND_T9_PENCIL_GAP;
    }
    (void)nd_draw_text(ui->draw, x, y, label, ui->font_n, ND_WHITE);
    return width;
}

/* nd_progress.c -- ProgressScreen: a step name, the bar, and the reading.
 *
 * Ported from System/ui/framework.py:1522. Six call sites, all in Update and
 * Downgrade.
 *
 * ============ NOTHING IS EVER DRAWN ON TOP OF THE BAR ============
 *
 * The Python comment is worth keeping: a percentage sitting across its own
 * fill is the one thing that makes a progress bar look broken. The label goes
 * above it, the reading below it, and the boxes below never overlap.
 *
 * ============ THE PERCENT GATE IS A PERFORMANCE FEATURE ============
 *
 * draw() returns false and paints NOTHING when the whole percentage has not
 * moved. The copy loop calls it per 256 KB chunk; repainting every time would
 * make the progress display slower than the write it is reporting on.
 * `percent == -1` is C's spelling of the Python's `self._percent = None`,
 * i.e. "nothing drawn yet", and set_step() resets to it to force a repaint.
 *
 * ============ THE FIVE BOXES ARE READ BY TESTS ============
 *
 * header_box, label_box, bar_box, status_box and hint_box are computed once
 * in init and are public, because neodct/tests/test_update_ui.py asserts on
 * them directly -- "nothing within 3 px of the bar", "the reading is below
 * the bar", "a long label stays inside x in [4, 236]". On this panel they are
 * (0,4,240,19) (0,44,240,65) (20,79,220,93) (20,102,220,117) (0,124,240,139),
 * and divider_y is 24. They are DERIVED, not hard-coded: bar_top is
 * int(content_bottom * 0.55) and label_y hangs off it.
 *
 * ============ _centered USES LEFT + RIGHT, NOT LEFT + WIDTH ============
 *
 * (box[0] + box[2] - text_w) // 2. For status_box that is (20 + 220 - w)//2,
 * which is the same as centring on the whole 240 px screen only because the
 * box is symmetric. Port the expression, not the coincidence.
 */

#include <stdio.h>
#include <string.h>

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

/* Python's // floors; a label wider than its box makes the numerator
 * negative, which is where floor and C's truncation part company. */
static int32_t floordiv2(int32_t v)
{
    return (v >= 0) ? (v / 2) : -(((-v) + 1) / 2);
}

static const nd_font *step_font_of(const nd_ui *ui)
{
    return (ui->font_n != NULL) ? ui->font_n : ui->font_md;
}

static const nd_font *small_font_of(const nd_ui *ui)
{
    return (ui->font_s != NULL) ? ui->font_s : step_font_of(ui);
}

void nd_progress_init(nd_progress *p, nd_ui *ui, const char *step, const char *header,
                      const char *hint, nd_progress_detail_fn detail, void *detail_ctx)
{
    int32_t width;
    int32_t bottom;
    int32_t step_h = 0;
    int32_t small_h = 0;
    int32_t bar_top;
    int32_t label_y;
    int32_t status_y;
    int32_t hint_y;
    const nd_font *font_step;
    const nd_font *font_small;

    if (p == NULL || ui == NULL)
        return;

    memset(p, 0, sizeof *p);
    p->ui = ui;
    p->step = (step != NULL) ? step : "";
    p->header = header;
    p->hint = hint;
    p->detail = detail;
    p->detail_ctx = detail_ctx;
    p->percent = -1; /* Python's None: nothing drawn yet */

    width = nd_ui_width(ui);
    bottom = nd_ui_content_bottom(ui);
    font_step = step_font_of(ui);
    font_small = small_font_of(ui);

    nd_text_size(font_step, "Ag", NULL, &step_h);
    nd_text_size(font_small, "Ag", NULL, &small_h);

    p->header_box = ND_RECT(0, 4, width, 4 + small_h);
    p->divider_y = p->header_box.y1 + 5;

    bar_top = nd_trunc32((double)bottom * 0.55);
    p->bar_box = ND_RECT(ND_PROGRESS_BAR_MARGIN, bar_top, width - ND_PROGRESS_BAR_MARGIN,
                         bar_top + ND_PROGRESS_BAR_HEIGHT);

    /* "Backing up your data" does not fit at full size; 14 px of air above the
     * bar is what keeps the label off it when the ladder drops a rung. */
    label_y = bar_top - 14 - step_h;
    p->label_box = ND_RECT(0, label_y, width, label_y + step_h);

    status_y = p->bar_box.y1 + 9;
    p->status_box = ND_RECT(ND_PROGRESS_BAR_MARGIN, status_y, width - ND_PROGRESS_BAR_MARGIN,
                            status_y + small_h);

    hint_y = bottom - small_h - 6;
    p->hint_box = ND_RECT(0, hint_y, width, hint_y + small_h);
}

void nd_progress_set_step(nd_progress *p, const char *step)
{
    if (p == NULL)
        return;
    p->step = (step != NULL) ? step : "";
    p->percent = -1;
}

/* _centered(text, font, box) -> (max(0, (box[0] + box[2] - text_w) // 2), box[1]) */
static int32_t centered_x(const nd_font *f, const char *text, nd_rect box)
{
    int32_t w = 0;

    nd_text_size(f, text, &w, NULL);
    return nd_max32(0, floordiv2(box.x0 + box.x1 - w));
}

bool nd_progress_draw(nd_progress *p, int64_t done, int64_t total)
{
    nd_ui *ui;
    nd_draw *d;
    int32_t width;
    int32_t bottom;
    int32_t percent;
    int32_t span;
    int32_t filled;
    const nd_font *font_small;
    const nd_font *ladder[3];
    size_t n_ladder;
    char reading[16];
    char detail_text[128];
    nd_softkey bar;

    if (p == NULL || p->ui == NULL || p->ui->draw == NULL)
        return false;

    /* int(done * 100 / total) -- Python's true division then int(), which
     * truncates toward zero. Doubles reproduce it exactly: Python's / on ints
     * is the same IEEE double. total == 0 means "done", not "divide by zero". */
    percent = (total != 0) ? nd_trunc32((double)done * 100.0 / (double)total) : 100;
    percent = nd_clamp32(percent, 0, 100);
    if (percent == p->percent)
        return false;
    p->percent = percent;

    ui = p->ui;
    d = ui->draw;
    width = nd_ui_width(ui);
    bottom = nd_ui_content_bottom(ui);
    font_small = small_font_of(ui);

    (void)nd_draw_rect_fill(d, ND_RECT(0, 0, width, bottom), ND_BLACK);

    if (p->header != NULL && p->header[0] != '\0') {
        (void)nd_draw_text(d, 10, p->header_box.y0, p->header, font_small, ND_WHITE);
        (void)nd_draw_line(d, 10, p->divider_y, width - 10, p->divider_y, ND_WHITE, 1);
    }

    if (p->step != NULL && p->step[0] != '\0') {
        int32_t room = width - 16;
        const nd_font *font;
        char label[ND_TEXT_LINE_MAX];

        n_ladder = nd_font_ladder(ui, ladder, ND_ARRAY_LEN(ladder));
        if (n_ladder == 0u) {
            ladder[0] = step_font_of(ui);
            n_ladder = (ladder[0] != NULL) ? 1u : 0u;
        }
        font = (n_ladder > 0u) ? nd_fit_font(p->step, room, ladder, n_ladder) : NULL;
        if (font != NULL) {
            (void)nd_text_ellipsize(label, sizeof label, p->step, font, room);
            (void)nd_draw_text(d, centered_x(font, label, p->label_box), p->label_box.y0, label,
                               font, ND_WHITE);
        }
    }

    /* The bar. width=1 outline drawn INSIDE the inclusive box, so the frame
     * occupies rows 79 and 93 and columns 20 and 220. */
    (void)nd_draw_rect_outline(d, p->bar_box, ND_WHITE, 1);
    span = (p->bar_box.x1 - ND_PROGRESS_INSET) - (p->bar_box.x0 + ND_PROGRESS_INSET);
    filled = nd_trunc32((double)span * (double)percent / 100.0);
    if (filled > 0) {
        (void)nd_draw_rect_fill(
            d,
            ND_RECT(p->bar_box.x0 + ND_PROGRESS_INSET, p->bar_box.y0 + ND_PROGRESS_INSET,
                    p->bar_box.x0 + ND_PROGRESS_INSET + filled, p->bar_box.y1 - ND_PROGRESS_INSET),
            ND_WHITE);
    }

    (void)snprintf(reading, sizeof reading, "%d%%", (int)percent);
    detail_text[0] = '\0';
    if (p->detail != NULL)
        p->detail(p->detail_ctx, done, total, detail_text, sizeof detail_text);

    if (detail_text[0] != '\0') {
        int32_t detail_w = 0;

        (void)nd_draw_text(d, p->status_box.x0, p->status_box.y0, reading, font_small, ND_WHITE);
        nd_text_size(font_small, detail_text, &detail_w, NULL);
        (void)nd_draw_text(d, p->status_box.x1 - detail_w, p->status_box.y0, detail_text,
                           font_small, ND_WHITE);
    } else {
        (void)nd_draw_text(d, centered_x(font_small, reading, p->status_box), p->status_box.y0,
                           reading, font_small, ND_WHITE);
    }

    if (p->hint != NULL && p->hint[0] != '\0') {
        char hint[ND_TEXT_LINE_MAX];

        (void)nd_text_ellipsize(hint, sizeof hint, p->hint, font_small, width - 16);
        (void)nd_draw_text(d, centered_x(font_small, hint, p->hint_box), p->hint_box.y0, hint,
                           font_small, ND_WHITE);
    }

    /* update("") clears the strip and draws nothing -- deliberate, and the
     * reason nd_softkey_update() must not treat "" as an error. */
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "", false);
    (void)nd_ui_present(ui);
    return true;
}

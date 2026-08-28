/* nd_vlist.c -- VerticalList, the ordinary three-row menu, and LevelSelector,
 * which is a VerticalList of "Level 1".."Level N" with an OK softkey.
 *
 * Thirteen apps use VerticalList. It is the single most-drawn screen in the
 * OS after the home screen.
 *
 * The two live in one file because in Python LevelSelector subclasses
 * VerticalList, and spec-ui-framework.md is explicit that they must not be
 * split: they share the item window, the notch arithmetic and the key loop,
 * and separating them would mean exposing all three only to hand them back.
 *
 * ============ THE CLEAR IS 0..145, NOT 0..175 ============
 *
 * draw() clears rows 0 to content_bottom inclusive and leaves the softkey
 * strip alone. That is not an oversight to tidy up: it is the whole reason
 * callers can write
 *
 *     nd_softkey_update(&bar, "Select", false);   // paint, do not present
 *     nd_vlist_show(&list);                       // its draw presents both
 *
 * and get one frame instead of two. LevelSelector's show() is built on the
 * same guarantee.
 *
 * ============ FOUR NUMBERS THAT DECIDE THE PIXELS ============
 *
 *   line_height = max(28, content_height // 3) = 33   rows at 40, 73, 106
 *   item_height = max(24, line_height - 4)     = 29   the white bar's height
 *   selected_right = max(20, bar_x - 10)       = 225  the bar stops short of
 *                                                     the scrollbar
 *   the scrollbar track is GREY (128,128,128) and width 1 -- the only grey
 *   pixels in the entire framework. Every other track is white and width 2.
 *
 * The notch position is a FLOAT that Pillow truncates. Rounding it instead
 * moves the notch a pixel on most list lengths.
 *
 * ============ HOLD-TO-REPEAT ============
 *
 * Holding Up or Down scrolls. It is not implemented here and it must not be:
 * nd_input synthesises repeat presses for codes 103/105/106/108 from its own
 * held state (400 ms, then every 120 ms -- see nd_keypad.h), so a held arrow
 * arrives at this loop as an ordinary stream of presses and scrolls the list
 * exactly as tapping would. What this module has to get right is only that it
 * does not filter them and does not coalesce them: one press, one row, one
 * redraw. A widget that tried to derive repeat itself would fight the layer
 * that already does it and would behave differently on the i2c keypad from
 * under QEMU, which is precisely what putting it in nd_input avoids.
 *
 * No golden frame changes: repeat produces the same frames a person tapping
 * produces, just sooner.
 */

#include <stdio.h>
#include <string.h>

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_keycodes.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

/* ================================================================== *
 * VerticalList
 * ================================================================== */

void nd_vlist_init(nd_vlist *l, nd_ui *ui, const char *title, const char *const *items,
                   size_t n_items, int32_t app_id)
{
    if (l == NULL)
        return;

    l->ui = ui;
    nd_header_init_int(&l->header, ui, app_id);
    l->title = title;
    l->items = items;
    l->n_items = (items != NULL) ? n_items : 0u;
    l->selected_index = 0u;
    l->window_start = 0u;
    /* 3 until draw() recomputes it from the panel height. Python sets the
     * same starting value in __init__ and show() reads it before the first
     * draw only if a caller drives handle_key() by hand. */
    l->max_lines = 3u;
}

void nd_vlist_draw(nd_vlist *l)
{
    nd_ui *ui;
    nd_draw *d;
    int32_t screen_w;
    int32_t content_bottom;
    int32_t header_y;
    int32_t reserved;
    int32_t y_start;
    int32_t content_height;
    int32_t line_height;
    int32_t item_height;
    int32_t bar_x;
    int32_t selected_right;
    int32_t track_top;
    int32_t track_bottom;
    const nd_font *item_font;
    char title[ND_TEXT_LINE_MAX];
    size_t max_start;
    size_t i;
    double notch_y;

    if (l == NULL || l->ui == NULL || l->ui->draw == NULL)
        return;

    ui = l->ui;
    d = ui->draw;
    screen_w = nd_ui_width(ui);
    content_bottom = nd_ui_content_bottom(ui);
    header_y = nd_ui_header_divider_y(ui);

    /* 1. Clear -- rows 0..content_bottom only. See the header comment. */
    nd_ui_paint_chrome_content(ui);

    /* 2. Title, trimmed so it cannot run under the right-aligned breadcrumb,
     *    and drawn at y = 0 -- every other widget's title sits at y = 5. */
    reserved = nd_header_width(&l->header, (int32_t)l->selected_index + 1);
    (void)nd_text_fit(title, sizeof title, l->title, ui->font_xl, screen_w - 5 - reserved - 6);
    (void)nd_draw_text(d, 5, 0, title, ui->font_xl, ND_WHITE);
    nd_header_draw(&l->header, (int32_t)l->selected_index + 1);

    /* 3. Divider. */
    (void)nd_draw_line(d, 0, header_y, screen_w, header_y, ND_WHITE, 1);

    /* 4. Row metrics, recomputed every frame exactly as the Python does. */
    y_start = header_y + 10;
    content_height = nd_max32(1, content_bottom - y_start - 4);
    line_height = nd_max32(28, content_height / 3);
    item_height = nd_max32(24, line_height - 4);
    l->max_lines = (size_t)nd_min32(3, nd_max32(1, content_height / line_height));
    item_font = (ui->font_md != NULL) ? ui->font_md : ui->font_n;

    if (l->selected_index < l->window_start)
        l->window_start = l->selected_index;
    /* And the same the other way. The key loop below keeps this invariant
     * itself, so this only ever fires on the first draw after a CALLER set
     * selected_index by hand -- Settings and Sleepy both open a list on the
     * value already in force. Without it a preselected row past the first
     * windowful is simply not in the window, and the frame comes back with no
     * selection bar at all: a menu that looks like it has lost its place. */
    else if (l->selected_index >= l->window_start + l->max_lines)
        l->window_start = l->selected_index - l->max_lines + 1u;
    max_start = (l->n_items > l->max_lines) ? (l->n_items - l->max_lines) : 0u;
    if (l->window_start > max_start)
        l->window_start = max_start;

    bar_x = screen_w - 5;
    selected_right = nd_max32(20, bar_x - 10);

    for (i = 0u; i < l->max_lines; i++) {
        size_t item_idx = l->window_start + i;
        const char *item_text;
        int32_t y;
        int32_t text_h = 0;
        int32_t text_y;

        if (item_idx >= l->n_items)
            break;

        y = y_start + (int32_t)i * line_height;
        item_text = (l->items[item_idx] != NULL) ? l->items[item_idx] : "";
        /* The INK height of this particular string, so a row of "Erase" and a
         * row of "Send entry" do not sit at the same y. That is what the
         * screens look like today. */
        nd_text_size(item_font, item_text, NULL, &text_h);
        text_y = y + nd_max32(0, (item_height - text_h) / 2);

        if (item_idx == l->selected_index) {
            (void)nd_draw_rect_fill(d, ND_RECT(0, y, selected_right, y + item_height), ND_WHITE);
            (void)nd_draw_text(d, 10, text_y, item_text, item_font, ND_BLACK);
        } else {
            (void)nd_draw_text(d, 10, text_y, item_text, item_font, ND_WHITE);
        }
    }

    /* 5. Scrollbar: grey, width 1. The only grey in the framework. */
    track_top = y_start;
    track_bottom = nd_max32(track_top, content_bottom - 5);
    (void)nd_draw_line(d, bar_x, track_top, bar_x, track_bottom, ND_GRAY, 1);

    if (l->n_items > 1u) {
        double step = (double)(track_bottom - track_top) / (double)(l->n_items - 1u);
        notch_y = (double)track_top + ((double)l->selected_index * step);
    } else {
        notch_y = (double)track_top;
    }
    (void)nd_draw_rect_fill(
        d, ND_RECT(bar_x - 2, nd_trunc32(notch_y - 3.0), bar_x + 2, nd_trunc32(notch_y + 3.0)),
        ND_WHITE);

    (void)nd_ui_present(ui);
}

int32_t nd_vlist_handle_key(nd_vlist *l, int32_t key)
{
    if (l == NULL)
        return ND_WIDGET_BACK;

    if (key == ND_KEY_DOWN) {
        if (l->n_items > 0u && l->selected_index < l->n_items - 1u) {
            l->selected_index++;
            if (l->selected_index >= l->window_start + l->max_lines)
                l->window_start++;
        }
        return ND_VLIST_CONTINUE;
    }
    if (key == ND_KEY_UP) {
        if (l->selected_index > 0u) {
            l->selected_index--;
            if (l->selected_index < l->window_start)
                l->window_start--;
        }
        return ND_VLIST_CONTINUE;
    }
    /* Digits 1..9 only -- codes 2..10. Code 11 is '0' and is NOT a shortcut.
     * A shortcut past the end of the list is ignored WITHOUT a redraw, which
     * is why this cannot be folded into the arrow branches. */
    if (key >= ND_KEY_1 && key <= ND_KEY_9) {
        size_t idx = (size_t)(key - ND_KEY_1);

        if (idx < l->n_items)
            return (int32_t)idx;
        return ND_VLIST_CONTINUE;
    }
    if (key == ND_KEY_ENTER)
        return (int32_t)l->selected_index;
    if (key == ND_KEY_CLEAR)
        return ND_WIDGET_BACK;

    return ND_VLIST_CONTINUE;
}

/* See nd_ui_set_repaint(). */
static void vlist_repaint(void *ctx)
{
    nd_vlist_draw((nd_vlist *)ctx);
}

int32_t nd_vlist_show(nd_vlist *l)
{
    nd_ui_repaint saved;
    int32_t out;

    if (l == NULL)
        return ND_WIDGET_BACK;

    nd_vlist_draw(l);
    saved = nd_ui_set_repaint(l->ui, vlist_repaint, l);

    for (;;) {
        int32_t key = nd_ui_wait_for_key(l->ui);
        int32_t r = nd_vlist_handle_key(l, key);

        if (r != ND_VLIST_CONTINUE) {
            out = r;
            break;
        }

        /* Python redraws on Up and Down UNCONDITIONALLY -- including at the
         * ends of the list, where nothing moved. Keep it: the redraw is what
         * repaints over a caller's transient overlay, and holding an arrow at
         * the bottom of a list is visibly a steady screen rather than a dead
         * one. Every other ignored key falls through with no redraw. */
        if (key == ND_KEY_UP || key == ND_KEY_DOWN)
            nd_vlist_draw(l);
    }

    nd_ui_restore_repaint(l->ui, saved);
    return out;
}

/* ================================================================== *
 * LevelSelector
 * ================================================================== */

void nd_levelsel_init(nd_levelsel *s, nd_ui *ui, int32_t current, int32_t count, const char *title,
                      int32_t app_id)
{
    int32_t n;
    int32_t i;
    int32_t sel;

    if (s == NULL)
        return;

    /* Python builds range(1, count + 1) with no ceiling; nine is every caller
     * the tree has and CODING-STANDARDS.md section 1.5 will not have an array
     * sized by an argument. A larger count is clamped, not refused. */
    n = nd_clamp32(count, 0, ND_LEVELSEL_MAX);
    s->count = n;

    for (i = 0; i < n; i++) {
        /* i + 1 is 1..ND_LEVELSEL_MAX by construction; the clamp is spelled
         * out so the compiler can see it too -- without it -Wformat-truncation
         * assumes eleven digits and refuses the 16-byte label. */
        (void)snprintf(s->labels[i], sizeof s->labels[i], "Level %d",
                       (int)nd_clamp32(i + 1, 1, ND_LEVELSEL_MAX));
        s->label_ptrs[i] = s->labels[i];
    }
    for (; i < ND_LEVELSEL_MAX; i++) {
        s->labels[i][0] = '\0';
        s->label_ptrs[i] = s->labels[i];
    }

    nd_vlist_init(&s->list, ui, (title != NULL) ? title : "Level", s->label_ptrs, (size_t)n,
                  app_id);

    /* max(0, min(count - 1, current - 1)) -- against the CALLER's count, not
     * the clamped one, so a nonsense count still lands on row 0. */
    sel = nd_max32(0, nd_min32(count - 1, current - 1));
    if (n > 0)
        sel = nd_min32(sel, n - 1);
    else
        sel = 0;
    s->list.selected_index = (size_t)sel;
}

int32_t nd_levelsel_show(nd_levelsel *s)
{
    nd_softkey bar;
    int32_t choice;

    if (s == NULL)
        return ND_WIDGET_BACK;

    /* Painted, NOT presented. The list's draw() clears only rows 0..145, so
     * this "OK" survives and is pushed to the panel as part of the list's own
     * frame -- one repaint, not two. */
    nd_softkey_init(&bar, s->list.ui, false);
    nd_softkey_update(&bar, "OK", false);

    choice = nd_vlist_show(&s->list);
    if (choice < 0)
        return ND_WIDGET_BACK; /* Python returns None */
    return choice + 1;         /* levels are 1-based */
}

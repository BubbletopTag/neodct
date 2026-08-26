/* nd_appsel.c -- AppSelector, the Nokia-style main menu: one big icon at a
 * time, the app's name above it, a page number and a scrollbar down the right
 * edge.
 *
 * This is the screen the phone is judged by. Nine golden frames cover it --
 * one per menu entry that shoot_docs.py visits, plus the 240x240 panel shot --
 * so every number below is measured against a reference, not chosen.
 *
 * ============ THE NUMBERS, WORKED OUT FOR THIS PANEL ============
 *
 *   header_y      = max(30, H*0.11)                      = 30
 *   title_y       = header_y - 16                        = 14   (negative-ish
 *                                                          on purpose: the ink
 *                                                          starts 3 px lower)
 *   icon_y        = header_y + max(24, (145-30)*0.22)    = 55
 *   icon_cap      = min(175, max(24, 145 - 55 - 8))      = 82
 *   bar_x         = W - 8                                = 232
 *   track_top     = header_y + 6                         = 36
 *   track_bottom  = max(track_top, 145 - 10)             = 135
 *
 * 0.22 * 115 is 25.299999999999997 in IEEE754 and int() takes 25. Computing it
 * as 115 * 22 / 100 would also give 25, but only by luck on this panel; the
 * double is what the Python evaluates, so it is what is evaluated here.
 *
 * ============ THREE THINGS THAT DECIDE THE PIXELS ============
 *
 * 1. The track is drawn with width 2, and nd_draw.h RULE 2 says a wide line
 *    grows in the MINOR axis only -- so a vertical track at x=232 lights
 *    columns 232 and 233 and stops at row 135 exactly.
 *
 * 2. The notch is a FLOAT. step = (135-36)/(n-1) is 99/23 = 4.3043... for the
 *    24 shipped apps, and Pillow truncates the corners of the rectangle it is
 *    handed. Rounding instead moves the notch a pixel on most indices.
 *
 * 3. The icon is fetched with max_size=82, so the cache holds an 82x82
 *    thumbnail rather than the 120x120 original. That is not only a memory
 *    decision: the thumbnail's dimensions are what centres it, and asking for
 *    the full-size art and scaling per frame would land it elsewhere.
 *
 * ============ WHAT IS DELIBERATELY MISSING ============
 *
 * `title` is stored and never drawn. The header line shows the selected app's
 * name instead. It is kept so a reader of both sources sees the same
 * constructor. nd_widgets.h says so too.
 *
 * There is no HeaderWidget here and no breadcrumb: the page number in the top
 * right is drawn by hand, at (W - 5 - w, 10), with font_n.
 */

#include <stdio.h>
#include <string.h>

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_input.h"
#include "nd_keycodes.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

/* Python's `//` floors; C's `/` truncates toward zero. They differ for a
 * negative numerator, which happens the moment a name is wider than the
 * screen -- "Remote Shell" at 24 px is 138 px, so nothing shipped reaches it,
 * but a manifest is user-supplied data and the centring must not shift by a
 * pixel depending on the sign. */
static int32_t floordiv2(int32_t v)
{
    return (v >= 0) ? (v / 2) : -(((-v) + 1) / 2);
}

void nd_appsel_init(nd_appsel *s, nd_ui *ui, const char *title, const nd_app_entry *items,
                    size_t n_items, const nd_image *background)
{
    if (s == NULL)
        return;

    s->ui = ui;
    s->title = title;
    s->items = items;
    s->n_items = (items != NULL) ? n_items : 0u;
    s->background = background;
    s->selected_index = 0u;
}

void nd_appsel_draw(nd_appsel *s)
{
    nd_ui *ui;
    nd_draw *d;
    int32_t screen_w;
    int32_t screen_h;
    int32_t softkey_h;
    int32_t content_bottom;
    int32_t header_y;
    int32_t icon_y;
    int32_t icon_cap;
    int32_t bar_x;
    int32_t track_top;
    int32_t track_bottom;
    int32_t w = 0;
    int32_t h = 0;
    const nd_app_entry *current;
    const char *icon_path;
    double notch_y;
    char page_num[16];

    if (s == NULL || s->ui == NULL || s->ui->draw == NULL)
        return;

    ui = s->ui;
    d = ui->draw;
    screen_w = nd_ui_width(ui);
    screen_h = nd_ui_height(ui);
    softkey_h = nd_ui_softkey_height(ui);
    content_bottom = nd_ui_content_bottom(ui);
    header_y = nd_ui_header_divider_y(ui);

    /* 1. Background. The wallpaper is pasted WITHOUT a mask -- it is opaque
     *    RGB and the Python passes no third argument. This is also the only
     *    full-height clear in the widget set that is not a rectangle: when
     *    there is no wallpaper the fill runs 0..H inclusive, i.e. one row
     *    past the bottom, and is clipped. */
    if (s->background != NULL) {
        (void)nd_image_blit(ui->canvas, s->background, 0, 0);
    } else {
        (void)nd_draw_rect_fill(d, ND_RECT(0, 0, screen_w, screen_h), ND_BLACK);
    }

    /* An empty list is a real state: the scan can fail, and every later step
     * would divide by zero or index past the end. */
    if (s->n_items == 0u) {
        int32_t y;

        nd_ui_text_size(ui, "No Apps", ui->font_n, &w, &h);
        y = nd_max32(header_y, header_y + ((content_bottom - header_y - h) / 2));
        (void)nd_draw_text(d, floordiv2(screen_w - w), y, "No Apps", ui->font_n, ND_WHITE);
        (void)nd_ui_present(ui);
        return;
    }

    if (s->selected_index >= s->n_items)
        s->selected_index = 0u;
    current = &s->items[s->selected_index];

    /* 2. The app's name, centred, at 24 px. */
    nd_ui_text_size(ui, current->name, ui->font_xl, &w, &h);
    (void)nd_draw_text(d, floordiv2(screen_w - w), header_y - 16, current->name, ui->font_xl,
                       ND_WHITE);

    /* 3. The icon, centred horizontally at a fixed y. */
    icon_y = header_y + nd_max32(24, nd_trunc32((double)(content_bottom - header_y) * 0.22));
    icon_path = current->icon;
    if (icon_path != NULL && icon_path[0] != '\0') {
        const nd_image *img;

        icon_cap = nd_min32(ND_APP_SELECTOR_ICON_MAX, nd_max32(24, content_bottom - icon_y - 8));
        img = nd_ui_get_image_max(ui, icon_path, icon_cap);
        if (img != NULL) {
            /* paste(img, (ix, iy), img): composited through the icon's own
             * alpha, so a transparent corner shows the wallpaper rather than
             * punching a black square into it. */
            (void)nd_image_blit_alpha(ui->canvas, img, floordiv2(screen_w - img->w), icon_y);
        } else {
            int32_t px = floordiv2(screen_w - icon_cap);
            int32_t qw = 0;
            int32_t qh = 0;

            (void)nd_draw_rect_outline(d, ND_RECT(px, icon_y, px + icon_cap, icon_y + icon_cap),
                                       ND_WHITE, 1);
            nd_ui_text_size(ui, "?", ui->font_xl, &qw, &qh);
            (void)nd_draw_text(d, px + ((icon_cap - qw) / 2), icon_y + ((icon_cap - qh) / 2), "?",
                               ui->font_xl, ND_WHITE);
        }
    }

    /* 4. "Select" sits INSIDE the softkey strip, vertically centred on the
     *    string's own ink height -- the core's transparent bar has already
     *    been overwritten by the background paste above, so this is the only
     *    thing in those 30 rows. */
    nd_ui_text_size(ui, "Select", ui->font_n, &w, &h);
    (void)nd_draw_text(d, floordiv2(screen_w - w),
                       content_bottom + nd_max32(0, (softkey_h - h) / 2), "Select", ui->font_n,
                       ND_WHITE);

    /* 5. The scrollbar. */
    bar_x = screen_w - 8;
    track_top = header_y + 6;
    track_bottom = nd_max32(track_top, content_bottom - 10);
    (void)nd_draw_line(d, bar_x, track_top, bar_x, track_bottom, ND_WHITE, 2);

    if (s->n_items > 1u) {
        double step = (double)(track_bottom - track_top) / (double)(s->n_items - 1u);

        notch_y = (double)track_top + ((double)s->selected_index * step);
    } else {
        notch_y = (double)track_top;
    }
    (void)nd_draw_rect_fill(
        d, ND_RECT(bar_x - 4, nd_trunc32(notch_y - 3.0), bar_x + 2, nd_trunc32(notch_y + 3.0)),
        ND_WHITE);

    /* 6. The page number, right-aligned 5 px in from the edge. */
    (void)snprintf(page_num, sizeof page_num, "%zu", s->selected_index + 1u);
    nd_ui_text_size(ui, page_num, ui->font_n, &w, &h);
    (void)nd_draw_text(d, screen_w - 5 - w, 10, page_num, ui->font_n, ND_WHITE);

    (void)nd_ui_present(ui);
}

int32_t nd_appsel_show(nd_appsel *s)
{
    if (s == NULL)
        return ND_WIDGET_BACK;

    /* Input flush, so a key still in flight from the home screen does not
     * page the menu the instant it appears. nd_input.h pins the timeout: this
     * widget and PagedList poll with 0.01 s, MessageDialog with 0.0, and the
     * two must stay apart. The guard bounds a channel that never goes quiet;
     * the Python has no such bound and would spin forever. */
    if (s->ui != NULL && s->ui->input != NULL) {
        nd_key_event ev;
        int guard;

        for (guard = 0; guard < 256; guard++) {
            if (!nd_input_read_event(s->ui->input, 0.01, &ev))
                break;
        }
    }

    nd_appsel_draw(s);

    for (;;) {
        int32_t key = nd_ui_wait_for_key(s->ui);

        /* Checked before anything else: with no apps, Down would take a
         * modulo by zero and Enter would index past the end. Only the two
         * ways out respond, and both mean "back". */
        if (s->n_items == 0u) {
            if (key == ND_KEY_CLEAR || key == ND_KEY_ENTER)
                return ND_WIDGET_BACK;
            continue;
        }

        if (key == ND_KEY_DOWN) {
            s->selected_index = (s->selected_index + 1u) % s->n_items;
            nd_appsel_draw(s);
        } else if (key == ND_KEY_UP) {
            /* Python's `(i - 1) % n` on a negative gives n-1; size_t would
             * wrap to SIZE_MAX first, so the addition is not optional. */
            s->selected_index = (s->selected_index + s->n_items - 1u) % s->n_items;
            nd_appsel_draw(s);
        } else if (key == ND_KEY_ENTER) {
            return (int32_t)s->selected_index;
        } else if (key == ND_KEY_CLEAR) {
            return ND_WIDGET_BACK;
        }
        /* Every other key is ignored WITHOUT a redraw -- the legacy 50 and 46
         * aliases were removed from the Python and are not reinstated here. */
    }
}

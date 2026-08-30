/* month.c -- the month grid: the only screen in this app that is not built
 * out of nd_widgets.h, and the reason the app exists.
 *
 * calendar_app.h has the key map and why it is what it is. This file is the
 * two halves that map needs: laying six rows of seven out as data, and
 * painting them.
 *
 * ============ IT FOLLOWS THE FRAMEWORK'S RULES, NOT ITS WIDGETS ============
 *
 * There is no grid widget and there should not be one -- nothing else in the
 * OS wants a grid. But every rule the widgets follow applies here anyway, and
 * getting one of them wrong is what would make this screen look like it came
 * from somewhere else:
 *
 *   * the clear is rows 0..content_bottom, so a caller's softkey survives
 *     into this frame (nd_widgets.h rule 1);
 *   * the title sits at y = 0 in font_xl and is trimmed with nd_text_fit()
 *     against the breadcrumb's reserved width, exactly as nd_vlist_draw()
 *     does it;
 *   * the divider is one white pixel row at nd_ui_header_divider_y();
 *   * text is positioned by its INK extents, so a row of "17" and a row of
 *     "8" do not sit on the same pixel (nd_widgets.h rule 2);
 *   * the selected cell is white with black text, which is what a selected
 *     row is everywhere else in this OS.
 *
 * ============ THE GEOMETRY IS DERIVED, NOT WRITTEN DOWN ============
 *
 * On the 240x175 panel it comes out as a 34x16 cell with the grid starting at
 * y = 48 and ending at 143, two rows short of the softkey. Those numbers are
 * computed from the panel every frame rather than baked in, for the reason
 * nd_ui.h gives about nd_ui_header_divider_y(): anyone hard-coding 30 breaks
 * a future panel, and anyone recomputing it wrong breaks today's.
 *
 * ============ WHY A DAY WITH SOMETHING ON IT GETS A BAR ============
 *
 * A 16 px cell holds a two-digit number and about three pixels underneath it.
 * A dot in three pixels reads as a dead pixel at this size; a 10x2 bar reads
 * as a mark. It is drawn in the cell's own foreground colour, so it inverts
 * with the selection instead of disappearing into it.
 */

#include <string.h>

#include "calendar_app.h"

#include "nd_app.h"
#include "nd_calendar.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_keycodes.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

/* ------------------------------------------------------------------ *
 * The grid as data
 * ------------------------------------------------------------------ */

int32_t nd_cal_grid_fill(int32_t year, int32_t month, int32_t day, nd_cal_cell *out)
{
    int32_t lead;
    int32_t i;
    int32_t found = -1;
    int32_t cy = year;
    int32_t cm = month;
    int32_t cd = 1;

    if (out == NULL || month < 1 || month > 12)
        return -1;

    /* Back up to the Monday on or before the 1st. nd_cal_weekday() is
     * Monday-based, so the number of leading days IS the 1st's weekday. */
    lead = nd_cal_weekday(year, month, 1);
    nd_cal_step_days(&cy, &cm, &cd, -lead);

    for (i = 0; i < ND_CAL_GRID_CELLS; i++) {
        out[i].year = cy;
        out[i].month = cm;
        out[i].day = cd;
        out[i].in_month = (cy == year && cm == month);
        if (out[i].in_month && cd == day)
            found = i;
        nd_cal_step_days(&cy, &cm, &cd, 1);
    }
    return found;
}

/* ------------------------------------------------------------------ *
 * The key map
 * ------------------------------------------------------------------ */

nd_cal_nav nd_cal_month_key(int32_t key, int32_t *year, int32_t *month, int32_t *day)
{
    if (year == NULL || month == NULL || day == NULL)
        return ND_CAL_NAV_NONE;

    switch (key) {
    case ND_KEY_ENTER:
        return ND_CAL_NAV_OPEN;
    case ND_KEY_CLEAR:
        return ND_CAL_NAV_BACK;

    /* The rocker and the 3x3 block agree; Left and Right exist only on a
     * development keyboard and are folded into 4 and 6 rather than given a
     * meaning the phone could not reach. */
    case ND_KEY_UP:
    case ND_KEY_2:
        nd_cal_step_days(year, month, day, -7);
        return ND_CAL_NAV_MOVED;
    case ND_KEY_DOWN:
    case ND_KEY_8:
        nd_cal_step_days(year, month, day, 7);
        return ND_CAL_NAV_MOVED;
    case ND_KEY_LEFT:
    case ND_KEY_4:
        nd_cal_step_days(year, month, day, -1);
        return ND_CAL_NAV_MOVED;
    case ND_KEY_RIGHT:
    case ND_KEY_6:
        nd_cal_step_days(year, month, day, 1);
        return ND_CAL_NAV_MOVED;

    case ND_KEY_1:
    case ND_KEY_STAR:
        nd_cal_step_months(year, month, day, -1);
        return ND_CAL_NAV_MOVED;
    case ND_KEY_3:
    case ND_KEY_HASH:
        nd_cal_step_months(year, month, day, 1);
        return ND_CAL_NAV_MOVED;

    case ND_KEY_7:
        nd_cal_step_months(year, month, day, -12);
        return ND_CAL_NAV_MOVED;
    case ND_KEY_9:
        nd_cal_step_months(year, month, day, 12);
        return ND_CAL_NAV_MOVED;

    case ND_KEY_5:
        /* The middle of the block is where you were: today. A calendar you
         * have paged three years into needs one key back. */
        nd_cal_split((time_t)nd_time_now(), year, month, day, NULL, NULL);
        return ND_CAL_NAV_MOVED;

    default:
        return ND_CAL_NAV_NONE;
    }
}

/* ------------------------------------------------------------------ *
 * Painting
 * ------------------------------------------------------------------ */

/* Everything the frame needs, computed once. Derived from the panel; see the
 * file header for why none of it is a constant. */
typedef struct {
    int32_t screen_w;
    int32_t content_bottom;
    int32_t header_y;
    int32_t weekday_y;
    int32_t grid_x;
    int32_t grid_y;
    int32_t cell_w;
    int32_t cell_h;
} grid_metrics;

static void measure(const nd_ui *ui, grid_metrics *g)
{
    g->screen_w = nd_ui_width(ui);
    g->content_bottom = nd_ui_content_bottom(ui);
    g->header_y = nd_ui_header_divider_y(ui);

    /* Four pixels under the divider for the weekday initials, then the grid
     * fourteen further down -- which is the 14 px face's own line, so the two
     * rows read as a header and a body rather than as one crowded block. */
    g->weekday_y = g->header_y + 4;
    g->grid_y = g->header_y + 18;

    g->cell_w = nd_max32(1, g->screen_w / ND_CAL_GRID_COLS);
    g->cell_h = nd_max32(8, (g->content_bottom - g->grid_y) / ND_CAL_GRID_ROWS);
    /* The 240 px panel divides into seven columns with two pixels left over;
     * they go one to each edge rather than all to the right, so the grid is
     * centred and the last column is not against the bezel. */
    g->grid_x = (g->screen_w - g->cell_w * ND_CAL_GRID_COLS) / 2;
}

/* Centred by INK width within the column, which is why "11" and "1" are both
 * centred rather than both starting at the same x. */
static void draw_centred(nd_draw *d, const nd_font *f, const char *text, int32_t x0, int32_t box_w,
                         int32_t y, nd_color c)
{
    int32_t tw = 0;

    nd_text_size(f, text, &tw, NULL);
    (void)nd_draw_text(d, x0 + nd_max32(0, (box_w - tw) / 2), y, text, f, c);
}

void nd_cal_month_draw(nd_ui *ui, int32_t year, int32_t month, int32_t day, uint32_t mask)
{
    nd_cal_cell cells[ND_CAL_GRID_CELLS];
    grid_metrics g;
    nd_header header;
    nd_draw *d;
    const nd_font *small;
    char title[ND_TEXT_LINE_MAX];
    char label[8];
    int32_t reserved;
    int32_t selected;
    int32_t today_y = 0;
    int32_t today_m = 0;
    int32_t today_d = 0;
    int32_t i;

    if (ui == NULL || ui->draw == NULL || ui->canvas == NULL)
        return;
    if (month < 1 || month > 12)
        return;

    d = ui->draw;
    small = (ui->font_s != NULL) ? ui->font_s : ui->font_n;
    if (small == NULL)
        return;

    measure(ui, &g);
    selected = nd_cal_grid_fill(year, month, day, cells);
    nd_cal_split((time_t)nd_time_now(), &today_y, &today_m, &today_d, NULL, NULL);

    /* 1. Clear rows 0..content_bottom only -- the softkey strip is the
     *    caller's and must survive. This is the wallpapered background the
     *    rest of the framework draws, not a black fill: it falls back to
     *    black on its own when there is no wallpaper, when the feature is
     *    off, or when a manifest says useWallpaper false. */
    nd_ui_paint_chrome_content(ui);

    /* 2. "August 2026", and the breadcrumb itself. Sub-index -1 is the
     *    widgets' spelling of Python's None: this screen is not a list and
     *    has no row number.
     *
     *    ============ THE TITLE STEPS DOWN A SIZE, IT DOES NOT TRUNCATE ====
     *
     *    Every list title in this OS is drawn at 24 px and shortened with
     *    "..." when it will not fit. That is right for a name, which is still
     *    recognisable half-drawn -- and wrong for this, because there are only
     *    twelve possible titles and four of them do not fit. "September 2..."
     *    is not a month.
     *
     *    So it uses the framework's OTHER answer to the same question,
     *    _fit_font: the first size in a ladder whose ink fits, and the
     *    smallest one if none does. Eight months keep the 24 px title every
     *    other screen has; September, November and December drop to 18 and are
     *    fully readable. Nothing is ever cut. */
    nd_header_init(&header, ui, ND_CAL_APP_ROOT);
    reserved = nd_header_width(&header, -1);
    (void)nd_snprintf(title, sizeof title, "%s %d", nd_cal_month_names[month - 1], (int)year);
    {
        const nd_font *ladder[3];
        size_t n_ladder = 0u;

        if (ui->font_xl != NULL)
            ladder[n_ladder++] = ui->font_xl;
        if (ui->font_n != NULL)
            ladder[n_ladder++] = ui->font_n;
        if (ui->font_md != NULL)
            ladder[n_ladder++] = ui->font_md;
        if (n_ladder == 0u)
            ladder[n_ladder++] = small;

        (void)nd_draw_text(d, 5, 0, title,
                           nd_fit_font(title, g.screen_w - 5 - reserved - 6, ladder, n_ladder),
                           ND_WHITE);
    }
    nd_header_draw(&header, -1);

    /* 3. Divider. */
    (void)nd_draw_line(d, 0, g.header_y, g.screen_w, g.header_y, ND_WHITE, 1);

    /* 4. M T W T F S S, in grey, because they are a label for the grid and
     *    not part of it -- the same grey the one scrollbar track in the
     *    framework is drawn in. */
    for (i = 0; i < ND_CAL_GRID_COLS; i++) {
        draw_centred(d, small, nd_cal_weekday_initials[i], g.grid_x + i * g.cell_w, g.cell_w,
                     g.weekday_y, ND_GRAY);
    }

    /* 5. The cells.
     *
     * ============ THE CELL IS A BOX AND ONE ROW UNDER IT ============
     *
     * A digit's INK at 14 px is thirteen rows tall (measured: bbox top 1,
     * bottom 14) and the cell is sixteen. That leaves two rows, and it is why
     * the marker is one pixel high and lives OUTSIDE the box rather than
     * inside it: a bar drawn under the number within the box lands on the
     * number's own last two rows and reads as a strikethrough, which is what
     * the first draft of this screen actually did.
     *
     * So the box is the cell minus its last row, the digit is centred in the
     * box, and the marker is the last row. Nothing overlaps by construction,
     * the marker stays white when the box inverts -- it is not part of the
     * selection -- and there is still a clear row before the next week's
     * digits begin. */
    for (i = 0; i < ND_CAL_GRID_CELLS; i++) {
        const nd_cal_cell *cell = &cells[i];
        int32_t col = i % ND_CAL_GRID_COLS;
        int32_t row = i / ND_CAL_GRID_COLS;
        int32_t x0 = g.grid_x + col * g.cell_w;
        int32_t y0 = g.grid_y + row * g.cell_h;
        int32_t box_h = g.cell_h - 1;
        int32_t text_h = 0;
        bool is_selected = (i == selected);
        bool is_today = cell->year == today_y && cell->month == today_m && cell->day == today_d;
        bool has_event = cell->in_month && cell->day >= 1 && cell->day <= 31 &&
                         (mask & (1u << (unsigned)(cell->day - 1))) != 0u;
        nd_rect box = ND_RECT(x0, y0, x0 + g.cell_w - 1, y0 + box_h - 1);
        nd_color ink;

        (void)nd_snprintf(label, sizeof label, "%d", (int)cell->day);

        if (is_selected) {
            /* White with black text, which is what a selected row looks like
             * everywhere else in this OS. */
            (void)nd_draw_rect_fill(d, box, ND_WHITE);
            ink = ND_BLACK;
        } else {
            if (is_today)
                (void)nd_draw_rect_outline(d, box, ND_WHITE, 1);
            /* A neighbouring month's days are context, not choices. Grey says
             * that without leaving a hole in the grid, which is what blanking
             * them would do. */
            ink = cell->in_month ? ND_WHITE : ND_GRAY;
        }

        nd_text_size(small, label, NULL, &text_h);
        draw_centred(d, small, label, x0, g.cell_w, y0 + nd_max32(0, (box_h - text_h) / 2), ink);

        if (has_event) {
            int32_t bar_w = nd_min32(14, g.cell_w - 8);
            int32_t bx = x0 + (g.cell_w - bar_w) / 2;

            (void)nd_draw_rect_fill(
                d, ND_RECT(bx, y0 + g.cell_h - 1, bx + bar_w - 1, y0 + g.cell_h - 1), ND_WHITE);
        }
    }

    (void)nd_ui_present(ui);
}

/* ============ WHAT THE ANIMATED WALLPAPER NEEDS FROM THIS SCREEN ========
 *
 * The month is where the app sits when nothing is happening, so it is the
 * screen an animated wallpaper is behind for longest. The framework advances
 * a frame inside nd_ui_read_keypress() and then calls whatever repainter is
 * registered -- see nd_ui_set_repaint() -- so a screen that registers none
 * freezes the animation for exactly as long as it is open.
 *
 * The grid redraws from four numbers plus the month's event mask, and the
 * mask is the expensive one (a query over the whole diary). So the repainter
 * carries the mask that is already loaded rather than reloading it: a
 * wallpaper frame must not cost a database round trip. */
typedef struct {
    nd_ui *ui;
    const int32_t *year;
    const int32_t *month;
    const int32_t *day;
    const uint32_t *mask;
} month_repaint_ctx;

static void month_repaint(void *ctx)
{
    const month_repaint_ctx *c = (const month_repaint_ctx *)ctx;

    nd_cal_month_draw(c->ui, *c->year, *c->month, *c->day, *c->mask);
}

bool nd_cal_month_show(nd_ui *ui, int32_t *year, int32_t *month, int32_t *day)
{
    nd_softkey bar;
    uint32_t mask;
    int32_t shown_year;
    int32_t shown_month;
    month_repaint_ctx rc;
    nd_ui_repaint saved;
    bool out;

    if (ui == NULL || year == NULL || month == NULL || day == NULL)
        return false;

    /* Painted, NOT presented: the draw below clears only rows
     * 0..content_bottom, so this label is pushed to the panel as part of the
     * grid's own frame. One repaint, not two -- nd_levelsel_show() does the
     * same thing for the same reason. */
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "Options", false);

    shown_year = *year;
    shown_month = *month;
    mask = nd_cal_month_mask(*year, *month);
    nd_cal_month_draw(ui, *year, *month, *day, mask);

    rc.ui = ui;
    rc.year = year;
    rc.month = month;
    rc.day = day;
    rc.mask = &mask;
    saved = nd_ui_set_repaint(ui, month_repaint, &rc);

    for (;;) {
        int32_t key;
        nd_cal_nav nav;

        /* A poll rather than nd_ui_wait_for_key(), so that the teardown
         * contract in nd_app.h is honoured from the app's OUTERMOST loop.
         * This screen is where the app sits when nothing is happening, which
         * makes it exactly the loop an incoming call arrives at -- and a
         * blocking wait there would hold the sound card until the core gave
         * up and SIGKILLed us, with the phone ringing silently in the
         * meantime.
         *
         * The timeout is nd_ui_widget_timeout() rather than a literal 0.1 so
         * that an animation runs at the GIF's rate instead of being capped at
         * ten frames a second. It only ever shortens the wait -- it returns
         * the default whenever nothing is animating -- so the teardown poll
         * stays at least as responsive as it was. */
        key = nd_ui_read_keypress(ui, nd_ui_widget_timeout(ui, 0.1));
        if (nd_app_should_exit()) {
            out = false;
            break;
        }
        if (key == ND_KEY_NONE)
            continue;

        nav = nd_cal_month_key(key, year, month, day);
        if (nav == ND_CAL_NAV_OPEN) {
            out = true;
            break;
        }
        if (nav == ND_CAL_NAV_BACK) {
            out = false;
            break;
        }
        if (nav != ND_CAL_NAV_MOVED)
            continue;

        /* The mask is a query over every event in the diary, so it is
         * reloaded only when the month under the cursor actually changed.
         * Walking a week down inside one month is a redraw and no sqlite at
         * all, which is what makes holding Down feel like scrolling a list. */
        if (*year != shown_year || *month != shown_month) {
            mask = nd_cal_month_mask(*year, *month);
            shown_year = *year;
            shown_month = *month;
        }
        nd_cal_month_draw(ui, *year, *month, *day, mask);
    }

    /* Restore, not clear: this screen is opened from the app's own loop and
     * re-entered after every dialog, so the slot it found is the one it owes
     * back. See nd_ui_set_repaint(). */
    nd_ui_restore_repaint(ui, saved);
    return out;
}

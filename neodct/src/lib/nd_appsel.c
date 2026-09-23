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
 *   icon_y        = header_y + 8                         = 38
 *   icon_cap      = min(175, max(24, (145-38) * 3/4))    = 80
 *   reflection    = min(icon_h/3, 145 - (38 + icon_h))
 *   bar_x         = W - 8                                = 232
 *   track_top     = header_y + 6                         = 36
 *   track_bottom  = max(track_top, 145 - 10)             = 135
 *
 * The icon numbers are the ones the theme changed and the reason is in the
 * body of nd_appsel_draw(): the Python's 82 px icon at y=55 ran to row 137
 * and left eight rows under it, which is fine for an icon that simply stops
 * and impossible for one standing on a reflection. The scrollbar's extent did
 * not move.
 *
 * ============ THREE THINGS THAT DECIDE THE PIXELS ============
 *
 * 1. The scrollbar is nd_theme_scrollbar at the same centre column and the
 *    same extent as the width-2 white line it replaces. Its notch is still a
 *    FLOAT that truncates -- see nd_widgets.h rule 3.
 *
 * 2. The icon is fetched with max_size=icon_cap, so the cache holds a
 *    thumbnail rather than the 120x120 original. That is not only a memory
 *    decision: the thumbnail's dimensions are what centres it and what sizes
 *    the reflection, and asking for the full-size art and scaling per frame
 *    would land both elsewhere.
 *
 * 3. The reflection reads the icon's rows BOTTOM-UP through its own alpha
 *    (nd_theme_reflection). It allocates nothing, which is why it can run in
 *    the menu's repaint at the wallpaper's frame rate.
 *
 * ============ WHAT IS DELIBERATELY MISSING ============
 *
 * `title` is stored and never drawn. The title bar shows the selected app's
 * name instead. It is kept so a reader of both sources sees the same
 * constructor. nd_widgets.h says so too.
 *
 * There is still no HeaderWidget here and no breadcrumb: the page number goes
 * into nd_theme_titlebar as its badge.
 */

#include <stdio.h>
#include <string.h>

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_input.h"
#include "nd_keycodes.h"
#include "nd_text.h"
#include "nd_theme.h"
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
        /* The sky rather than black -- see nd_ui_paint_chrome(), which makes
         * the same choice for every other screen. Ramped over the panel so a
         * menu with no wallpaper and a dialog opened on top of it agree about
         * what colour row 144 is. */
        nd_theme_gradient_v_ramped(ui->canvas, ND_RECT(0, 0, screen_w - 1, screen_h - 1), 0,
                                   screen_h - 1, ND_TH_SKY_TOP, ND_TH_SKY_BOT, 255u);
    }
    /* The scrim goes on either ground: the title plate and the "Select" label
     * both sit on it, and over a bright wallpaper white type on a blue plate
     * still wants the picture behind it held down. */
    nd_theme_scrim(ui->canvas, ND_RECT(0, 0, screen_w - 1, screen_h - 1), 0, content_bottom,
                   ND_TH_APPSEL_SCRIM_A, 0u);

    /* An empty list is a real state: the scan can fail, and every later step
     * would divide by zero or index past the end. */
    if (s->n_items == 0u) {
        int32_t y;

        nd_ui_text_size(ui, "No Apps", ui->font_n, &w, &h);
        y = nd_max32(header_y, header_y + ((content_bottom - header_y - h) / 2));
        nd_theme_text_light(d, floordiv2(screen_w - w), y, "No Apps", ui->font_n);
        (void)nd_ui_present(ui);
        return;
    }

    if (s->selected_index >= s->n_items)
        s->selected_index = 0u;
    current = &s->items[s->selected_index];

    /* ============ WHAT MOVED, AND WHY ============
     *
     * The Python's layout put a centred 24 px name at y=14, an 82 px icon at
     * y=55 filling everything down to row 137, and a page number floating in
     * the top right. That leaves eight rows under the icon, which is fine for
     * an icon that ends where it ends and impossible for one that is supposed
     * to be standing on something.
     *
     * So the name goes into a title plate occupying rows 0..29 -- the rows
     * every other screen in the OS gives its title bar, which is the point --
     * and the icon moves up under it and gives back a fifth of its height to
     * a reflection. The numbers are recomputed from the panel rather than
     * from the old constants; ND_APP_SELECTOR_ICON_MAX still caps them.
     *
     * The page number is the title bar's badge now, for the same reason the
     * VerticalList's breadcrumb is: two right-aligned strings on the same row
     * drawn by two different pieces of code eventually stop agreeing.
     */

    /* 2. The title bar, with the name centred rather than left-aligned --
     *    this is the one screen whose title is centred, and it was centred
     *    before. nd_theme_titlebar left-aligns, so the plate is drawn through
     *    it with an empty title and the name is placed here. */
    /* Clamped into an int before formatting. The index cannot exceed
     * ND_APP_MAX here -- the guard above reset it -- but -Wformat-truncation
     * reasons about size_t and assumes twenty digits, which does not fit the
     * 16-byte buffer. nd_vlist.c's LevelSelector spells the same clamp out
     * for the same reason. */
    (void)snprintf(page_num, sizeof page_num, "%d",
                   (int)nd_clamp32((int32_t)(s->selected_index + 1u), 1, ND_APP_MAX));
    (void)nd_theme_titlebar(ui->canvas, d, screen_w, header_y, NULL, NULL, page_num, ui->font_n);
    {
        const nd_font *tf = nd_ui_font_bold(ui, ui->font_xl);
        char fitted[ND_TEXT_LINE_MAX];
        int32_t badge_w = 0;

        nd_ui_text_size(ui, page_num, ui->font_n, &badge_w, NULL);
        /* Trimmed against the badge on BOTH sides, because the name is
         * centred: a name that just fits on the left would otherwise reach
         * under the page number on the right. "Remote Shell" at 24 px bold is
         * the string that found this. */
        (void)nd_text_fit(fitted, sizeof fitted, current->name, tf, screen_w - 2 * (badge_w + 12));
        nd_ui_text_size(ui, fitted, tf, &w, NULL);
        /* Centred in the title band by the INK BOX, bearing included -- this
         * band has an edge to clip against and a face with a large bearing
         * pushes the name off it. See nd_theme_ink_centre_y(). */
        /* BAR ink, not content ink. The selector draws this title itself
         * rather than handing it to nd_theme_titlebar (only the caller knows
         * how to trim it against the badge), and in doing so it was reaching
         * for ink_light -- "type over the background" -- while the badge
         * beside it, drawn by the title bar, used bar_ink.
         *
         * The two are the same colour in a theme whose type is white
         * everywhere, so it never showed. Under a theme with a pale ground
         * and dark content type it is a charcoal app name sitting next to a
         * white page number on the same pink plate. */
        nd_theme_text_bar(d, floordiv2(screen_w - w),
                          nd_theme_ink_centre_y(tf, fitted, header_y), fitted, tf);
    }

    /* 3. The icon: a glow, the picture, then its reflection. */
    icon_y = header_y + 8;
    icon_path = current->icon;
    icon_cap =
        nd_min32(ND_APP_SELECTOR_ICON_MAX, nd_max32(24, ((content_bottom - icon_y) * 3) / 4));
    if (icon_path != NULL && icon_path[0] != '\0') {
        const nd_image *img = nd_ui_get_image_max(ui, icon_path, icon_cap);

        if (img != NULL) {
            int32_t ix = floordiv2(screen_w - img->w);

            /* The glow goes under the icon and is sized to it. Centred on the
             * icon's middle, reaching a little past its corners, so a
             * circular icon and a square one both sit in one. */
            nd_theme_glow(ui->canvas, screen_w / 2, icon_y + img->h / 2, (img->w * 3) / 4,
                          ND_TH_SKY_TOP, 90u);

            /* paste(img, (ix, iy), img): composited through the icon's own
             * alpha, so a transparent corner shows the wallpaper rather than
             * punching a square into it. */
            (void)nd_image_blit_alpha(ui->canvas, img, ix, icon_y);

            /* And the floor it stands on. Bounded by what is left above the
             * softkey strip, so a tall icon loses reflection rather than
             * spilling into the bar. */
            nd_theme_reflection(ui->canvas, img, ix, icon_y + img->h,
                                nd_min32(img->h / 3, content_bottom - (icon_y + img->h)), 110u);
        } else {
            /* The missing-icon placeholder, on the theme's terms: a glass
             * plate with a question mark on it rather than a wire outline. */
            int32_t px = floordiv2(screen_w - icon_cap);
            int32_t qw = 0;
            int32_t qh = 0;

            nd_theme_panel(ui->canvas, ND_RECT(px, icon_y, px + icon_cap, icon_y + icon_cap), 10);
            nd_ui_text_size(ui, "?", ui->font_xl, &qw, &qh);
            nd_theme_text_dark(d, px + ((icon_cap - qw) / 2), icon_y + ((icon_cap - qh) / 2), "?",
                               ui->font_xl);
        }
    }

    /* 4. "Select" sits INSIDE the softkey strip. The selector paints its own
     *    background over the core's transparent bar (step 1), so it has
     *    always had to draw this itself rather than letting nd_softkey do it.
     *    It gets the same plate nd_softkey_update() would have given it --
     *    the two are side by side every time the menu is opened from the home
     *    screen, and a bar that changed shape on the way in would be the most
     *    visible seam in the OS. */
    {
        const nd_font *f = nd_ui_font_bold(ui, ui->font_n);
        nd_theme_plate p = nd_theme_plate_bar(6);
        nd_rect plate = ND_RECT(2, content_bottom + 2, screen_w - 3, screen_h - 3);

        nd_theme_plate_draw(ui->canvas, plate, &p);
        /* Positioned exactly as nd_softkey_update() positions its label --
         * ink box, bearing subtracted. The two bars are side by side every
         * time the menu is opened from the home screen, and this one drifting
         * by a couple of rows is the seam the comment above is about. */
        nd_ui_text_size(ui, "Select", f, &w, NULL);
        nd_theme_text_bar(d, floordiv2(screen_w - w),
                          plate.y0 + nd_theme_ink_centre_y(f, "Select", nd_rect_h(plate)),
                          "Select", f);
    }

    /* 5. The scrollbar. Same centre column and same extent as before; see
     *    nd_theme_scrollbar for the thumb's truncating arithmetic. */
    bar_x = screen_w - 8;
    track_top = header_y + 6;
    track_bottom = nd_max32(track_top, content_bottom - 10);
    nd_theme_scrollbar(ui->canvas, bar_x, track_top, track_bottom, s->selected_index, s->n_items);

    (void)nd_ui_present(ui);
    (void)softkey_h;
}

/* See nd_ui_set_repaint(): the menu is the screen an animated wallpaper is
 * most visible on, and it is a blocking loop. */
static void appsel_repaint(void *ctx)
{
    nd_appsel_draw((nd_appsel *)ctx);
}

static int32_t appsel_loop(nd_appsel *s);

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

    /* The loop is a separate function only so the repainter is put back on
     * every way out of it, of which there are three. */
    {
        nd_ui_repaint saved = nd_ui_set_repaint(s->ui, appsel_repaint, s);
        int32_t choice = appsel_loop(s);

        nd_ui_restore_repaint(s->ui, saved);
        return choice;
    }
}

static int32_t appsel_loop(nd_appsel *s)
{
    for (;;) {
        int32_t key = nd_ui_wait_for_key(s->ui);

        /* Checked before anything else: with no apps, Down would take a
         * modulo by zero and Enter would index past the end. Only the two
         * ways out respond, and both mean "back". */
        /* Before the empty-list guard as well as before the navigation, so a
         * menu with nothing in it is not a screen the phone can ring on. */
        if (key == ND_KEY_INCOMING_CALL)
            return ND_APPSEL_RINGING;

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

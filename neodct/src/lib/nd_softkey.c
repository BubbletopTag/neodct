/* nd_softkey.c -- SoftKeyBar, the 30 px strip along the bottom that says what
 * the middle button does right now.
 *
 * The most-used widget in the system: 25+ call sites across the core, every
 * app, both Dialer screens, and six other widgets that build one inside their
 * own draw(). Ported from System/ui/framework.py:447.
 *
 * ============ OWNERSHIP ============
 *
 * This is widget 1 of the 14 in nd_widgets.h and it belongs to the UI
 * framework work package. The core-loop author left a WEAK placeholder here
 * so the home-screen frames -- every one of which has "Menu" or "Read" in rows
 * 145..174 -- could be verified before the widgets landed, and invited the
 * widgets author to take it over. Taken over: the definitions below are
 * strong, the behaviour is unchanged apart from `has_text` (see below), and
 * nd_ui.c still only calls the two public functions.
 *
 * ============ TRANSPARENCY IS EXPLICIT HERE, DELIBERATELY ============
 *
 * framework.py:465 decides it with `not hasattr(ui, 'softkey')`, a
 * construction-order side effect: core/main.py assigns
 * self.softkey = SoftKeyBar(self) at line 596, so the core's own bar is built
 * at the one moment the attribute does not yet exist and comes out
 * transparent. Every bar built afterwards -- by an app, a dialog or a list --
 * sees the attribute and is opaque.
 *
 * There is no C equivalent of that trick and emulating it would be worse than
 * useless, so nd_softkey_init() takes a boolean. EXACTLY ONE BAR IN THE WHOLE
 * SYSTEM PASSES true: the core's, at construction step 9. Getting it backwards
 * makes the home screen's strip a black band over the wallpaper, or every
 * app's strip transparent over stale pixels.
 *
 * ============ AN EMPTY STRING IS NOT AN ERROR ============
 *
 * update(NULL) and update("") both clear the strip and draw nothing. Python
 * spells the test `if new_text:`, and ProgressScreen and PagedList's empty
 * state both depend on it -- they clear the strip on purpose.
 *
 * `has_text` is the C stand-in for "current_text is not None", NOT for
 * "current_text is non-empty" -- current_text[0] already answers the second
 * question, so making the flag repeat it would throw away the one piece of
 * state a char array cannot hold.
 *
 * ============ ORDER, WHICH CALLERS DEPEND ON ============
 *
 * Callers routinely do nd_softkey_update(&bar, "Select", false) BEFORE running
 * a list, because a VerticalList's draw() clears only rows 0..145 and the
 * "Select" survives into the frame the list presents. Change either half of
 * that and the softkey vanishes or double-draws.
 */

#include <string.h>

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

/* Python's // on a possibly-negative numerator floors toward -inf; C's /
 * truncates toward zero. Only reachable with a label wider than the screen,
 * which is exactly the case where the two disagree. */
static int32_t floordiv2(int32_t v)
{
    return (v >= 0) ? (v / 2) : -(((-v) + 1) / 2);
}

void nd_softkey_init(nd_softkey *bar, nd_ui *ui, bool transparent)
{
    if (bar == NULL)
        return;

    memset(bar, 0, sizeof *bar);
    bar->ui = ui;
    bar->height = nd_ui_softkey_height(ui);
    bar->y_start = nd_ui_height(ui) - bar->height;
    bar->transparent = transparent;
    bar->current_text[0] = '\0';
    bar->has_text = false;
}

void nd_softkey_update(nd_softkey *bar, const char *text, bool present)
{
    nd_ui *ui;
    int32_t screen_w;
    int32_t screen_h;
    bool painted_slice = false;

    const nd_image *paper;

    if (bar == NULL || bar->ui == NULL)
        return;
    ui = bar->ui;
    if (ui->canvas == NULL || ui->draw == NULL)
        return;

    screen_w = nd_ui_width(ui);
    screen_h = nd_ui_height(ui);

    paper = bar->transparent ? nd_ui_wallpaper(ui) : NULL;
    if (paper != NULL) {
        /* wallpaper.crop((0, y_start, w, h)) then canvas.paste(slice, box).
         * PIL's box is half-open, so the last row copied is screen_h - 1;
         * nd_rect is inclusive, hence the -1 on both far edges. blit_region
         * skips the temporary the Python allocates thirty times a second. */
        nd_rect src = ND_RECT(0, bar->y_start, screen_w - 1, screen_h - 1);

        painted_slice =
            nd_image_blit_region(ui->canvas, paper, src, 0, bar->y_start) == ND_OK;
        if (!painted_slice) {
            /* The Python's bare `except: rectangle(..., fill="black")`. */
            (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, bar->y_start, screen_w, screen_h),
                                    ND_BLACK);
        }
    } else {
        /* OPAQUE: always black, so a scrolling list or a game's graphics
         * cannot show through. The literal (0, y_start, w, h) is
         * Pillow-inclusive and therefore one row and one column past the
         * canvas; both are clipped, which is what Pillow does too. */
        (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, bar->y_start, screen_w, screen_h), ND_BLACK);
    }

    if (text != NULL && text[0] != '\0' && ui->font_n != NULL) {
        int32_t w = 0;
        int32_t h = 0;

        /* The INK height, so a label of "OK" and a label of "Options" do not
         * sit on the same row. That is what the screens look like today. */
        nd_ui_text_size(ui, text, ui->font_n, &w, &h);
        (void)nd_draw_text(ui->draw, floordiv2(screen_w - w),
                           bar->y_start + floordiv2(bar->height - h), text, ui->font_n, ND_WHITE);
    }

    if (text != NULL) {
        (void)nd_strlcpy(bar->current_text, text, sizeof bar->current_text);
        bar->has_text = true;
    } else {
        bar->current_text[0] = '\0';
        bar->has_text = false;
    }

    if (present)
        (void)nd_ui_present(ui);
}

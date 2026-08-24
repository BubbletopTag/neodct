/* nd_infoscreen.c -- InfoScreen, a centred label with a big number under it.
 *
 * Ported from System/ui/framework.py:1473. "Top score / 385" in Games, and
 * the duration readouts in CallLog. Not a warning and not a dialog: no icon,
 * no accept/cancel distinction, and both 28 and 14 leave it.
 *
 * There is no draw(); the Python paints inline inside show(), and the whole
 * widget is one function in C for the same reason.
 *
 * ============ NULL AND "" ARE DIFFERENT ============
 *
 * `value=None` means "no value" and re-centres the title on its own.
 * `value="0"` renders "0". The Python distinguishes them with `is None`, so
 * the C distinguishes NULL from "": an empty string still reserves its ink
 * height (zero) and its 10 px gap, which moves the title up by five pixels.
 * That is the Python's behaviour, quirk included.
 *
 * ============ CENTRING IS BY INK, SO IT MOVES ============
 *
 * get_text_size() is the ink box of that exact string. "Top score" is 21 px
 * tall at 20 px and "1250" is 21 at 24 px because it has no ascender above
 * the digits and no descender below them. Centring therefore shifts with the
 * characters, visibly, and that is what the screens look like today.
 */

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_keycodes.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

/* Python's // floors. Every numerator here is clamped with max(0, ...) or is
 * a screen width minus an ink width, which goes negative for a string wider
 * than the panel -- the one case where floor and truncate disagree. */
static int32_t floordiv2(int32_t v)
{
    return (v >= 0) ? (v / 2) : -(((-v) + 1) / 2);
}

int32_t nd_infoscreen_show(nd_ui *ui, const char *title, const char *value,
                           const char *softkey_text)
{
    nd_draw *d;
    nd_softkey bar;
    int32_t screen_w;
    int32_t content_bottom;
    int32_t tw = 0;
    int32_t th = 0;
    int32_t ty;

    if (ui == NULL || ui->draw == NULL || ui->font_n == NULL || ui->font_xl == NULL)
        return ND_KEY_NONE;

    d = ui->draw;
    screen_w = nd_ui_width(ui);
    content_bottom = nd_ui_content_bottom(ui);

    /* `self.title = title or ""`, and softkey_text defaults to "Back". */
    if (title == NULL)
        title = "";
    if (softkey_text == NULL)
        softkey_text = "Back";

    /* Rows 0..145 only: the strip below is repainted by the bar at the end. */
    (void)nd_draw_rect_fill(d, ND_RECT(0, 0, screen_w, content_bottom), ND_BLACK);

    nd_text_size(ui->font_n, title, &tw, &th);

    if (value == NULL) {
        ty = nd_max32(0, floordiv2(content_bottom - th));
        (void)nd_draw_text(d, floordiv2(screen_w - tw), ty, title, ui->font_n, ND_WHITE);
    } else {
        int32_t vw = 0;
        int32_t vh = 0;
        const int32_t gap = 10;
        int32_t total;

        nd_text_size(ui->font_xl, value, &vw, &vh);
        total = th + gap + vh;
        ty = nd_max32(0, floordiv2(content_bottom - total));
        (void)nd_draw_text(d, floordiv2(screen_w - tw), ty, title, ui->font_n, ND_WHITE);
        (void)nd_draw_text(d, floordiv2(screen_w - vw), ty + th + gap, value, ui->font_xl,
                           ND_WHITE);
    }

    /* A fresh, opaque bar at its default present=true -- this is what pushes
     * the frame; there is no separate fb.update(). */
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, softkey_text, true);

    for (;;) {
        int32_t key = nd_ui_wait_for_key(ui);

        if (key == ND_KEY_ENTER || key == ND_KEY_CLEAR)
            return key;
    }
}

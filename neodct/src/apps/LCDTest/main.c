/* apps/LCDTest/main.c -- red, green, blue, colour bars.
 *
 * A one-to-one port of System/engineering/apps/LCDTest/main.py. App id 9001,
 * engineering menu, manifest name "LCD Test". It floods the content area with
 * one solid colour at a time so a dead pixel, a stuck subpixel or a bad
 * ribbon cable has something to show up against, and finishes on a TV-style
 * test card.
 *
 * ============ TWO PRESENTS PER PATTERN, AND THEY ARE KEPT ============
 *
 * main.py's loop is:
 *
 *     draw_fn()
 *     softkey.update("Next")        # present defaults to True -> commits
 *     ui.fb.update(ui.canvas)       # commits the SAME pixels again
 *
 * so every pattern reaches the panel twice. On the device that is a
 * duplicated blit and nothing more; here it is load-bearing twice over --
 * the virtual clock ticks once per committed frame, and nd_capture's ring
 * holds both, so collapsing them into one would change the frame count the
 * capture and test_lcdtest.c both pin. nd_widgets.h's rule 4 calls this out
 * as something six widgets do on purpose. Kept.
 *
 * ============ THE CONTENT CLEAR IS 0..content_bottom, NOT 0..H ============
 *
 * _draw_color fills rows 0..145 inclusive and leaves 146..174 to the softkey
 * bar, which is repainted immediately after. Row 145 therefore belongs to
 * BOTH, and the softkey wins because it runs second. Flooding the full height
 * instead would look identical here and would put a red row under the softkey
 * text the moment the bar was ever drawn transparent.
 */

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_keycodes.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#include "lcdtest.h"

/* main.py: KEY_NAV = 28, KEY_BACK = 14. Spelled with nd_keycodes.h's names;
 * the numbers are the ones the Python names hold. */
#define LCD_KEY_NAV  ND_KEY_ENTER
#define LCD_KEY_BACK ND_KEY_BACK

const char *const nd_lcdtest_names[ND_LCDTEST_N_PATTERNS] = {"Red", "Green", "Blue", "TV Test"};

const nd_color nd_lcdtest_fills[3] = {
    ND_RGB(0xFF, 0x00, 0x00), /* "#FF0000" */
    ND_RGB(0x00, 0xFF, 0x00), /* "#00FF00" */
    ND_RGB(0x00, 0x00, 0xFF), /* "#0000FF" */
};

/* White, yellow, cyan, green, magenta, red, blue -- the SMPTE order, and the
 * order matters to a technician reading the card. */
const nd_color nd_lcdtest_bars[ND_LCDTEST_N_BARS] = {
    ND_RGB(0xFF, 0xFF, 0xFF), ND_RGB(0xFF, 0xFF, 0x00), ND_RGB(0x00, 0xFF, 0xFF),
    ND_RGB(0x00, 0xFF, 0x00), ND_RGB(0xFF, 0x00, 0xFF), ND_RGB(0xFF, 0x00, 0x00),
    ND_RGB(0x00, 0x00, 0xFF),
};

const nd_color nd_lcdtest_stripes[ND_LCDTEST_N_STRIPES] = {
    ND_RGB(0x00, 0x21, 0x4A), /* "#00214A" */
    ND_RGB(0xFF, 0xFF, 0xFF), /* "#FFFFFF" */
    ND_RGB(0x32, 0x00, 0x6A), /* "#32006A" */
    ND_RGB(0x00, 0x00, 0x00), /* "#000000" */
};

const nd_color nd_lcdtest_band = ND_RGB(0x20, 0x20, 0x20); /* "#202020" */

int32_t nd_lcdtest_top_h(int32_t content_bottom)
{
    /* int(content_bottom * 0.7). nd_trunc32 truncates toward zero, which is
     * what int() does; a lround() here would give 102 on this panel. */
    return nd_trunc32((double)content_bottom * 0.7);
}

int32_t nd_lcdtest_mid_y(int32_t content_bottom)
{
    int32_t top_h = nd_lcdtest_top_h(content_bottom);

    return top_h + (content_bottom - top_h) / 2;
}

void nd_lcdtest_span(int32_t screen_w, int32_t n, int32_t i, int32_t *x0, int32_t *x1)
{
    /* max(1, screen_w // len(...)): a panel narrower than the bar count would
     * otherwise give a zero-width bar and paint nothing at all. */
    int32_t w = screen_w / n;

    if (w < 1)
        w = 1;
    if (x0 != NULL)
        *x0 = i * w;
    if (x1 != NULL)
        *x1 = (i == n - 1) ? screen_w : (i + 1) * w;
}

void nd_lcdtest_draw_color(nd_ui *ui, nd_color c)
{
    if (ui == NULL || ui->draw == NULL)
        return;
    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, nd_ui_width(ui), nd_ui_content_bottom(ui)), c);
}

void nd_lcdtest_draw_tv(nd_ui *ui)
{
    int32_t screen_w;
    int32_t bottom;
    int32_t top_h;
    int32_t lower_h;
    int32_t i;

    if (ui == NULL || ui->draw == NULL)
        return;

    screen_w = nd_ui_width(ui);
    bottom = nd_ui_content_bottom(ui);
    top_h = nd_lcdtest_top_h(bottom);

    for (i = 0; i < ND_LCDTEST_N_BARS; i++) {
        int32_t x0;
        int32_t x1;

        nd_lcdtest_span(screen_w, ND_LCDTEST_N_BARS, i, &x0, &x1);
        /* The boxes OVERLAP by one column: bar i ends at x1 and bar i+1
         * starts there. Pillow's rectangle is inclusive of both corners and
         * the later bar wins, so the seam is the later colour. */
        (void)nd_draw_rect_fill(ui->draw, ND_RECT(x0, 0, x1, top_h), nd_lcdtest_bars[i]);
    }

    lower_h = bottom - top_h;
    if (lower_h > 0) {
        int32_t mid_y = top_h + lower_h / 2;

        (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, top_h, screen_w, mid_y), nd_lcdtest_band);
        for (i = 0; i < ND_LCDTEST_N_STRIPES; i++) {
            int32_t x0;
            int32_t x1;

            nd_lcdtest_span(screen_w, ND_LCDTEST_N_STRIPES, i, &x0, &x1);
            (void)nd_draw_rect_fill(ui->draw, ND_RECT(x0, mid_y, x1, bottom),
                                    nd_lcdtest_stripes[i]);
        }
    }
}

static void draw_pattern(nd_ui *ui, int32_t idx)
{
    if (idx == ND_LCDTEST_TV)
        nd_lcdtest_draw_tv(ui);
    else
        nd_lcdtest_draw_color(ui, nd_lcdtest_fills[idx]);
}

int app_run(nd_ui *ui)
{
    nd_softkey softkey;
    int32_t idx = 0;

    if (ui == NULL || ui->draw == NULL || ui->canvas == NULL)
        return 1;

    /* Opaque: by the time any app runs the core's own bar already exists, so
     * framework.py's hasattr check has decided. See nd_ui.h. */
    nd_softkey_init(&softkey, ui, false);

    for (;;) {
        int32_t key;

        draw_pattern(ui, idx);
        /* present=True, as main.py has it, and then again below. */
        nd_softkey_update(&softkey, "Next", true);
        if (nd_ui_present(ui) != ND_OK)
            return 0;

        key = nd_ui_wait_for_key(ui);
        if (key == LCD_KEY_NAV)
            idx = (idx + 1) % ND_LCDTEST_N_PATTERNS;
        else if (key == LCD_KEY_BACK)
            return 0;
        /* nd_app.h: any loop longer than a frame polls this. The Python had
         * no equivalent -- IncomingCall came out of read_keypress -- and
         * OPEN-QUESTIONS.md question 1 records the substitution. */
        if (nd_app_should_exit())
            return 0;
    }
}

/* Nothing held: no sound card, no child, no file. Exported anyway, because
 * nd_app.h requires it so a missing symbol always means the author forgot. */
void app_shutdown(void) {}

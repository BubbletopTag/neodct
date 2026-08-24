/* nd_ui_metrics.c -- the five derived-geometry helpers every widget positions
 * itself with.
 *
 * ============ WHY THE GEOMETRY IS FUNCTIONS AND NOT CONSTANTS ============
 *
 * framework.py reads W / H / SOFTKEY_H off the ui object with
 * int(getattr(ui, "X", DEFAULT)), so a context that never assigned them still
 * lays out correctly -- which is what the Python unit tests' FakeUI relies on,
 * and what a widget drawn during construction step 13 (the alpha security
 * notice, a blocking modal inside the constructor) depends on. The C struct
 * always HAS the fields, so the equivalent of "the attribute is missing" is
 * "the field is still zero", and that is the fallback implemented here. No
 * panel is 0 px wide, so no real context is affected.
 *
 * _header_divider_y is max(30, int(H * 0.11)). On a 175 px band the floor wins
 * and the answer is 30. Port the formula, not the 30: anyone hard-coding it
 * breaks a future panel and anyone recomputing it wrong breaks today's.
 *
 * ============ THE SAME FIVE LIVE WEAKLY IN nd_ui.c ============
 *
 * The core-loop work package needed them before this file existed and left
 * weak copies behind that agree with these to the digit. Weak loses to strong,
 * so these are the ones that link; if this file ever goes away the core still
 * builds. Do not "deduplicate" by deleting one arbitrarily -- delete the weak
 * pair, and only after checking the numbers still match.
 *
 * The four nd_ui entry points widgets also call -- nd_ui_present(),
 * nd_ui_text_size(), nd_ui_wait_for_key() and nd_ui_read_keypress() -- are
 * NOT here. They belong to nd_ui.c, which owns them strongly, because in the
 * core the two key calls have to tick the battery, the modem and the ring
 * before they return.
 */

#include "nd_ui.h"

/* ------------------------------------------------------------------ *
 * Derived geometry
 * ------------------------------------------------------------------ */

int32_t nd_ui_width(const struct nd_ui *ui)
{
    if (ui != NULL && ui->w > 0)
        return ui->w;
    return ND_UI_W;
}

int32_t nd_ui_height(const struct nd_ui *ui)
{
    if (ui != NULL && ui->h > 0)
        return ui->h;
    return ND_UI_H;
}

int32_t nd_ui_softkey_height(const struct nd_ui *ui)
{
    if (ui != NULL && ui->softkey_h > 0)
        return ui->softkey_h;
    return ND_SOFTKEY_H;
}

int32_t nd_ui_content_bottom(const struct nd_ui *ui)
{
    return nd_ui_height(ui) - nd_ui_softkey_height(ui);
}

int32_t nd_ui_header_divider_y(const struct nd_ui *ui)
{
    /* int() truncates toward zero, and 175 * 0.11 is 19.25 -> 19, so the
     * max() floor is what actually decides this on the shipped panel. */
    return nd_max32(30, nd_trunc32((double)nd_ui_height(ui) * 0.11));
}

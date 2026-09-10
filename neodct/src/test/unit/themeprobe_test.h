/* themeprobe_test.h -- asking the ACTIVE theme what it draws, so that a
 * pixel test can assert geometry without also asserting decoration.
 *
 * ============ WHY THIS EXISTS ============
 *
 * The widget suites check where things land by reading pixels back, and
 * every one of them grew its probes while the glass look was compiled in.
 * That made "is this pixel part of the title bar" spellable as "is it much
 * bluer than the ground", and it worked, and it was wrong for a reason that
 * only showed up when the built-in look changed: it was a statement about
 * ONE palette, written where a statement about the widget belonged.
 *
 * When the classic face became the default, twenty-odd assertions across
 * three files failed on a renderer that was drawing exactly the right thing.
 * A white lozenge on black is not bluer than its ground. A title strip whose
 * bar colour IS the background paints nothing at its corners. A text field
 * drawn as a hollow rule has no light interior. All three are correct
 * classic renderings and all three read as regressions.
 *
 * ============ WHAT A TEST SHOULD ASSERT INSTEAD ============
 *
 *   - Geometry is the WIDGET's: where a plate starts, how tall a thumb is,
 *     which rows a field occupies. Assert it unguarded, in every theme.
 *
 *   - Decoration is the THEME's: whether a bar is painted at all, whether a
 *     plate casts a shadow, whether a panel has a light interior. Assert it
 *     guarded on the flag or the palette fact that decides it, so that BOTH
 *     looks are checked -- the decorated one for having it and the flat one
 *     for not leaking it.
 *
 * The predicates below are the second kind. They read nd_theme_pal and
 * nd_theme_style_of, which is what the renderer reads, so a test guarded on
 * one of them stays true for a theme nobody has written yet.
 *
 * They are static inline on purpose: a fixture that uses one and not the
 * others must not trip -Wunused-function.
 */

#ifndef NEODCT_THEMEPROBE_TEST_H
#define NEODCT_THEMEPROBE_TEST_H

#include <stdbool.h>
#include <stdint.h>

#include "nd_theme.h"

/* Two colours are "the same" to a probe if every channel is within this.
 * Wider than exact equality because a theme may spell its bar and its sky
 * with values that differ only in rounding and still mean "no bar". */
#define ND_TP_SAME 6

/* Channel-wise maximum difference. */
static inline int32_t nd_tp_dist(nd_color a, nd_color b)
{
    int32_t d[3];
    int32_t best = 0;
    int32_t i;

    d[0] = (int32_t)a.r - (int32_t)b.r;
    d[1] = (int32_t)a.g - (int32_t)b.g;
    d[2] = (int32_t)a.b - (int32_t)b.b;
    for (i = 0; i < 3; i++) {
        int32_t v = d[i] < 0 ? -d[i] : d[i];

        if (v > best)
            best = v;
    }
    return best;
}

/* Does this theme PAINT its title strip and softkey strip, or are they the
 * background with type standing on it?
 *
 * The classic face is the second: bar_top and bar_bot are the sky, and what
 * makes the strip a strip is the white type in it. A glass theme lays a
 * full-width plate over the same rows. An assertion that the bar reaches
 * both edges of the screen is real for the first kind and a false alarm for
 * the second, so it is guarded on this. */
static inline bool nd_tp_bars_painted(void)
{
    return nd_tp_dist(ND_TH_BAR_TOP, ND_TH_SKY_TOP) > ND_TP_SAME ||
           nd_tp_dist(ND_TH_BAR_BOT, ND_TH_SKY_BOT) > ND_TP_SAME;
}

/* Does a panel -- a text field's well, a dialog's body, a readout -- have an
 * interior distinct from the background?
 *
 * Same question for glass_*. The classic face draws a field as a hollow
 * rule: its edges are chrome and its inside is whatever was behind it, which
 * is the opposite of a light plate and is the same statement about where the
 * field is. */
static inline bool nd_tp_panels_painted(void)
{
    return nd_tp_dist(ND_TH_GLASS_TOP, ND_TH_SKY_TOP) > ND_TP_SAME ||
           nd_tp_dist(ND_TH_GLASS_BOT, ND_TH_SKY_BOT) > ND_TP_SAME;
}

/* How many rows a plate spends before its FILL begins.
 *
 * A bevelled theme's plate opens with a dark border row and a white hairline
 * one row inside it; a flat one fills from its top edge. A test that pins a
 * plate's position to the pixel adds this to the edge it expects, and then
 * says the same true thing under either look. */
#define ND_TP_FILL_INSET (ND_TH_BEVEL ? 2 : 0)

#endif /* NEODCT_THEMEPROBE_TEST_H */

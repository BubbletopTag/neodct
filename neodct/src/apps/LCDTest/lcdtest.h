/* lcdtest.h -- what apps/LCDTest/main.c shows its unit test.
 *
 * The four patterns and the two geometry rules are pulled out of app_run()
 * so test_lcdtest.c can check them against the Python's numbers directly,
 * rather than only through a rendered frame. Nothing else includes this.
 */

#ifndef ND_LCDTEST_H_INCLUDED
#define ND_LCDTEST_H_INCLUDED

#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* main.py's `patterns` list, in its order: the softkey steps through it and
 * wraps, so the order is what the third press shows. */
typedef enum {
    ND_LCDTEST_RED = 0,
    ND_LCDTEST_GREEN,
    ND_LCDTEST_BLUE,
    ND_LCDTEST_TV,
    ND_LCDTEST_N_PATTERNS
} nd_lcdtest_pattern;

/* The names are never drawn -- main.py builds (name, fn) tuples and then only
 * ever uses fn. Kept because dropping the half of a data structure that
 * nothing reads is how the next person loses what the pattern was called. */
extern const char *const nd_lcdtest_names[ND_LCDTEST_N_PATTERNS];

/* The three flood fills, indexed by pattern. ND_LCDTEST_TV has no entry. */
extern const nd_color nd_lcdtest_fills[3];

/* The TV card. Seven bars across the top 70%, then a #202020 band and four
 * stripes under it. */
#define ND_LCDTEST_N_BARS    7
#define ND_LCDTEST_N_STRIPES 4
extern const nd_color nd_lcdtest_bars[ND_LCDTEST_N_BARS];
extern const nd_color nd_lcdtest_stripes[ND_LCDTEST_N_STRIPES];
extern const nd_color nd_lcdtest_band;

/* int(content_bottom * 0.7). 101 on this panel, NOT 101.5 rounded to 102 --
 * Python's int() truncates and 145 * 0.7 is 101.49999999999999 in a double
 * anyway, so both roads lead to 101. */
int32_t nd_lcdtest_top_h(int32_t content_bottom);

/* top_h + (content_bottom - top_h) // 2, i.e. 123. Undefined when the lower
 * section is empty; main.py skips the whole lower half in that case. */
int32_t nd_lcdtest_mid_y(int32_t content_bottom);

/* Column boundaries: bar/stripe `i` covers x0..x1 INCLUSIVE, and the last one
 * is stretched to screen_w so the integer division cannot leave a sliver of
 * whatever was underneath down the right edge. */
void nd_lcdtest_span(int32_t screen_w, int32_t n, int32_t i, int32_t *x0, int32_t *x1);

/* The two painters, exported so the test can drive one without a key loop. */
void nd_lcdtest_draw_color(nd_ui *ui, nd_color c);
void nd_lcdtest_draw_tv(nd_ui *ui);

#ifdef __cplusplus
}
#endif

#endif /* ND_LCDTEST_H_INCLUDED */

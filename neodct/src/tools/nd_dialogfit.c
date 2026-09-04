/* nd_dialogfit.c -- does this MessageDialog message fit, or is it being cut
 * off with nothing to show for it?
 *
 * A build-host tool. Never installed on the phone.
 *
 * ============ WHY THIS EXISTS AS A TOOL ============
 *
 * nd_msgdialog clips a message that will not fit and marks the clip with
 * U+2026, which font.ttf has no glyph for. It draws NOTHING. So an overlong
 * message does not look truncated, it looks like a sentence that ends there,
 * and no test fails: test_msgdialog_invisible_ellipsis() asserts the bottom
 * rows stay dark and passed happily through every one of the bugs below,
 * because a dark band proves the renderer respected max_lines and says
 * nothing about whether text was thrown away to manage it.
 *
 * Six shipped messages were being cut off when this was written -- among them
 * the modem fault notice, the SD-card FORMAT confirmation (which destroys
 * data and stopped mid-sentence at "It is split in two: one part"), the crash
 * handler and a Remote Shell warning about keys readable by anyone holding
 * the card. Every one of them had been reviewed by somebody.
 *
 * nd_msgdialog_measure() pins a message a unit test can name. Most cannot be:
 * they are inline literals in 24 different app files, or snprintf'd at
 * runtime. This measures any string you hand it, against the real font, so
 * the whole set can be swept.
 *
 * ============ USE ============
 *
 *     nd-dialogfit <path-to-font.ttf> "message" ["message" ...]
 *
 * It prints the look, the line count, the budget and every wrapped line with
 * its pixel width, marking with X the lines that would be thrown away. The
 * budget assumes the common case: the default warning triangle, which every
 * dialog gets unless it names an icon that fails to load, and which is what
 * sets the body origin.
 *
 * A message that interpolates a %s has NO fixed answer -- measure it with the
 * longest value it can be handed, or bound the interpolation. Three of the
 * six bugs above were exactly this: a variable-length string pushing the
 * fixed guidance off the bottom.
 */

#include <stdio.h>
#include <string.h>

#include "nd_font.h"
#include "nd_text.h"

#define SCREEN_W 240
#define MARGIN   8
/* nd_ui_content_bottom() = 175 - ND_SOFTKEY_H(30) */
#define CONTENT_BOTTOM 145
/* margin + icon->h(24) + 6, which beats the title's 8 + ink("Modem") + 6 */
#define BODY_START_Y 38

int main(int argc, char **argv)
{
    const char *path = argv[1];
    int i;

    nd_font *f14 = nd_font_load(path, ND_FONT_PX_S);
    nd_font *f18 = nd_font_load(path, ND_FONT_PX_MD);
    nd_font *f20 = nd_font_load(path, ND_FONT_PX_N);
    int32_t max_w = SCREEN_W - MARGIN * 2;
    int32_t ag_h14 = 0, ag_h20 = 0, title_h = 0;

    if (f14 == NULL || f18 == NULL || f20 == NULL) {
        fprintf(stderr, "font load failed: %s\n", path);
        return 1;
    }
    nd_text_size(f14, "Ag", NULL, &ag_h14);
    nd_text_size(f20, "Ag", NULL, &ag_h20);
    nd_text_size(f18, "Modem", NULL, &title_h);

    printf("max_w=%d  line_h(14px)=%d  line_h(20px)=%d  title ink=%d\n", max_w, ag_h14 + 3,
           ag_h20 + 3, title_h);
    printf("body_start_y=%d (icon path) vs %d (title path)\n", MARGIN + 24 + 6,
           MARGIN + title_h + 6);

    for (i = 2; i < argc; i++) {
        char store20[3][ND_TEXT_LINE_MAX];
        char store[24][ND_TEXT_LINE_MAX];
        nd_lines alert, lines;
        const nd_font *body;
        int32_t line_h, max_lines;
        bool centered;
        size_t k;

        nd_lines_init(&alert, store20, 3u);
        nd_text_wrap_break_pop(&alert, argv[i], f20, max_w);

        nd_lines_init(&lines, store, 24u);
        if (alert.n <= 2u && !alert.truncated) {
            body = f20;
            centered = true;
            for (k = 0u; k < alert.n; k++)
                (void)nd_lines_push(&lines, nd_lines_at(&alert, k));
        } else {
            body = f14;
            centered = false;
            nd_text_wrap_break_pop(&lines, argv[i], f14, max_w);
        }
        line_h = (centered ? ag_h20 : ag_h14) + 3;
        max_lines = (CONTENT_BOTTOM - BODY_START_Y - MARGIN) / line_h;

        printf("\n=== [%d] %s look, %d lines, budget %d -> %s\n", i - 1,
               centered ? "ALERT/20px/centred" : "PARAGRAPH/14px/left", (int)lines.n, max_lines,
               ((int)lines.n <= max_lines && !lines.truncated) ? "FITS" : "*** CLIPPED ***");
        for (k = 0u; k < lines.n; k++) {
            int32_t w = 0;
            const char *s = nd_lines_at(&lines, k);

            nd_text_size(body, s, &w, NULL);
            printf("   %c %2d | %3dpx | \"%s\"\n", ((int)k < max_lines) ? ' ' : 'X', (int)k + 1, w,
                   s);
        }
    }
    return 0;
}

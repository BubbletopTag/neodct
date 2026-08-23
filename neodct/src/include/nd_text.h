/* nd_text.h -- the text-fitting and word-wrapping helpers, all of which are
 * subtly different from each other on purpose.
 *
 * There are SIX text-fitting routines in the Python framework and they do not
 * agree. Merging them "because they are the same" silently changes several
 * screens. The differences are real and each is pinned by at least one
 * existing test or golden frame:
 *
 *   nd_text_fit            fit_text          gives up and returns "" 
 *   nd_text_ellipsize      _ellipsize        gives up and returns the ORIGINAL
 *   nd_text_wrap           _wrap_lines       long word left over-wide, trailing
 *                                            blanks popped
 *   nd_text_wrap_break     TextInputLong     long word hard-broken, trailing
 *                                            blanks KEPT, empty gives [""]
 *   nd_text_wrap_break_pop MessageDialog     long word hard-broken, trailing
 *                                            blanks popped, empty gives []
 *   (two more live with their widgets: PagedList._wrap_to_lines and the
 *    Dialer's binary-search fitter -- see nd_widgets.h)
 *
 * Deduplicate only after the golden frames pass, and only with the owner's
 * agreement.
 *
 * NOTHING HERE ALLOCATES. Every wrapper writes into an nd_lines the caller
 * declared on its own stack.
 */

#ifndef ND_TEXT_H
#define ND_TEXT_H

#include "nd_font.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nd_ui;

/* One wrapped line. The widest thing the phone draws is a 240 px line of
 * 14 px text, about 48 characters; 256 bytes is comfortable for UTF-8. */
#define ND_TEXT_LINE_MAX 256

/* Hard cap per CODING-STANDARDS.md section 1.5: nothing sized by input goes
 * on the stack without a ceiling. The tallest consumer is DetailPage's body,
 * which pages through at most a few dozen lines at a time. */
#define ND_TEXT_LINES_MAX 128

/* A growable-looking, actually fixed, list of lines. Declare the storage
 * where you use it:
 *
 *     char storage[8][ND_TEXT_LINE_MAX];
 *     nd_lines lines;
 *     nd_lines_init(&lines, storage, 8);
 *
 * When the wrap needs more lines than cap, the extra lines are dropped and
 * truncated is set. Callers that care must check it; callers that do not are
 * choosing the Python's behaviour of clipping at the viewport anyway. */
typedef struct {
    char (*buf)[ND_TEXT_LINE_MAX];
    size_t cap;
    size_t n;
    bool   truncated;
} nd_lines;

void        nd_lines_init(nd_lines *l, char (*storage)[ND_TEXT_LINE_MAX], size_t cap);
void        nd_lines_clear(nd_lines *l);
bool        nd_lines_push(nd_lines *l, const char *s);
const char *nd_lines_at(const nd_lines *l, size_t i); /* "" when out of range */

/* ------------------------------------------------------------------ *
 * Fitting one line
 * ------------------------------------------------------------------ */

/* fit_text(ui, text, font, max_width): shorten with "..." until it fits.
 *   max_width <= 0 or empty text -> ""
 *   already fits                 -> unchanged
 *   otherwise, for end from len-1 down to 1, try text[:end] with trailing
 *   whitespace stripped plus "..."; the first that fits wins
 *   nothing fits                 -> ""
 * Only VerticalList calls it, to keep the title clear of the breadcrumb.
 * Writes into out; returns out. */
char *nd_text_fit(char *out, size_t out_sz, const char *text, const nd_font *f, int32_t max_w);

/* _ellipsize(ui, text, font, max_w): the same idea, three differences.
 *   already fits    -> unchanged
 *   otherwise drop characters from the right while (trimmed + "...") is too
 *   wide, then return trimmed + "..."
 *   nothing left    -> the ORIGINAL UNTRIMMED TEXT, not ""
 * That last asymmetry with nd_text_fit is deliberate. Port both. */
char *nd_text_ellipsize(char *out, size_t out_sz, const char *text, const nd_font *f,
                        int32_t max_w);

/* _font_ladder(ui, "font_n", "font_md", "font_s") -- the 20/18/14 ladder with
 * duplicates removed (a UI whose font_md failed to load has font_n twice).
 * Writes at most max entries, returns how many. */
size_t nd_font_ladder(const struct nd_ui *ui, const nd_font *out[], size_t max);

/* _fit_font: the first font in the ladder whose ink width for text is
 * <= max_w; when none fits, the LAST font in the ladder. Never NULL if
 * n_fonts > 0. */
const nd_font *nd_fit_font(const char *text, int32_t max_w, const nd_font *const *fonts,
                           size_t n_fonts);

/* ------------------------------------------------------------------ *
 * Wrapping
 * ------------------------------------------------------------------ */

/* _wrap_lines: the plain wrapper. Splits on \n and \r\n. A blank source line
 * emits a blank output line. Runs of spaces collapse. A word wider than max_w
 * is emitted alone on an OVER-WIDE line -- it is not broken. Trailing blank
 * lines are dropped, so the result may be empty. */
void nd_text_wrap(nd_lines *out, const char *text, const nd_font *f, int32_t max_w);

/* TextInputLong._wrap_text: as above, but an over-long word is hard-broken at
 * whatever character would overflow, trailing blank lines are KEPT, and an
 * empty result comes back as one empty line rather than none. */
void nd_text_wrap_break(nd_lines *out, const char *text, const nd_font *f, int32_t max_w);

/* MessageDialog._wrap_text: hard-breaks like nd_text_wrap_break, but pops
 * trailing blanks and can return zero lines, like nd_text_wrap. The one line
 * of difference between this and the previous function is load-bearing. */
void nd_text_wrap_break_pop(nd_lines *out, const char *text, const nd_font *f, int32_t max_w);

/* ------------------------------------------------------------------ *
 * Decoration
 * ------------------------------------------------------------------ */

/* _underline_tail: a 1 px rule one pixel below the ink box, under the last
 * tail_len bytes of line. Used to underline the provisional T9 word.
 *
 * The rule's y comes from the INK height of the WHOLE line, so it moves
 * depending on whether the line contains a descender. Port that; do not
 * "stabilise" it against the font's line height. */
struct nd_draw;
void nd_underline_tail(struct nd_draw *d, int32_t x, int32_t y, const char *line, size_t tail_len,
                       const nd_font *f);

#ifdef __cplusplus
}
#endif

#endif /* ND_TEXT_H */

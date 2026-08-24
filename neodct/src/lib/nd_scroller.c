/* nd_scroller.c -- TextScroller, the help reader.
 *
 * Ported from System/ui/framework.py:1364. Games, Settings, Tones and
 * MusicPlayer all show their instructions through it.
 *
 * ============ A BLANK LINE IS A BREATH, NOT A LINE ============
 *
 * The one idea in this widget. A blank source line costs gap_h = 8 px, not
 * the full 25 px of a line of 20 px type, and a page never STARTS with a gap.
 * The Python comment says why: giving a paragraph break the height of a line
 * turned one changelog into five screens of paging.
 *
 * ============ THE SOFTKEY IS THE PRESENT ============
 *
 * draw() ends with SoftKeyBar(ui).update(...) at its default present=True,
 * and there is no separate fb.update(). Add one and the panel repaints twice
 * per page turn.
 *
 * ============ PAGINATION IS RECOMPUTED FROM SCRATCH EVERY DRAW ============
 *
 * The Python re-wraps and re-paginates inside draw(), every time, and the
 * page index is clamped afterwards. It is wasteful and it is also what makes
 * a caller mutating .text between draws work, so it is reproduced. The wrap
 * lands in a file-scope buffer rather than on the stack: 128 lines at
 * ND_TEXT_LINE_MAX is 32 KB, which CODING-STANDARDS.md section 1.5 will not
 * have on a thread stack, and section 4's rule is "allocate once, reuse" --
 * this is the once. Exactly one TextScroller is ever on screen (both draw()
 * and show() are modal), so a single shared buffer is not a reentrancy
 * hazard; a second scroller drawn from inside the first one's key loop would
 * be, and nothing in the tree does that.
 */

#include <string.h>

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_keycodes.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

/* See the header comment: 32,768 bytes of BSS, mapped demand-zero, shared by
 * every process that links libneodct and touched only while a scroller is up. */
static char g_line_store[ND_SCROLLER_MAX_LINES][ND_TEXT_LINE_MAX];
static int32_t g_heights[ND_SCROLLER_MAX_LINES];

/* Index of the first entry of each page, plus a terminator, so page p covers
 * g_page_start[p] .. g_page_start[p+1]-1. A page always holds at least one
 * entry, so there can never be more pages than entries. */
static size_t g_page_start[ND_SCROLLER_MAX_LINES + 1u];
static size_t g_n_pages;
static nd_lines g_lines;

void nd_scroller_init(nd_scroller *s, nd_ui *ui, const char *text, const char *more_text,
                      const char *back_text)
{
    if (s == NULL)
        return;

    memset(s, 0, sizeof *s);
    s->ui = ui;
    s->text = (text != NULL) ? text : "";
    /* The Python's defaults are keyword arguments, not `or` fallbacks, so a
     * caller really can ask for an empty strip -- but NULL has no Python
     * spelling here and is treated as "the default". */
    s->more_text = (more_text != NULL) ? more_text : "More";
    s->back_text = (back_text != NULL) ? back_text : "Back";
    s->font = (ui != NULL && ui->font_n != NULL) ? ui->font_n : ((ui != NULL) ? ui->font_md : NULL);
    s->margin = 10;
    s->top = 8;
    s->page = 0u;
}

/* framework.py:1379. Fills g_line_store / g_heights / g_page_start and returns
 * the number of pages; *line_h_out gets the 25 px line height.
 *
 * The whole of _paginate, including its two guards: a page is only broken
 * BEFORE an entry that would overflow the budget (so a single over-tall entry
 * still gets a page of its own), and a blank entry that would open a page is
 * dropped entirely. */
size_t nd_scroller_paginate(const nd_scroller *s, size_t *line_h_out)
{
    int32_t content_bottom;
    int32_t screen_w;
    int32_t line_h;
    int32_t gap_h;
    int32_t budget;
    int32_t used = 0;
    size_t n_entries = 0u;
    size_t i;
    int32_t ag_h = 0;

    g_n_pages = 0u;
    g_page_start[0] = 0u;
    if (line_h_out != NULL)
        *line_h_out = 0u;

    if (s == NULL || s->ui == NULL || s->font == NULL)
        return 0u;

    screen_w = nd_ui_width(s->ui);
    content_bottom = nd_ui_content_bottom(s->ui);

    nd_lines_init(&g_lines, g_line_store, ND_SCROLLER_MAX_LINES);
    nd_text_wrap(&g_lines, s->text, s->font, screen_w - (s->margin * 2));
    /* `_wrap_lines(...) or [""]` -- an empty result is one empty line, which
     * then becomes the degenerate single page at the bottom of this function. */
    if (g_lines.n == 0u)
        (void)nd_lines_push(&g_lines, "");

    nd_text_size(s->font, "Ag", NULL, &ag_h);
    line_h = ag_h + 4;
    gap_h = nd_max32(4, line_h / 3);
    budget = content_bottom - s->top - 4;
    if (line_h_out != NULL)
        *line_h_out = (size_t)line_h;

    for (i = 0u; i < g_lines.n; i++) {
        bool blank = (g_line_store[i][0] == '\0');
        int32_t height = blank ? gap_h : line_h;
        bool page_open = (n_entries > g_page_start[g_n_pages]);

        if (page_open && used + height > budget) {
            g_n_pages++;
            g_page_start[g_n_pages] = n_entries;
            used = 0;
            page_open = false;
        }
        /* Never start a page with an empty gap. */
        if (blank && !page_open)
            continue;

        /* The wrap is stable, so entry k is line k for every k that survives
         * -- except the gaps this rule drops, which is why the entries are
         * compacted in place rather than indexed through the line list. */
        if (n_entries != i)
            memcpy(g_line_store[n_entries], g_line_store[i], ND_TEXT_LINE_MAX);
        g_heights[n_entries] = height;
        n_entries++;
        used += height;
    }

    if (n_entries > g_page_start[g_n_pages]) {
        g_n_pages++;
        g_page_start[g_n_pages] = n_entries;
    }

    if (g_n_pages == 0u) {
        /* `pages or [[("", line_h)]]` -- one page holding one empty line. */
        g_line_store[0][0] = '\0';
        g_heights[0] = line_h;
        g_page_start[0] = 0u;
        g_page_start[1] = 1u;
        g_n_pages = 1u;
    }
    return g_n_pages;
}

bool nd_scroller_draw(nd_scroller *s)
{
    nd_ui *ui;
    int32_t screen_w;
    int32_t content_bottom;
    size_t n_pages;
    size_t line_h = 0u;
    size_t i;
    int32_t y;
    bool last_page;
    nd_softkey bar;

    if (s == NULL || s->ui == NULL || s->ui->draw == NULL)
        return true;

    ui = s->ui;
    screen_w = nd_ui_width(ui);
    content_bottom = nd_ui_content_bottom(ui);

    n_pages = nd_scroller_paginate(s, &line_h);
    if (n_pages == 0u)
        return true;
    if (s->page > n_pages - 1u)
        s->page = n_pages - 1u;

    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, screen_w, content_bottom), ND_BLACK);

    y = s->top;
    for (i = g_page_start[s->page]; i < g_page_start[s->page + 1u]; i++) {
        if (g_line_store[i][0] != '\0')
            (void)nd_draw_text(ui->draw, s->margin, y, g_line_store[i], s->font, ND_WHITE);
        y += g_heights[i];
    }

    last_page = (s->page >= n_pages - 1u);

    /* present defaults to true in the Python, and this IS the frame's present.
     * There is no fb.update() after it. */
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, last_page ? s->back_text : s->more_text, true);
    return last_page;
}

void nd_scroller_show(nd_scroller *s)
{
    bool last_page;

    if (s == NULL)
        return;

    last_page = nd_scroller_draw(s);

    for (;;) {
        int32_t key = nd_ui_wait_for_key(s->ui);

        if (key == ND_KEY_ENTER || key == ND_KEY_DOWN) {
            if (last_page)
                return;
            s->page++;
            last_page = nd_scroller_draw(s);
        } else if (key == ND_KEY_UP) {
            if (s->page > 0u) {
                s->page--;
                last_page = nd_scroller_draw(s);
            }
        } else if (key == ND_KEY_CLEAR) {
            return;
        }
    }
}

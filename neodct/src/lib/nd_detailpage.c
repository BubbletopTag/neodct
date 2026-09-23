/* nd_detailpage.c -- DetailPage: picture, title, rule, scrolling body.
 *
 * Ported from System/ui/framework.py:1644. The most complex widget in the
 * framework and the only one that allocates. Update and Downgrade use it;
 * neodct/tests/test_update_ui.py has eighteen tests on it, which makes it the
 * best-specified screen in the tree.
 *
 * ============ CLOSURES BECOME A TAGGED UNION ============
 *
 * The Python builds a list of (paint_callable, height) pairs and walks it.
 * Function pointers with captured state would need a heap closure each, could
 * not be inspected by a test, and would put an indirect call in the render
 * path. nd_widgets.h therefore specifies a tagged block instead, and
 * spec-ui-framework.md agrees. The mapping is one-to-one:
 *
 *   Python closure          block kind      what it draws
 *   ---------------------   -------------   -------------------------------
 *   paint_hero              ND_BLOCK_HERO   picture left, text column right
 *   paint_image             ND_BLOCK_IMAGE  the picture, centred
 *   centered(text, font)    ND_BLOCK_TEXT   x precomputed at layout time
 *   paint_line              ND_BLOCK_TEXT   x = MARGIN
 *   paint_rule              ND_BLOCK_RULE   a hairline across the middle
 *   (None, line_height//2)  ND_BLOCK_GAP    a paragraph break: a breath
 *
 * Precomputing a centred block's x is safe because the Python's closure
 * measures the same string in the same font at paint time and cannot get a
 * different answer.
 *
 * ============ WHY THE HERO IS RE-LAID-OUT IN draw() ============
 *
 * A hero row is a (text, font, height) triple and there can be several of
 * them; one nd_detail_block holds one string, and the struct is frozen. So
 * ND_BLOCK_HERO carries only the group's height and the rows are recomputed
 * from (image, title, subtitle, badge) when it is painted. That recomputation
 * is pure -- same inputs, same fitted font, same wrap -- so the pixels cannot
 * drift from the layout pass that sized the block. See D-3 in
 * OPEN-QUESTIONS.md.
 *
 * ============ A BLOCK SLICED BY THE FOLD IS SKIPPED ENTIRELY ============
 *
 * y + height <= viewport_height, or the block is not drawn at all. Half a
 * line of type at the bottom edge reads as a bug; the scrollbar is what says
 * there is more to come. This is why the column is painted into ui->scratch
 * and blitted: clipping by construction rather than by arithmetic.
 *
 * ============ THE TWO-PASS BODY WRAP ============
 *
 * Whether a scrollbar steals 8 px depends on the page height, which depends
 * on the wrap, which depends on the width. Measure without the scrollbar,
 * redo once if the page turned out to need one, and stop -- at most two
 * passes, because narrowed != body_width can only be false after the first.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

/* ------------------------------------------------------------------ *
 * The private half of the allocation
 * ------------------------------------------------------------------ */

/* nd_detailpage has no field for the further-shrunk hero picture, and it must
 * not: `const nd_image *image` is documented as borrowed from the image
 * cache. But _fitted_hero() may thumbnail that picture down again, and the
 * result is a copy this page owns. So one allocation carries both: a private
 * header the caller never sees, followed by the block array that p->blocks
 * points at. nd_detailpage_free() recovers the header by subtracting the
 * member offset, which is exactly what offsetof is for. */
typedef struct {
    nd_image *owned_image; /* the shrunk hero copy, or NULL */
    size_t cap;
    nd_detail_block blocks[];
} detail_priv;

static detail_priv *priv_of(const nd_detailpage *p)
{
    if (p == NULL || p->blocks == NULL)
        return NULL;
    return (detail_priv *)(void *)((char *)p->blocks - offsetof(detail_priv, blocks));
}

/* ------------------------------------------------------------------ *
 * Hero scratch -- see the file comment on why draw() re-lays it out
 * ------------------------------------------------------------------ */

/* One title, one badge and the wrapped subtitle. 24 subtitle lines in a
 * ~140 px column is four times the viewport's capacity, so the cap can only
 * bite on a subtitle that was never going to be readable. 6,656 bytes of BSS,
 * demand-zero, touched only while a DetailPage is on screen -- the
 * "allocate once, reuse" rule of CODING-STANDARDS.md section 4, and the
 * reason draw() itself allocates nothing. */
#define HERO_MAX_SUB  24
#define HERO_MAX_ROWS (HERO_MAX_SUB + 2)

static char g_hero_title[ND_TEXT_LINE_MAX];
static char g_hero_badge[ND_TEXT_LINE_MAX];
static char g_hero_sub[HERO_MAX_SUB][ND_TEXT_LINE_MAX];

typedef struct {
    const char *text;
    const nd_font *font;
    int32_t h;
} hero_row;

typedef struct {
    hero_row rows[HERO_MAX_ROWS];
    size_t n_rows;
    int32_t text_x;
    int32_t stack_h;
    int32_t inner;
    int32_t height;
} hero_layout;

/* ------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------ */

/* Python's // floors. (width - text_w) goes negative for a string wider than
 * the panel, which is the one case where floor and C's truncation differ. */
static int32_t floordiv2(int32_t v)
{
    return (v >= 0) ? (v / 2) : -(((-v) + 1) / 2);
}

static const nd_font *title_font_of(const nd_ui *ui)
{
    return (ui->font_n != NULL) ? ui->font_n : ui->font_md;
}

static const nd_font *small_font_of(const nd_ui *ui)
{
    return (ui->font_s != NULL) ? ui->font_s : title_font_of(ui);
}

static int32_t viewport_h(const nd_detailpage *p)
{
    /* The Python's viewport is a 4-tuple used only for [1] and [3], never as
     * a Pillow rectangle, so this is y1 - y0 and NOT nd_rect_h(). With no
     * header it is 143 - 4 = 139; with one, 143 - 30 = 113. */
    return p->viewport.y1 - p->viewport.y0;
}

static nd_detail_block *push_block(nd_detailpage *p, nd_block_kind kind, int32_t height, int32_t x,
                                   const nd_font *font, const char *text)
{
    detail_priv *pv = priv_of(p);
    nd_detail_block *b;

    if (pv == NULL || p->n_blocks >= pv->cap)
        return NULL;
    b = &p->blocks[p->n_blocks++];
    b->kind = kind;
    b->height = height;
    b->x = x;
    b->font = font;
    b->text[0] = '\0';
    if (text != NULL)
        (void)nd_strlcpy(b->text, text, sizeof b->text);
    return b;
}

static int32_t blocks_height(const nd_detailpage *p, size_t from, size_t to)
{
    int32_t sum = 0;
    size_t i;

    for (i = from; i < to; i++)
        sum += p->blocks[i].height;
    return sum;
}

/* centered(text, font): the closure's own (width - text_w) // 2. */
static int32_t centered_x(int32_t width, const nd_font *f, const char *text)
{
    int32_t w = 0;

    nd_text_size(f, text, &w, NULL);
    return floordiv2(width - w);
}

/* ------------------------------------------------------------------ *
 * _hero_block: picture on the left, everything it is about on the right
 * ------------------------------------------------------------------ */

/* Stacking the two used up the whole screen before a word of the body got a
 * look in, which is the difference between a page you read and a page you
 * have to scroll to find out anything at all. */
static void hero_compute(const nd_detailpage *p, const nd_image *image, hero_layout *out)
{
    const nd_ui *ui = p->ui;
    const nd_font *font_small = small_font_of(ui);
    const nd_font *ladder[3];
    const nd_font *title_font;
    size_t n_ladder;
    int32_t width = nd_ui_width(ui);
    int32_t column;
    int32_t title_h = 0;
    nd_lines sub;
    size_t i;

    memset(out, 0, sizeof *out);
    out->text_x = ND_DETAIL_MARGIN + image->w + 8;
    column = width - out->text_x - ND_DETAIL_MARGIN - ND_DETAIL_SCROLLBAR_W;

    n_ladder = nd_font_ladder(ui, ladder, ND_ARRAY_LEN(ladder));
    if (n_ladder == 0u) {
        ladder[0] = title_font_of(ui);
        n_ladder = (ladder[0] != NULL) ? 1u : 0u;
    }
    /* _fit_font sees the RAW title; _ellipsize then trims for that font. */
    title_font = (n_ladder > 0u) ? nd_fit_font(p->title, column, ladder, n_ladder) : font_small;
    (void)nd_text_ellipsize(g_hero_title, sizeof g_hero_title, p->title, title_font, column);
    if (g_hero_title[0] != '\0') {
        int32_t ag = 0;

        nd_text_size(title_font, "Ag", NULL, &ag);
        title_h = ag + 5;
    }

    /* `rows = [r for r in rows if r[0]]` -- an empty string drops out, taking
     * its height with it, so a page with no title loses the whole row. */
    if (g_hero_title[0] != '\0') {
        out->rows[out->n_rows].text = g_hero_title;
        out->rows[out->n_rows].font = title_font;
        out->rows[out->n_rows].h = title_h;
        out->n_rows++;
    }

    if (p->subtitle[0] != '\0') {
        nd_lines_init(&sub, g_hero_sub, HERO_MAX_SUB);
        nd_text_wrap(&sub, p->subtitle, font_small, column);
        for (i = 0u; i < sub.n && out->n_rows < HERO_MAX_ROWS; i++) {
            if (g_hero_sub[i][0] == '\0')
                continue;
            out->rows[out->n_rows].text = g_hero_sub[i];
            out->rows[out->n_rows].font = font_small;
            out->rows[out->n_rows].h = p->line_height;
            out->n_rows++;
        }
    }

    if (p->badge[0] != '\0' && out->n_rows < HERO_MAX_ROWS) {
        (void)nd_text_ellipsize(g_hero_badge, sizeof g_hero_badge, p->badge, font_small, column);
        if (g_hero_badge[0] != '\0') {
            out->rows[out->n_rows].text = g_hero_badge;
            out->rows[out->n_rows].font = font_small;
            out->rows[out->n_rows].h = p->line_height;
            out->n_rows++;
        }
    }

    for (i = 0u; i < out->n_rows; i++)
        out->stack_h += out->rows[i].h;
    out->inner = nd_max32(image->h, out->stack_h);
    out->height = out->inner + 6;
}

/* _fitted_hero: shrink the picture until the first body line fits on screen.
 *
 * Whatever the picture and the details add up to, the first line of what
 * changed has to be visible without touching a key -- so the picture gives
 * ground rather than the page turning into a hero card with everything worth
 * reading below the fold. The 8 px step and the 40 px floor are load-bearing.
 *
 * Each iteration thumbnails the PREVIOUS iteration's result, so successive
 * LANCZOS passes compound exactly as the Python's chain of copy()+thumbnail()
 * does. The first copy is the only one taken from the cache's image. */
static void fitted_hero(nd_detailpage *p, hero_layout *out)
{
    detail_priv *pv = priv_of(p);
    const nd_image *image = p->image;

    for (;;) {
        hero_compute(p, image, out);
        if (out->height + p->line_height <= viewport_h(p))
            break;
        if (image->h <= ND_DETAIL_MIN_IMAGE)
            break;

        if (pv->owned_image == NULL) {
            pv->owned_image = nd_image_copy(image);
            if (pv->owned_image == NULL)
                break; /* out of memory: keep the size we have */
        }
        {
            int32_t side = nd_max32(ND_DETAIL_MIN_IMAGE, pv->owned_image->h - 8);

            if (nd_image_thumbnail(pv->owned_image, side, side) != ND_OK)
                break;
        }
        image = pv->owned_image;
    }
    p->image = image;
}

/* ------------------------------------------------------------------ *
 * _layout
 * ------------------------------------------------------------------ */

static void layout(nd_detailpage *p, nd_lines *scratch)
{
    const nd_ui *ui = p->ui;
    const nd_font *font_title = title_font_of(ui);
    const nd_font *font_small = small_font_of(ui);
    int32_t width = nd_ui_width(ui);
    int32_t vh = viewport_h(p);
    size_t i;

    p->n_blocks = 0u;

    if (p->image != NULL && (p->subtitle[0] != '\0' || p->badge[0] != '\0')) {
        /* CASE A -- the hero row. */
        hero_layout hero;

        fitted_hero(p, &hero);
        (void)push_block(p, ND_BLOCK_HERO, hero.height, ND_DETAIL_MARGIN, NULL, NULL);
    } else if (p->image != NULL) {
        /* CASE B -- the picture on its own, with the title under it. */
        (void)push_block(p, ND_BLOCK_IMAGE, p->image->h + 8, floordiv2(width - p->image->w), NULL,
                         NULL);
        if (p->title[0] != '\0') {
            int32_t title_h = 0;

            nd_text_size(font_title, "Ag", NULL, &title_h);
            (void)push_block(p, ND_BLOCK_TEXT, title_h + 6, centered_x(width, font_title, p->title),
                             font_title, p->title);
        }
    } else {
        /* CASE C -- nothing to sit beside, so centre the type instead. */
        if (p->title[0] != '\0') {
            int32_t title_h = 0;

            nd_text_size(font_title, "Ag", NULL, &title_h);
            (void)push_block(p, ND_BLOCK_TEXT, title_h + 6, centered_x(width, font_title, p->title),
                             font_title, p->title);
        }
        if (p->subtitle[0] != '\0') {
            nd_text_wrap(scratch, p->subtitle, font_small, width - ND_DETAIL_MARGIN * 2);
            for (i = 0u; i < scratch->n; i++) {
                const char *line = nd_lines_at(scratch, i);

                (void)push_block(p, ND_BLOCK_TEXT, p->line_height,
                                 centered_x(width, font_small, line), font_small, line);
            }
        }
        if (p->badge[0] != '\0') {
            (void)push_block(p, ND_BLOCK_TEXT, p->line_height + 4,
                             centered_x(width, font_small, p->badge), font_small, p->badge);
        }
    }

    p->body_top = blocks_height(p, 0u, p->n_blocks);

    if (p->body[0] != '\0') {
        const int32_t rule_h = 10;
        int32_t body_width = width - (ND_DETAIL_MARGIN * 2);
        size_t mark;
        int pass;

        /* The rule is the first thing to go when the page is tight: it
         * separates, but it does not say anything. */
        if (p->n_blocks > 0u && p->body_top + rule_h + p->line_height <= vh) {
            (void)push_block(p, ND_BLOCK_RULE, rule_h, ND_DETAIL_MARGIN, NULL, NULL);
            p->body_top += rule_h;
        }

        mark = p->n_blocks;
        for (pass = 0; pass < 2; pass++) {
            int32_t narrowed = body_width - ND_DETAIL_SCROLLBAR_W;
            int32_t height;

            p->n_blocks = mark;
            nd_text_wrap(scratch, p->body, font_small, body_width);
            for (i = 0u; i < scratch->n; i++) {
                const char *line = nd_lines_at(scratch, i);

                if (line[0] == '\0') {
                    /* A paragraph break is a breath, not an empty line. */
                    (void)push_block(p, ND_BLOCK_GAP, p->line_height / 2, 0, NULL, NULL);
                } else {
                    (void)push_block(p, ND_BLOCK_TEXT, p->line_height, ND_DETAIL_MARGIN, font_small,
                                     line);
                }
            }

            height = blocks_height(p, 0u, p->n_blocks);
            if (height <= vh || body_width == narrowed)
                break;
            body_width = narrowed;
        }
    }

    p->content_height = blocks_height(p, 0u, p->n_blocks);
    p->scrollable = p->content_height > vh;
}

/* ------------------------------------------------------------------ *
 * Construction
 * ------------------------------------------------------------------ */

nd_err nd_detailpage_init(nd_detailpage *p, nd_ui *ui, const char *title, const char *subtitle,
                          const char *body, const char *image_path, const char *badge,
                          const char *header, const char *softkey_text)
{
    detail_priv *pv;
    nd_lines scratch;
    char(*line_store)[ND_TEXT_LINE_MAX];
    int32_t small_h = 0;
    int32_t top;
    const nd_font *font_small;

    if (p == NULL || ui == NULL)
        return ND_ERR_INVAL;

    memset(p, 0, sizeof *p);
    p->ui = ui;
    /* `title or ""`, `subtitle or ""`, `badge or ""`, `body or ""`; header
     * keeps its None, because the Python tests it with `if self.header`. */
    p->title = (title != NULL) ? title : "";
    p->subtitle = (subtitle != NULL) ? subtitle : "";
    p->body = (body != NULL) ? body : "";
    p->badge = (badge != NULL) ? badge : "";
    p->header = header;
    p->softkey_text = (softkey_text != NULL) ? softkey_text : "OK";
    p->accept_keys[0] = ND_KEY_ENTER;
    p->n_accept = 1u;
    p->cancel_keys[0] = ND_KEY_CLEAR;
    p->n_cancel = 1u;
    p->offset = 0;

    font_small = small_font_of(ui);
    if (font_small == NULL)
        return ND_ERR_INVAL;
    nd_text_size(font_small, "Ag", NULL, &small_h);
    p->line_height = small_h + 3;

    top = 4;
    if (p->header != NULL && p->header[0] != '\0') {
        /* header_box = (0, 4, width, 4 + small_h); divider_y = its bottom + 5.
         * Neither has a field in the frozen struct, so draw() recomputes them
         * from the same two numbers. */
        top = (4 + small_h) + 5 + 6;
    }
    /* Two pixels of air above the softkey bar: the page must never look like
     * its last line is sitting on the softkey. */
    p->viewport = ND_RECT(0, top, nd_ui_width(ui), nd_ui_content_bottom(ui) - 2);

    /* _prepare_image: a path goes through the cache with max_size=64, so the
     * picture that comes back is borrowed and already inside the box. */
    if (image_path != NULL && image_path[0] != '\0')
        p->image = nd_ui_get_image_max(ui, image_path, ND_DETAIL_IMAGE_MAX);

    /* 256 blocks at ~280 bytes each. Claimed in full because the two-pass
     * body wrap does not know its own answer until it has run, then handed
     * back down to what the page actually used -- typically a dozen blocks. */
    pv = calloc(1u, sizeof *pv + (size_t)ND_DETAIL_MAX_BLOCKS * sizeof(nd_detail_block));
    if (pv == NULL) {
        nd_log_err(ND_LOG_UI, "DetailPage: cannot allocate %d blocks", ND_DETAIL_MAX_BLOCKS);
        return ND_ERR_NOMEM;
    }
    pv->cap = ND_DETAIL_MAX_BLOCKS;
    p->blocks = pv->blocks;

    /* 128 * 256 = 32,768 bytes, alive only for the length of this function --
     * far too much for a thread stack, and nothing needs it after layout. */
    line_store = calloc(ND_TEXT_LINES_MAX, sizeof *line_store);
    if (line_store == NULL) {
        nd_detailpage_free(p);
        return ND_ERR_NOMEM;
    }
    nd_lines_init(&scratch, line_store, ND_TEXT_LINES_MAX);

    layout(p, &scratch);

    free(line_store);

    /* Hand the unused tail back. A page is typically a dozen blocks, so this
     * returns ~68 KB of the 71 KB claimed above -- worth doing on a device
     * that keeps the page alive for as long as somebody is reading it. A
     * failed shrink is not a failure: the page keeps the larger block. */
    {
        detail_priv *smaller = realloc(pv, sizeof *pv + p->n_blocks * sizeof(nd_detail_block));

        if (smaller != NULL) {
            smaller->cap = p->n_blocks;
            p->blocks = smaller->blocks;
        }
    }
    return ND_OK;
}

void nd_detailpage_free(nd_detailpage *p)
{
    detail_priv *pv = priv_of(p);

    if (pv == NULL)
        return;
    nd_image_free(pv->owned_image);
    p->blocks = NULL;
    p->n_blocks = 0u;
    /* p->image may point at pv->owned_image, which has just gone. */
    p->image = NULL;
    free(pv);
}

int32_t nd_detailpage_max_offset(const nd_detailpage *p)
{
    if (p == NULL)
        return 0;
    return nd_max32(0, p->content_height - viewport_h(p));
}

/* ------------------------------------------------------------------ *
 * Drawing
 * ------------------------------------------------------------------ */

static void paste_image(nd_image *dst, const nd_image *src, int32_t x, int32_t y)
{
    if (src->fmt == ND_PIXFMT_RGBA8888)
        (void)nd_image_blit_alpha(dst, src, x, y);
    else
        (void)nd_image_blit(dst, src, x, y);
}

static void paint_hero(nd_detailpage *p, nd_image *col, nd_draw *cd, int32_t y)
{
    hero_layout hero;
    int32_t row_y;
    size_t i;

    if (p->image == NULL)
        return;
    hero_compute(p, p->image, &hero);

    paste_image(col, p->image, ND_DETAIL_MARGIN, y + 3 + floordiv2(hero.inner - p->image->h));

    row_y = y + 3 + floordiv2(hero.inner - hero.stack_h);
    for (i = 0u; i < hero.n_rows; i++) {
        (void)nd_draw_text(cd, hero.text_x, row_y, hero.rows[i].text, hero.rows[i].font, ND_WHITE);
        row_y += hero.rows[i].h;
    }
}

static void draw_scrollbar(nd_detailpage *p)
{
    nd_draw *d = p->ui->draw;
    int32_t x = nd_ui_width(p->ui) - 5;
    int32_t top = p->viewport.y0 + 2;
    int32_t base = p->viewport.y1 - 2;
    int32_t travel = base - top - 10;
    int32_t max_off = nd_detailpage_max_offset(p);
    int32_t position;

    /* width 2 grows in the MINOR AXIS ONLY: columns 235 and 236, not 234-236. */
    (void)nd_draw_line(d, x, top, x, base, ND_WHITE, 2);

    /* `max_offset or 1` -- Python's guard for a page that is not scrollable,
     * which cannot reach here anyway. The division is float, then int(). */
    position = top + nd_trunc32((double)travel *
                                ((double)p->offset / (double)((max_off != 0) ? max_off : 1)));
    (void)nd_draw_rect_fill(d, ND_RECT(x - 3, position, x + 3, position + 10), ND_WHITE);
}

void nd_detailpage_draw(nd_detailpage *p)
{
    nd_ui *ui;
    nd_draw *d;
    nd_draw col_draw;
    nd_image *col;
    const nd_font *font_small;
    int32_t width;
    int32_t vh;
    int32_t y;
    size_t i;
    nd_softkey bar;

    if (p == NULL || p->ui == NULL || p->ui->draw == NULL || p->ui->canvas == NULL)
        return;

    ui = p->ui;
    d = ui->draw;
    width = nd_ui_width(ui);
    vh = nd_max32(1, viewport_h(p));
    font_small = small_font_of(ui);

    nd_ui_paint_chrome_content(ui);

    if (p->header != NULL && p->header[0] != '\0') {
        int32_t small_h = 0;
        int32_t divider_y;

        nd_text_size(font_small, "Ag", NULL, &small_h);
        divider_y = (4 + small_h) + 5;
        (void)nd_draw_text(d, ND_DETAIL_MARGIN, 4, p->header, font_small, ND_WHITE);
        (void)nd_draw_line(d, ND_DETAIL_MARGIN, divider_y, width - ND_DETAIL_MARGIN, divider_y,
                           ND_WHITE, 1);
    }

    /* The column is painted into its own surface and pasted, so scrolled text
     * is clipped by construction rather than by careful arithmetic. The
     * Python allocates 240x139x3 = 100,080 bytes here on EVERY draw; this
     * borrows the one ui->scratch surface built at startup instead. */
    col = ui->scratch;
    if (col == NULL || col->h < vh) {
        nd_log_err(ND_LOG_UI, "DetailPage: no scratch column (need %dx%d)", width, vh);
        return;
    }
    if (nd_draw_bind(&col_draw, col) != ND_OK)
        return;
    /* The column is pasted at (0, viewport.y0), so it is cleared with the
     * wallpaper's rows STARTING THERE. Clearing it with the wallpaper's rows
     * 0..vh would paste the top of the picture into the middle of the screen
     * and the seam would be obvious against the header above it. */
    {
        const nd_image *paper = nd_ui_chrome_wallpaper(ui);

        if (paper != NULL)
            (void)nd_image_blit_region(
                col, paper, ND_RECT(0, p->viewport.y0, col->w - 1, p->viewport.y0 + vh - 1), 0, 0);
        else
            (void)nd_draw_rect_fill(&col_draw, ND_RECT(0, 0, col->w, vh - 1), ND_BLACK);
    }

    /* A page that fits is centred: a few words pinned to the top of an
     * otherwise black screen reads as a crash rather than as an answer. */
    y = -p->offset;
    if (!p->scrollable)
        y += floordiv2(vh - p->content_height);

    for (i = 0u; i < p->n_blocks; i++) {
        nd_detail_block *b = &p->blocks[i];

        /* Blocks that would be sliced by the bottom edge are left for the
         * next scroll. ND_BLOCK_GAP is the Python's `paint is None`. */
        if (b->kind != ND_BLOCK_GAP && y + b->height > 0 && y + b->height <= vh) {
            switch (b->kind) {
            case ND_BLOCK_HERO:
                paint_hero(p, col, &col_draw, y);
                break;
            case ND_BLOCK_IMAGE:
                if (p->image != NULL)
                    paste_image(col, p->image, b->x, y);
                break;
            case ND_BLOCK_RULE:
                (void)nd_draw_line(&col_draw, ND_DETAIL_MARGIN * 3, y + 4,
                                   width - ND_DETAIL_MARGIN * 3, y + 4, ND_WHITE, 1);
                break;
            case ND_BLOCK_TEXT:
                (void)nd_draw_text(&col_draw, b->x, y, b->text, b->font, ND_WHITE);
                break;
            case ND_BLOCK_GAP:
            default:
                break;
            }
        }
        y += b->height;
    }

    /* canvas.paste(column, (0, viewport[1])): the whole column, inclusive
     * source box, so the last row copied is vh - 1. */
    (void)nd_image_blit_region(ui->canvas, col, ND_RECT(0, 0, col->w - 1, vh - 1), 0,
                               p->viewport.y0);

    if (p->scrollable)
        draw_scrollbar(p);

    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, p->softkey_text, false);
    (void)nd_ui_present(ui);
}

/* ------------------------------------------------------------------ *
 * Input
 * ------------------------------------------------------------------ */

bool nd_detailpage_handle_key(nd_detailpage *p, int32_t key)
{
    int32_t new_off;

    if (p == NULL)
        return false;

    if (key == ND_KEY_DOWN)
        new_off = nd_min32(nd_detailpage_max_offset(p), p->offset + p->line_height);
    else if (key == ND_KEY_UP)
        new_off = nd_max32(0, p->offset - p->line_height);
    else
        return false;

    /* No redraw at either end, so holding Down at the bottom is silent. */
    if (new_off == p->offset)
        return false;
    p->offset = new_off;
    nd_detailpage_draw(p);
    return true;
}

static bool key_in(const int32_t *keys, size_t n, int32_t key)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        if (keys[i] == key)
            return true;
    }
    return false;
}

/* See nd_ui_set_repaint(). */
static void detailpage_repaint(void *ctx)
{
    nd_detailpage_draw((nd_detailpage *)ctx);
}

int32_t nd_detailpage_show(nd_detailpage *p)
{
    if (p == NULL)
        return ND_KEY_NONE;

    nd_detailpage_draw(p);
    {
        nd_ui_repaint saved = nd_ui_set_repaint(p->ui, detailpage_repaint, p);
        int32_t out;

        for (;;) {
            int32_t key = nd_ui_wait_for_key(p->ui);

            if (key_in(p->accept_keys, p->n_accept, key) ||
                key_in(p->cancel_keys, p->n_cancel, key)) {
                out = key;
                break;
            }
            (void)nd_detailpage_handle_key(p, key);
        }

        nd_ui_restore_repaint(p->ui, saved);
        return out;
    }
}

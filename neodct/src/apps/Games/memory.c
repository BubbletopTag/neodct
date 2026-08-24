/* apps/Games/memory.c -- Nokia-style Memory.
 *
 * A one-to-one port of System/apps/Games/memory.py (213 lines): forty cards
 * in an 8x5 grid, a cursor moved with the arrows or 2-4-6-8, key 5 turns a
 * card, and twenty little pictures drawn with the seven ImageDraw primitives
 * rather than with an icon file.
 *
 * golden/game-memory.png is the FIRST frame of a new game: forty face-down
 * white cards, the cursor ring around (0,0) drawn in the gap between cards,
 * and the black inner border that marks a face-down card under the cursor.
 * The shuffle is not visible in that frame -- every card is face down -- but
 * the frame is still frame class `recut`, because it is captured through the
 * same run as game-snake and the two share a reference set.
 *
 * ============ THE CURSOR IS DRAWN IN THE GAP, NOT ON THE CARD ============
 *
 * A card is (px+2, py+2, px+cell-3, py+cell-3) -- a 23x23 box inside a 27 px
 * cell, so there is a 2 px gutter all round. The cursor ring is the full
 * cell, (px, py, px+26, py+26), so it sits IN that gutter and never covers a
 * card's edge. On a face-down card, which is solid white, a ring in the
 * gutter would be invisible against the neighbours, so a second BLACK ring
 * is drawn around the card itself. That black ring is the only black ink on
 * a white card anywhere in the game and it is what makes the cursor
 * readable.
 *
 * ============ THE SHUFFLE IS NOT THE PYTHON'S ============
 *
 * `random.seed(time.time())` then `random.shuffle(kinds)`. Decision 4 says
 * not to reimplement MT19937, so nd_rand_shuffle() -- the same Fisher-Yates
 * from the top down that CPython runs, over a different stream -- lays the
 * board out differently. Deterministic from run to run, which is what the
 * recut reference needs.
 *
 * ============ TWENTY GLYPHS, AND THE ONE THAT IS A TRAP ============
 *
 * draw_glyph() is a chain of twenty shapes. Nineteen are ordinary. Kind 15
 * is `polygon([(x0,y0), (x1,y0), (x0,y1), (x1,y1)])` -- a SELF-INTERSECTING
 * quadrilateral, whose interior is decided by Pillow's scanline parity rule
 * and comes out as two triangles meeting at a point, not as a bow tie with a
 * filled crossing. nd_draw_polygon() reproduces that rule (nd_draw.h names
 * this shape as the reason it has to), so the C draws the same hourglass.
 *
 * All the divisions here are Python's `//` on non-negative operands, so
 * plain C `/` on int32_t is the same operation. The boxes are always on
 * screen, so no operand is ever negative.
 */

#include <string.h>

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_keycodes.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"

#include "games.h"

/* ------------------------------------------------------------------ *
 * draw_glyph
 * ------------------------------------------------------------------ */

/* polygon(points, outline=c). Pillow's outline path is one ImagingDrawLine
 * per edge plus the closing edge, all at width 1 -- NOT a wide-line stroke
 * and not the fill rule. Only glyph 5 needs it. */
static void poly_outline(nd_draw *d, const nd_point *p, size_t n, nd_color c)
{
    size_t i;

    for (i = 0u; i + 1u < n; i++)
        (void)nd_draw_line(d, p[i].x, p[i].y, p[i + 1u].x, p[i + 1u].y, c, 1);
    if (n >= 2u)
        (void)nd_draw_line(d, p[n - 1u].x, p[n - 1u].y, p[0].x, p[0].y, c, 1);
}

void nd_memory_draw_glyph(nd_draw *d, nd_rect box, int32_t kind, nd_color c)
{
    int32_t x0 = box.x0;
    int32_t y0 = box.y0;
    int32_t x1 = box.x1;
    int32_t y1 = box.y1;
    int32_t cx = (x0 + x1) / 2;
    int32_t cy = (y0 + y1) / 2;
    int32_t w = x1 - x0;
    int32_t t;
    int32_t q;
    int32_t r;
    int32_t step;
    int32_t xx;
    int32_t yy;
    int32_t i;
    int32_t j;
    nd_point pts[8];

    if (d == NULL)
        return;

    switch (kind) {
    case 0: /* filled circle */
        (void)nd_draw_ellipse_fill(d, box, c);
        break;
    case 1: /* ring */
        (void)nd_draw_ellipse_outline(d, box, c, 1);
        (void)nd_draw_ellipse_outline(d, ND_RECT(x0 + 3, y0 + 3, x1 - 3, y1 - 3), c, 1);
        break;
    case 2: /* filled square */
        (void)nd_draw_rect_fill(d, box, c);
        break;
    case 3: /* hollow square */
        (void)nd_draw_rect_outline(d, box, c, 1);
        break;
    case 4: /* filled diamond */
    case 5: /* hollow diamond -- the same four points, outlined */
        pts[0] = (nd_point){cx, y0};
        pts[1] = (nd_point){x1, cy};
        pts[2] = (nd_point){cx, y1};
        pts[3] = (nd_point){x0, cy};
        if (kind == 4)
            (void)nd_draw_polygon(d, pts, 4u, c);
        else
            poly_outline(d, pts, 4u, c);
        break;
    case 6: /* triangle up */
        pts[0] = (nd_point){cx, y0};
        pts[1] = (nd_point){x1, y1};
        pts[2] = (nd_point){x0, y1};
        (void)nd_draw_polygon(d, pts, 3u, c);
        break;
    case 7: /* triangle down */
        pts[0] = (nd_point){x0, y0};
        pts[1] = (nd_point){x1, y0};
        pts[2] = (nd_point){cx, y1};
        (void)nd_draw_polygon(d, pts, 3u, c);
        break;
    case 8: /* plus */
        t = nd_max32(2, w / 4);
        (void)nd_draw_rect_fill(d, ND_RECT(cx - t / 2, y0, cx + t / 2, y1), c);
        (void)nd_draw_rect_fill(d, ND_RECT(x0, cy - t / 2, x1, cy + t / 2), c);
        break;
    case 9: /* X -- the only DIAGONAL wide line in the whole overlay */
        (void)nd_draw_line(d, x0, y0, x1, y1, c, 2);
        (void)nd_draw_line(d, x0, y1, x1, y0, c, 2);
        break;
    case 10: /* star (4-point) */
        q = nd_max32(1, w / 6);
        pts[0] = (nd_point){cx, y0};
        pts[1] = (nd_point){cx + q, cy - q};
        pts[2] = (nd_point){x1, cy};
        pts[3] = (nd_point){cx + q, cy + q};
        pts[4] = (nd_point){cx, y1};
        pts[5] = (nd_point){cx - q, cy + q};
        pts[6] = (nd_point){x0, cy};
        pts[7] = (nd_point){cx - q, cy - q};
        (void)nd_draw_polygon(d, pts, 8u, c);
        break;
    case 11: /* heart */
        r = nd_max32(2, w / 4);
        (void)nd_draw_ellipse_fill(d, ND_RECT(x0, y0, x0 + 2 * r, y0 + 2 * r), c);
        (void)nd_draw_ellipse_fill(d, ND_RECT(x1 - 2 * r, y0, x1, y0 + 2 * r), c);
        pts[0] = (nd_point){x0, y0 + r};
        pts[1] = (nd_point){x1, y0 + r};
        pts[2] = (nd_point){cx, y1};
        (void)nd_draw_polygon(d, pts, 3u, c);
        break;
    case 12: /* horizontal bars */
        for (yy = y0; yy <= y1; yy += 4)
            (void)nd_draw_line(d, x0, yy, x1, yy, c, 2);
        break;
    case 13: /* vertical bars */
        for (xx = x0; xx <= x1; xx += 4)
            (void)nd_draw_line(d, xx, y0, xx, y1, c, 2);
        break;
    case 14: /* checker -- range(y0, y1, step) EXCLUDES y1 */
        step = nd_max32(3, w / 3);
        for (i = 0, yy = y0; yy < y1; yy += step, i++) {
            for (j = 0, xx = x0; xx < x1; xx += step, j++) {
                if ((i + j) % 2 == 0) {
                    (void)nd_draw_rect_fill(d,
                                            ND_RECT(xx, yy, nd_min32(xx + step - 1, x1),
                                                    nd_min32(yy + step - 1, y1)),
                                            c);
                }
            }
        }
        break;
    case 15: /* hourglass -- self-intersecting; see the header comment */
        pts[0] = (nd_point){x0, y0};
        pts[1] = (nd_point){x1, y0};
        pts[2] = (nd_point){x0, y1};
        pts[3] = (nd_point){x1, y1};
        (void)nd_draw_polygon(d, pts, 4u, c);
        break;
    case 16: /* arrow up */
        t = nd_max32(2, w / 4);
        pts[0] = (nd_point){cx, y0};
        pts[1] = (nd_point){x1, cy};
        pts[2] = (nd_point){x0, cy};
        (void)nd_draw_polygon(d, pts, 3u, c);
        (void)nd_draw_rect_fill(d, ND_RECT(cx - t / 2, cy, cx + t / 2, y1), c);
        break;
    case 17: /* arrow right */
        t = nd_max32(2, w / 4);
        pts[0] = (nd_point){x1, cy};
        pts[1] = (nd_point){cx, y0};
        pts[2] = (nd_point){cx, y1};
        (void)nd_draw_polygon(d, pts, 3u, c);
        (void)nd_draw_rect_fill(d, ND_RECT(x0, cy - t / 2, cx, cy + t / 2), c);
        break;
    case 18: /* corners */
        r = nd_max32(2, w / 3);
        (void)nd_draw_rect_fill(d, ND_RECT(x0, y0, x0 + r, y0 + r), c);
        (void)nd_draw_rect_fill(d, ND_RECT(x1 - r, y0, x1, y0 + r), c);
        (void)nd_draw_rect_fill(d, ND_RECT(x0, y1 - r, x0 + r, y1), c);
        (void)nd_draw_rect_fill(d, ND_RECT(x1 - r, y1 - r, x1, y1), c);
        break;
    default: /* 19, and the Python's trailing `else` for anything else */
        (void)nd_draw_rect_outline(d, box, c, 1);
        r = nd_max32(1, w / 5);
        (void)nd_draw_ellipse_fill(d, ND_RECT(cx - r, cy - r, cx + r, cy + r), c);
        break;
    }
}

/* ------------------------------------------------------------------ *
 * The game
 * ------------------------------------------------------------------ */

void nd_memory_init(nd_memory *g, nd_ui *ui)
{
    int32_t usable_h;
    int32_t i;

    if (g == NULL)
        return;
    memset(g, 0, sizeof *g);

    g->ui = ui;
    g->screen_w = nd_ui_width(ui);
    g->screen_h = nd_ui_height(ui);
    g->softkey_h = nd_ui_softkey_height(ui);
    g->content_bottom = nd_games_content_bottom(ui);

    /* On the shipped 240x175 panel: usable_h 137, cell min(29, 27) = 27,
     * board_x 12, board_y 5. */
    usable_h = g->content_bottom - 8;
    g->cell = nd_min32((g->screen_w - 8) / ND_MEMORY_COLS, usable_h / ND_MEMORY_ROWS);
    g->board_x = (g->screen_w - ND_MEMORY_COLS * g->cell) / 2;
    g->board_y = (g->content_bottom - ND_MEMORY_ROWS * g->cell) / 2;

    /* `list(range(20)) * 2` -- 0..19 then 0..19 again, THEN shuffled. */
    for (i = 0; i < ND_MEMORY_CARDS; i++)
        g->cards[i] = i % ND_MEMORY_KINDS;
    for (i = 0; i < ND_MEMORY_CARDS; i++)
        g->state[i] = ND_MEMORY_DOWN;

    nd_rand_seed((uint32_t)nd_time_now());
    nd_rand_shuffle(g->cards, (size_t)ND_MEMORY_CARDS, sizeof g->cards[0]);

    g->cursor_col = 0;
    g->cursor_row = 0;
    g->first_pick = -1; /* None */
    g->score = 0;
    g->misses = 0;
}

nd_rect nd_memory_card_rect(const nd_memory *g, int32_t col, int32_t row)
{
    int32_t px = g->board_x + col * g->cell;
    int32_t py = g->board_y + row * g->cell;

    return ND_RECT(px + 2, py + 2, px + g->cell - 3, py + g->cell - 3);
}

void nd_memory_render(nd_memory *g)
{
    int32_t row;
    int32_t col;
    int32_t px;
    int32_t py;
    int32_t idx;

    if (g == NULL || g->ui == NULL || g->ui->draw == NULL)
        return;

    (void)nd_draw_rect_fill(g->ui->draw, ND_RECT(0, 0, g->screen_w, g->screen_h), ND_BLACK);

    for (row = 0; row < ND_MEMORY_ROWS; row++) {
        for (col = 0; col < ND_MEMORY_COLS; col++) {
            nd_rect rect = nd_memory_card_rect(g, col, row);

            idx = row * ND_MEMORY_COLS + col;
            if (g->state[idx] == ND_MEMORY_DOWN) {
                (void)nd_draw_rect_fill(g->ui->draw, rect, ND_WHITE);
            } else if (g->state[idx] == ND_MEMORY_UP) {
                (void)nd_draw_rect_outline(g->ui->draw, rect, ND_WHITE, 1);
                nd_memory_draw_glyph(g->ui->draw,
                                     ND_RECT(rect.x0 + 4, rect.y0 + 4, rect.x1 - 4, rect.y1 - 4),
                                     g->cards[idx], ND_WHITE);
            }
            /* "gone" draws nothing at all. */
        }
    }

    /* The cursor ring lives in the 2 px gutter around the current cell. */
    px = g->board_x + g->cursor_col * g->cell;
    py = g->board_y + g->cursor_row * g->cell;
    (void)nd_draw_rect_outline(g->ui->draw, ND_RECT(px, py, px + g->cell - 1, py + g->cell - 1),
                               ND_WHITE, 1);

    idx = g->cursor_row * ND_MEMORY_COLS + g->cursor_col;
    if (g->state[idx] == ND_MEMORY_DOWN) {
        (void)nd_draw_rect_outline(g->ui->draw, nd_memory_card_rect(g, g->cursor_col, g->cursor_row),
                                   ND_BLACK, 1);
    }

    (void)nd_ui_present(g->ui);
}

void nd_memory_move_cursor(nd_memory *g, int32_t dx, int32_t dy)
{
    if (g == NULL)
        return;
    /* Python's `%` on a negative left operand is non-negative; C's is not,
     * so moving left from column 0 needs ((v % n) + n) % n. */
    g->cursor_col = (((g->cursor_col + dx) % ND_MEMORY_COLS) + ND_MEMORY_COLS) % ND_MEMORY_COLS;
    g->cursor_row = (((g->cursor_row + dy) % ND_MEMORY_ROWS) + ND_MEMORY_ROWS) % ND_MEMORY_ROWS;
}

void nd_memory_flip(nd_memory *g)
{
    int32_t idx;
    int32_t first;

    if (g == NULL)
        return;
    idx = g->cursor_row * ND_MEMORY_COLS + g->cursor_col;
    if (g->state[idx] != ND_MEMORY_DOWN)
        return;

    g->state[idx] = ND_MEMORY_UP;
    if (g->first_pick < 0) {
        g->first_pick = idx;
        nd_memory_render(g);
        return;
    }

    first = g->first_pick;
    g->first_pick = -1;
    /* Both cards are face up in THIS frame; the pause below is what lets the
     * player see the second one before it is judged. */
    nd_memory_render(g);

    if (g->cards[first] == g->cards[idx]) {
        g->score += nd_max32(ND_MEMORY_PAIR_MIN,
                             ND_MEMORY_PAIR_BASE - ND_MEMORY_MISS_PENALTY * g->misses);
        g->misses = 0;
        nd_games_dwell(0.25);
        g->state[first] = ND_MEMORY_GONE;
        g->state[idx] = ND_MEMORY_GONE;
    } else {
        g->misses++;
        nd_games_dwell(ND_MEMORY_REVEAL_SECS);
        g->state[first] = ND_MEMORY_DOWN;
        g->state[idx] = ND_MEMORY_DOWN;
    }
    nd_memory_render(g);
}

bool nd_memory_finished(const nd_memory *g)
{
    int32_t i;

    if (g == NULL)
        return false;
    for (i = 0; i < ND_MEMORY_CARDS; i++) {
        if (g->state[i] != ND_MEMORY_GONE)
            return false;
    }
    return true;
}

int32_t nd_memory_play(nd_memory *g)
{
    if (g == NULL)
        return -1;

    nd_memory_render(g);

    for (;;) {
        int32_t key = nd_games_poll_key(g->ui, 0.1);
        int32_t dx = 0;
        int32_t dy = 0;

        /* Not in the Python. A SIGTERM mid-game leaves without recording a
         * score, exactly as Back does. */
        if (nd_app_should_exit())
            return -1;

        if (key == ND_KEY_NONE)
            continue;

        if (key == ND_GAMES_KEY_BACK)
            return -1; /* `return None` */

        if (nd_games_dir_for_key(key, &dx, &dy)) {
            nd_memory_move_cursor(g, dx, dy);
            nd_memory_render(g);
        } else if (key == ND_GAMES_KEY_ENTER || key == ND_GAMES_KEY_NUM_5) {
            nd_memory_flip(g);
            if (nd_memory_finished(g)) {
                nd_games_dwell(0.3);
                return g->score;
            }
        }
    }
}

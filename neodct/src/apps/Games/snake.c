/* apps/Games/snake.c -- Nokia-style Snake.
 *
 * A one-to-one port of System/apps/Games/snake.py (133 lines): a bordered
 * 29x14 playfield of 8 px cells, the score in 18 px type at (4,1), and speed
 * and points that both scale with the chosen level.
 *
 * golden/game-snake.png is the FIRST frame of a new game -- render() runs
 * before the loop starts, and under capture the clock only advances when a
 * frame is committed, so the first tick never falls due. The reference is
 * therefore the three-cell snake at (13,7),(14,7),(15,7), the score "0", the
 * border, and one food cell wherever the generator put it.
 *
 * ============ THE FOOD CELL IS THE ONE RANDOM PIXEL ============
 *
 * `random.seed(time.time())` then `random.choice(open_cells)`. Decision 4
 * says not to reimplement MT19937, so this uses libneodct's pinned LCG and
 * the reference frame is re-cut from the C build (frame class `recut`). The
 * seed is still the clock, so under capture it is the virtual clock and the
 * cell is the same on every machine and every run.
 *
 * open_cells is built X-MAJOR -- `for x in range(GRID_W) for y in
 * range(GRID_H)` -- and the order decides which cell a given index picks, so
 * it is reproduced even though the C generator draws different indices.
 *
 * ============ NO SOFTKEY IS EVER DRAWN DURING PLAY ============
 *
 * render() clears (0, 0, screen_w, screen_h) -- the WHOLE screen, softkey
 * band included -- and draws nothing below the board. The bottom 30 rows
 * stay black for the entire game, and game-snake.png has no ink below y=137.
 *
 * ============ THE TICK IS POLLED, NOT SLEPT ============
 *
 * play() waits at most 50 ms at a time (`min(timeout, 0.05)`), so a turn
 * queued 5 ms after a tick is still acted on at the next one and the game
 * never feels like it dropped a key. The cost is a poll every 50 ms even
 * when the tick is 367 ms away, which is what the Python does and what the
 * hardware was measured against.
 */

#include <string.h>

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_keycodes.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"

#include "games.h"

double nd_snake_tick_delay(int32_t level)
{
    double d = 0.40 - 0.033 * (double)level;

    return (d > 0.09) ? d : 0.09;
}

static bool same_cell(nd_point a, nd_point b)
{
    return a.x == b.x && a.y == b.y;
}

nd_rect nd_snake_cell_rect(const nd_snake *g, int32_t x, int32_t y)
{
    int32_t px = g->board_x + x * ND_SNAKE_CELL;
    int32_t py = g->board_y + y * ND_SNAKE_CELL;

    return ND_RECT(px + 1, py + 1, px + ND_SNAKE_CELL - 2, py + ND_SNAKE_CELL - 2);
}

void nd_snake_spawn_food(nd_snake *g)
{
    int32_t open_x[ND_SNAKE_MAX_LEN];
    int32_t open_y[ND_SNAKE_MAX_LEN];
    int32_t n = 0;
    int32_t x;
    int32_t y;
    int32_t pick;

    if (g == NULL)
        return;

    /* `[(x, y) for x in range(GRID_W) for y in range(GRID_H) if (x, y) not
     * in body]` -- x outer, y inner. The order is what a generator index
     * selects from, so it is load-bearing even here. */
    for (x = 0; x < ND_SNAKE_GRID_W; x++) {
        for (y = 0; y < ND_SNAKE_GRID_H; y++) {
            nd_point c = {x, y};
            size_t i;
            bool taken = false;

            for (i = 0u; i < g->n_body; i++) {
                if (same_cell(g->body[i], c)) {
                    taken = true;
                    break;
                }
            }
            if (!taken) {
                open_x[n] = x;
                open_y[n] = y;
                n++;
            }
        }
    }

    if (n == 0) {
        g->has_food = false; /* `self.food = None` */
        return;
    }

    pick = nd_rand_below(n);
    g->food.x = open_x[pick];
    g->food.y = open_y[pick];
    g->has_food = true;
}

void nd_snake_init(nd_snake *g, nd_ui *ui, int32_t level)
{
    int32_t cx;
    int32_t cy;

    if (g == NULL)
        return;
    memset(g, 0, sizeof *g);

    g->ui = ui;
    g->level = nd_clamp32(level, 1, 9); /* max(1, min(9, int(level))) */

    g->screen_w = nd_ui_width(ui);
    g->screen_h = nd_ui_height(ui);
    g->softkey_h = nd_ui_softkey_height(ui);
    /* Read by the Python and never used by it; kept so the two constructors
     * line up field for field. */
    g->content_bottom = nd_games_content_bottom(ui);

    g->score_h = 20;
    g->board_x = (g->screen_w - ND_SNAKE_GRID_W * ND_SNAKE_CELL) / 2;
    g->board_y = g->score_h + 4;
    g->board_w = ND_SNAKE_GRID_W * ND_SNAKE_CELL;
    g->board_h = ND_SNAKE_GRID_H * ND_SNAKE_CELL;

    cx = ND_SNAKE_GRID_W / 2; /* 14 */
    cy = ND_SNAKE_GRID_H / 2; /*  7 */
    g->body[0].x = cx + 1;
    g->body[0].y = cy;
    g->body[1].x = cx;
    g->body[1].y = cy;
    g->body[2].x = cx - 1;
    g->body[2].y = cy;
    g->n_body = 3u;

    g->direction.x = 1;
    g->direction.y = 0;
    g->n_turns = 0u;
    g->score = 0;

    /* random.seed(time.time()) -- the clock, which is the virtual one under
     * capture. See the header comment and OPEN-QUESTIONS.md decision 4. */
    nd_rand_seed((uint32_t)nd_time_now());
    nd_snake_spawn_food(g);
}

void nd_snake_queue_turn(nd_snake *g, int32_t dx, int32_t dy)
{
    nd_point last;

    if (g == NULL)
        return;
    last = (g->n_turns > 0u) ? g->turn_queue[g->n_turns - 1u] : g->direction;

    /* No 180-degree turns: a new direction that cancels the last one out. */
    if (dx + last.x == 0 && dy + last.y == 0)
        return;
    if (dx == last.x && dy == last.y)
        return;
    if (g->n_turns >= ND_SNAKE_TURN_QUEUE)
        return;

    g->turn_queue[g->n_turns].x = dx;
    g->turn_queue[g->n_turns].y = dy;
    g->n_turns++;
}

bool nd_snake_step(nd_snake *g)
{
    nd_point head;
    nd_point next;
    bool drop_tail;
    size_t i;
    size_t limit;

    if (g == NULL || g->n_body == 0u)
        return false;

    if (g->n_turns > 0u) {
        /* turn_queue.pop(0) -- from the FRONT, so two turns queued in one
         * tick are taken in the order they were pressed. */
        g->direction = g->turn_queue[0];
        if (g->n_turns > 1u)
            g->turn_queue[0] = g->turn_queue[1];
        g->n_turns--;
    }

    head = g->body[0];
    next.x = head.x + g->direction.x;
    next.y = head.y + g->direction.y;

    if (next.x < 0 || next.x >= ND_SNAKE_GRID_W || next.y < 0 || next.y >= ND_SNAKE_GRID_H)
        return false; /* wall */

    /* `set(self.snake) - ({tail} if new_head != self.food else set())`: the
     * tail cell frees up this tick UNLESS we are about to grow into food, in
     * which case it stays put and turning into it is death. */
    drop_tail = !(g->has_food && same_cell(next, g->food));
    limit = drop_tail ? g->n_body - 1u : g->n_body;
    for (i = 0u; i < limit; i++) {
        if (same_cell(g->body[i], next))
            return false; /* bit itself */
    }

    /* snake.insert(0, new_head) */
    if (g->n_body >= ND_SNAKE_MAX_LEN)
        return false; /* unreachable: a full board is a self-collision first */
    memmove(&g->body[1], &g->body[0], g->n_body * sizeof g->body[0]);
    g->body[0] = next;
    g->n_body++;

    if (g->has_food && same_cell(next, g->food)) {
        g->score += g->level;
        nd_snake_spawn_food(g);
    } else {
        g->n_body--; /* snake.pop() */
    }
    return true;
}

void nd_snake_render(nd_snake *g)
{
    char score[16];
    size_t i;

    if (g == NULL || g->ui == NULL || g->ui->draw == NULL)
        return;

    /* The WHOLE screen, softkey band included. */
    (void)nd_draw_rect_fill(g->ui->draw, ND_RECT(0, 0, g->screen_w, g->screen_h), ND_BLACK);

    (void)nd_snprintf(score, sizeof score, "%d", g->score);
    (void)nd_draw_text(g->ui->draw, 4, 1, score, g->ui->font_md, ND_WHITE);

    (void)nd_draw_rect_outline(g->ui->draw,
                               ND_RECT(g->board_x - 2, g->board_y - 2, g->board_x + g->board_w + 1,
                                       g->board_y + g->board_h + 1),
                               ND_WHITE, 1);

    if (g->has_food) {
        (void)nd_draw_rect_outline(g->ui->draw, nd_snake_cell_rect(g, g->food.x, g->food.y),
                                   ND_WHITE, 1);
    }

    for (i = 0u; i < g->n_body; i++) {
        (void)nd_draw_rect_fill(g->ui->draw, nd_snake_cell_rect(g, g->body[i].x, g->body[i].y),
                                ND_WHITE);
    }

    (void)nd_ui_present(g->ui);
}

int32_t nd_snake_play(nd_snake *g)
{
    double next_move;

    if (g == NULL)
        return -1;

    nd_snake_render(g);
    next_move = nd_time_now() + nd_snake_tick_delay(g->level);

    for (;;) {
        double timeout = next_move - nd_time_now();
        int32_t key;
        int32_t dx = 0;
        int32_t dy = 0;

        if (timeout < 0.0)
            timeout = 0.0;
        if (timeout > 0.05)
            timeout = 0.05; /* min(timeout, 0.05) */

        key = nd_games_poll_key(g->ui, timeout);
        if (key != ND_KEY_NONE) {
            if (key == ND_GAMES_KEY_BACK)
                return -1; /* `return None` -- quit, no score */
            if (nd_games_dir_for_key(key, &dx, &dy))
                nd_snake_queue_turn(g, dx, dy);
        }

        /* Not in the Python. A SIGTERM during a game leaves without
         * recording a score, which is what Back does and is the safer of the
         * two: a call arriving mid-game must not overwrite a top score. */
        if (nd_app_should_exit())
            return -1;

        if (nd_time_now() >= next_move) {
            if (!nd_snake_step(g)) {
                nd_snake_render(g);
                nd_games_dwell(0.4);
                return g->score;
            }
            nd_snake_render(g);
            next_move = nd_time_now() + nd_snake_tick_delay(g->level);
        }
    }
}

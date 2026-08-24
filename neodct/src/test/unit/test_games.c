/* test_games.c -- the Games app, app id 6: Snake, Memory and the three menus.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. The strings are the Python's, INCLUDING THE ORDER OF THE ROOT MENU.
 *     `["Memory", "Snake"]` is not alphabetical and is not the order the
 *     source file lists the two games in; index 0 is Memory, and every key
 *     script in shoot_docs.py counts on it. Getting it backwards would send
 *     the Down-then-Enter recipe into the wrong game and would still render
 *     a plausible-looking frame.
 *
 *  2. DIR_KEYS maps exactly eight codes and nothing else -- the four arrows
 *     and 2/4/6/8 -- and key 5, which turns a card in Memory, is NOT one of
 *     them.
 *
 *  3. tick_delay(level) is `max(0.09, 0.40 - 0.033 * level)` at every level
 *     the game can be set to, and the 0.09 floor is unreachable for 1..9.
 *     Level is clamped to 1..9 in the constructor.
 *
 *  4. Snake's geometry is the derived 4/24/232/112 board with 6x6 cells, and
 *     the opening position is the three cells (15,7),(14,7),(13,7) with the
 *     head first.
 *
 *  5. queue_turn refuses a 180, refuses a repeat of the last queued turn,
 *     and holds at most two; step() takes them from the FRONT.
 *
 *  6. step() dies on a wall and on its own body, grows and scores `level`
 *     points on food, and -- the one that is easy to get wrong -- lets the
 *     snake move INTO the cell its own tail is vacating, because the tail
 *     frees up on the same tick unless the head is landing on food.
 *
 *  7. spawn_food() draws from the open cells in X-MAJOR order, never puts
 *     food under the snake, and clears has_food on a full board.
 *
 *  8. Memory's geometry is cell 27 at (12,5) with 23x23 cards, the deck is
 *     twenty kinds twice over after the shuffle, and the cursor wraps with
 *     PYTHON's modulo -- left from column 0 is column 7, not -1.
 *
 *  9. flip() scores max(2, 10 - 2*misses), zeroes the miss counter on a
 *     match, and puts a mismatched pair back face down.
 *
 * 10. All twenty glyphs draw inside their box and nothing outside it, and
 *     kind 15 -- the self-intersecting quadrilateral -- comes out as an
 *     HOURGLASS rather than a filled bow tie. That shape is the reason
 *     nd_draw.h promises Pillow's parity rule, so it is checked by shape and
 *     not merely by "some ink appeared".
 *
 * 11. THE THREE GOLDEN FRAMES. app-games is `exact` against the Python.
 *     game-snake and game-memory are frame class `recut` (OPEN-QUESTIONS.md
 *     decision 4): the reference for game-snake was re-captured from this
 *     build because the food cell comes out of libneodct's pinned LCG rather
 *     than CPython's MT19937. game-memory needed no recut -- every card is
 *     face down in that frame, so the shuffle does not reach a pixel -- and
 *     it is still judged against the Python's own reference here.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_settings.h"
#include "nd_vclock.h"

#include "smallapp_test.h"

#include "../../apps/Games/games.h"

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);

    bool (*dir_for_key)(int32_t, int32_t *, int32_t *);
    int64_t (*setting_int)(const char *, int64_t);
    bool (*setting_set)(const char *, int64_t);
    int32_t (*content_bottom)(const nd_ui *);

    double (*tick_delay)(int32_t);
    void (*snake_init)(nd_snake *, nd_ui *, int32_t);
    void (*snake_spawn_food)(nd_snake *);
    void (*snake_queue_turn)(nd_snake *, int32_t, int32_t);
    bool (*snake_step)(nd_snake *);
    nd_rect (*snake_cell_rect)(const nd_snake *, int32_t, int32_t);
    void (*snake_render)(nd_snake *);

    void (*memory_init)(nd_memory *, nd_ui *);
    nd_rect (*memory_card_rect)(const nd_memory *, int32_t, int32_t);
    void (*memory_render)(nd_memory *);
    void (*memory_move_cursor)(nd_memory *, int32_t, int32_t);
    void (*memory_flip)(nd_memory *);
    bool (*memory_finished)(const nd_memory *);
    void (*memory_draw_glyph)(nd_draw *, nd_rect, int32_t, nd_color);

    const char *const *root_menu;
    const char *const *snake_menu;
    const char *const *memory_menu;
    const char *const *snake_instructions;
    const char *const *memory_instructions;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");

    *(void **)&api.dir_for_key = sa_sym(h, "nd_games_dir_for_key");
    *(void **)&api.setting_int = sa_sym(h, "nd_games_setting_int");
    *(void **)&api.setting_set = sa_sym(h, "nd_games_setting_set");
    *(void **)&api.content_bottom = sa_sym(h, "nd_games_content_bottom");

    *(void **)&api.tick_delay = sa_sym(h, "nd_snake_tick_delay");
    *(void **)&api.snake_init = sa_sym(h, "nd_snake_init");
    *(void **)&api.snake_spawn_food = sa_sym(h, "nd_snake_spawn_food");
    *(void **)&api.snake_queue_turn = sa_sym(h, "nd_snake_queue_turn");
    *(void **)&api.snake_step = sa_sym(h, "nd_snake_step");
    *(void **)&api.snake_cell_rect = sa_sym(h, "nd_snake_cell_rect");
    *(void **)&api.snake_render = sa_sym(h, "nd_snake_render");

    *(void **)&api.memory_init = sa_sym(h, "nd_memory_init");
    *(void **)&api.memory_card_rect = sa_sym(h, "nd_memory_card_rect");
    *(void **)&api.memory_render = sa_sym(h, "nd_memory_render");
    *(void **)&api.memory_move_cursor = sa_sym(h, "nd_memory_move_cursor");
    *(void **)&api.memory_flip = sa_sym(h, "nd_memory_flip");
    *(void **)&api.memory_finished = sa_sym(h, "nd_memory_finished");
    *(void **)&api.memory_draw_glyph = sa_sym(h, "nd_memory_draw_glyph");

    api.root_menu = (const char *const *)sa_sym(h, "nd_games_root_menu");
    api.snake_menu = (const char *const *)sa_sym(h, "nd_games_snake_menu");
    api.memory_menu = (const char *const *)sa_sym(h, "nd_games_memory_menu");
    api.snake_instructions = (const char *const *)sa_sym(h, "nd_games_snake_instructions");
    api.memory_instructions = (const char *const *)sa_sym(h, "nd_games_memory_instructions");

    return api.run != NULL && api.shutdown != NULL && api.dir_for_key != NULL &&
           api.setting_int != NULL && api.setting_set != NULL && api.content_bottom != NULL &&
           api.tick_delay != NULL && api.snake_init != NULL && api.snake_spawn_food != NULL &&
           api.snake_queue_turn != NULL && api.snake_step != NULL && api.snake_cell_rect != NULL &&
           api.snake_render != NULL && api.memory_init != NULL && api.memory_card_rect != NULL &&
           api.memory_render != NULL && api.memory_move_cursor != NULL && api.memory_flip != NULL &&
           api.memory_finished != NULL && api.memory_draw_glyph != NULL && api.root_menu != NULL &&
           api.snake_menu != NULL && api.memory_menu != NULL && api.snake_instructions != NULL &&
           api.memory_instructions != NULL;
}

/* ------------------------------------------------------------------ *
 * 1. The strings
 * ------------------------------------------------------------------ */

static void test_strings(void)
{
    /* MEMORY IS FIRST. See the header comment. */
    CHECK_STR(api.root_menu[0], "Memory", "root menu item 0");
    CHECK_STR(api.root_menu[1], "Snake", "root menu item 1");

    CHECK_STR(api.snake_menu[0], "New game", "snake menu 0");
    CHECK_STR(api.snake_menu[1], "Level", "snake menu 1");
    CHECK_STR(api.snake_menu[2], "Top score", "snake menu 2");
    CHECK_STR(api.snake_menu[3], "Instructions", "snake menu 3");

    /* Memory has no Level entry: its board is always 8x5. */
    CHECK_STR(api.memory_menu[0], "New game", "memory menu 0");
    CHECK_STR(api.memory_menu[1], "Top score", "memory menu 1");
    CHECK_STR(api.memory_menu[2], "Instructions", "memory menu 2");

    CHECK_STR(*api.snake_instructions,
              "Feed the snake by steering it to the food. Every bite makes it grow "
              "longer. Use keys 2, 4, 6 and 8 to change direction. The game ends if "
              "the snake runs into the walls or into its own body. A higher level "
              "means more speed and more points for each bite.",
              "SNAKE_INSTRUCTIONS");
    CHECK_STR(*api.memory_instructions,
              "All the cards lie face down. Move the cursor with keys 2, 4, 6 and 8 "
              "and turn a card over with key 5. Two matching cards are cleared from "
              "the board. Find every pair to finish the game. The fewer tries you "
              "need, the higher your score.",
              "MEMORY_INSTRUCTIONS");

    /* Neither string carries a newline: TextScroller does the wrapping, and
     * an embedded '\n' would cost a whole line of a page rather than 8 px. */
    CHECK(strchr(*api.snake_instructions, '\n') == NULL, "SNAKE_INSTRUCTIONS is one line");
    CHECK(strchr(*api.memory_instructions, '\n') == NULL, "MEMORY_INSTRUCTIONS is one line");
}

/* ------------------------------------------------------------------ *
 * 2. DIR_KEYS
 * ------------------------------------------------------------------ */

static void expect_dir(int32_t key, int32_t want_dx, int32_t want_dy, const char *what)
{
    int32_t dx = 999;
    int32_t dy = 999;

    if (!api.dir_for_key(key, &dx, &dy)) {
        CHECK(false, what);
        return;
    }
    CHECK_INT(dx, want_dx, what);
    CHECK_INT(dy, want_dy, what);
}

static void test_dir_keys(void)
{
    int32_t dx = 7;
    int32_t dy = 7;
    int32_t key;

    expect_dir(ND_GAMES_KEY_UP, 0, -1, "KEY_UP");
    expect_dir(ND_GAMES_KEY_NUM_2, 0, -1, "'2'");
    expect_dir(ND_GAMES_KEY_DOWN, 0, 1, "KEY_DOWN");
    expect_dir(ND_GAMES_KEY_NUM_8, 0, 1, "'8'");
    expect_dir(ND_GAMES_KEY_LEFT, -1, 0, "KEY_LEFT");
    expect_dir(ND_GAMES_KEY_NUM_4, -1, 0, "'4'");
    expect_dir(ND_GAMES_KEY_RIGHT, 1, 0, "KEY_RIGHT");
    expect_dir(ND_GAMES_KEY_NUM_6, 1, 0, "'6'");

    /* '5' turns a card in Memory. If it steered as well, every flip would
     * also move the cursor. */
    CHECK(!api.dir_for_key(ND_GAMES_KEY_NUM_5, &dx, &dy), "'5' does not steer");
    CHECK(!api.dir_for_key(ND_GAMES_KEY_ENTER, &dx, &dy), "Enter does not steer");
    CHECK(!api.dir_for_key(ND_GAMES_KEY_BACK, &dx, &dy), "Back does not steer");
    CHECK_INT(dx, 7, "a rejected key leaves *dx alone");
    CHECK_INT(dy, 7, "a rejected key leaves *dy alone");

    /* Exactly eight codes in the whole evdev range, and no others. */
    {
        int32_t n = 0;

        for (key = 0; key < 256; key++) {
            if (api.dir_for_key(key, NULL, NULL))
                n++;
        }
        CHECK_INT(n, 8, "DIR_KEYS has exactly eight entries");
    }
}

/* ------------------------------------------------------------------ *
 * 3. tick_delay
 * ------------------------------------------------------------------ */

static void test_tick_delay(void)
{
    /* spec-apps-core.md section 5c's table, recomputed rather than copied so
     * that a typo in one of them shows up as a disagreement. */
    int32_t level;

    for (level = 1; level <= 9; level++) {
        double want = 0.40 - 0.033 * (double)level;

        CHECK(want > 0.09, "the 0.09 floor is unreachable for levels 1..9");
        CHECK_DBL(api.tick_delay(level), want, "tick_delay(level)");
    }
    /* The floor exists and does bite outside the range the UI can produce. */
    CHECK_DBL(api.tick_delay(20), 0.09, "tick_delay clamps at 0.09");
}

/* ------------------------------------------------------------------ *
 * 4-7. Snake
 * ------------------------------------------------------------------ */

static void test_snake_geometry(sa_fixture *fx)
{
    nd_snake g;
    nd_rect r;

    api.snake_init(&g, &fx->ui, 5);

    CHECK_INT(g.level, 5, "level");
    CHECK_INT(g.score_h, 20, "score_h");
    CHECK_INT(g.board_x, 4, "board_x = (240 - 29*8) / 2");
    CHECK_INT(g.board_y, 24, "board_y = score_h + 4");
    CHECK_INT(g.board_w, 232, "board_w");
    CHECK_INT(g.board_h, 112, "board_h");
    CHECK_INT(g.content_bottom, 145, "content_bottom");

    /* A 6x6 inclusive box inside each 8 px cell: 1 px of gutter all round. */
    r = api.snake_cell_rect(&g, 0, 0);
    CHECK_INT(r.x0, 5, "cell(0,0).x0");
    CHECK_INT(r.y0, 25, "cell(0,0).y0");
    CHECK_INT(r.x1, 10, "cell(0,0).x1");
    CHECK_INT(r.y1, 30, "cell(0,0).y1");

    r = api.snake_cell_rect(&g, 28, 13);
    CHECK_INT(r.x1, 4 + 28 * 8 + 6, "cell(28,13).x1");
    CHECK_INT(r.y1, 24 + 13 * 8 + 6, "cell(28,13).y1");
    /* The last cell's box stays inside the border at (2,22)-(237,137). */
    CHECK(r.x1 < 237 && r.y1 < 137, "the last cell is inside the border");

    /* max(1, min(9, level)) */
    api.snake_init(&g, &fx->ui, 0);
    CHECK_INT(g.level, 1, "level 0 clamps up");
    api.snake_init(&g, &fx->ui, 99);
    CHECK_INT(g.level, 9, "level 99 clamps down");
}

static void test_snake_start(sa_fixture *fx)
{
    nd_snake g;

    api.snake_init(&g, &fx->ui, 5);

    CHECK_INT((int)g.n_body, 3, "three cells");
    CHECK_INT(g.body[0].x, 15, "head x");
    CHECK_INT(g.body[0].y, 7, "head y");
    CHECK_INT(g.body[1].x, 14, "middle x");
    CHECK_INT(g.body[2].x, 13, "tail x");
    CHECK_INT(g.direction.x, 1, "heading right");
    CHECK_INT(g.direction.y, 0, "heading right");
    CHECK_INT((int)g.n_turns, 0, "no queued turns");
    CHECK_INT(g.score, 0, "score starts at 0");
    CHECK(g.has_food, "a new game has food");
}

static void test_snake_turns(sa_fixture *fx)
{
    nd_snake g;

    api.snake_init(&g, &fx->ui, 5);

    /* Heading right; a left turn would be a 180 and is refused. */
    api.snake_queue_turn(&g, -1, 0);
    CHECK_INT((int)g.n_turns, 0, "no 180-degree turns");

    /* The same direction again is not a turn. */
    api.snake_queue_turn(&g, 1, 0);
    CHECK_INT((int)g.n_turns, 0, "a repeat of the current heading is dropped");

    api.snake_queue_turn(&g, 0, -1); /* up */
    CHECK_INT((int)g.n_turns, 1, "up is queued");
    /* Against the QUEUED turn, not the current heading: right would now be a
     * 90, and up again a repeat. */
    api.snake_queue_turn(&g, 0, -1);
    CHECK_INT((int)g.n_turns, 1, "a repeat of the queued turn is dropped");
    api.snake_queue_turn(&g, 0, 1);
    CHECK_INT((int)g.n_turns, 1, "a 180 against the queued turn is dropped");
    api.snake_queue_turn(&g, -1, 0);
    CHECK_INT((int)g.n_turns, 2, "left after up is queued");
    api.snake_queue_turn(&g, 0, 1);
    CHECK_INT((int)g.n_turns, 2, "the queue holds at most two");

    /* pop(0): the FIRST queued turn is the one taken. */
    g.has_food = false;
    CHECK(api.snake_step(&g), "step");
    CHECK_INT(g.direction.x, 0, "took the up turn first");
    CHECK_INT(g.direction.y, -1, "took the up turn first");
    CHECK_INT((int)g.n_turns, 1, "one turn left in the queue");
    CHECK(api.snake_step(&g), "step");
    CHECK_INT(g.direction.x, -1, "then the left turn");
    CHECK_INT((int)g.n_turns, 0, "queue drained");
}

static void test_snake_step(sa_fixture *fx)
{
    nd_snake g;
    int32_t i;

    /* --- a wall --- */
    api.snake_init(&g, &fx->ui, 5);
    g.has_food = false;
    for (i = 0; i < 13; i++)
        CHECK(api.snake_step(&g), "steps up to the wall");
    /* Head started at x=15 and the grid ends at 28, so the 14th step walks
     * off the right-hand edge. */
    CHECK(!api.snake_step(&g), "the wall kills");

    /* --- its own body --- *
     *
     * A FIVE-cell snake is the shortest that can bite a cell that is neither
     * its neck nor its tail, which is the only case the tail rule does not
     * already decide. The body runs (14,8) <- (15,8) <- (15,7) <- (14,7) <-
     * (13,7); turning up from (14,8) lands on (14,7), the fourth cell, with
     * (13,7) still occupied behind it. */
    api.snake_init(&g, &fx->ui, 5);
    g.has_food = false;
    g.body[0] = (nd_point){14, 8};
    g.body[1] = (nd_point){15, 8};
    g.body[2] = (nd_point){15, 7};
    g.body[3] = (nd_point){14, 7};
    g.body[4] = (nd_point){13, 7};
    g.n_body = 5u;
    g.direction = (nd_point){0, -1};
    g.n_turns = 0u;
    CHECK(!api.snake_step(&g), "biting its own body kills");

    /* --- the tail cell frees up on the same tick --- *
     *
     * The same five cells, but turning RIGHT from (14,8) onto (15,8), which
     * is the neck and is NOT vacated. Death, and the check that "the tail
     * frees up" has not been generalised into "any body cell frees up". */
    api.snake_init(&g, &fx->ui, 5);
    g.has_food = false;
    g.body[0] = (nd_point){14, 8};
    g.body[1] = (nd_point){15, 8};
    g.body[2] = (nd_point){15, 7};
    g.body[3] = (nd_point){14, 7};
    g.body[4] = (nd_point){13, 7};
    g.n_body = 5u;
    g.direction = (nd_point){1, 0};
    g.n_turns = 0u;
    CHECK(!api.snake_step(&g), "only the TAIL frees up, not the cell behind the head");

    /* The genuine tail-chase: head at (14,8) moving to (15,8), which is the
     * tail and is about to be vacated. */
    api.snake_init(&g, &fx->ui, 5);
    g.has_food = false;
    g.body[0] = (nd_point){14, 8};
    g.body[1] = (nd_point){14, 7};
    g.body[2] = (nd_point){15, 7};
    g.body[3] = (nd_point){15, 8};
    g.n_body = 4u;
    g.direction = (nd_point){1, 0};
    g.n_turns = 0u;
    CHECK(api.snake_step(&g), "moving into the vacating tail survives");
    CHECK_INT((int)g.n_body, 4, "length unchanged");

    /* --- food: grow, score += level, and the tail does NOT free up --- */
    api.snake_init(&g, &fx->ui, 7);
    g.body[0] = (nd_point){14, 8};
    g.body[1] = (nd_point){14, 7};
    g.body[2] = (nd_point){15, 7};
    g.body[3] = (nd_point){15, 8};
    g.n_body = 4u;
    g.direction = (nd_point){1, 0};
    g.n_turns = 0u;
    g.has_food = true;
    g.food = (nd_point){15, 8}; /* the tail cell, which now stays put */
    CHECK(!api.snake_step(&g), "food on the tail means the tail does not move, so this is death");

    api.snake_init(&g, &fx->ui, 7);
    g.body[0] = (nd_point){10, 7};
    g.body[1] = (nd_point){9, 7};
    g.body[2] = (nd_point){8, 7};
    g.n_body = 3u;
    g.direction = (nd_point){1, 0};
    g.n_turns = 0u;
    g.has_food = true;
    g.food = (nd_point){11, 7};
    CHECK(api.snake_step(&g), "eating survives");
    CHECK_INT((int)g.n_body, 4, "the snake grew");
    CHECK_INT(g.score, 7, "score += level");
    CHECK(g.has_food, "new food appeared");
    CHECK(!(g.food.x == 11 && g.food.y == 7), "the new food is not under the head");
}

static void test_snake_food(sa_fixture *fx)
{
    nd_snake g;
    int32_t seen[ND_SNAKE_GRID_W][ND_SNAKE_GRID_H];
    int32_t trial;
    size_t i;

    memset(seen, 0, sizeof seen);
    api.snake_init(&g, &fx->ui, 5);

    /* 400 draws over a fixed three-cell body: the food must never land on
     * the snake and must stay on the board. */
    for (trial = 0; trial < 400; trial++) {
        api.snake_spawn_food(&g);
        CHECK(g.has_food, "a board with 403 free cells always has food");
        if (!g.has_food)
            break;
        if (g.food.x < 0 || g.food.x >= ND_SNAKE_GRID_W || g.food.y < 0 ||
            g.food.y >= ND_SNAKE_GRID_H) {
            CHECK(false, "food is on the board");
            break;
        }
        for (i = 0u; i < g.n_body; i++) {
            if (g.body[i].x == g.food.x && g.body[i].y == g.food.y) {
                CHECK(false, "food is never under the snake");
                break;
            }
        }
        seen[g.food.x][g.food.y] = 1;
    }
    sa_checks++;

    /* A full board is `random.choice([])` guarded by `if open_cells`, which
     * is the Python's `self.food = None`. */
    {
        int32_t x;
        int32_t y;

        g.n_body = 0u;
        for (x = 0; x < ND_SNAKE_GRID_W; x++) {
            for (y = 0; y < ND_SNAKE_GRID_H; y++) {
                g.body[g.n_body].x = x;
                g.body[g.n_body].y = y;
                g.n_body++;
            }
        }
        api.snake_spawn_food(&g);
        CHECK(!g.has_food, "a full board has no food");
    }

    /* X-MAJOR order. With every cell free but one column, the index the
     * generator returns selects along y first -- so hide all of column 0
     * except (0,13) and leave column 1 empty, and check the reachable set
     * still contains (0,13). Cheaper and more direct: exactly two cells
     * free, and both must be reachable. */
    {
        int32_t x;
        int32_t y;
        bool got_a = false;
        bool got_b = false;

        g.n_body = 0u;
        for (x = 0; x < ND_SNAKE_GRID_W; x++) {
            for (y = 0; y < ND_SNAKE_GRID_H; y++) {
                if ((x == 0 && y == 13) || (x == 28 && y == 0))
                    continue;
                g.body[g.n_body].x = x;
                g.body[g.n_body].y = y;
                g.n_body++;
            }
        }
        CHECK_INT((int)g.n_body, ND_SNAKE_MAX_LEN - 2, "404 cells occupied");
        for (trial = 0; trial < 200; trial++) {
            api.snake_spawn_food(&g);
            if (!g.has_food)
                break;
            if (g.food.x == 0 && g.food.y == 13)
                got_a = true;
            else if (g.food.x == 28 && g.food.y == 0)
                got_b = true;
            else {
                CHECK(false, "food landed on an occupied cell");
                break;
            }
        }
        CHECK(got_a && got_b, "both free cells are reachable");
    }
}

/* ------------------------------------------------------------------ *
 * 8-9. Memory
 * ------------------------------------------------------------------ */

static void test_memory_geometry(sa_fixture *fx)
{
    nd_memory g;
    nd_rect r;

    api.memory_init(&g, &fx->ui);

    /* usable_h 137, cell min((240-8)/8, 137/5) = min(29, 27) = 27. */
    CHECK_INT(g.cell, 27, "cell");
    CHECK_INT(g.board_x, 12, "board_x = (240 - 8*27) / 2");
    CHECK_INT(g.board_y, 5, "board_y = (145 - 5*27) / 2");

    r = api.memory_card_rect(&g, 0, 0);
    CHECK_INT(r.x0, 14, "card(0,0).x0");
    CHECK_INT(r.y0, 7, "card(0,0).y0");
    CHECK_INT(r.x1, 36, "card(0,0).x1");
    CHECK_INT(r.y1, 29, "card(0,0).y1");
    CHECK_INT(r.x1 - r.x0 + 1, 23, "a card is 23 px wide");

    /* The board fits above the softkey band: the last row's cursor ring ends
     * at board_y + 5*27 - 1 = 139, and nothing is drawn under it. */
    r = api.memory_card_rect(&g, 7, 4);
    CHECK_INT(r.x1, 12 + 7 * 27 + 24, "card(7,4).x1");
    CHECK(r.y1 < ND_UI_H - ND_SOFTKEY_H, "the last row is above the softkey band");
}

static void test_memory_deck(sa_fixture *fx)
{
    nd_memory g;
    int32_t count[ND_MEMORY_KINDS];
    int32_t i;

    memset(count, 0, sizeof count);
    api.memory_init(&g, &fx->ui);

    for (i = 0; i < ND_MEMORY_CARDS; i++) {
        CHECK(g.cards[i] >= 0 && g.cards[i] < ND_MEMORY_KINDS, "card kind is 0..19");
        if (g.cards[i] >= 0 && g.cards[i] < ND_MEMORY_KINDS)
            count[g.cards[i]]++;
        CHECK_INT(g.state[i], ND_MEMORY_DOWN, "every card starts face down");
    }
    for (i = 0; i < ND_MEMORY_KINDS; i++)
        CHECK_INT(count[i], 2, "each kind appears exactly twice");

    CHECK_INT(g.cursor_col, 0, "cursor col");
    CHECK_INT(g.cursor_row, 0, "cursor row");
    CHECK_INT(g.first_pick, -1, "no first pick");
    CHECK_INT(g.score, 0, "score");
    CHECK_INT(g.misses, 0, "misses");
    CHECK(!api.memory_finished(&g), "a fresh board is not finished");
}

static void test_memory_cursor(sa_fixture *fx)
{
    nd_memory g;

    api.memory_init(&g, &fx->ui);

    /* Python's % is non-negative for a negative left operand; C's is not. */
    api.memory_move_cursor(&g, -1, 0);
    CHECK_INT(g.cursor_col, 7, "left from column 0 wraps to 7");
    api.memory_move_cursor(&g, 0, -1);
    CHECK_INT(g.cursor_row, 4, "up from row 0 wraps to 4");
    api.memory_move_cursor(&g, 1, 1);
    CHECK_INT(g.cursor_col, 0, "right from column 7 wraps to 0");
    CHECK_INT(g.cursor_row, 0, "down from row 4 wraps to 0");
}

static int32_t find_partner(const nd_memory *g, int32_t idx)
{
    int32_t i;

    for (i = 0; i < ND_MEMORY_CARDS; i++) {
        if (i != idx && g->cards[i] == g->cards[idx])
            return i;
    }
    return -1;
}

static int32_t find_mismatch(const nd_memory *g, int32_t idx)
{
    int32_t i;

    for (i = 0; i < ND_MEMORY_CARDS; i++) {
        if (g->cards[i] != g->cards[idx])
            return i;
    }
    return -1;
}

static void put_cursor(nd_memory *g, int32_t idx)
{
    g->cursor_col = idx % ND_MEMORY_COLS;
    g->cursor_row = idx / ND_MEMORY_COLS;
}

static void test_memory_flip(sa_fixture *fx)
{
    nd_memory g;
    int32_t partner;
    int32_t other;

    api.memory_init(&g, &fx->ui);

    /* --- a match on the first try: 10 - 2*0 = 10 --- */
    partner = find_partner(&g, 0);
    CHECK(partner > 0, "card 0 has a partner");
    put_cursor(&g, 0);
    api.memory_flip(&g);
    CHECK_INT(g.first_pick, 0, "the first pick is remembered");
    CHECK_INT(g.state[0], ND_MEMORY_UP, "the first card is face up");

    /* Flipping the same card again is a no-op: it is no longer "down". */
    api.memory_flip(&g);
    CHECK_INT(g.first_pick, 0, "re-flipping the same card changes nothing");

    put_cursor(&g, partner);
    api.memory_flip(&g);
    CHECK_INT(g.score, ND_MEMORY_PAIR_BASE, "a first-try pair scores 10");
    CHECK_INT(g.misses, 0, "misses stay at 0");
    CHECK_INT(g.state[0], ND_MEMORY_GONE, "matched cards are cleared");
    CHECK_INT(g.state[partner], ND_MEMORY_GONE, "matched cards are cleared");
    CHECK_INT(g.first_pick, -1, "first_pick reset");

    /* --- a mismatch: both go back face down and misses goes up --- */
    api.memory_init(&g, &fx->ui);
    other = find_mismatch(&g, 0);
    CHECK(other > 0, "card 0 has a non-partner");
    put_cursor(&g, 0);
    api.memory_flip(&g);
    put_cursor(&g, other);
    api.memory_flip(&g);
    CHECK_INT(g.misses, 1, "a miss is counted");
    CHECK_INT(g.score, 0, "a miss scores nothing");
    CHECK_INT(g.state[0], ND_MEMORY_DOWN, "a mismatched card goes back down");
    CHECK_INT(g.state[other], ND_MEMORY_DOWN, "a mismatched card goes back down");

    /* --- the penalty, and its floor --- */
    g.misses = 3;
    partner = find_partner(&g, 0);
    put_cursor(&g, 0);
    api.memory_flip(&g);
    put_cursor(&g, partner);
    api.memory_flip(&g);
    CHECK_INT(g.score, 4, "10 - 2*3 = 4");
    CHECK_INT(g.misses, 0, "a match zeroes the miss counter");

    g.misses = 20;
    other = -1;
    {
        int32_t i;

        for (i = 0; i < ND_MEMORY_CARDS; i++) {
            if (g.state[i] == ND_MEMORY_DOWN) {
                other = i;
                break;
            }
        }
    }
    CHECK(other >= 0, "cards remain");
    partner = find_partner(&g, other);
    put_cursor(&g, other);
    api.memory_flip(&g);
    put_cursor(&g, partner);
    api.memory_flip(&g);
    CHECK_INT(g.score, 4 + ND_MEMORY_PAIR_MIN, "the penalty floors at 2, never below");

    /* --- finished() --- */
    {
        int32_t i;

        for (i = 0; i < ND_MEMORY_CARDS; i++)
            g.state[i] = ND_MEMORY_GONE;
        CHECK(api.memory_finished(&g), "an empty board is finished");
        g.state[17] = ND_MEMORY_DOWN;
        CHECK(!api.memory_finished(&g), "one card left is not finished");
    }
}

/* ------------------------------------------------------------------ *
 * 10. The twenty glyphs
 * ------------------------------------------------------------------ */

static int32_t ink_in_row(const nd_image *img, int32_t y, int32_t x0, int32_t x1)
{
    int32_t n = 0;
    int32_t x;

    for (x = x0; x <= x1; x++) {
        if (nd_image_get_px(img, x, y).r > 127u)
            n++;
    }
    return n;
}

/* THE ORACLE FOR ALL TWENTY GLYPHS, taken from Pillow 12.3.0 running the
 * shipped System/apps/Games/memory.py `draw_glyph` at the box the game
 * really uses, (18,11)-(32,25). Each row is the ink pixel COUNT and an
 * FNV-1a 32 hash over the (x, y) of every ink pixel in raster order across
 * the whole 240x175 canvas, so a shape that is the right size in the wrong
 * place fails as loudly as one that is the wrong size.
 *
 * "some ink appeared inside the box" is not a check -- nineteen of these
 * shapes would pass it while drawing something else entirely. This is the
 * only assertion in the file that would catch, say, a filled diamond drawn
 * with the polygon's winding reversed, or the checkerboard starting on the
 * wrong parity.
 *
 * Regenerate with:
 *   cd neodct/overlay/NeoDCT/System/apps/Games && python3 - <<'EOF'
 *   from PIL import Image, ImageDraw
 *   import importlib.util
 *   s = importlib.util.spec_from_file_location('mem', 'memory.py')
 *   m = importlib.util.module_from_spec(s); s.loader.exec_module(m)
 *   box = (18, 11, 32, 25)
 *   for kind in range(20):
 *       im = Image.new('RGB', (240, 175), 'black')
 *       m.draw_glyph(ImageDraw.Draw(im), box, kind, 'white')
 *       px, h, n = im.load(), 2166136261, 0
 *       for y in range(175):
 *           for x in range(240):
 *               if px[x, y][0] > 127:
 *                   n += 1
 *                   for b in (x & 255, x >> 8, y & 255, y >> 8):
 *                       h = ((h ^ b) * 16777619) & 0xffffffff
 *       print('    {%3d, 0x%08xu},' % (n, h))
 *   EOF
 */
static const struct {
    int32_t ink;
    uint32_t hash;
} GLYPH_REF[ND_MEMORY_KINDS] = {
    {177, 0xe3a9372eu}, /*  0 filled circle    */
    {64, 0x94d17845u},  /*  1 ring             */
    {225, 0x51d93a7eu}, /*  2 filled square    */
    {56, 0x73898395u},  /*  3 hollow square    */
    {113, 0xe9bfbe0eu}, /*  4 filled diamond   */
    {28, 0xe5706485u},  /*  5 hollow diamond   */
    {113, 0x2ece53ceu}, /*  6 triangle up      */
    {113, 0x5fbdc8ceu}, /*  7 triangle down    */
    {81, 0xa478920eu},  /*  8 plus             */
    {83, 0x5a145e8eu},  /*  9 X                */
    {69, 0xfec2ee0eu},  /* 10 star             */
    {128, 0x8be9c9cfu}, /* 11 heart            */
    {120, 0xbb1bbb35u}, /* 12 horizontal bars  */
    {120, 0x40227c65u}, /* 13 vertical bars    */
    {113, 0xd359d3feu}, /* 14 checker          */
    {127, 0xda4b55f6u}, /* 15 hourglass        */
    {85, 0x47855d74u},  /* 16 arrow up         */
    {85, 0x7a750424u},  /* 17 arrow right      */
    {100, 0x6d578425u}, /* 18 corners          */
    {77, 0x7319ec36u},  /* 19 dot in a box     */
};

static uint32_t fnv1a_byte(uint32_t h, int32_t v)
{
    return (h ^ (uint32_t)(v & 0xff)) * 16777619u;
}

static void test_glyphs(sa_fixture *fx)
{
    /* The real box the game uses: a 23x23 card inset by 4, so 15x15 with
     * w = 14 -- the width every `//` in draw_glyph is taken against. */
    const nd_rect box = ND_RECT(18, 11, 32, 25);
    int32_t kind;

    for (kind = 0; kind < ND_MEMORY_KINDS; kind++) {
        int32_t inside = 0;
        int32_t outside = 0;
        uint32_t hash = 2166136261u;
        int32_t ink = 0;
        int32_t x;
        int32_t y;

        (void)nd_draw_rect_fill(fx->ui.draw, ND_RECT(0, 0, ND_UI_W - 1, ND_UI_H - 1), ND_BLACK);
        api.memory_draw_glyph(fx->ui.draw, box, kind, ND_WHITE);

        for (y = 0; y < ND_UI_H; y++) {
            for (x = 0; x < ND_UI_W; x++) {
                if (nd_image_get_px(fx->canvas, x, y).r <= 127u)
                    continue;
                ink++;
                hash = fnv1a_byte(hash, x);
                hash = fnv1a_byte(hash, x >> 8);
                hash = fnv1a_byte(hash, y);
                hash = fnv1a_byte(hash, y >> 8);
                if (x >= box.x0 && x <= box.x1 && y >= box.y0 && y <= box.y1)
                    inside++;
                else
                    outside++;
            }
        }
        CHECK(inside > 0, "every glyph draws something");
        CHECK_INT(ink, GLYPH_REF[kind].ink, "glyph ink count matches Pillow");
        CHECK_INT((long long)hash, (long long)GLYPH_REF[kind].hash,
                  "glyph pixels match Pillow exactly");

        /* Kind 9 -- the X -- genuinely spills 4 pixels, and Pillow does the
         * same, so this pins the quirk rather than forbidding it.
         *
         * It is two DIAGONAL lines at width 2. A wide line grows along its
         * minor axis, which for an axis-aligned line stays inside the box but
         * for a diagonal is perpendicular to the run -- so the ends poke past
         * the corners. Measured on Pillow with this exact box (18,11)-(32,25),
         * the overflow is at (17,25) (33,25) (18,26) and (32,26): four pixels,
         * one past each end of each stroke.
         *
         * Asserting 0 here would be asserting a tidier drawing than the phone
         * has ever produced, and the only way to satisfy it would be to make
         * the C stop matching Pillow. */
        CHECK_INT(outside, kind == 9 ? 4 : 0, "glyph spill outside its box");
    }

    /* Kind 15 is polygon([(x0,y0),(x1,y0),(x0,y1),(x1,y1)]) -- a
     * SELF-INTERSECTING quadrilateral. Under Pillow's parity rule it is an
     * hourglass: wide at the top and bottom edges, pinched to nothing in the
     * middle. A naive fill of the convex hull would be a solid square, and a
     * "fill the crossing too" implementation would be a bow tie with a
     * filled waist. Measure the waist. */
    {
        int32_t top;
        int32_t mid;
        int32_t bot;

        (void)nd_draw_rect_fill(fx->ui.draw, ND_RECT(0, 0, ND_UI_W - 1, ND_UI_H - 1), ND_BLACK);
        api.memory_draw_glyph(fx->ui.draw, box, 15, ND_WHITE);

        top = ink_in_row(fx->canvas, box.y0, box.x0, box.x1);
        mid = ink_in_row(fx->canvas, (box.y0 + box.y1) / 2, box.x0, box.x1);
        bot = ink_in_row(fx->canvas, box.y1, box.x0, box.x1);

        CHECK(top > mid, "glyph 15 is wide at the top and pinched in the middle");
        CHECK(bot > mid, "glyph 15 is wide at the bottom and pinched in the middle");
        CHECK(mid <= 2, "glyph 15's waist is a point, not a filled crossing");
        CHECK(top >= 14, "glyph 15's top edge spans the box");
    }

    /* Kind 3 is an outline, so its interior is empty; kind 2 is the same box
     * filled. The two together are the check that outline and fill are not
     * quietly the same call. */
    {
        int32_t hollow;
        int32_t solid;

        (void)nd_draw_rect_fill(fx->ui.draw, ND_RECT(0, 0, ND_UI_W - 1, ND_UI_H - 1), ND_BLACK);
        api.memory_draw_glyph(fx->ui.draw, box, 3, ND_WHITE);
        hollow = ink_in_row(fx->canvas, (box.y0 + box.y1) / 2, box.x0, box.x1);

        (void)nd_draw_rect_fill(fx->ui.draw, ND_RECT(0, 0, ND_UI_W - 1, ND_UI_H - 1), ND_BLACK);
        api.memory_draw_glyph(fx->ui.draw, box, 2, ND_WHITE);
        solid = ink_in_row(fx->canvas, (box.y0 + box.y1) / 2, box.x0, box.x1);

        CHECK_INT(hollow, 2, "a hollow square's middle row is two pixels");
        CHECK_INT(solid, 15, "a filled square's middle row is the whole width");
    }

    /* Anything outside 0..19 falls to the Python's trailing `else`, which is
     * shape 19 -- not "nothing". */
    {
        int32_t as19 = 0;
        int32_t as99 = 0;
        int32_t x;
        int32_t y;

        (void)nd_draw_rect_fill(fx->ui.draw, ND_RECT(0, 0, ND_UI_W - 1, ND_UI_H - 1), ND_BLACK);
        api.memory_draw_glyph(fx->ui.draw, box, 19, ND_WHITE);
        for (y = box.y0; y <= box.y1; y++)
            for (x = box.x0; x <= box.x1; x++)
                as19 += (nd_image_get_px(fx->canvas, x, y).r > 127u) ? 1 : 0;

        (void)nd_draw_rect_fill(fx->ui.draw, ND_RECT(0, 0, ND_UI_W - 1, ND_UI_H - 1), ND_BLACK);
        api.memory_draw_glyph(fx->ui.draw, box, 99, ND_WHITE);
        for (y = box.y0; y <= box.y1; y++)
            for (x = box.x0; x <= box.x1; x++)
                as99 += (nd_image_get_px(fx->canvas, x, y).r > 127u) ? 1 : 0;

        CHECK(as19 > 0, "shape 19 draws");
        CHECK_INT(as99, as19, "an out-of-range kind draws shape 19");
    }
}

/* ------------------------------------------------------------------ *
 * The settings helpers
 * ------------------------------------------------------------------ */

static void test_settings_helpers(void)
{
    CHECK_INT((int)api.setting_int("games.no.such.key", 5), 5, "an absent key is the default");

    CHECK(api.setting_set(ND_SET_GAMES_SNAKE_LEVEL, 7), "set");
    CHECK_INT((int)api.setting_int(ND_SET_GAMES_SNAKE_LEVEL, 5), 7, "read back");

    /* `int(get_setting(...) or default)`: an EMPTY value is falsy in Python
     * and becomes the default rather than an int() failure. */
    CHECK(nd_settings_set(ND_SET_GAMES_SNAKE_LEVEL, "") == ND_OK, "store an empty value");
    CHECK_INT((int)api.setting_int(ND_SET_GAMES_SNAKE_LEVEL, 5), 5, "empty falls back");

    /* An unparseable value raises ValueError in Python and is swallowed by
     * the try/except, which is the default again. */
    CHECK(nd_settings_set(ND_SET_GAMES_SNAKE_LEVEL, "nine") == ND_OK, "store a word");
    CHECK_INT((int)api.setting_int(ND_SET_GAMES_SNAKE_LEVEL, 5), 5, "a word falls back");
    CHECK(nd_settings_set(ND_SET_GAMES_SNAKE_LEVEL, "7x") == ND_OK, "store trailing junk");
    CHECK_INT((int)api.setting_int(ND_SET_GAMES_SNAKE_LEVEL, 5), 5, "trailing junk falls back");

    /* int() accepts surrounding whitespace and a sign, and nothing else. */
    CHECK(nd_settings_set(ND_SET_GAMES_SNAKE_LEVEL, "  8 ") == ND_OK, "store padded");
    CHECK_INT((int)api.setting_int(ND_SET_GAMES_SNAKE_LEVEL, 5), 8, "whitespace is stripped");
    CHECK(nd_settings_set(ND_SET_GAMES_MEMORY_TOPSCORE, "-3") == ND_OK, "store a negative");
    CHECK_INT((int)api.setting_int(ND_SET_GAMES_MEMORY_TOPSCORE, 0), -3, "a sign is accepted");

    CHECK_INT((int)api.setting_int(NULL, 42), 42, "a NULL key is the default");
    CHECK(!api.setting_set(NULL, 1), "a NULL key cannot be written");

    /* Leave the store as a fresh phone would have it, so the frame tests
     * below cannot be steered by what this one wrote. */
    (void)nd_settings_set(ND_SET_GAMES_SNAKE_LEVEL, "5");
    (void)nd_settings_set(ND_SET_GAMES_MEMORY_TOPSCORE, "0");
}

/* ------------------------------------------------------------------ *
 * 11. The golden frames
 * ------------------------------------------------------------------ */

/* shoot_docs.py's shoot_stock_apps case ("Games", [], "app-games", -1, 240).
 * keys=[] means the first wait_for_key raises ScriptExhausted with the list
 * already on the panel; a held Back reaches the same wait and ends the app on
 * the same frame. */
static void test_frame_app_games(void)
{
    sa_fixture fx;
    int rc;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    if (!sa_hold(&fx, ND_KEY_CLEAR)) {
        CHECK(false, "held key");
        sa_fx_free(&fx);
        return;
    }

    nd_vclock_enable();
    rc = api.run(&fx.ui);

    CHECK_INT(rc, 0, "Back on the root menu returns 0");
    CHECK_INT((int)nd_capture_frames_drawn(fx.cap), 1, "one frame: the root VerticalList");
    sa_expect_golden(&fx, nd_capture_recent(fx.cap, 0u), "app-games");

    nd_vclock_disable();
    sa_fx_free(&fx);
}

/* shoot_docs.py's shoot_games cases. The budget is what picks the frame, the
 * way it does for the two Calculator screens: everything the held Back draws
 * on the way out is refused by nd_capture and never recorded.
 *
 *   game-snake   Games menu, the Down redraw, the Snake menu, the board = 4
 *   game-memory  Games menu, the Memory menu, the board                 = 3
 */
static void run_game_frame(const int32_t *keys, size_t n_keys, int64_t budget, const char *slug)
{
    sa_fixture fx;
    int rc;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    if (!sa_send_all(&fx, keys, n_keys) || !sa_hold(&fx, ND_KEY_CLEAR)) {
        CHECK(false, "key script");
        sa_fx_free(&fx);
        return;
    }

    nd_vclock_enable();
    nd_capture_set_budget(fx.cap, budget);
    rc = api.run(&fx.ui);
    nd_capture_clear_budget(fx.cap);

    CHECK_INT(rc, 0, "the app returns 0");
    CHECK_INT((int)nd_capture_frames_drawn(fx.cap), (int)budget, "the budget is the frame count");
    sa_expect_golden(&fx, nd_capture_recent(fx.cap, 0u), slug);

    nd_vclock_disable();
    sa_fx_free(&fx);
}

static void test_frame_game_snake(void)
{
    /* Down first, because the root menu lists Memory before Snake. */
    static const int32_t keys[] = {ND_KEY_DOWN, ND_KEY_ENTER, ND_KEY_ENTER};

    run_game_frame(keys, ND_ARRAY_LEN(keys), 4, "game-snake");
}

static void test_frame_game_memory(void)
{
    static const int32_t keys[] = {ND_KEY_ENTER, ND_KEY_ENTER};

    run_game_frame(keys, ND_ARRAY_LEN(keys), 3, "game-memory");
}

/* ------------------------------------------------------------------ *
 * Null safety
 * ------------------------------------------------------------------ */

static void test_null_safety(void)
{
    nd_snake s;
    nd_memory m;

    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown(); /* must be safe with nothing held */

    api.snake_init(NULL, NULL, 5);
    api.snake_spawn_food(NULL);
    api.snake_queue_turn(NULL, 1, 0);
    CHECK(!api.snake_step(NULL), "step(NULL) is death, not a fault");
    api.snake_render(NULL);

    api.memory_init(NULL, NULL);
    api.memory_move_cursor(NULL, 1, 0);
    api.memory_flip(NULL);
    CHECK(!api.memory_finished(NULL), "finished(NULL)");
    api.memory_render(NULL);
    api.memory_draw_glyph(NULL, ND_RECT(0, 0, 4, 4), 0, ND_WHITE);

    /* A game built against a NULL context still has to be inert rather than
     * fatal: nd_ui_width(NULL) answers with the panel constants. */
    api.snake_init(&s, NULL, 5);
    CHECK_INT(s.board_x, 4, "geometry survives a NULL ui");
    api.snake_render(&s);
    api.memory_init(&m, NULL);
    CHECK_INT(m.cell, 27, "geometry survives a NULL ui");
    api.memory_render(&m);

    CHECK_INT(api.content_bottom(NULL), ND_UI_H - ND_SOFTKEY_H, "content_bottom(NULL)");
    sa_checks++;
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

/* The eight pure-logic tests share one fixture: none of them presents a
 * frame that another can see, and building four fonts and a 126,000-byte
 * canvas eight times over is the slowest thing this file would otherwise do. */
static void test_logic(void)
{
    sa_fixture fx;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    test_snake_geometry(&fx);
    test_snake_start(&fx);
    test_snake_turns(&fx);
    test_snake_step(&fx);
    test_snake_food(&fx);
    test_memory_geometry(&fx);
    test_memory_deck(&fx);
    test_memory_cursor(&fx);
    test_memory_flip(&fx);
    test_glyphs(&fx);
    sa_fx_free(&fx);
}

int main(void)
{
    void *h = sa_begin("Games", "ndgames");
    int rc;

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }
    if (nd_settings_init() != ND_OK) {
        fprintf(stderr, "test_games: nd_settings_init failed\n");
        (void)dlclose(h);
        return 1;
    }

    RUN(test_strings);
    RUN(test_dir_keys);
    RUN(test_tick_delay);
    RUN(test_logic);
    RUN(test_settings_helpers);
    RUN(test_frame_app_games);
    RUN(test_frame_game_snake);
    RUN(test_frame_game_memory);
    RUN(test_null_safety);

    rc = sa_end(h, "test_games");
    return rc;
}

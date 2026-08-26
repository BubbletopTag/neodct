/* games.h -- Snake, Memory and the bits they share. App id 6.
 *
 * System/apps/Games/ is four files and 503 lines: main.py (the three menus),
 * games_common.py (key codes, poll_key, the two settings helpers), snake.py
 * and memory.py. The C keeps the same split -- main.c, snake.c, memory.c --
 * and this header is games_common.py plus the two game objects.
 *
 * test/unit/test_games.c dlopen()s the BUILT app.so and dlsym()s these, the
 * way test_tones.c and test_phonebook.c do.
 *
 * ============ THE TWO GAMES DO NOT MATCH THE PYTHON, ON PURPOSE ==========
 *
 * OPEN-QUESTIONS.md decision 4 is settled: both games call
 * `random.seed(time.time())` and then draw from CPython's MT19937, and this
 * port uses libneodct's pinned LCG instead. Snake's food cell and Memory's
 * shuffle therefore differ from the Python's, so `game-snake.png` and
 * `game-memory.png` are frame class `recut` -- re-captured from the C build
 * and exact against that reference thereafter. Every other frame in the set,
 * `app-games` included, is still byte-exact against the Python.
 *
 * The seed is `(uint32_t)nd_time_now()`, which under capture is the virtual
 * clock and is therefore the same on every machine and every run.
 *
 * ============ HOLD-TO-REPEAT NEEDED NO CODE HERE ==========================
 *
 * The games poll `read_keypress()` in a tight loop, which is exactly where
 * decision 2's held-key state reaches them: `nd_input` synthesises repeat
 * presses for 103/105/106/108 from its own held state, so holding Down in
 * Memory walks the cursor and holding a direction in Snake re-queues a turn
 * that `queue_turn()` already ignores. Neither game calls
 * `nd_input_set_repeat_codes()`, and that is deliberate -- the repeat set is
 * process-global, there is no getter to restore it from, and an app that
 * narrowed it would strip the exit key's repeat from its own caller. Same
 * reasoning as OPEN-QUESTIONS.md W-8, which reached it for the widgets.
 */

#ifndef ND_GAMES_H_INCLUDED
#define ND_GAMES_H_INCLUDED

#include "nd_draw.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== *
 * games_common.py
 * ================================================================== */

/* APP_ID = 6 -- manifest.json, and the breadcrumb on every menu here. */
#define ND_GAMES_APP_ID 6

/* The same numbers nd_keycodes.h carries, respelled with games_common's own
 * names so the two files can be read side by side. Checked against
 * nd_keycodes.h by a static assertion in main.c. */
#define ND_GAMES_KEY_ENTER 28
#define ND_GAMES_KEY_BACK  14
#define ND_GAMES_KEY_UP    103
#define ND_GAMES_KEY_DOWN  108
#define ND_GAMES_KEY_LEFT  105
#define ND_GAMES_KEY_RIGHT 106
#define ND_GAMES_KEY_NUM_2 3
#define ND_GAMES_KEY_NUM_4 5
#define ND_GAMES_KEY_NUM_5 6
#define ND_GAMES_KEY_NUM_6 7
#define ND_GAMES_KEY_NUM_8 9

/* DIR_KEYS: the eight keys that steer, and nothing else. false leaves *dx
 * and *dy untouched, which is the Python's `DIR_KEYS.get(key)` being None. */
bool nd_games_dir_for_key(int32_t key, int32_t *dx, int32_t *dy);

/* poll_key(ui, timeout): ui.read_keypress(timeout), with every failure
 * folded to "no key". Returns ND_KEY_NONE when nothing arrived. */
int32_t nd_games_poll_key(nd_ui *ui, double timeout_s);

/* get_setting_int / set_setting_value. The read is
 * `int(get_setting(key, str(default)) or default)` with every exception
 * folded to `default`; the write logs
 * "[Games] Setting write failed (<key>)" and returns false. */
int64_t nd_games_setting_int(const char *key, int64_t dflt);
bool nd_games_setting_set(const char *key, int64_t value);

/* time.sleep(). Skipped while the virtual clock is running: under capture
 * time is a frame counter and a real sleep moves no pixel, it only makes the
 * oracle slower. OPEN-QUESTIONS.md PB-3. */
void nd_games_dwell(double seconds);

/* getattr(ui, "content_bottom", H - SOFTKEY_H) -- the field when it is set,
 * the derived value otherwise. Both games compute their board from it. */
int32_t nd_games_content_bottom(const nd_ui *ui);

/* ================================================================== *
 * main.py -- the menus
 * ================================================================== */

#define ND_GAMES_ROOT_ITEMS   2 /* "Memory", "Snake" -- MEMORY IS FIRST */
#define ND_GAMES_SNAKE_ITEMS  4
#define ND_GAMES_MEMORY_ITEMS 3

extern const char *const nd_games_root_menu[ND_GAMES_ROOT_ITEMS];
extern const char *const nd_games_snake_menu[ND_GAMES_SNAKE_ITEMS];
extern const char *const nd_games_memory_menu[ND_GAMES_MEMORY_ITEMS];

extern const char *const nd_games_snake_instructions;
extern const char *const nd_games_memory_instructions;

/* SNAKE_LEVEL_KEY / SNAKE_TOP_KEY / MEMORY_TOP_KEY are nd_settings.h's
 * ND_SET_GAMES_* and are not respelled here. The defaults are: level 5, both
 * top scores 0. */
#define ND_GAMES_DEFAULT_LEVEL 5

/* ================================================================== *
 * snake.py
 * ================================================================== */

#define ND_SNAKE_GRID_W 29
#define ND_SNAKE_GRID_H 14
#define ND_SNAKE_CELL   8

/* 29 * 14 = 406 cells, which is also the longest a snake can be: the step
 * that would fill the last cell is the step that runs into its own body. */
#define ND_SNAKE_MAX_LEN (ND_SNAKE_GRID_W * ND_SNAKE_GRID_H)

/* `len(self.turn_queue) < 2` -- so two, and the third turn in a tick is
 * dropped rather than buffered. */
#define ND_SNAKE_TURN_QUEUE 2

typedef struct {
    nd_ui *ui;
    int32_t level; /* clamped to 1..9 in the constructor */

    int32_t screen_w;
    int32_t screen_h;
    int32_t softkey_h;
    int32_t content_bottom; /* read, never used -- as in the Python */

    int32_t score_h; /* 20  */
    int32_t board_x; /* 4   */
    int32_t board_y; /* 24  */
    int32_t board_w; /* 232 */
    int32_t board_h; /* 112 */

    nd_point body[ND_SNAKE_MAX_LEN]; /* head first */
    size_t n_body;

    nd_point direction;
    nd_point turn_queue[ND_SNAKE_TURN_QUEUE];
    size_t n_turns;

    int32_t score;
    nd_point food;
    bool has_food; /* false is the Python's `self.food = None` */
} nd_snake;

/* max(0.09, 0.40 - 0.033 * level). The floor is never reached for 1..9. */
double nd_snake_tick_delay(int32_t level);

/* SnakeGame(ui, level): geometry, the three-cell snake, and the first food.
 * SEEDS THE PRNG from the clock, exactly where the Python does. */
void nd_snake_init(nd_snake *g, nd_ui *ui, int32_t level);

/* random.choice over the open cells in X-MAJOR order -- x outer, y inner.
 * Clears has_food when the board is full. */
void nd_snake_spawn_food(nd_snake *g);

void nd_snake_queue_turn(nd_snake *g, int32_t dx, int32_t dy);

/* One tick. false means the snake died -- wall or its own body. */
bool nd_snake_step(nd_snake *g);

/* (board_x + 8x + 1, board_y + 8y + 1, +6, +6): a 6x6 inclusive box inside
 * each 8 px cell. */
nd_rect nd_snake_cell_rect(const nd_snake *g, int32_t x, int32_t y);

/* Clears the WHOLE screen, softkey band included -- no softkey is ever drawn
 * during play and the bottom 30 rows stay black. Presents. */
void nd_snake_render(nd_snake *g);

/* One game. Returns the score, or -1 where the Python returned None: the
 * player pressed Back, or the app was asked to shut down. */
int32_t nd_snake_play(nd_snake *g);

/* ================================================================== *
 * memory.py
 * ================================================================== */

#define ND_MEMORY_COLS         8
#define ND_MEMORY_ROWS         5
#define ND_MEMORY_CARDS        (ND_MEMORY_COLS * ND_MEMORY_ROWS) /* 40 */
#define ND_MEMORY_KINDS        (ND_MEMORY_CARDS / 2)             /* 20 */
#define ND_MEMORY_PAIR_BASE    10
#define ND_MEMORY_MISS_PENALTY 2
#define ND_MEMORY_PAIR_MIN     2
#define ND_MEMORY_REVEAL_SECS  0.9

typedef enum { ND_MEMORY_DOWN = 0, ND_MEMORY_UP, ND_MEMORY_GONE } nd_memory_state;

typedef struct {
    nd_ui *ui;

    int32_t screen_w;
    int32_t screen_h;
    int32_t softkey_h;
    int32_t content_bottom;

    int32_t cell;    /* 27 */
    int32_t board_x; /* 12 */
    int32_t board_y; /*  5 */

    int32_t cards[ND_MEMORY_CARDS];
    nd_memory_state state[ND_MEMORY_CARDS];

    int32_t cursor_col;
    int32_t cursor_row;
    int32_t first_pick; /* -1 is the Python's None */
    int32_t score;
    int32_t misses; /* misses since the last match */
} nd_memory;

/* MemoryGame(ui): geometry, `list(range(20)) * 2`, seed, shuffle. */
void nd_memory_init(nd_memory *g, nd_ui *ui);

/* (px+2, py+2, px+cell-3, py+cell-3): a 23x23 inclusive box. */
nd_rect nd_memory_card_rect(const nd_memory *g, int32_t col, int32_t row);

void nd_memory_render(nd_memory *g);

/* Wraps with Python's modulo, which is non-negative for a negative left
 * operand and C's is not. */
void nd_memory_move_cursor(nd_memory *g, int32_t dx, int32_t dy);

void nd_memory_flip(nd_memory *g);
bool nd_memory_finished(const nd_memory *g);

/* One game. Returns the score, or -1 where the Python returned None. */
int32_t nd_memory_play(nd_memory *g);

/* One of twenty little pictures inside an inclusive box. `kind` outside
 * 0..19 draws shape 19, which is the Python's trailing `else`. */
void nd_memory_draw_glyph(nd_draw *d, nd_rect box, int32_t kind, nd_color c);

#ifdef __cplusplus
}
#endif

#endif /* ND_GAMES_H_INCLUDED */

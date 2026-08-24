/* apps/Games/main.c -- the Games app's three menus and games_common.py.
 *
 * A one-to-one port of System/apps/Games/main.py (104 lines) and
 * games_common.py (53 lines). The two games themselves are snake.c and
 * memory.c.
 *
 * golden/app-games.png is the root VerticalList: "Games" in 24 px type,
 * "Memory" highlighted, "Snake" under it, a "Select" softkey and the "6-1"
 * breadcrumb. MEMORY IS FIRST -- the list is ["Memory", "Snake"] and index 0
 * is Memory, which is the opposite of the alphabetical order a reader
 * expects and is what the frame holds.
 *
 * ============ EVERY MENU IS REBUILT INSIDE ITS LOOP ============
 *
 * spec-apps-core.md section 0b: `_show_menu` constructs the VerticalList per
 * call, so all three menus reset to item 0 after every game, every level
 * pick and every instruction screen. That is different from CallLog and
 * Messages, whose PagedLists are built once and remember their page, and the
 * difference is visible. Keep it.
 *
 * ============ WHAT games_common.py's try/except BLOCKS ARE FOR ============
 *
 * Every helper in games_common.py swallows its exceptions and returns a
 * default: `poll_key` falls back to sleeping, `get_setting_int` to the
 * caller's default, `set_setting_value` to printing and returning False. In
 * Python those catch a missing System.core.SettingsStorage import, which is
 * what happens when the app is loaded outside the phone. In C the settings
 * layer is linked in and cannot be missing, so what remains is the value
 * cases: an unparseable stored level, a read-only /NeoDCT/User. Both are
 * folded exactly as the Python folds them.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "nd_app.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_settings.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

#include "games.h"

/* games_common.py's key codes against the system's. A typo in either table
 * would send the cursor the wrong way and nothing else would notice. */
_Static_assert(ND_GAMES_KEY_ENTER == ND_KEY_ENTER, "KEY_ENTER");
_Static_assert(ND_GAMES_KEY_BACK == ND_KEY_CLEAR, "KEY_BACK");
_Static_assert(ND_GAMES_KEY_UP == ND_KEY_UP, "KEY_UP");
_Static_assert(ND_GAMES_KEY_DOWN == ND_KEY_DOWN, "KEY_DOWN");
_Static_assert(ND_GAMES_KEY_LEFT == ND_KEY_LEFT, "KEY_LEFT");
_Static_assert(ND_GAMES_KEY_RIGHT == ND_KEY_RIGHT, "KEY_RIGHT");
_Static_assert(ND_GAMES_KEY_NUM_2 == ND_KEY_2, "KEY_NUM_2");
_Static_assert(ND_GAMES_KEY_NUM_4 == ND_KEY_4, "KEY_NUM_4");
_Static_assert(ND_GAMES_KEY_NUM_5 == ND_KEY_5, "KEY_NUM_5");
_Static_assert(ND_GAMES_KEY_NUM_6 == ND_KEY_6, "KEY_NUM_6");
_Static_assert(ND_GAMES_KEY_NUM_8 == ND_KEY_8, "KEY_NUM_8");

/* ------------------------------------------------------------------ *
 * The strings
 * ------------------------------------------------------------------ */

const char *const nd_games_root_menu[ND_GAMES_ROOT_ITEMS] = {"Memory", "Snake"};

const char *const nd_games_snake_menu[ND_GAMES_SNAKE_ITEMS] = {"New game", "Level", "Top score",
                                                               "Instructions"};

const char *const nd_games_memory_menu[ND_GAMES_MEMORY_ITEMS] = {"New game", "Top score",
                                                                 "Instructions"};

/* Single lines with no embedded newlines: TextScroller wraps them itself. */
const char *const nd_games_snake_instructions =
    "Feed the snake by steering it to the food. Every bite makes it grow "
    "longer. Use keys 2, 4, 6 and 8 to change direction. The game ends if "
    "the snake runs into the walls or into its own body. A higher level "
    "means more speed and more points for each bite.";

const char *const nd_games_memory_instructions =
    "All the cards lie face down. Move the cursor with keys 2, 4, 6 and 8 "
    "and turn a card over with key 5. Two matching cards are cleared from "
    "the board. Find every pair to finish the game. The fewer tries you "
    "need, the higher your score.";

/* ------------------------------------------------------------------ *
 * games_common.py
 * ------------------------------------------------------------------ */

bool nd_games_dir_for_key(int32_t key, int32_t *dx, int32_t *dy)
{
    int32_t x;
    int32_t y;

    switch (key) {
    case ND_GAMES_KEY_UP:
    case ND_GAMES_KEY_NUM_2:
        x = 0;
        y = -1;
        break;
    case ND_GAMES_KEY_DOWN:
    case ND_GAMES_KEY_NUM_8:
        x = 0;
        y = 1;
        break;
    case ND_GAMES_KEY_LEFT:
    case ND_GAMES_KEY_NUM_4:
        x = -1;
        y = 0;
        break;
    case ND_GAMES_KEY_RIGHT:
    case ND_GAMES_KEY_NUM_6:
        x = 1;
        y = 0;
        break;
    default:
        return false;
    }
    if (dx != NULL)
        *dx = x;
    if (dy != NULL)
        *dy = y;
    return true;
}

int32_t nd_games_poll_key(nd_ui *ui, double timeout_s)
{
    int32_t key;

    if (ui == NULL) {
        /* `time.sleep(timeout); return None` -- the branch taken when the
         * context has no read_keypress at all. */
        nd_games_dwell(timeout_s);
        return ND_KEY_NONE;
    }
    key = nd_ui_read_keypress(ui, timeout_s);
    /* ND_KEY_INCOMING_CALL never reaches an app -- a call arrives as
     * SIGTERM (nd_app.h) -- but folding every negative to "no key" keeps the
     * two games from having to know that. */
    return (key < 0) ? ND_KEY_NONE : key;
}

/* Python's int(str), for the one place a settings value is parsed. Leading
 * and trailing whitespace and one sign are accepted and nothing else;
 * underscore separators and non-ASCII digits are not, the same deviation
 * OPEN-QUESTIONS.md M-10 records. */
static bool py_int(const char *s, int64_t *out)
{
    const char *p = s;
    bool neg = false;
    bool any = false;
    int64_t v = 0;

    if (s == NULL)
        return false;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
        p++;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        int64_t d = (int64_t)(*p - '0');

        if (v > (INT64_MAX - d) / 10)
            v = INT64_MAX / 10;
        v = v * 10 + d;
        any = true;
        p++;
    }
    while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
        p++;
    if (!any || *p != '\0')
        return false;

    *out = neg ? -v : v;
    return true;
}

int64_t nd_games_setting_int(const char *key, int64_t dflt)
{
    char dflt_text[32];
    char buf[64];
    int64_t v = 0;

    if (key == NULL)
        return dflt;
    /* get_setting(key, str(default)) -- the default really is passed as a
     * string, so a key that is absent everywhere comes back as its own
     * decimal spelling and is then parsed. */
    if (nd_snprintf(dflt_text, sizeof dflt_text, "%lld", (long long)dflt) != ND_OK)
        return dflt;
    if (nd_settings_get_copy(key, dflt_text, buf, sizeof buf) != ND_OK)
        return dflt;
    /* `... or default`: an empty answer is falsy and becomes the default. */
    if (buf[0] == '\0')
        return dflt;
    if (!py_int(buf, &v))
        return dflt;
    return v;
}

bool nd_games_setting_set(const char *key, int64_t value)
{
    char buf[32];

    if (key == NULL)
        return false;
    if (nd_snprintf(buf, sizeof buf, "%lld", (long long)value) != ND_OK)
        return false;
    if (nd_settings_set(key, buf) != ND_OK) {
        nd_log(ND_LOG_GAMES, "Setting write failed (%s)", key);
        return false;
    }
    return true;
}

void nd_games_dwell(double seconds)
{
    struct timespec req;

    if (seconds <= 0.0 || nd_vclock_enabled())
        return;
    req.tv_sec = (time_t)seconds;
    req.tv_nsec = (long)((seconds - (double)req.tv_sec) * 1e9);
    while (nanosleep(&req, &req) != 0)
        break; /* EINTR only; anything else would spin */
}

int32_t nd_games_content_bottom(const nd_ui *ui)
{
    /* getattr(ui, "content_bottom", H - SOFTKEY_H). The field when it is
     * set, the derived value otherwise -- a zero field is C's spelling of
     * the missing attribute (OPEN-QUESTIONS.md W-2). */
    if (ui != NULL && ui->content_bottom > 0)
        return ui->content_bottom;
    return nd_ui_content_bottom(ui);
}

/* ------------------------------------------------------------------ *
 * main.py -- the menus
 * ------------------------------------------------------------------ */

static int32_t show_menu(nd_ui *ui, const char *title, const char *const *items, size_t n)
{
    nd_vlist menu;
    nd_softkey bar;

    nd_vlist_init(&menu, ui, title, items, n, ND_GAMES_APP_ID);
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "Select", false);
    return nd_vlist_show(&menu);
}

/* _finish_game(). `score is None` is score < 0 here: both games return a
 * score of at least 0, so there is no value to confuse it with. */
static void finish_game(nd_ui *ui, int32_t score, const char *top_key)
{
    char text[24];
    int64_t top;

    if (score < 0)
        return;

    top = nd_games_setting_int(top_key, 0);
    (void)nd_snprintf(text, sizeof text, "%d", score);

    if ((int64_t)score > top) {
        (void)nd_games_setting_set(top_key, score);
        (void)nd_infoscreen_show(ui, "New top score:", text, "OK");
    } else {
        (void)nd_infoscreen_show(ui, "Game over! Score:", text, "OK");
    }
}

static void show_top_score(nd_ui *ui, const char *key)
{
    char text[24];

    (void)nd_snprintf(text, sizeof text, "%lld", (long long)nd_games_setting_int(key, 0));
    /* softkey_text defaults to "Back" here -- unlike the two end-of-game
     * screens above, which ask for "OK". */
    (void)nd_infoscreen_show(ui, "Top score", text, "Back");
}

static void show_instructions(nd_ui *ui, const char *text)
{
    nd_scroller reader;

    /* TextScroller(ui, TEXT) -- more_text and back_text keep their defaults,
     * "More" and "Back" (OPEN-QUESTIONS.md D-9). */
    nd_scroller_init(&reader, ui, text, NULL, NULL);
    nd_scroller_show(&reader);
}

static void snake_menu(nd_ui *ui)
{
    for (;;) {
        int32_t choice = show_menu(ui, "Snake", nd_games_snake_menu, ND_GAMES_SNAKE_ITEMS);

        if (choice == ND_WIDGET_BACK)
            return;

        switch (choice) {
        case 0: {
            nd_snake game;
            int32_t level =
                (int32_t)nd_games_setting_int(ND_SET_GAMES_SNAKE_LEVEL, ND_GAMES_DEFAULT_LEVEL);

            nd_snake_init(&game, ui, level);
            finish_game(ui, nd_snake_play(&game), ND_SET_GAMES_SNAKE_TOPSCORE);
            break;
        }
        case 1: {
            nd_levelsel picker;
            int32_t current =
                (int32_t)nd_games_setting_int(ND_SET_GAMES_SNAKE_LEVEL, ND_GAMES_DEFAULT_LEVEL);
            int32_t picked;

            /* LevelSelector(ui, current=current, app_id=6): count 9 and
             * title "Level" are the widget's own defaults. */
            nd_levelsel_init(&picker, ui, current, ND_LEVELSEL_MAX, "Level", ND_GAMES_APP_ID);
            picked = nd_levelsel_show(&picker);
            if (picked >= 0) /* `if picked is not None` */
                (void)nd_games_setting_set(ND_SET_GAMES_SNAKE_LEVEL, picked);
            break;
        }
        case 2:
            show_top_score(ui, ND_SET_GAMES_SNAKE_TOPSCORE);
            break;
        case 3:
            show_instructions(ui, nd_games_snake_instructions);
            break;
        default:
            break;
        }

        /* Not in the Python, which had IncomingCall to unwind it. nd_app.h:
         * a loop that outlives a frame polls this. */
        if (nd_app_should_exit())
            return;
    }
}

static void memory_menu(nd_ui *ui)
{
    for (;;) {
        int32_t choice = show_menu(ui, "Memory", nd_games_memory_menu, ND_GAMES_MEMORY_ITEMS);

        if (choice == ND_WIDGET_BACK)
            return;

        switch (choice) {
        case 0: {
            nd_memory game;

            nd_memory_init(&game, ui);
            finish_game(ui, nd_memory_play(&game), ND_SET_GAMES_MEMORY_TOPSCORE);
            break;
        }
        case 1:
            show_top_score(ui, ND_SET_GAMES_MEMORY_TOPSCORE);
            break;
        case 2:
            show_instructions(ui, nd_games_memory_instructions);
            break;
        default:
            break;
        }

        if (nd_app_should_exit())
            return;
    }
}

/* ------------------------------------------------------------------ *
 * app_run
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    if (ui == NULL)
        return 1;

    for (;;) {
        int32_t choice = show_menu(ui, "Games", nd_games_root_menu, ND_GAMES_ROOT_ITEMS);

        if (choice == ND_WIDGET_BACK)
            return 0;
        if (choice == 0)
            memory_menu(ui);
        else if (choice == 1)
            snake_menu(ui);

        if (nd_app_should_exit())
            return 0;
    }
}

/* Nothing is held: no sound card, no child process, no allocation that
 * outlives a screen. Both games live on app_run()'s stack. */
void app_shutdown(void) {}

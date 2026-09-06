/* nd_t9_bridge.c -- keypad -> virtual keyboard, for LinuxShell and netsurf.
 *
 * Ported from System/hw/t9_uinput.py, classes T9ShellBridge and
 * T9BrowserBridge.
 *
 * Two consumers, both of them keypad-only hardware talking to a program that
 * expects a keyboard. A thread reads the keypad, runs the presses through the
 * shared multi-tap engine, and types the result into a uinput device that the
 * console and netsurf see as an ordinary keyboard.
 *
 * The bridge may only run while the program it is feeding OWNS THE SCREEN.
 * The UI loop is blocked in waitpid() then, so the i2c bus is free; two
 * threads scanning the same expander would interleave row drives and produce
 * phantom keys.
 *
 * ============ WHY THE BROWSER GETS ITS OWN MODE ============
 *
 * The enrolment wizard collects Up and Down and no Left or Right at all, so
 * netsurf can never see a horizontal arrow unless the number pad stands in
 * for one. Cursor mode is a fourth stop on the # cycle rather than a separate
 * toggle key, because # is already the only "change what the keys mean" key
 * the phone has and users know it as that.
 *
 * Predictive is deliberately left out of the browser's cycle: it answers a
 * keypress with a list of candidate words, and there is no candidate UI on
 * the far side of a uinput keyboard, so the mode would look like keys that do
 * nothing.
 *
 * ============ EVERY KEY THE PHONE HAS DOES SOMETHING IN CURSOR MODE ====
 *
 * It used not to. Cursor mode answered 2/4/5/6/8 and the six passthrough
 * keys, and 1, 3, 7, 9, 0 and * were dropped on the floor -- six of the
 * sixteen keys this phone has, dead in the app the owner reported the keypad
 * broken in. The bottom half of the map below is that recovered, and the two
 * halves are recovered in two different ways because the far side of the
 * uinput device is not ours:
 *
 *   1 3 7 9   the diagonals, and they work today. Read the nine digits as
 *             the 3x3 grid they are printed as and the corners ARE the
 *             diagonals -- 2/4/6/8 are already the edges of that grid and 5
 *             its centre, so nothing had to be invented, only finished. A
 *             diagonal is sent as the two arrows it is made of, which is
 *             exactly what a person would otherwise press one after the
 *             other, so netsurf needs to know nothing about it.
 *
 *   * 0       page up and page down, and they need the netsurf half that
 *             nd_t9.h specifies. There is no way to express "scroll a
 *             screenful" in arrows: the cursor steps six pixels and only
 *             scrolls once it is pinned at an edge, so a screenful is about
 *             thirty presses, and thirty synthesised arrows would cost
 *             netsurf thirty pointer warps and redraws on a single core.
 *             They are sent anyway, because until that half lands they are
 *             consumed with no effect -- the same nothing they did before --
 *             and the day it lands the phone gains the keys with no change
 *             here.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nd_input.h"
#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_log.h"
#include "nd_t9.h"

#define POLL_TIMEOUT_S 0.05

static const int32_t PASSTHROUGH_CODES[] = {28, 14, 103, 105, 106, 108};

/* Keypad key -> one keycode, for the browser's cursor mode.
 *
 * Spelled with the ND_KEY_* names on both sides now that the table has grown
 * past the five entries a reader could hold in their head: every number in it
 * was a bare literal, and "{9, 108}" is two different keys wearing each
 * other's numbers -- keypad 8 sending KEY_DOWN. The values are unchanged. */
static const struct {
    int32_t from;
    uint16_t to;
} BROWSER_NAV_CODES[] = {
    {ND_KEY_2, ND_KEY_UP},
    {ND_KEY_4, ND_KEY_LEFT},
    {ND_KEY_5, ND_KEY_ENTER}, /* follow a link */
    {ND_KEY_6, ND_KEY_RIGHT},
    {ND_KEY_8, ND_KEY_DOWN},
    /* The two that netsurf owes an implementation; see nd_t9.h. * takes page
     * up and 0 page down because 0 sits directly under 8, the down key, and
     * reads as "further down" -- and because * is the only other key left,
     * # having been the mode cycle since the Python. */
    {ND_KEY_STAR, ND_T9_BROWSER_KEY_PAGE_UP},
    {ND_KEY_0, ND_T9_BROWSER_KEY_PAGE_DOWN},
};

/* Keypad corner -> the two arrows that corner means. Sent in the order given:
 * vertical first, so a diagonal into a page edge scrolls the way the pointer
 * was already going before the horizontal half is clamped against the side. */
static const struct {
    int32_t from;
    uint16_t first;
    uint16_t second;
} BROWSER_DIAG_CODES[] = {
    {ND_KEY_1, ND_KEY_UP, ND_KEY_LEFT},
    {ND_KEY_3, ND_KEY_UP, ND_KEY_RIGHT},
    {ND_KEY_7, ND_KEY_DOWN, ND_KEY_LEFT},
    {ND_KEY_9, ND_KEY_DOWN, ND_KEY_RIGHT},
};

/* Which mode the pad is in, one keycode each, sent on every change. The index
 * is b->pos: 0 is cursor mode and 1..3 are the engine's, which is the order
 * nd_t9_bridge_mode_label() names and the order # walks. */
static const uint16_t BROWSER_MODE_CODES[] = {
    ND_T9_BROWSER_KEY_MODE_NAV,
    ND_T9_BROWSER_KEY_MODE_ABC,
    ND_T9_BROWSER_KEY_MODE_UPPER,
    ND_T9_BROWSER_KEY_MODE_123,
};

/* nd_input.h spells the keyboard as an ANONYMOUS struct typedef
 * (`typedef struct { ... } nd_uinput_kbd;`), while nd_t9.h forward-declares
 * `struct nd_uinput_kbd` for the bridge's signature. Those are two different
 * types to the compiler. The header is frozen and other modules already
 * compile against both, so the bridge keeps the real type internally and
 * casts at the two entry points. Recorded for whoever unfreezes nd_input.h:
 * giving the struct a tag makes both spellings the same type and this cast
 * disappears. */
struct nd_t9_bridge {
    nd_bridge_kind kind;
    nd_input *in;
    nd_uinput_kbd *kbd;
    nd_t9_engine engine;

    /* Browser only. 0 = cursor, 1..n = modes[pos - 1]. */
    size_t pos;
    nd_t9_mode modes[4];
    size_t n_modes;

    pthread_t thread;
    bool running;
    volatile bool stop;
};

static bool is_passthrough(int32_t code)
{
    size_t i;

    for (i = 0u; i < ND_ARRAY_LEN(PASSTHROUGH_CODES); i++) {
        if (PASSTHROUGH_CODES[i] == code)
            return true;
    }
    return false;
}

/* T9BrowserBridge.cycle_mode: cursor is the mode before abc. */
static void browser_cycle_mode(nd_t9_bridge *b)
{
    nd_t9_engine_reset(&b->engine);
    b->pos = (b->pos + 1u) % (b->n_modes + 1u);
    if (b->pos != 0u) {
        const nd_t9_mode *all;
        size_t n = 0u;
        size_t i;

        /* Index into the ENGINE's own list, which still contains the mode the
         * bridge skips, so the engine ends up in the mode this bridge names. */
        all = nd_t9_engine_modes(&b->engine, &n);
        for (i = 0u; i < n; i++) {
            if (all[i] == b->modes[b->pos - 1u]) {
                (void)nd_t9_engine_set_mode_index(&b->engine, i);
                break;
            }
        }
    }

    /* TELL THE FAR SIDE. nd_t9_bridge_mode_label() has always known which of
     * the four the pad is in, and for three releases the only thing that ever
     * asked was a log line at launch -- so the mode changed under the owner
     * with nothing on the screen saying so, on a phone where the same key
     * types 'a' or '2' or scrolls depending on a state they could not see.
     *
     * The core cannot draw it: netsurf owns the panel from the moment it
     * starts until it exits. A keycode can get there, because a keycode is
     * the one channel that already runs from here to inside netsurf's own
     * event loop. See nd_t9.h for what netsurf does with it.
     *
     * The bounds check is not decoration: b->pos is taken modulo n_modes + 1
     * and n_modes comes from the engine's list, so a mode added to the engine
     * would walk this table off its end rather than fail to say something. */
    if (b->pos < ND_ARRAY_LEN(BROWSER_MODE_CODES))
        (void)nd_uinput_send_key(b->kbd, BROWSER_MODE_CODES[b->pos], false);
}

/* KEY_TAB. Spelled out rather than included from linux/input-event-codes.h,
 * which this file does not otherwise need and which is not available on every
 * host the tests build on. */
#define SHELL_KEY_TAB 15u

static void shell_handle(nd_t9_bridge *b, int32_t code)
{
    nd_t9_op op;

    /* Star completes. A shell driven from a keypad types every path a letter
     * at a time out of a multi-tap cycle, and busybox ash has had
     * FEATURE_TAB_COMPLETION the whole time -- there was just no key that
     * produced a Tab.
     *
     * Star is free here, in the exact sense that matters: in the letter modes
     * the engine resets and returns an op carrying no character, so the press
     * has always been swallowed. Nothing is being taken away. Mode cycling
     * lives on #, so digits are untouched.
     *
     * 123 mode is left alone, where star is a literal '*'. A shell that
     * cannot glob is a worse thing than one that cannot complete, so the two
     * share the key by mode instead of one displacing the other.
     *
     * The engine is reset first for the same reason nav keys reset it: a
     * half-finished multi-tap letter has to be committed before something
     * else reaches the terminal, or the next press of that digit would
     * backspace over text the shell has already been given. */
    if (code == ND_KEY_STAR && nd_t9_engine_mode(&b->engine) != ND_T9_MODE_123) {
        nd_t9_engine_reset(&b->engine);
        (void)nd_uinput_send_key(b->kbd, SHELL_KEY_TAB, false);
        return;
    }

    if (is_passthrough(code)) {
        nd_t9_engine_reset(&b->engine);
        (void)nd_uinput_send_key(b->kbd, (uint16_t)code, false);
        return;
    }
    op = nd_t9_engine_press(&b->engine, code);
    switch (op.kind) {
    case ND_T9_OP_APPEND:
        (void)nd_uinput_type_char(b->kbd, op.ch);
        break;
    case ND_T9_OP_REPLACE:
        /* Cycling a multi-tap letter is a backspace and a new character;
         * there is no "edit the last one" on a real keyboard. */
        (void)nd_uinput_backspace(b->kbd);
        (void)nd_uinput_type_char(b->kbd, op.ch);
        break;
    default:
        /* MODE has nothing to send -- the shell prompt has no indicator --
         * and WORD/NEXT cannot occur: predictive is not in either cycle. */
        break;
    }
}

void nd_t9_bridge_handle_code(nd_t9_bridge *b, int32_t code)
{
    if (b == NULL)
        return;

    if (b->kind == ND_BRIDGE_BROWSER) {
        if (code == ND_KEY_HASH) {
            browser_cycle_mode(b);
            return;
        }
        if (b->pos == 0u) {
            size_t i;

            for (i = 0u; i < ND_ARRAY_LEN(BROWSER_NAV_CODES); i++) {
                if (BROWSER_NAV_CODES[i].from == code) {
                    (void)nd_uinput_send_key(b->kbd, BROWSER_NAV_CODES[i].to, false);
                    return;
                }
            }
            for (i = 0u; i < ND_ARRAY_LEN(BROWSER_DIAG_CODES); i++) {
                if (BROWSER_DIAG_CODES[i].from == code) {
                    /* Two whole keystrokes, not a chord: netsurf moves its
                     * pointer once per press and clamps each axis on its own,
                     * so a diagonal into a corner has to arrive as the two
                     * moves it is or one of them is lost. The second is sent
                     * even if the first failed -- a half-written keystroke is
                     * a pointer that drifts sideways, which is worse than a
                     * press that did nothing. */
                    (void)nd_uinput_send_key(b->kbd, BROWSER_DIAG_CODES[i].first, false);
                    (void)nd_uinput_send_key(b->kbd, BROWSER_DIAG_CODES[i].second, false);
                    return;
                }
            }
            if (is_passthrough(code))
                (void)nd_uinput_send_key(b->kbd, (uint16_t)code, false);
            /* Nothing is left but a key the phone does not have. Letters stay
             * out of cursor mode on purpose -- typing one by accident while
             * scrolling is worse than the press doing nothing -- and every key
             * the phone DOES have is answered above. */
            return;
        }
    }
    shell_handle(b, code);
}

static void *bridge_loop(void *arg)
{
    nd_t9_bridge *b = (nd_t9_bridge *)arg;

    while (!b->stop) {
        int32_t code = nd_input_read_key(b->in, POLL_TIMEOUT_S);

        if (code == ND_KEY_NONE)
            continue;
        nd_t9_bridge_handle_code(b, code);
    }
    return NULL;
}

nd_t9_bridge *nd_t9_bridge_start(nd_bridge_kind kind, struct nd_input *in,
                                 struct nd_uinput_kbd *kbd)
{
    /* owned by the caller; free with nd_t9_bridge_stop() */
    nd_t9_bridge *b;
    nd_input *input = (nd_input *)in;

    /* No matrix keypad means QEMU or a dev build, where a real keyboard
     * already reaches the console. Returning NULL is the correct no-op. */
    if (input == NULL || kbd == NULL || !nd_input_has_matrix(input))
        return NULL;

    b = calloc(1u, sizeof *b);
    if (b == NULL)
        return NULL;

    b->kind = kind;
    b->in = input;
    b->kbd = (nd_uinput_kbd *)kbd;
    if (nd_t9_engine_init(&b->engine, ND_T9_FILTER_ANY, 0.0, NULL, NULL) != ND_OK) {
        free(b);
        return NULL;
    }

    if (kind == ND_BRIDGE_BROWSER) {
        const nd_t9_mode *all;
        size_t n = 0u;
        size_t i;

        all = nd_t9_engine_modes(&b->engine, &n);
        for (i = 0u; i < n; i++) {
            if (all[i] != ND_T9_MODE_WORD)
                b->modes[b->n_modes++] = all[i];
        }
        b->pos = 0u; /* the browser starts as a d-pad */
    }

    if (pthread_create(&b->thread, NULL, bridge_loop, b) != 0) {
        nd_log_err(ND_LOG_INPUT, "T9 bridge thread failed to start");
        free(b);
        return NULL;
    }
    b->running = true;
    return b;
}

void nd_t9_bridge_stop(nd_t9_bridge *b)
{
    if (b == NULL)
        return;
    b->stop = true;
    if (b->running)
        (void)pthread_join(b->thread, NULL);
    nd_uinput_close(b->kbd);
    free(b);
}

/* ------------------------------------------------------------------ *
 * Exposed for the tests, which cannot start a thread against real hardware
 * ------------------------------------------------------------------ */

nd_t9_bridge *nd_t9_bridge_new_for_test(nd_bridge_kind kind, nd_uinput_kbd *kbd)
{
    /* owned by the caller; free with nd_t9_bridge_free_for_test() */
    nd_t9_bridge *b = calloc(1u, sizeof *b);

    if (b == NULL)
        return NULL;
    b->kind = kind;
    b->kbd = kbd;
    if (nd_t9_engine_init(&b->engine, ND_T9_FILTER_ANY, 0.0, NULL, NULL) != ND_OK) {
        free(b);
        return NULL;
    }
    if (kind == ND_BRIDGE_BROWSER) {
        const nd_t9_mode *all;
        size_t n = 0u;
        size_t i;

        all = nd_t9_engine_modes(&b->engine, &n);
        for (i = 0u; i < n; i++) {
            if (all[i] != ND_T9_MODE_WORD)
                b->modes[b->n_modes++] = all[i];
        }
    }
    return b;
}

void nd_t9_bridge_free_for_test(nd_t9_bridge *b)
{
    free(b);
}

/* "nav" while the browser's cursor mode is active, otherwise the engine's
 * own label. This is what the mode line in the browser chrome shows. */
const char *nd_t9_bridge_mode_label(const nd_t9_bridge *b)
{
    if (b == NULL)
        return "abc";
    if (b->kind == ND_BRIDGE_BROWSER && b->pos == 0u)
        return "nav";
    return nd_t9_mode_label(nd_t9_engine_mode(&b->engine));
}

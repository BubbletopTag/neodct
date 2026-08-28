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

/* Keypad digit -> arrow, for the browser's cursor mode. */
static const struct {
    int32_t from;
    uint16_t to;
} BROWSER_NAV_CODES[] = {
    {3, 103}, /* keypad 2 -> up            */
    {5, 105}, /* keypad 4 -> left          */
    {6, 28},  /* keypad 5 -> follow a link */
    {7, 106}, /* keypad 6 -> right         */
    {9, 108}, /* keypad 8 -> down          */
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
            if (is_passthrough(code))
                (void)nd_uinput_send_key(b->kbd, (uint16_t)code, false);
            /* Every other key is inert here: typing a letter by accident
             * while scrolling is worse than the press doing nothing. */
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

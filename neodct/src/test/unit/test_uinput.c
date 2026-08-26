/* test_uinput.c -- the virtual keyboard and the two T9 bridges.
 *
 * Ported from neodct/tests/test_t9_uinput.py. Those tests work by handing
 * UInputKeyboard a pipe with `fd=` so no /dev/uinput and no root are needed,
 * and PORT-PLAN.md WP-10 is explicit that the injection point must survive
 * into the C as nd_uinput_attach(). It did, and this is it in use.
 *
 * What is being checked is the SHAPE of what goes on the wire, because that
 * is what the console and netsurf actually see: a shifted character is four
 * key transitions and four SYN_REPORTs, in that order, and a multi-tap cycle
 * is a backspace followed by the new letter -- there is no "edit the last
 * character" on a real keyboard.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "nd_keycodes.h"
#include "nd_keypad.h"

#include "platform_test.h"

#define EV_SYN_T     0x00
#define EV_KEY_T     0x01
#define SYN_REPORT_T 0

typedef struct {
    long tv_sec;
    long tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
} ev_native;

typedef struct {
    nd_uinput_kbd kbd;
    int read_fd;
    int write_fd;
} fake_kbd;

static void fake_kbd_open(fake_kbd *fk)
{
    int fds[2];
    int flags;

    CHECK_INT(pipe(fds), 0);
    fk->read_fd = fds[0];
    fk->write_fd = fds[1];
    flags = fcntl(fk->read_fd, F_GETFL, 0);
    CHECK_INT(fcntl(fk->read_fd, F_SETFL, flags | O_NONBLOCK), 0);
    CHECK_INT(nd_uinput_attach(&fk->kbd, fk->write_fd), ND_OK);
    CHECK(!fk->kbd.owns_device);
}

static void fake_kbd_close(fake_kbd *fk)
{
    /* nd_uinput_close() closes the descriptor, matching
     * UInputKeyboard.close(), so the write end must not be closed twice. */
    nd_uinput_close(&fk->kbd);
    (void)close(fk->read_fd);
}

static size_t drain(fake_kbd *fk, ev_native *out, size_t max)
{
    uint8_t buf[4096];
    ssize_t got = read(fk->read_fd, buf, sizeof buf);
    size_t n = 0u;
    size_t off = 0u;

    if (got <= 0)
        return 0u;
    while (off + sizeof(ev_native) <= (size_t)got && n < max) {
        memcpy(&out[n], buf + off, sizeof(ev_native));
        off += sizeof(ev_native);
        n++;
    }
    return n;
}

static void check_rec(const ev_native *r, uint16_t type, uint16_t code, int32_t value,
                      const char *what)
{
    g_checks++;
    if (r->type != type || r->code != code || r->value != value) {
        g_failures++;
        fprintf(stderr, "FAIL %s: got (%u, %u, %d) want (%u, %u, %d)\n", what, r->type, r->code,
                r->value, type, code, value);
    }
}

/* ------------------------------------------------------------------ *
 * The character table
 * ------------------------------------------------------------------ */

static void test_char_to_keypress_covers_the_us_layout(void)
{
    uint16_t code = 0u;
    bool shift = true;

    CHECK(nd_uinput_char_to_keypress('a', &code, &shift));
    CHECK_INT(code, 30);
    CHECK(!shift);

    /* An upper-case letter is the lower-case key plus shift. */
    CHECK(nd_uinput_char_to_keypress('A', &code, &shift));
    CHECK_INT(code, 30);
    CHECK(shift);

    CHECK(nd_uinput_char_to_keypress('!', &code, &shift));
    CHECK_INT(code, 2);
    CHECK(shift);

    CHECK(nd_uinput_char_to_keypress('1', &code, &shift));
    CHECK_INT(code, 2);
    CHECK(!shift);

    CHECK(nd_uinput_char_to_keypress(' ', &code, &shift));
    CHECK_INT(code, 57);
    CHECK(!shift);

    CHECK(nd_uinput_char_to_keypress('\n', &code, &shift));
    CHECK_INT(code, 28);

    /* Nothing on a US keyboard produces this, so it cannot be typed. */
    CHECK(!nd_uinput_char_to_keypress('\x01', &code, &shift));
    CHECK(!nd_uinput_char_to_keypress((char)0xC3, &code, &shift));
}

/* ------------------------------------------------------------------ *
 * Emission
 * ------------------------------------------------------------------ */

static void test_an_unshifted_key_is_down_syn_up_syn(void)
{
    fake_kbd fk;
    ev_native recs[16];
    size_t n;

    fake_kbd_open(&fk);
    CHECK_INT(nd_uinput_send_key(&fk.kbd, 30u, false), ND_OK);

    n = drain(&fk, recs, 16u);
    CHECK_INT(n, 4);
    check_rec(&recs[0], EV_KEY_T, 30u, 1, "press");
    check_rec(&recs[1], EV_SYN_T, SYN_REPORT_T, 0, "syn");
    check_rec(&recs[2], EV_KEY_T, 30u, 0, "release");
    check_rec(&recs[3], EV_SYN_T, SYN_REPORT_T, 0, "syn");

    fake_kbd_close(&fk);
}

static void test_a_shifted_key_brackets_the_press_with_shift(void)
{
    fake_kbd fk;
    ev_native recs[16];
    size_t n;

    /* Shift has to go down BEFORE and come up AFTER, each with its own
     * SYN_REPORT, or the console applies it to the wrong character. */
    fake_kbd_open(&fk);
    CHECK(nd_uinput_type_char(&fk.kbd, 'A'));

    n = drain(&fk, recs, 16u);
    CHECK_INT(n, 8);
    check_rec(&recs[0], EV_KEY_T, 42u, 1, "shift down");
    check_rec(&recs[2], EV_KEY_T, 30u, 1, "a down");
    check_rec(&recs[4], EV_KEY_T, 30u, 0, "a up");
    check_rec(&recs[6], EV_KEY_T, 42u, 0, "shift up");

    fake_kbd_close(&fk);
}

static void test_an_untypeable_character_is_refused(void)
{
    fake_kbd fk;
    ev_native recs[16];

    fake_kbd_open(&fk);
    CHECK(!nd_uinput_type_char(&fk.kbd, '\x01'));
    CHECK_INT(drain(&fk, recs, 16u), 0);
    fake_kbd_close(&fk);
}

static void test_backspace_is_key_14(void)
{
    fake_kbd fk;
    ev_native recs[16];

    fake_kbd_open(&fk);
    CHECK_INT(nd_uinput_backspace(&fk.kbd), ND_OK);
    CHECK_INT(drain(&fk, recs, 16u), 4);
    check_rec(&recs[0], EV_KEY_T, 14u, 1, "backspace");
    fake_kbd_close(&fk);
}

/* ------------------------------------------------------------------ *
 * The shell bridge
 * ------------------------------------------------------------------ */

static void test_the_shell_bridge_types_multi_tap_letters(void)
{
    fake_kbd fk;
    nd_t9_bridge *b;
    ev_native recs[32];
    size_t n;

    fake_kbd_open(&fk);
    b = nd_t9_bridge_new_for_test(ND_BRIDGE_SHELL, &fk.kbd);
    CHECK(b != NULL);

    nd_t9_bridge_handle_code(b, 3); /* key 2 -> 'a' */
    n = drain(&fk, recs, 32u);
    CHECK_INT(n, 4);
    check_rec(&recs[0], EV_KEY_T, 30u, 1, "a");

    /* Cycling is a backspace and a new character: there is no "replace the
     * last one" on the far side of a uinput device. */
    nd_t9_bridge_handle_code(b, 3); /* -> 'b' */
    n = drain(&fk, recs, 32u);
    CHECK_INT(n, 8);
    check_rec(&recs[0], EV_KEY_T, 14u, 1, "backspace");
    check_rec(&recs[4], EV_KEY_T, 48u, 1, "b");

    nd_t9_bridge_free_for_test(b);
    fake_kbd_close(&fk);
}

static void test_the_shell_bridge_passes_navigation_straight_through(void)
{
    fake_kbd fk;
    nd_t9_bridge *b;
    ev_native recs[32];
    size_t n;

    /* NeoDCT keycodes ARE Linux keycodes, so enter, clear and the arrows need
     * no translation table -- which is why there is not one. */
    fake_kbd_open(&fk);
    b = nd_t9_bridge_new_for_test(ND_BRIDGE_SHELL, &fk.kbd);
    CHECK(b != NULL);

    nd_t9_bridge_handle_code(b, ND_KEY_ENTER);
    n = drain(&fk, recs, 32u);
    CHECK_INT(n, 4);
    check_rec(&recs[0], EV_KEY_T, 28u, 1, "enter");

    /* A passthrough also commits any pending multi-tap cycle, so the next
     * press of the same key starts a fresh letter rather than cycling. */
    nd_t9_bridge_handle_code(b, 3);
    (void)drain(&fk, recs, 32u);
    nd_t9_bridge_handle_code(b, ND_KEY_LEFT);
    (void)drain(&fk, recs, 32u);
    nd_t9_bridge_handle_code(b, 3);
    n = drain(&fk, recs, 32u);
    CHECK_INT(n, 4);
    check_rec(&recs[0], EV_KEY_T, 30u, 1, "a again, not b");

    nd_t9_bridge_free_for_test(b);
    fake_kbd_close(&fk);
}

static void test_the_shell_bridge_sends_nothing_on_a_mode_change(void)
{
    fake_kbd fk;
    nd_t9_bridge *b;
    ev_native recs[32];

    /* The shell prompt has no mode indicator, so there is nothing to send. */
    fake_kbd_open(&fk);
    b = nd_t9_bridge_new_for_test(ND_BRIDGE_SHELL, &fk.kbd);
    CHECK(b != NULL);

    nd_t9_bridge_handle_code(b, ND_KEY_HASH);
    CHECK_INT(drain(&fk, recs, 32u), 0);
    CHECK_STR(nd_t9_bridge_mode_label(b), "ABC");

    nd_t9_bridge_handle_code(b, 3);
    CHECK_INT(drain(&fk, recs, 32u), 8); /* shift + 'a' = 'A' */

    nd_t9_bridge_free_for_test(b);
    fake_kbd_close(&fk);
}

/* ------------------------------------------------------------------ *
 * The browser bridge
 * ------------------------------------------------------------------ */

static void test_the_browser_starts_as_a_dpad(void)
{
    fake_kbd fk;
    nd_t9_bridge *b;
    ev_native recs[32];
    size_t n;

    /* The enrolment wizard collects Up and Down and no Left or Right at all,
     * so netsurf can never see a horizontal arrow unless the number pad
     * stands in for one. */
    fake_kbd_open(&fk);
    b = nd_t9_bridge_new_for_test(ND_BRIDGE_BROWSER, &fk.kbd);
    CHECK(b != NULL);
    CHECK_STR(nd_t9_bridge_mode_label(b), "nav");

    nd_t9_bridge_handle_code(b, 3); /* keypad 2 -> up */
    n = drain(&fk, recs, 32u);
    CHECK_INT(n, 4);
    check_rec(&recs[0], EV_KEY_T, 103u, 1, "up");

    nd_t9_bridge_handle_code(b, 5); /* keypad 4 -> left */
    n = drain(&fk, recs, 32u);
    check_rec(&recs[0], EV_KEY_T, 105u, 1, "left");

    nd_t9_bridge_handle_code(b, 6); /* keypad 5 -> follow the link */
    n = drain(&fk, recs, 32u);
    check_rec(&recs[0], EV_KEY_T, 28u, 1, "enter");

    nd_t9_bridge_handle_code(b, 7); /* keypad 6 -> right */
    n = drain(&fk, recs, 32u);
    check_rec(&recs[0], EV_KEY_T, 106u, 1, "right");

    nd_t9_bridge_handle_code(b, 9); /* keypad 8 -> down */
    n = drain(&fk, recs, 32u);
    check_rec(&recs[0], EV_KEY_T, 108u, 1, "down");

    nd_t9_bridge_free_for_test(b);
    fake_kbd_close(&fk);
}

static void test_every_other_key_is_inert_while_scrolling(void)
{
    fake_kbd fk;
    nd_t9_bridge *b;
    ev_native recs[32];

    /* Typing a letter by accident while scrolling is worse than the press
     * doing nothing. */
    fake_kbd_open(&fk);
    b = nd_t9_bridge_new_for_test(ND_BRIDGE_BROWSER, &fk.kbd);
    CHECK(b != NULL);

    nd_t9_bridge_handle_code(b, 4);  /* keypad 3: no arrow, no letter */
    nd_t9_bridge_handle_code(b, 11); /* keypad 0                      */
    nd_t9_bridge_handle_code(b, 42); /* star                          */
    CHECK_INT(drain(&fk, recs, 32u), 0);

    /* Enter, clear and the real arrows still pass through. */
    nd_t9_bridge_handle_code(b, ND_KEY_CLEAR);
    CHECK_INT(drain(&fk, recs, 32u), 4);

    nd_t9_bridge_free_for_test(b);
    fake_kbd_close(&fk);
}

static void test_hash_walks_nav_abc_upper_123_and_back(void)
{
    fake_kbd fk;
    nd_t9_bridge *b;
    ev_native recs[32];

    /* Cursor mode is a fourth stop on the # cycle rather than a separate
     * toggle key, because # is already the only "change what the keys mean"
     * key the phone has. Predictive is left out: there is no candidate UI on
     * the far side of a uinput keyboard. */
    fake_kbd_open(&fk);
    b = nd_t9_bridge_new_for_test(ND_BRIDGE_BROWSER, &fk.kbd);
    CHECK(b != NULL);

    CHECK_STR(nd_t9_bridge_mode_label(b), "nav");
    nd_t9_bridge_handle_code(b, ND_KEY_HASH);
    CHECK_STR(nd_t9_bridge_mode_label(b), "abc");
    nd_t9_bridge_handle_code(b, ND_KEY_HASH);
    CHECK_STR(nd_t9_bridge_mode_label(b), "ABC");
    nd_t9_bridge_handle_code(b, ND_KEY_HASH);
    CHECK_STR(nd_t9_bridge_mode_label(b), "123");
    nd_t9_bridge_handle_code(b, ND_KEY_HASH);
    CHECK_STR(nd_t9_bridge_mode_label(b), "nav");
    CHECK_INT(drain(&fk, recs, 32u), 0);

    /* In abc the number pad types instead of scrolling. */
    nd_t9_bridge_handle_code(b, ND_KEY_HASH);
    nd_t9_bridge_handle_code(b, 3);
    CHECK_INT(drain(&fk, recs, 32u), 4);
    check_rec(&recs[0], EV_KEY_T, 30u, 1, "a");

    nd_t9_bridge_free_for_test(b);
    fake_kbd_close(&fk);
}

static void test_a_bridge_without_a_matrix_is_a_no_op(void)
{
    fake_kbd fk;

    /* QEMU and dev builds have a real keyboard reaching the console already,
     * so starting a bridge there would double every keystroke. */
    fake_kbd_open(&fk);
    CHECK(nd_t9_bridge_start(ND_BRIDGE_SHELL, NULL, (struct nd_uinput_kbd *)&fk.kbd) == NULL);
    fake_kbd_close(&fk);
}

int main(void)
{
    RUN(test_char_to_keypress_covers_the_us_layout);
    RUN(test_an_unshifted_key_is_down_syn_up_syn);
    RUN(test_a_shifted_key_brackets_the_press_with_shift);
    RUN(test_an_untypeable_character_is_refused);
    RUN(test_backspace_is_key_14);
    RUN(test_the_shell_bridge_types_multi_tap_letters);
    RUN(test_the_shell_bridge_passes_navigation_straight_through);
    RUN(test_the_shell_bridge_sends_nothing_on_a_mode_change);
    RUN(test_the_browser_starts_as_a_dpad);
    RUN(test_every_other_key_is_inert_while_scrolling);
    RUN(test_hash_walks_nav_abc_upper_123_and_back);
    RUN(test_a_bridge_without_a_matrix_is_a_no_op);
    return pt_report("test_uinput");
}

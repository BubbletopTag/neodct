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
#include "nd_t9.h"

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
 * Tab completion on the star key
 * ------------------------------------------------------------------ */

/* A shell without completion on a keypad is a cruel thing: every path is
 * typed a letter at a time out of a multi-tap cycle. Tab is what fixes that,
 * and busybox ash has had FEATURE_TAB_COMPLETION all along -- there was simply
 * no key that produced a Tab.
 *
 * Star is where it goes because in the shell's letter modes star does nothing
 * whatsoever: the engine resets and returns an op with no character, so the
 * press has always been swallowed. Mode cycling is on #, not star, so digits
 * are unaffected. */
static void test_star_sends_tab_in_the_shell(void)
{
    fake_kbd fk;
    nd_t9_bridge *b;
    ev_native recs[32];

    fake_kbd_open(&fk);
    b = nd_t9_bridge_new_for_test(ND_BRIDGE_SHELL, &fk.kbd);
    CHECK(b != NULL);

    nd_t9_bridge_handle_code(b, ND_KEY_STAR);
    CHECK_INT(drain(&fk, recs, 32u), 4);
    check_rec(&recs[0], EV_KEY_T, 15u, 1, "tab");

    nd_t9_bridge_free_for_test(b);
    fake_kbd_close(&fk);
}

/* A half-typed multi-tap letter is committed, not carried across the Tab.
 * Pressing 2 then star must complete "a", not leave the cycle live so that
 * the next 2 turns it into "b" after the shell has already seen it. */
static void test_star_commits_a_pending_letter_first(void)
{
    fake_kbd fk;
    nd_t9_bridge *b;
    ev_native recs[32];

    fake_kbd_open(&fk);
    b = nd_t9_bridge_new_for_test(ND_BRIDGE_SHELL, &fk.kbd);
    CHECK(b != NULL);

    nd_t9_bridge_handle_code(b, 3); /* 'a', still cycling */
    CHECK_INT(drain(&fk, recs, 32u), 4);
    nd_t9_bridge_handle_code(b, ND_KEY_STAR);
    CHECK_INT(drain(&fk, recs, 32u), 4);
    check_rec(&recs[0], EV_KEY_T, 15u, 1, "tab");

    /* The cycle is over, so this is a fresh 'a' rather than a backspace
     * and a 'b'. Four events, not eight. */
    nd_t9_bridge_handle_code(b, 3);
    CHECK_INT(drain(&fk, recs, 32u), 4);
    check_rec(&recs[0], EV_KEY_T, 30u, 1, "a again");

    nd_t9_bridge_free_for_test(b);
    fake_kbd_close(&fk);
}

/* 123 mode keeps the literal star. A shell without '*' cannot glob, and that
 * is a worse loss than completion is a gain -- so the two share the key by
 * mode rather than one replacing the other. */
static void test_star_still_types_a_star_in_numeric_mode(void)
{
    fake_kbd fk;
    nd_t9_bridge *b;
    ev_native recs[32];
    int guard;

    fake_kbd_open(&fk);
    b = nd_t9_bridge_new_for_test(ND_BRIDGE_SHELL, &fk.kbd);
    CHECK(b != NULL);

    /* # cycles: abc -> ABC -> 123. Walk to 123 by label rather than by a
     * fixed number of presses, so this does not break if a mode is added. */
    for (guard = 0; guard < 8; guard++) {
        if (strcmp(nd_t9_bridge_mode_label(b), "123") == 0)
            break;
        nd_t9_bridge_handle_code(b, ND_KEY_HASH);
        (void)drain(&fk, recs, 32u);
    }
    CHECK_STR(nd_t9_bridge_mode_label(b), "123");

    /* '*' is shift+8 on the far side of a uinput device: two keys, eight
     * events, shift first. */
    nd_t9_bridge_handle_code(b, ND_KEY_STAR);
    CHECK_INT(drain(&fk, recs, 32u), 8);
    check_rec(&recs[0], EV_KEY_T, 42u, 1, "leftshift");
    check_rec(&recs[2], EV_KEY_T, 9u, 1, "8, shifted into '*'");

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

static void test_the_corners_of_the_pad_are_the_diagonals(void)
{
    fake_kbd fk;
    nd_t9_bridge *b;
    ev_native recs[32];

    /* 1, 3, 7 and 9 did nothing at all for three releases -- with 0 and *,
     * six of this phone's sixteen keys dead in the app the owner reported
     * the keypad broken in. Read the nine digits as the 3x3 grid they are
     * printed on and the corners ARE the diagonals; 2/4/6/8 were already its
     * edges and 5 its centre.
     *
     * Each arrives as the two arrows it is made of, VERTICAL FIRST, because
     * netsurf moves its pointer once per press and clamps each axis on its
     * own -- a diagonal that arrived as one event would lose an axis at a
     * page edge. */
    fake_kbd_open(&fk);
    b = nd_t9_bridge_new_for_test(ND_BRIDGE_BROWSER, &fk.kbd);
    CHECK(b != NULL);

    nd_t9_bridge_handle_code(b, ND_KEY_1);
    CHECK_INT(drain(&fk, recs, 32u), 8);
    check_rec(&recs[0], EV_KEY_T, 103u, 1, "1 is up");
    check_rec(&recs[4], EV_KEY_T, 105u, 1, "1 is then left");

    nd_t9_bridge_handle_code(b, ND_KEY_3);
    CHECK_INT(drain(&fk, recs, 32u), 8);
    check_rec(&recs[0], EV_KEY_T, 103u, 1, "3 is up");
    check_rec(&recs[4], EV_KEY_T, 106u, 1, "3 is then right");

    nd_t9_bridge_handle_code(b, ND_KEY_7);
    CHECK_INT(drain(&fk, recs, 32u), 8);
    check_rec(&recs[0], EV_KEY_T, 108u, 1, "7 is down");
    check_rec(&recs[4], EV_KEY_T, 105u, 1, "7 is then left");

    nd_t9_bridge_handle_code(b, ND_KEY_9);
    CHECK_INT(drain(&fk, recs, 32u), 8);
    check_rec(&recs[0], EV_KEY_T, 108u, 1, "9 is down");
    check_rec(&recs[4], EV_KEY_T, 106u, 1, "9 is then right");

    nd_t9_bridge_free_for_test(b);
    fake_kbd_close(&fk);
}

static void test_star_and_zero_are_the_page_keys(void)
{
    fake_kbd fk;
    nd_t9_bridge *b;
    ev_native recs[32];

    /* The cursor steps six pixels and only scrolls once it is pinned against
     * an edge, so a screenful of a web page is about thirty presses of 8 and
     * there is no key that does it in one. These two are that key, and they
     * are sent as the real KEY_PAGEUP/KEY_PAGEDOWN rather than as a burst of
     * arrows: thirty synthesised arrows would cost netsurf thirty pointer
     * warps and redraws on a single core and would read as a hang.
     *
     * The keycodes are the contract in nd_t9.h, checked against ITS names
     * rather than against literals repeated here, because the whole point of
     * the contract is that one place says what the number is. */
    fake_kbd_open(&fk);
    b = nd_t9_bridge_new_for_test(ND_BRIDGE_BROWSER, &fk.kbd);
    CHECK(b != NULL);

    nd_t9_bridge_handle_code(b, ND_KEY_STAR);
    CHECK_INT(drain(&fk, recs, 32u), 4);
    check_rec(&recs[0], EV_KEY_T, ND_T9_BROWSER_KEY_PAGE_UP, 1, "star is page up");

    nd_t9_bridge_handle_code(b, ND_KEY_0);
    CHECK_INT(drain(&fk, recs, 32u), 4);
    check_rec(&recs[0], EV_KEY_T, ND_T9_BROWSER_KEY_PAGE_DOWN, 1, "0 is page down");

    nd_t9_bridge_free_for_test(b);
    fake_kbd_close(&fk);
}

static void test_a_key_the_phone_does_not_have_is_still_inert(void)
{
    fake_kbd fk;
    nd_t9_bridge *b;
    ev_native recs[32];

    /* Every one of the sixteen keys now means something in cursor mode, so
     * what is left to be inert is what a dev QWERTY produces and the phone
     * cannot: typing a letter by accident while scrolling is worse than the
     * press doing nothing. */
    fake_kbd_open(&fk);
    b = nd_t9_bridge_new_for_test(ND_BRIDGE_BROWSER, &fk.kbd);
    CHECK(b != NULL);

    nd_t9_bridge_handle_code(b, 30); /* KEY_A */
    nd_t9_bridge_handle_code(b, 57); /* KEY_SPACE */
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

    /* Four changes, four announcements, and nothing else on the wire. This
     * used to check that a mode change sent NOTHING, which was true and was
     * the bug: the label existed and the owner could not see it. */
    CHECK_INT(drain(&fk, recs, 32u), 16);

    /* In abc the number pad types instead of scrolling. */
    nd_t9_bridge_handle_code(b, ND_KEY_HASH);
    nd_t9_bridge_handle_code(b, 3);
    CHECK_INT(drain(&fk, recs, 32u), 4);
    check_rec(&recs[0], EV_KEY_T, 30u, 1, "a");

    nd_t9_bridge_free_for_test(b);
    fake_kbd_close(&fk);
}

static void test_the_mode_is_announced_as_a_keycode(void)
{
    fake_kbd fk;
    nd_t9_bridge *b;
    ev_native recs[32];

    /* THE MODE HAS TO CROSS THE UINPUT DEVICE, because netsurf owns the panel
     * for the whole session and the core cannot paint over it. One keycode
     * per mode, not a single "it advanced" pulse: a pulse makes the far side
     * keep its own copy of the cycle, and that copy is wrong for the rest of
     * the session the first time it misses one -- which it does whenever the
     * owner presses # during the seconds netsurf spends starting up, since
     * the bridge is already running by then.
     *
     * So each of these restates the whole truth, and the test walks the cycle
     * twice to prove the second lap says the same thing as the first. */
    fake_kbd_open(&fk);
    b = nd_t9_bridge_new_for_test(ND_BRIDGE_BROWSER, &fk.kbd);
    CHECK(b != NULL);

    nd_t9_bridge_handle_code(b, ND_KEY_HASH);
    CHECK_INT(drain(&fk, recs, 32u), 4);
    check_rec(&recs[0], EV_KEY_T, ND_T9_BROWSER_KEY_MODE_ABC, 1, "abc");

    nd_t9_bridge_handle_code(b, ND_KEY_HASH);
    CHECK_INT(drain(&fk, recs, 32u), 4);
    check_rec(&recs[0], EV_KEY_T, ND_T9_BROWSER_KEY_MODE_UPPER, 1, "ABC");

    nd_t9_bridge_handle_code(b, ND_KEY_HASH);
    CHECK_INT(drain(&fk, recs, 32u), 4);
    check_rec(&recs[0], EV_KEY_T, ND_T9_BROWSER_KEY_MODE_123, 1, "123");

    nd_t9_bridge_handle_code(b, ND_KEY_HASH);
    CHECK_INT(drain(&fk, recs, 32u), 4);
    check_rec(&recs[0], EV_KEY_T, ND_T9_BROWSER_KEY_MODE_NAV, 1, "back to nav");

    nd_t9_bridge_handle_code(b, ND_KEY_HASH);
    CHECK_INT(drain(&fk, recs, 32u), 4);
    check_rec(&recs[0], EV_KEY_T, ND_T9_BROWSER_KEY_MODE_ABC, 1, "abc again");

    nd_t9_bridge_free_for_test(b);
    fake_kbd_close(&fk);
}

/* Every signalling keycode has to be DECLARED before the kernel will carry
 * it: an undeclared code is dropped inside evdev with no error on the
 * descriptor and nothing in dmesg, which is a key that passes every test on
 * this host -- where the keyboard is a pipe -- and does nothing on the phone.
 * The pipe cannot catch that, so what is checked here is the table itself. */
static void test_the_signalling_codes_are_inside_libnsfbs_range(void)
{
    /* libnsfb's evdev table (src/surface/linux_evdev.c) maps codes 1..111 and
     * answers NSFB_KEY_UNKNOWN for anything above, and its linux surface drops
     * an UNKNOWN before netsurf ever sees the event. A contract keycode above
     * 111 is therefore unreachable however free it looks here. */
    CHECK(ND_T9_BROWSER_KEY_PAGE_UP <= 111u);
    CHECK(ND_T9_BROWSER_KEY_PAGE_DOWN <= 111u);
    CHECK(ND_T9_BROWSER_KEY_MODE_NAV <= 111u);
    CHECK(ND_T9_BROWSER_KEY_MODE_ABC <= 111u);
    CHECK(ND_T9_BROWSER_KEY_MODE_UPPER <= 111u);
    CHECK(ND_T9_BROWSER_KEY_MODE_123 <= 111u);

    /* And none of them may collide with a key the bridge already sends, or
     * the far side cannot tell a mode change from a keystroke. */
    CHECK(ND_T9_BROWSER_KEY_PAGE_UP != ND_KEY_UP);
    CHECK(ND_T9_BROWSER_KEY_PAGE_DOWN != ND_KEY_DOWN);
    CHECK(ND_T9_BROWSER_KEY_MODE_NAV != ND_KEY_ENTER);
    CHECK(ND_T9_BROWSER_KEY_MODE_ABC != ND_KEY_CLEAR);
    CHECK(ND_T9_BROWSER_KEY_MODE_UPPER != ND_KEY_LEFT);
    CHECK(ND_T9_BROWSER_KEY_MODE_123 != ND_KEY_RIGHT);
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
    RUN(test_star_sends_tab_in_the_shell);
    RUN(test_star_commits_a_pending_letter_first);
    RUN(test_star_still_types_a_star_in_numeric_mode);
    RUN(test_the_browser_starts_as_a_dpad);
    RUN(test_the_corners_of_the_pad_are_the_diagonals);
    RUN(test_star_and_zero_are_the_page_keys);
    RUN(test_a_key_the_phone_does_not_have_is_still_inert);
    RUN(test_hash_walks_nav_abc_upper_123_and_back);
    RUN(test_the_mode_is_announced_as_a_keycode);
    RUN(test_the_signalling_codes_are_inside_libnsfbs_range);
    RUN(test_a_bridge_without_a_matrix_is_a_no_op);
    return pt_report("test_uinput");
}
